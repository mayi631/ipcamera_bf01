#ifndef _CVI_RTSP_H
#define _CVI_RTSP_H

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_RTSP_VIDEO_FRAME 8
#define MAX_RTSP_NAME_LEN (32)
#define RTSP_INSTANCE_NUM (4)

typedef enum {
    CVI_RTSP_VIDEO_H264 = 0,
    CVI_RTSP_VIDEO_H265,
    CVI_RTSP_VIDEO_MJPEG,
    CVI_RTSP_VIDEO_BUTT
} CVI_RTSP_VIDEO_FORMAT_E;

typedef enum {
    CVI_RTSP_AUDIO_PCM = 0,
    CVI_RTSP_AUDIO_AAC,
    CVI_RTSP_AUDIO_BUTT
} CVI_RTSP_AUDIO_FORMAT_E;

typedef enum FRAME_TYPE_E {
    FRAME_TYPE_VIDEO = 0,
    FRAME_TYPE_AUDIO,
    FRAME_TYPE_BUTT
} FRAME_TYPE_E;

typedef struct CVI_RTSP_FRAME {
    FRAME_TYPE_E type;
    int32_t iskey[MAX_RTSP_VIDEO_FRAME];
    uint64_t vi_pts[MAX_RTSP_VIDEO_FRAME];
    unsigned char *data[MAX_RTSP_VIDEO_FRAME];
    size_t len[MAX_RTSP_VIDEO_FRAME];
} CVI_RTSP_FRAME_S;

typedef enum CVI_RTSP_EVENT_TYPE {
    CVI_RTSP_EVENT_CLI_CONNECT,
    CVI_RTSP_EVENT_CLI_DISCONNECT,
    CVI_RTSP_EVENT_CLI_BUTT
} CVI_RTSP_EVENT_TYPE_E;

typedef struct CVI_RTSP_EVENT {
    int32_t rtsp_id;
    char rtsp_name[MAX_RTSP_NAME_LEN];
    CVI_RTSP_EVENT_TYPE_E e;
    char cli_ipaddr[64];
} CVI_RTSP_EVENT_S;

typedef void(CVI_RTSP_EVENT_CALLBACK)(CVI_RTSP_EVENT_S *e);

typedef struct CVI_RTSP_INFO {
    int32_t max_conn;
    int32_t timeout;
    int32_t port;
    char username[MAX_RTSP_NAME_LEN];
    char password[MAX_RTSP_NAME_LEN];
} CVI_RTSP_INFO_S;

typedef struct CVI_RTSP_MEDIA_INFO {
    int32_t id;
    char rtsp_name[MAX_RTSP_NAME_LEN];
    CVI_RTSP_VIDEO_FORMAT_E video_codec;
    CVI_RTSP_AUDIO_FORMAT_E audio_codec;
    float framerate;
    int32_t bitrate_kbps;
    int32_t video_port;
    int32_t audio_sample_rate;
    int32_t audio_channels;
    int32_t audio_pernum;
    int32_t audio_port;
    int32_t rtp_port; /*Must be an even number*/
    CVI_RTSP_EVENT_CALLBACK *cb;
} CVI_RTSP_MEDIA_INFO_S;

int32_t CVI_RTSP_SendFrame(void *hdl, CVI_RTSP_FRAME_S *frame);
int32_t CVI_RTSP_Create(void **hdl, CVI_RTSP_INFO_S *info, CVI_RTSP_MEDIA_INFO_S *media_info);
int32_t CVI_RTSP_Destroy(void *hdl);

#ifdef __cplusplus
}
#endif

#endif