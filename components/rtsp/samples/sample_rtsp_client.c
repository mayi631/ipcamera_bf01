/*
 * RTSP 客户端示例：
 * 1) 连接 RTSP URL（TCP/UDP）；
 * 2) 拉流并把接收到的完整帧写入文件；
 * 3) 退出时由 RTSP_ClientDestroy 发送 TEARDOWN。
 *
 * 输出为裸码流（H264/H265/MJPEG）或裸音频 payload（AAC/PCM）。
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cvi_log.h"
#include "cvi_osal.h"
#include "rtsp.h"

typedef struct SAMPLE_RTSP_CLIENT_ATTR {
	char url[256];
	RTSP_TRANSPORT_E transport;
	char video_out[128];
	char audio_out[128];
	int32_t duration_sec;
} SAMPLE_RTSP_CLIENT_ATTR_S;

static void sample_rtsp_client_usage(const char *argv0)
{
	printf("用法: %s -u <rtsp_url> [-t tcp|udp] [-v video_out] [-a audio_out] [-d sec]\n", argv0);
}

static int32_t sample_rtsp_client_parse(int32_t argc, char **argv, SAMPLE_RTSP_CLIENT_ATTR_S *attr)
{
	if (attr == NULL) {
		return -1;
	}
	memset(attr, 0, sizeof(*attr));
	attr->transport = RTSP_TRANS_TCP;
	attr->duration_sec = 10;

	for (int32_t i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-u") && i + 1 < argc) {
			strncpy(attr->url, argv[++i], sizeof(attr->url) - 1);
		} else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
			const char *t = argv[++i];
			if (!strcmp(t, "udp")) {
				attr->transport = RTSP_TRANS_UDP;
			} else {
				attr->transport = RTSP_TRANS_TCP;
			}
		} else if (!strcmp(argv[i], "-v") && i + 1 < argc) {
			strncpy(attr->video_out, argv[++i], sizeof(attr->video_out) - 1);
		} else if (!strcmp(argv[i], "-a") && i + 1 < argc) {
			strncpy(attr->audio_out, argv[++i], sizeof(attr->audio_out) - 1);
		} else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			attr->duration_sec = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			sample_rtsp_client_usage(argv[0]);
			return 1;
		}
	}

	if (attr->url[0] == '\0') {
		sample_rtsp_client_usage(argv[0]);
		return -1;
	}

	if (attr->video_out[0] == '\0') {
		strncpy(attr->video_out, "recv_video.bin", sizeof(attr->video_out) - 1);
	}
	if (attr->audio_out[0] == '\0') {
		strncpy(attr->audio_out, "recv_audio.bin", sizeof(attr->audio_out) - 1);
	}

	return 0;
}

int main(int argc, char *argv[])
{
	SAMPLE_RTSP_CLIENT_ATTR_S attr;
	int32_t ret = sample_rtsp_client_parse(argc, argv, &attr);
	if (ret != 0) {
		return ret > 0 ? 0 : -1;
	}

	RTSP_CLIENT_ATTR_S cli_attr;
	memset(&cli_attr, 0, sizeof(cli_attr));
	strncpy(cli_attr.url, attr.url, sizeof(cli_attr.url) - 1);
	cli_attr.transport = attr.transport;
	cli_attr.timeout_ms = 3000;
	cli_attr.max_frame_size = 8 * 1024 * 1024;

	void *hdl = NULL;
	ret = RTSP_ClientCreate(&hdl, &cli_attr);
	if (ret != 0) {
		CVI_LOGE("rtsp client 创建失败");
		return -1;
	}

	FILE *vfp = fopen(attr.video_out, "wb");
	FILE *afp = fopen(attr.audio_out, "wb");
	if (!vfp) {
		CVI_LOGE("打开视频文件失败 %s", attr.video_out);
	}
	if (!afp) {
		CVI_LOGE("打开音频文件失败 %s", attr.audio_out);
	}

	uint64_t start = 0;
	cvi_osal_get_boot_time_us(&start);
	while (1) {
		uint64_t now = 0;
		cvi_osal_get_boot_time_us(&now);
		if (attr.duration_sec > 0 && now - start >= (uint64_t)attr.duration_sec * 1000 * 1000) {
			break;
		}
		RTSP_FRAME_S frame;
		memset(&frame, 0, sizeof(frame));
		frame.type = FRAME_TYPE_BUTT;
		ret = RTSP_RecvFrame(hdl, &frame, 200);
		if (ret == 0) {
			if (frame.type == FRAME_TYPE_VIDEO && vfp) {
				fwrite(frame.data[0], 1, frame.len[0], vfp);
				fflush(vfp);
			} else if (frame.type == FRAME_TYPE_AUDIO && afp) {
				fwrite(frame.data[0], 1, frame.len[0], afp);
				fflush(afp);
			}
			RTSP_ReleaseFrame(hdl, &frame);
		} else {
			cvi_osal_task_sleep(10 * 1000);
		}
	}

	if (vfp) {
		fclose(vfp);
	}
	if (afp) {
		fclose(afp);
	}

	RTSP_ClientDestroy(hdl);
	return 0;
}
