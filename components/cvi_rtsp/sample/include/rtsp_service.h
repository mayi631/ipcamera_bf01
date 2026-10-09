
#ifndef _CVI_RTSP_SER_API_H_
#define _CVI_RTSP_SER_API_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CVI_RTSP_VIDEO_CODEC_H264 = 0,
    CVI_RTSP_VIDEO_CODEC_H265,
    CVI_RTSP_VIDEO_CODEC_MJPEG,
    CVI_RTSP_VIDEO_CODEC_BUTT
} CVI_RTSP_VIDEO_CODEC_E;

typedef enum {
    CVI_RTSP_AUDIO_CODEC_NONE,
    CVI_RTSP_AUDIO_CODEC_PCM,
    CVI_RTSP_AUDIO_CODEC_AAC,
    CVI_RTSP_AUDIO_CODEC_BUTT
} CVI_RTSP_AUDIO_CODEC_E;

typedef struct CVI_RTSP_SERVICE_PARAM {
    int32_t rtsp_id;
    char rtsp_name[32];
    int32_t max_conn;
    int32_t timeout;
    int32_t port;
    CVI_RTSP_VIDEO_CODEC_E video_codec;
    CVI_RTSP_AUDIO_CODEC_E audio_codec;

    uint32_t width;
    uint32_t height;
    float framerate;
    int32_t bitrate_kbps;
    int32_t audio_sample_rate;
    int32_t audio_channels;
    int32_t audio_pernum;

} CVI_RTSP_SERVICE_PARAM_S;

typedef void *CVI_RTSP_SERVICE_HANDLE_T;

#ifdef __cplusplus
}
#endif

#endif
