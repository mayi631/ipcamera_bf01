#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "app_ipcam_paramparse.h"
#include "app_ipcam_stream.h"
#include "bf01_rtsp_publish.h"
#include "cvi_mbuf.h"
#include "cvi_sys.h"
#include "app_ipcam_event.h"
#include "app_ipcam_capture.h"
#include "app_ipcam_venc.h"

static pthread_mutex_t stream_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t stream_thread;
static int stream_joinable;
static int stream_running;
static int stream_stopping;
static int stream_error;
static unsigned int stream_generation;
static unsigned int stream_channel;
static uint64_t stream_frames;
static uint64_t stream_expiry;
static unsigned int stream_reconnects;
static char stream_id[129];
static char stream_url[1600];
static char stream_state[16] = "idle";

static uint64_t stream_clock(void)
{
    struct timespec timestamp;
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    return (uint64_t)timestamp.tv_sec * 1000 + timestamp.tv_nsec / 1000000;
}

static int stopping(void)
{
    pthread_mutex_lock(&stream_mutex);
    int result = stream_stopping;
    pthread_mutex_unlock(&stream_mutex);
    return result;
}

static int stream_session(void)
{
    APP_VENC_CHN_CFG_S *encoder = app_ipcam_VencChnCfg_Get((VENC_CHN)stream_channel);
    CVI_MBUF_HANDLE *reader = NULL;
    CVI_MEDIA_FRAME_INFO_T frame = {0};
    struct bf01_rtsp_publish *publisher = NULL;
    int error = 0;
    if (!encoder || !encoder->bEnable || !encoder->bStart || !(encoder->StreamTo & 1) ||
        (encoder->enType != PT_H264 && encoder->enType != PT_H265) || encoder->enGopMode == VENC_GOPMODE_BIPREDB)
        error = ENODEV;
    if (!error) {
        reader = app_ipcam_Mbuf_CreateReader(encoder->VencChn, 1);
        frame.frameBuf = malloc(CVI_MBUF_STREAM_MAX_SIZE);
        if (!reader || !frame.frameBuf)
            error = ENOMEM;
        else if (CVI_VENC_RequestIDR(encoder->VencChn, CVI_TRUE) != CVI_SUCCESS)
            error = EIO;
    }
    uint64_t progress = stream_clock();
    uint64_t previous_pts = 0;
    uint32_t previous_index = 0;
    uint64_t recovery_started = 0;
    unsigned int recovery_discarded = 0;
    const int codec = !error && encoder->enType == PT_H265 ? 265 : 264;
    while (!error && !stopping()) {
        uint64_t now = stream_clock();
        if (stream_expiry && app_event_realtime() >= stream_expiry) {
            error = ESTALE;
            break;
        }
        if (now - progress > (publisher ? 5000 : 10000)) {
            fprintf(stderr, "[live] phase=%s errno=%d stalled_ms=%llu discarded=%u\n",
                recovery_started ? "resync" : (publisher ? "read-frame" : "first-frame"),
                ETIMEDOUT, (unsigned long long)(now - progress), recovery_discarded);
            error = ETIMEDOUT;
            break;
        }
        frame.frameBufLen = CVI_MBUF_STREAM_MAX_SIZE;
        int received = app_ipcam_Mbuf_ReadFrame(reader, 0, &frame, 100);
        if (received <= 0) {
            if (received < 0)
                usleep(10000);
            continue;
        }
        if (frame.frameParam.frameType == CVI_MEDIA_AFRAME_A)
            continue;
        if (!frame.frameParam.frameLen || frame.frameParam.frameLen > CVI_MBUF_STREAM_MAX_SIZE) {
            error = EIO;
            break;
        }
        CVI_U64 current_pts = 0;
        if (CVI_SYS_GetCurPTS(&current_pts) == CVI_SUCCESS &&
            current_pts > frame.frameParam.framePts && current_pts - frame.frameParam.framePts > 500000) {
            if (!recovery_started) {
                recovery_started = stream_clock();
                int request = CVI_VENC_RequestIDR(encoder->VencChn, CVI_TRUE);
                fprintf(stderr, "[live] stale frame age_ms=%llu index=%u request_rc=%d\n",
                    (unsigned long long)((current_pts - frame.frameParam.framePts) / 1000),
                    frame.frameParam.frameIndex, request);
            }
            recovery_discarded++;
            continue;
        }
        uint32_t frame_index = frame.frameParam.frameIndex;
        if (publisher && (recovery_started || frame_index != (uint32_t)(previous_index + 1u))) {
            int idr = bf01_rtsp_publish_is_idr(codec, frame.frameBuf, frame.frameParam.frameLen);
            if (!recovery_started) {
                recovery_started = stream_clock();
                int request = idr ? CVI_SUCCESS : CVI_VENC_RequestIDR(encoder->VencChn, CVI_TRUE);
                fprintf(stderr, "[live] sequence gap expected=%u received=%u idr=%d request_rc=%d\n",
                    (uint32_t)(previous_index + 1u), frame_index, idr, request);
            }
            if (!idr) {
                recovery_discarded++;
                continue;
            }
        }
        if (!publisher) {
            if (frame.frameParam.frameType != CVI_MEDIA_VFRAME_I ||
                !bf01_rtsp_publish_is_idr(codec, frame.frameBuf, frame.frameParam.frameLen))
                continue;
            error = bf01_rtsp_publish_open(&publisher, stream_url,
                codec, encoder->u32Width, encoder->u32Height, frame.frameBuf, frame.frameParam.frameLen,
                frame.frameParam.framePts);
            if (error == ENODATA && stream_clock() - progress <= 10000) {
                error = 0;
                CVI_VENC_RequestIDR(encoder->VencChn, CVI_TRUE);
                continue;
            }
            if (error)
                break;
        }
        if (previous_pts && frame.frameParam.framePts <= previous_pts) {
            error = EPROTO;
            break;
        }
        previous_pts = frame.frameParam.framePts;
        error = bf01_rtsp_publish_frame(publisher, frame.frameBuf, frame.frameParam.frameLen, previous_pts);
        if (error)
            break;
        progress = stream_clock();
        previous_index = frame_index;
        if (recovery_started) {
            fprintf(stderr, "[live] resynced on IDR index=%u discarded=%u elapsed_ms=%llu\n",
                frame_index, recovery_discarded, (unsigned long long)(progress - recovery_started));
            recovery_started = 0;
            recovery_discarded = 0;
        }
        pthread_mutex_lock(&stream_mutex);
        stream_frames++;
        if (!stream_stopping && strcmp(stream_state, "pushing")) {
            strcpy(stream_state, "pushing");
            stream_error = 0;
            stream_generation++;
            fprintf(stderr, "[live] publishing streamId=%s channel=%u first_pts=%llu\n",
                stream_id, stream_channel, (unsigned long long)previous_pts);
        }
        pthread_mutex_unlock(&stream_mutex);
    }
    if (error)
        bf01_rtsp_publish_abort(publisher);
    else
        bf01_rtsp_publish_close(publisher);
    if (reader)
        app_ipcam_Mbuf_DestoryReader(reader);
    free(frame.frameBuf);
    return error;
}

static int stream_retryable(int error)
{
    return error == ETIMEDOUT || error == ECONNRESET || error == EPIPE ||
        error == ECONNREFUSED || error == ENETDOWN || error == ENETRESET ||
        error == ENETUNREACH || error == EHOSTUNREACH || error == EAGAIN;
}

static void *stream_worker(void *context)
{
    (void)context;
    int error;
    unsigned int delay = 500;
    for (;;) {
        uint64_t previous_frames = stream_frames;
        error = stream_session();
        if (stopping() || !stream_retryable(error) || !stream_expiry)
            break;
        if (app_event_realtime() >= stream_expiry) {
            error = ESTALE;
            break;
        }
        if (stream_frames != previous_frames)
            delay = 500;
        pthread_mutex_lock(&stream_mutex);
        stream_error = error;
        strcpy(stream_state, "starting");
        stream_reconnects++;
        stream_generation++;
        fprintf(stderr, "[live] reconnect streamId=%s attempt=%u errno=%d frames=%llu backoff_ms=%u\n",
            stream_id, stream_reconnects, error, (unsigned long long)stream_frames, delay);
        pthread_mutex_unlock(&stream_mutex);
        uint64_t deadline = stream_clock() + delay;
        while (!stopping() && stream_clock() < deadline && app_event_realtime() < stream_expiry)
            usleep(100000);
        if (stopping())
            break;
        if (app_event_realtime() >= stream_expiry) {
            error = ESTALE;
            break;
        }
        delay = delay < 2500 ? delay * 2 : 5000;
    }
    pthread_mutex_lock(&stream_mutex);
    stream_error = stream_stopping ? 0 : error;
    strcpy(stream_state, stream_stopping ? "idle" : "fail");
    stream_generation++;
    stream_running = 0;
    fprintf(stderr, "[live] finished streamId=%s state=%s errno=%d frames=%llu\n",
        stream_id, stream_state, stream_error, (unsigned long long)stream_frames);
    pthread_mutex_unlock(&stream_mutex);
    return NULL;
}

void app_ipcam_Stream_Status(cJSON *reply)
{
    pthread_mutex_lock(&stream_mutex);
    cJSON *status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, "streamId", stream_id);
    cJSON_AddStringToObject(status, "state", stream_state);
    cJSON_AddNumberToObject(status, "generation", stream_generation);
    cJSON_AddNumberToObject(status, "errno", stream_error);
    cJSON_AddNumberToObject(status, "frames", (double)stream_frames);
    cJSON_AddNumberToObject(status, "reconnects", stream_reconnects);
    cJSON_AddItemToObject(reply, "stream", status);
    pthread_mutex_unlock(&stream_mutex);
}

int app_ipcam_Stream_Apply(const cJSON *request, cJSON *reply)
{
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(request, "action");
    const cJSON *identity = cJSON_GetObjectItemCaseSensitive(request, "streamId");
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(request, "pushUrl");
    const cJSON *protocol = cJSON_GetObjectItemCaseSensitive(request, "pushProtocol");
    const cJSON *profile = cJSON_GetObjectItemCaseSensitive(request, "profile");
    const cJSON *expiry = cJSON_GetObjectItemCaseSensitive(request, "expireTime");
    if (!cJSON_IsString(action) || !cJSON_IsString(identity) || !*identity->valuestring ||
        strlen(identity->valuestring) >= sizeof(stream_id))
        return EINVAL;
    if (!strcmp(action->valuestring, "start")) {
        if (!cJSON_IsString(url) || strncmp(url->valuestring, "rtsp://", 7) ||
            strlen(url->valuestring) >= sizeof(stream_url) ||
            !cJSON_IsString(profile) || (strcmp(profile->valuestring, "main") && strcmp(profile->valuestring, "sub")) ||
            !cJSON_IsString(protocol) || strcmp(protocol->valuestring, "RTSP"))
            return ENOTSUP;
        if (!cJSON_IsNumber(expiry) ||
            !(expiry->valuedouble >= 1000000000000.0 && expiry->valuedouble <= 9999999999999.0) ||
            expiry->valuedouble <= (double)app_event_realtime())
            return ESTALE;
        int mode_error = app_event_set_live(1);
        if (!mode_error)
            mode_error = app_ipcam_Capture_Quiesce();
        if (!mode_error)
            mode_error = app_ipcam_Jpeg_Quiesce();
        if (mode_error)
            return mode_error;
    }
    pthread_mutex_lock(&stream_mutex);
    if (stream_joinable && !stream_running) {
        pthread_join(stream_thread, NULL);
        stream_joinable = 0;
    }
    int error = 0;
    if (!strcmp(action->valuestring, "stop")) {
        if (*stream_id && strcmp(stream_id, identity->valuestring))
            error = EINVAL;
        else if (stream_running) {
            stream_stopping = 1;
            strcpy(stream_state, "stopping");
            error = EINPROGRESS;
        } else if (strcmp(stream_state, "idle")) {
            strcpy(stream_state, "idle");
            stream_generation++;
        }
        if (!error && app_event_live())
            error = app_ipcam_Capture_Quiesce();
        if (!error && app_event_live())
            error = app_ipcam_Jpeg_Quiesce();
        if (!error)
            error = app_event_set_live(0);
    } else if (strcmp(action->valuestring, "start") || !cJSON_IsString(url) ||
        strlen(url->valuestring) >= sizeof(stream_url) || strncmp(url->valuestring, "rtsp://", 7) ||
        !cJSON_IsString(protocol) || strcmp(protocol->valuestring, "RTSP") || !cJSON_IsString(profile)) {
        error = ENOTSUP;
    } else if (stream_running) {
        if (strcmp(stream_id, identity->valuestring) || strcmp(stream_url, url->valuestring)) {
            stream_stopping = 1;
            strcpy(stream_state, "stopping");
            error = EINPROGRESS;
        } else
            error = !strcmp(stream_state, "pushing") ? 0 : EINPROGRESS;
    } else if (!strcmp(stream_id, identity->valuestring) && !strcmp(stream_url, url->valuestring) &&
        !strcmp(stream_state, "fail") && !stream_retryable(stream_error)) {
        error = stream_error ? stream_error : EIO;
    } else {
        APP_PARAM_RTSP_T *rtsp = app_ipcam_Rtsp_Param_Get();
        unsigned int session = !strcmp(profile->valuestring, "main") ? 0 : 1;
        if ((!strcmp(profile->valuestring, "main") || !strcmp(profile->valuestring, "sub")) &&
            session < (unsigned int)rtsp->session_cnt && session < RTSP_INSTANCE_NUM) {
            stream_channel = rtsp->VencChn[session];
            strcpy(stream_id, identity->valuestring);
            strcpy(stream_url, url->valuestring);
            strcpy(stream_state, "starting");
            stream_error = 0;
            stream_stopping = 0;
            stream_frames = 0;
            stream_reconnects = 0;
            stream_expiry = (uint64_t)expiry->valuedouble;
            stream_generation++;
            stream_running = 1;
            error = pthread_create(&stream_thread, NULL, stream_worker, NULL);
            if (!error) {
                stream_joinable = 1;
                error = EINPROGRESS;
            } else {
                stream_running = 0;
                stream_error = error;
                strcpy(stream_state, "fail");
            }
        } else
            error = EINVAL;
    }
    pthread_mutex_unlock(&stream_mutex);
    app_ipcam_Stream_Status(reply);
    return error;
}

void app_ipcam_Stream_Stop(void)
{
    pthread_mutex_lock(&stream_mutex);
    stream_stopping = 1;
    int joinable = stream_joinable;
    pthread_t worker = stream_thread;
    pthread_mutex_unlock(&stream_mutex);
    if (joinable)
        pthread_join(worker, NULL);
    pthread_mutex_lock(&stream_mutex);
    stream_joinable = 0;
    pthread_mutex_unlock(&stream_mutex);
}
