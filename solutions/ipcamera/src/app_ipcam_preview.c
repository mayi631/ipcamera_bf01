#include "app_ipcam_preview.h"

#ifdef BF01_PREVIEW_SUPPORT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "app_ipcam_paramparse.h"
#include "bf01_preview.h"
#include "cvi_mbuf.h"

static CVI_MBUF_HANDLE *preview_reader;
static CVI_MEDIA_FRAME_INFO_T preview_frame;
static unsigned int parameter_sets;
static time_t last_frame;
static time_t last_keyframe;
static int ready_written;

int app_ipcam_Preview_Enabled(void)
{
    const char *enabled = getenv("BF01_PREVIEW");
    return enabled && !strcmp(enabled, "1");
}

static int preview_video_ready(void *opaque)
{
    unsigned int offset;
    int result;
    int ready;
    struct timespec now;
    (void)opaque;
    clock_gettime(CLOCK_MONOTONIC, &now);
    preview_frame.frameBufLen = CVI_MBUF_STREAM_MAX_SIZE;
    result = app_ipcam_Mbuf_ReadFrame(preview_reader, 0, &preview_frame, 1);
    if (result > 0 && preview_frame.frameParam.frameLen > 0 &&
        preview_frame.frameParam.frameLen <= CVI_MBUF_STREAM_MAX_SIZE &&
        preview_frame.frameParam.frameType != CVI_MEDIA_AFRAME_A) {
        last_frame = now.tv_sec;
        for (offset = 0; offset + 4 < preview_frame.frameParam.frameLen; offset++) {
            unsigned char *data = preview_frame.frameBuf + offset;
            unsigned int prefix = 0;
            unsigned int nal_type;
            if (!data[0] && !data[1] && data[2] == 1)
                prefix = 3;
            else if (!data[0] && !data[1] && !data[2] && data[3] == 1)
                prefix = 4;
            if (!prefix)
                continue;
            nal_type = data[prefix] & 31;
            if (nal_type == 7) {
                parameter_sets &= ~4U;
                parameter_sets |= 1;
            } else if (nal_type == 8) {
                parameter_sets &= ~4U;
                parameter_sets |= 2;
            } else if (nal_type == 5 && (parameter_sets & 3) == 3) {
                parameter_sets |= 4;
                last_keyframe = now.tv_sec;
            }
            offset += prefix;
        }
    }
    ready = parameter_sets == 7 && last_frame && last_keyframe &&
        now.tv_sec - last_frame < 3 && now.tv_sec - last_keyframe < 3;
    if (ready && !ready_written) {
        FILE *marker = fopen("/run/bf01-preview/media.ready", "w");
        if (marker) {
            fprintf(marker, "%ld\n", (long)getpid());
            fclose(marker);
            ready_written = 1;
        }
    } else if (!ready && ready_written) {
        unlink("/run/bf01-preview/media.ready");
        ready_written = 0;
    }
    return ready;
}

int app_ipcam_Preview_Start(void)
{
    APP_PARAM_RTSP_T *rtsp;
    APP_VENC_CHN_CFG_S *encoder;
    struct bf01_preview_config config;
    const char *session_text;
    char *end;
    long session = 1;
    if (!app_ipcam_Preview_Enabled())
        return 0;
    rtsp = app_ipcam_Rtsp_Param_Get();
    session_text = getenv("BF01_PREVIEW_SESSION");
    if (session_text) {
        session = strtol(session_text, &end, 10);
        if (!*session_text || *end)
            return -1;
    }
    if (session < 0 || session >= rtsp->session_cnt || session >= RTSP_INSTANCE_NUM)
        return -1;
    encoder = app_ipcam_VencChnCfg_Get(rtsp->VencChn[session]);
    if (!encoder || encoder->enType != PT_H264)
        return -1;
    preview_reader = app_ipcam_Mbuf_CreateReader(rtsp->VencChn[session], 1);
    preview_frame.frameBuf = malloc(CVI_MBUF_STREAM_MAX_SIZE);
    if (!preview_reader || !preview_frame.frameBuf) {
        app_ipcam_Preview_Stop();
        return -1;
    }
    memset(&config, 0, sizeof(config));
    config.interface_name = "wlan0";
    config.http_port = 80;
    config.rtsp_port = rtsp->port;
    config.session = (unsigned int)session;
    config.video_ready = preview_video_ready;
    if (bf01_preview_start(&config) != 0) {
        app_ipcam_Preview_Stop();
        return -1;
    }
    CVI_VENC_RequestIDR(rtsp->VencChn[session], CVI_TRUE);
    return 0;
}

void app_ipcam_Preview_Stop(void)
{
    bf01_preview_stop();
    if (preview_reader)
        app_ipcam_Mbuf_DestoryReader(preview_reader);
    preview_reader = NULL;
    free(preview_frame.frameBuf);
    preview_frame.frameBuf = NULL;
    parameter_sets = 0;
    last_frame = 0;
    last_keyframe = 0;
    ready_written = 0;
    if (app_ipcam_Preview_Enabled())
        unlink("/run/bf01-preview/media.ready");
}
#endif
