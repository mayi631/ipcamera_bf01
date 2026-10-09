
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cvi_log.h"
#include "cvi_osal.h"
#include "cvi_rtsp.h"
#include "rtsp_service.h"

typedef struct CVI_RTSP_SER_ATTR {
    int32_t id;
    char rtsp_name[MAX_RTSP_NAME_LEN];
    int32_t max_conn;
    int32_t timeout;
    int32_t port;
    int32_t auth_en;
    char username[MAX_RTSP_NAME_LEN];
    char password[MAX_RTSP_NAME_LEN];
    CVI_RTSP_VIDEO_FORMAT_E video_codec;
    CVI_RTSP_AUDIO_FORMAT_E audio_codec;
    int32_t audio_en;

    float framerate;
    int32_t bitrate_kbps;
    int32_t audio_sample_rate;
    int32_t audio_channels;
    int32_t audio_pernum;
} CVI_RTSP_SER_ATTR_S;

typedef struct RTSP_SERVICE_CONTEXT {
    int32_t ref;
    int32_t mute;
    char *mute_data;
    CVI_RTSP_SER_ATTR_S attr;
    cvi_osal_mutex_handle_t mutex;
    cvi_osal_task_handle_t video_task;
    int32_t video_fd;
    cvi_osal_task_handle_t audio_task;
    int32_t audio_fd;
    int32_t video_exit;
    int32_t audio_exit;
    void *rtsp_ser;
} RTSP_SERVICE_CONTEXT_S;

static RTSP_SERVICE_CONTEXT_S *gstRtspSerCtx[RTSP_INSTANCE_NUM];
static cvi_osal_mutex_t rtsp_ctx_mutex = CVI_OSAL_MUTEX_INITIALIZER_R;
static const char *videofilename, *audiofilename;

static int32_t rtsp_service_get_filesize(const char *filename)
{
    if (filename && strlen(filename) > 0) {
        FILE *file = fopen(filename, "rb");
        if (!file) {
            return 0;
        }

        fseek(file, 0, SEEK_END);
        size_t file_size = ftell(file);
        fclose(file);
        return file_size;
    }
    return 0;
}

static void rtsp_service_video_task(void *arg)
{
    RTSP_SERVICE_CONTEXT_S *ctx = (RTSP_SERVICE_CONTEXT_S *)arg;
    int32_t filesize = rtsp_service_get_filesize(videofilename);
    if (filesize == 0) {
        return;
    }
    int32_t fd = open(videofilename, O_RDONLY);
    unsigned char *buf = (unsigned char *)malloc(filesize);
    if (!buf) {
        return;
    }
    int32_t len = read(fd, buf, filesize);
    CVI_LOGD("videofilename len %d", len);
    close(fd);
    while (!ctx->video_exit) {
        CVI_RTSP_FRAME_S frame;
        memset(&frame, 0x0, sizeof(CVI_RTSP_FRAME_S));
        if (filesize > 0) {
            frame.data[0] = buf;
            frame.iskey[0] = 1;
            frame.len[0] = filesize;
            frame.type = FRAME_TYPE_VIDEO;
            cvi_osal_get_boot_time_us(&frame.vi_pts[0]);
            CVI_RTSP_SendFrame(ctx->rtsp_ser, &frame);
        }
        cvi_osal_task_sleep(200 * 1000);
    }

    if (buf) {
        free(buf);
    }
}

static void rtsp_service_audio_task(void *arg)
{
    RTSP_SERVICE_CONTEXT_S *ctx = (RTSP_SERVICE_CONTEXT_S *)arg;
    int32_t filesize = rtsp_service_get_filesize(audiofilename);
    if (filesize == 0) {
        return;
    }
    int32_t fd = open(audiofilename, O_RDONLY);
    unsigned char *buf = (unsigned char *)malloc(filesize);
    if (!buf) {
        return;
    }
    int32_t len = read(fd, buf, filesize);
    CVI_LOGD("audiofilename len %d", len);
    close(fd);
    while (!ctx->video_exit) {
        CVI_RTSP_FRAME_S frame;
        memset(&frame, 0x0, sizeof(CVI_RTSP_FRAME_S));
        if (filesize > 0) {
            frame.data[0] = buf;
            frame.iskey[0] = 1;
            frame.len[0] = filesize;
            frame.type = FRAME_TYPE_AUDIO;
            cvi_osal_get_boot_time_us(&frame.vi_pts[0]);
            CVI_RTSP_SendFrame(ctx->rtsp_ser, &frame);
        }
        cvi_osal_task_sleep(200 * 1000);
    }
    if (buf) {
        free(buf);
    }
}

static void rtsp_service_stop_media_by_name(char *name)
{
    if (name == NULL || strlen(name) <= 0) {
        return;
    }
    cvi_osal_mutex_lock(&rtsp_ctx_mutex);
    for (int32_t i = 0; i < RTSP_INSTANCE_NUM; i++) {
        RTSP_SERVICE_CONTEXT_S *c = gstRtspSerCtx[i];
        if (c) {
            cvi_osal_mutex_lock(c->mutex);
            if (strcmp(c->attr.rtsp_name, name) == 0) {
                c->ref--;
                if (c->ref == 0) {
                    c->video_exit = 1;
                    c->audio_exit = 1;
                    cvi_osal_task_join(c->video_task);
                    cvi_osal_task_join(c->audio_task);
                    cvi_osal_task_destroy(&c->video_task);
                    cvi_osal_task_destroy(&c->audio_task);
                }
                cvi_osal_mutex_unlock(c->mutex);
                break;
            }
            cvi_osal_mutex_unlock(c->mutex);
        }
    }
    cvi_osal_mutex_unlock(&rtsp_ctx_mutex);
}

static void rtsp_service_start_media_by_name(char *name)
{
    if (name == NULL || strlen(name) <= 0) {
        return;
    }

    cvi_osal_mutex_lock(&rtsp_ctx_mutex);
    for (int32_t i = 0; i < RTSP_INSTANCE_NUM; i++) {
        RTSP_SERVICE_CONTEXT_S *c = gstRtspSerCtx[i];
        if (c) {
            cvi_osal_mutex_lock(c->mutex);
            if (strcmp(c->attr.rtsp_name, name) == 0) {
                if (c->ref == 0) {
                    c->video_exit = 0;
                    cvi_osal_task_attr_t video;
                    static char v_name[16] = {0};
                    snprintf(v_name, sizeof(v_name), "v_%s", name);
                    video.name = v_name;
                    video.entry = rtsp_service_video_task;
                    video.param = (void *)c;
                    video.priority = CVI_OSAL_PRI_RT_MID;
                    video.detached = false;
                    video.stack_size = 128 * 1024;
                    cvi_osal_task_create(&video, &c->video_task);

                    c->audio_exit = 0;
                    cvi_osal_task_attr_t audio;
                    static char a_name[16] = {0};
                    snprintf(a_name, sizeof(a_name), "a_%s", name);
                    audio.name = v_name;
                    audio.entry = rtsp_service_audio_task;
                    audio.param = (void *)c;
                    audio.priority = CVI_OSAL_PRI_RT_MID;
                    audio.detached = false;
                    audio.stack_size = 128 * 1024;
                    cvi_osal_task_create(&audio, &c->audio_task);
                }
                c->ref++;
                CVI_LOGD("session %s start play %d", name, c->ref);
                cvi_osal_mutex_unlock(c->mutex);
                break;
            }
            cvi_osal_mutex_unlock(c->mutex);
        }
    }
    cvi_osal_mutex_unlock(&rtsp_ctx_mutex);
}

static void rtsp_service_event_cb(CVI_RTSP_EVENT_S *e)
{
    if (e) {
        switch (e->e) {
        case CVI_RTSP_EVENT_CLI_CONNECT: {
            CVI_LOGD("rtsp_service: recv %s CONNECT %s Event", e->rtsp_name, e->cli_ipaddr);
            rtsp_service_start_media_by_name(e->rtsp_name);
        } break;
        case CVI_RTSP_EVENT_CLI_DISCONNECT: {
            CVI_LOGD("rtsp_service: recv %s DISCONNECT %s Event", e->rtsp_name, e->cli_ipaddr);
            rtsp_service_stop_media_by_name(e->rtsp_name);
        } break;
        default:
            break;
        }
    }
}

static int32_t rtsp_service_create(CVI_RTSP_SERVICE_HANDLE_T *hdl, CVI_RTSP_SER_ATTR_S *attr)
{
    cvi_osal_mutex_lock(&rtsp_ctx_mutex);
    if (gstRtspSerCtx[attr->id] != NULL) {
        cvi_osal_mutex_unlock(&rtsp_ctx_mutex);
        CVI_LOGI("%s is exist", attr->rtsp_name);
        return -1;
    }
    CVI_LOGD("rtsp_service init start %s", attr->rtsp_name);
    RTSP_SERVICE_CONTEXT_S *ctx = (RTSP_SERVICE_CONTEXT_S *)malloc(sizeof(RTSP_SERVICE_CONTEXT_S));
    if (ctx == NULL) {
        cvi_osal_mutex_unlock(&rtsp_ctx_mutex);
        return -1;
    }
    memset(ctx, 0x0, sizeof(RTSP_SERVICE_CONTEXT_S));
    memcpy(&ctx->attr, attr, sizeof(CVI_RTSP_SER_ATTR_S));
    ctx->mute = ((attr->audio_en == 1) ? 0 : 1);
    cvi_osal_mutex_attr_t mutex_attr;
    mutex_attr.name = "rtsp_service_mutex";
    mutex_attr.type = PTHREAD_MUTEX_NORMAL;
    cvi_osal_mutex_create(&mutex_attr, &ctx->mutex);
    CVI_LOGD("rtsp_service init mute %d", ctx->mute);

    ctx->mute_data = (char *)malloc(attr->audio_pernum * 2);

    CVI_RTSP_INFO_S rtsp_info;
    memset(&rtsp_info, 0x0, sizeof(CVI_RTSP_INFO_S));
    CVI_RTSP_MEDIA_INFO_S media_info;
    memset(&media_info, 0x0, sizeof(CVI_RTSP_MEDIA_INFO_S));

    strcpy(rtsp_info.username, attr->username);
    strcpy(rtsp_info.password, attr->password);
    rtsp_info.max_conn = attr->max_conn;
    rtsp_info.timeout = attr->timeout;
    rtsp_info.port = attr->port;
    CVI_LOGD("rtsp_service init rtsp_info %d %d %d", attr->port, attr->max_conn, attr->timeout);

    media_info.id = attr->id;
    strcpy(media_info.rtsp_name, attr->rtsp_name);
    media_info.framerate = attr->framerate;
    media_info.bitrate_kbps = attr->bitrate_kbps;
    media_info.video_codec = attr->video_codec;
    media_info.audio_codec = attr->audio_codec;
    media_info.audio_channels = attr->audio_channels;
    media_info.audio_pernum = attr->audio_pernum;
    media_info.audio_sample_rate = attr->audio_sample_rate;
    media_info.rtp_port = 10000;
    media_info.video_port = 20000;
    media_info.audio_port = 21000;
    media_info.cb = rtsp_service_event_cb;
    CVI_LOGD("rtsp_service init media_info %f %d %d", attr->framerate, attr->video_codec, attr->audio_pernum);

    CVI_RTSP_Create(&ctx->rtsp_ser, &rtsp_info, &media_info);
    if (ctx->rtsp_ser == NULL) {
        free(ctx->mute_data);
        cvi_osal_mutex_destroy(ctx->mutex);
        free(ctx);
        cvi_osal_mutex_unlock(&rtsp_ctx_mutex);
        return -1;
    }

    CVI_LOGI("rtsp name %d %s create success", attr->id, ctx->attr.rtsp_name);
    gstRtspSerCtx[attr->id] = ctx;
    *hdl = (CVI_RTSP_SERVICE_HANDLE_T *)ctx;
    cvi_osal_mutex_unlock(&rtsp_ctx_mutex);
    return 0;
}

static void rtsp_service_destroy(CVI_RTSP_SERVICE_HANDLE_T hdl)
{
    RTSP_SERVICE_CONTEXT_S *ctx = (RTSP_SERVICE_CONTEXT_S *)hdl;
    if (ctx) {
        cvi_osal_mutex_lock(&rtsp_ctx_mutex);
        if (gstRtspSerCtx[ctx->attr.id] == NULL ||
            gstRtspSerCtx[ctx->attr.id] != ctx) {
            CVI_LOGI("%s is no exist", ctx->attr.rtsp_name);
            cvi_osal_mutex_unlock(&rtsp_ctx_mutex);
            return;
        }
        CVI_RTSP_Destroy(ctx->rtsp_ser);
        cvi_osal_mutex_unlock(&rtsp_ctx_mutex);
        while (ctx->ref > 0) {
            rtsp_service_stop_media_by_name(ctx->attr.rtsp_name);
        }
        free(ctx->mute_data);
        gstRtspSerCtx[ctx->attr.id] = NULL;
        CVI_LOGD("gstRtspSerCtx reset NULL %d", ctx->attr.id);
        cvi_osal_mutex_destroy(ctx->mutex);
        free(ctx);
    }
}

static int32_t rtsp_service_parse_params(int32_t argc, char *argv[], CVI_RTSP_SER_ATTR_S *attr)
{
    if (argc < 9) {
        CVI_LOGI("Usage: %s <videofilename payloadtype[h264/h265] fps bitrate> <audiofilename payloadtype[pcm/aac] samplerate chns pernum>", argv[0]);
        return -1;
    }

    videofilename = argv[1];
    if (access(videofilename, F_OK) != 0) {
        CVI_LOGE("video file %s not exist", videofilename);
        return -1;
    }
    if (strcmp(argv[2], "h264") == 0) {
        attr->video_codec = CVI_RTSP_VIDEO_H264;
    } else if (strcmp(argv[2], "h265") == 0) {
        attr->video_codec = CVI_RTSP_VIDEO_H265;
    } else {
        CVI_LOGE("unknown video format");
        return -1;
    }

    if (strtof(argv[3], NULL) == 0.0) {
        CVI_LOGE("framerate is 0");
        return -1;
    }
    attr->framerate = strtof(argv[3], NULL);

    if (atoi(argv[4]) == 0) {
        CVI_LOGE("bitrate is 0");
        return -1;
    }
    attr->bitrate_kbps = atoi(argv[4]);

    audiofilename = argv[5];
    if (access(audiofilename, F_OK) != 0) {
        CVI_LOGE("audio file %s not exist", audiofilename);
        return -1;
    }
    attr->audio_en = 1;
    if (strcmp(argv[6], "pcm") == 0) {
        attr->audio_codec = CVI_RTSP_AUDIO_PCM;
    } else if (strcmp(argv[6], "aac") == 0) {
        attr->audio_codec = CVI_RTSP_AUDIO_AAC;
    } else {
        CVI_LOGE("unknown audio format");
        return -1;
    }

    if (atoi(argv[7]) == 0) {
        CVI_LOGE("samplerate is 0");
        return -1;
    }
    attr->audio_sample_rate = atoi(argv[7]);

    if (atoi(argv[8]) == 0) {
        CVI_LOGE("channels is 0");
        return -1;
    }
    attr->audio_channels = atoi(argv[8]);

    if (atoi(argv[9]) == 0) {
        CVI_LOGE("pernum is 0");
        return -1;
    }
    attr->audio_pernum = atoi(argv[9]);

    attr->port = 554;
    attr->timeout = 120;
    strcpy(attr->username, "test");
    strcpy(attr->password, "123456");
    attr->max_conn = RTSP_INSTANCE_NUM;
    attr->auth_en = 0;

    return 0;
}

int32_t main(int32_t argc, char *argv[])
{
    CVI_RTSP_SER_ATTR_S attr;
    CVI_RTSP_SERVICE_HANDLE_T hdl[RTSP_INSTANCE_NUM];
    int32_t ret = rtsp_service_parse_params(argc, argv, &attr);
    if (ret != 0) {
        return -1;
    }

    for (int32_t i = 0; i < RTSP_INSTANCE_NUM; i++) {
        attr.id = i;
        snprintf(attr.rtsp_name, MAX_RTSP_NAME_LEN, "rtsp_%d", i);
        ret = rtsp_service_create(&hdl[i], &attr);
        if (ret != 0) {
            CVI_LOGE("rtsp_service_create %d failed", i);
            return -1;
        }
    }

    while (1) {
        sleep(1);
    }

    rtsp_service_destroy(hdl);
    return 0;
}
