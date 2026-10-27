/*
 * RTSP 客户端拉流实现。
 *
 * 数据流程：
 * 1) RTSP: OPTIONS/DESCRIBE -> 解析 SDP -> SETUP(video/audio) -> PLAY；
 * 2) RTP: TCP interleaved 或 UDP 接收 -> 解析 RTP -> H264/H265/MJPEG 重组 -> 写入 rbuf；
 * 3) API: RTSP_RecvFrame 从 rbuf 拉取完整帧，RTSP_ReleaseFrame 释放内存。
 *
 * 注意：TCP interleaved 的数据以 '$' 开头，与 RTSP 文本共用连接。
 * 如果在读 RTSP 响应时把 '$' 当成文本解析，会导致握手失败并断链，服务端触发 SIGPIPE。
 * 现在读响应时会跳过 interleaved 包，避免该问题。
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <unistd.h>

#include "rtsp_comp.h"

#define RTSP_CLIENT_BUF_SIZE 4096
#define RTSP_CLIENT_MAX_URL 256
#define RTSP_CLIENT_MAX_SESSION 64
#define RTSP_CLIENT_MAX_RTP_PKT 2048
#define RTSP_CLIENT_DEFAULT_TIMEOUT_MS 3000
#define RTSP_CLIENT_DEFAULT_MAX_FRAME (8 * 1024 * 1024)
#define RTSP_CLIENT_RBUF_MULTI 4

typedef struct RTSP_REASM {
	uint8_t *buf;
	uint32_t size;
	uint32_t used;
	uint32_t last_ts;
	int32_t in_fragment;
} RTSP_REASM_S;

typedef struct RTSP_CLIENT_TRACK {
	int32_t enable;
	FRAME_TYPE_E type;
	int32_t payload_type;
	int32_t codec;
	int32_t clock_rate;
	int32_t rtp_channel;
	int32_t rtcp_channel;
	int32_t rtp_fd;
	int32_t rtcp_fd;
	char control[RTSP_CLIENT_MAX_URL];
	RTSP_REASM_S reasm;
	void *rbuf;
} RTSP_CLIENT_TRACK_S;

typedef struct RTSP_CLIENT {
	int32_t rtsp_fd;
	char server_ip[64];
	unsigned short server_port;
	char base_url[RTSP_CLIENT_MAX_URL];
	char stream_url[RTSP_CLIENT_MAX_URL];
	char username[MAX_RTSP_NAME_LEN];
	char password[MAX_RTSP_NAME_LEN];
	int32_t cseq;
	char session_id[RTSP_CLIENT_MAX_SESSION];
	int32_t timeout_ms;
	RTSP_TRANSPORT_E transport;
	uint32_t max_frame_size;
	RTSP_CLIENT_TRACK_S track[FRAME_TYPE_BUTT];
	cvi_osal_task_handle_t recv_task;
	int32_t recv_exit;
	uint64_t last_stat_us;
} RTSP_CLIENT_S;

typedef struct RTSP_RTP_PKT {
	uint8_t marker;
	uint8_t payload_type;
	uint16_t seq;
	uint32_t ts;
	uint8_t *payload;
	uint32_t payload_len;
} RTSP_RTP_PKT_S;

/*
 * rbuf 内存分配回调，按 4K 对齐。
 */
static void *rtsp_client_malloc(uint32_t size, const char *name)
{
	(void)name;
	if (size > 0) {
		return malloc(RTSP_ALIGN(size, 4096));
	}
	return NULL;
}

/*
 * rbuf 内存释放回调。
 */
static void rtsp_client_free(void *p)
{
	if (p) {
		free(p);
	}
}

/*
 * 简易大小写不敏感子串查找。
 */
static char *rtsp_client_strcasestr(const char *haystack, const char *needle)
{
	if (haystack == NULL || needle == NULL) {
		return NULL;
	}
	size_t nlen = strlen(needle);
	if (nlen == 0) {
		return (char *)haystack;
	}
	for (const char *p = haystack; *p; p++) {
		if (strncasecmp(p, needle, nlen) == 0) {
			return (char *)p;
		}
	}
	return NULL;
}

/*
 * 重组缓存状态清零。
 */
static void rtsp_client_reasm_reset(RTSP_REASM_S *reasm)
{
	reasm->used = 0;
	reasm->in_fragment = 0;
}

/*
 * 追加重组数据，超限时重置并返回失败。
 */
static int32_t rtsp_client_reasm_append(RTSP_REASM_S *reasm, const uint8_t *data, uint32_t len)
{
	if (reasm->used + len > reasm->size) {
		rtsp_client_reasm_reset(reasm);
		return -1;
	}
	memcpy(reasm->buf + reasm->used, data, len);
	reasm->used += len;
	return 0;
}

/*
 * 解析 rtsp://ip:port/path URL。
 */
static int32_t rtsp_client_parse_url(const char *url, char *server, unsigned short *port, char *path, size_t path_len)
{
	if (url == NULL || server == NULL || port == NULL || path == NULL) {
		return -1;
	}

	char *p = NULL;
	char *p_full = (char *)malloc(strlen(url) + 1);
	if (p_full == NULL) {
		return -1;
	}
	strcpy(p_full, url);

	if (strncmp(p_full, "rtsp://", 7) != 0) {
		free(p_full);
		return -1;
	}

	char *p_host = p_full + 7;
	char *p_slash = strchr(p_host, '/');
	if (p_slash == NULL) {
		free(p_full);
		return -1;
	}
	*p_slash = '\0';
	p = strchr(p_host, ':');
	if (p != NULL) {
		*p = '\0';
		*port = (unsigned short)atoi(p + 1);
	} else {
		*port = 554;
	}
	strncpy(server, p_host, 63);
	server[63] = '\0';

	p_slash++;
	while (*p_slash == ' ' || *p_slash == '\t') {
		p_slash++;
	}
	snprintf(path, path_len, "%s", p_slash);
	free(p_full);
	return 0;
}

/*
 * 发送 RTSP 文本请求。
 */
static int32_t rtsp_client_send(int32_t fd, const char *msg)
{
	int32_t len = strlen(msg);
	return network_tcp_write(fd, (char *)msg, len);
}

/*
 * 读取 RTSP 响应，自动跳过 TCP interleaved 的 '$' RTP 包。
 * 这是避免握手阶段被 RTP 数据打断的关键。
 */
static int32_t rtsp_client_read_response(int32_t fd, char **out_buf, int32_t *out_len, int32_t timeout_ms)
{
	int32_t buf_size = RTSP_CLIENT_BUF_SIZE;
	char *buf = (char *)malloc(buf_size);
	int32_t total = 0;
	int32_t header_end = -1;
	int32_t content_len = 0;
	if (buf == NULL) {
		return -1;
	}

	uint64_t start = 0;
	cvi_osal_get_boot_time_us(&start);
	while (1) {
		uint64_t now = 0;
		cvi_osal_get_boot_time_us(&now);
		int32_t left_ms = timeout_ms - (int32_t)((now - start) / 1000);
		if (left_ms <= 0) {
			free(buf);
			return -1;
		}

		fd_set rset;
		struct timeval tv;
		FD_ZERO(&rset);
		FD_SET(fd, &rset);
		tv.tv_sec = left_ms / 1000;
		tv.tv_usec = (left_ms % 1000) * 1000;
		int32_t ret = select(fd + 1, &rset, NULL, NULL, &tv);
		if (ret <= 0) {
			free(buf);
			return -1;
		}
		unsigned char ch = 0;
		int32_t n = recv(fd, &ch, 1, 0);
		if (n <= 0) {
			free(buf);
			return -1;
		}
		if (ch == '$') {
			unsigned char hdr[3];
			for (int32_t i = 0; i < 3; i++) {
				n = recv(fd, &hdr[i], 1, 0);
				if (n <= 0) {
					free(buf);
					return -1;
				}
			}
			int32_t pkt_len = (hdr[1] << 8) | hdr[2];
			while (pkt_len > 0) {
				unsigned char dummy[256];
				int32_t to_read = pkt_len > (int32_t)sizeof(dummy) ? (int32_t)sizeof(dummy) : pkt_len;
				n = recv(fd, dummy, to_read, 0);
				if (n <= 0) {
					free(buf);
					return -1;
				}
				pkt_len -= n;
			}
			continue;
		}

		if (total + 1 >= buf_size) {
			buf_size *= 2;
			char *new_buf = (char *)realloc(buf, buf_size);
			if (new_buf == NULL) {
				free(buf);
				return -1;
			}
			buf = new_buf;
		}
		buf[total++] = ch;
		buf[total] = '\0';
		if (header_end < 0) {
			if (total >= 4 && memcmp(buf + total - 4, "\r\n\r\n", 4) == 0) {
				header_end = total;
				char *cl = rtsp_client_strcasestr(buf, "Content-Length");
				if (cl) {
					sscanf(cl, "%*[^0-9]%d", &content_len);
				}
			}
		}
		if (header_end >= 0) {
			while (total < header_end + content_len) {
				now = 0;
				cvi_osal_get_boot_time_us(&now);
				left_ms = timeout_ms - (int32_t)((now - start) / 1000);
				if (left_ms <= 0) {
					free(buf);
					return -1;
				}
				FD_ZERO(&rset);
				FD_SET(fd, &rset);
				tv.tv_sec = left_ms / 1000;
				tv.tv_usec = (left_ms % 1000) * 1000;
				ret = select(fd + 1, &rset, NULL, NULL, &tv);
				if (ret <= 0) {
					free(buf);
					return -1;
				}
				n = recv(fd, buf + total, buf_size - total - 1, 0);
				if (n <= 0) {
					free(buf);
					return -1;
				}
				total += n;
				buf[total] = '\0';
				if (total >= buf_size - 1) {
					buf_size *= 2;
					char *new_buf = (char *)realloc(buf, buf_size);
					if (new_buf == NULL) {
						free(buf);
						return -1;
					}
					buf = new_buf;
				}
			}
			break;
		}
	}

	*out_buf = buf;
	*out_len = total;
	return 0;
}

/*
 * 解析 RTSP 状态码。
 */
static int32_t rtsp_client_parse_status(const char *resp)
{
	int32_t code = -1;
	if (resp && sscanf(resp, "RTSP/1.0 %d", &code) == 1) {
		return code;
	}
	return -1;
}

/*
 * 从 RTSP 响应中解析 Session。
 */
static void rtsp_client_parse_session(const char *resp, char *session_id, size_t size)
{
	char *p = NULL;
	if (resp == NULL || session_id == NULL) {
		return;
	}
	p = rtsp_client_strcasestr(resp, "Session:");
	if (p == NULL) {
		return;
	}
	p += strlen("Session:");
	while (*p == ' ') {
		p++;
	}
	size_t i = 0;
	while (p[i] != '\0' && p[i] != '\r' && p[i] != '\n' && p[i] != ';') {
		i++;
	}
	if (i >= size) {
		i = size - 1;
	}
	memcpy(session_id, p, i);
	session_id[i] = '\0';
}

/*
 * 解析 Content-Base，供 control URL 拼接。
 */
static void rtsp_client_parse_content_base(const char *resp, char *base, size_t size)
{
	char *p = NULL;
	if (resp == NULL || base == NULL) {
		return;
	}
	p = rtsp_client_strcasestr(resp, "Content-Base:");
	if (p == NULL) {
		return;
	}
	p += strlen("Content-Base:");
	while (*p == ' ') {
		p++;
	}
	size_t i = 0;
	while (p[i] != '\0' && p[i] != '\r' && p[i] != '\n') {
		i++;
	}
	if (i >= size) {
		i = size - 1;
	}
	memcpy(base, p, i);
	base[i] = '\0';
}

/*
 * 拼接 control URL，支持绝对与相对地址。
 */
static void rtsp_client_build_control_url(const char *base, const char *control, char *out, size_t out_len)
{
	if (control == NULL || out == NULL || out_len == 0) {
		return;
	}
	if (strncmp(control, "rtsp://", 7) == 0) {
		snprintf(out, out_len, "%s", control);
		return;
	}
	if (base == NULL || base[0] == '\0') {
		snprintf(out, out_len, "%s", control);
		return;
	}
	if (base[strlen(base) - 1] == '/') {
		snprintf(out, out_len, "%s%s", base, control);
	} else {
		snprintf(out, out_len, "%s/%s", base, control);
	}
}

/*
 * 根据 rtpmap 推断视频编码类型。
 */
static int32_t rtsp_client_codec_from_rtpmap_video(const char *encoding)
{
	if (encoding == NULL) {
		return RTSP_VIDEO_BUTT;
	}
	if (strncasecmp(encoding, "H264", 4) == 0) {
		return RTSP_VIDEO_H264;
	}
	if (strncasecmp(encoding, "H265", 4) == 0 || strncasecmp(encoding, "HEVC", 4) == 0) {
		return RTSP_VIDEO_H265;
	}
	if (strncasecmp(encoding, "JPEG", 4) == 0 || strncasecmp(encoding, "MJPEG", 5) == 0) {
		return RTSP_VIDEO_MJPEG;
	}
	return RTSP_VIDEO_BUTT;
}

/*
 * 根据 rtpmap 推断音频编码类型。
 */
static int32_t rtsp_client_codec_from_rtpmap_audio(const char *encoding)
{
	if (encoding == NULL) {
		return RTSP_AUDIO_BUTT;
	}
	if (strncasecmp(encoding, "L16", 3) == 0) {
		return RTSP_AUDIO_PCM;
	}
	if (strncasecmp(encoding, "mpeg4-generic", 13) == 0 || strncasecmp(encoding, "AAC", 3) == 0) {
		return RTSP_AUDIO_AAC;
	}
	return RTSP_AUDIO_BUTT;
}

/*
 * 解析 SDP，填充 track 信息与 control 地址。
 */
static int32_t rtsp_client_parse_sdp(RTSP_CLIENT_S *client, const char *sdp)
{
	if (client == NULL || sdp == NULL) {
		return -1;
	}
	char *dup = strdup(sdp);
	if (dup == NULL) {
		return -1;
	}

	FRAME_TYPE_E cur_type = FRAME_TYPE_BUTT;
	char *line = strtok(dup, "\r\n");
	while (line) {
		if (strncmp(line, "m=video", 7) == 0) {
			cur_type = FRAME_TYPE_VIDEO;
			client->track[cur_type].enable = 1;
			client->track[cur_type].type = cur_type;
			int32_t pt = 0;
			if (sscanf(line, "m=video %*s %*s %d", &pt) == 1) {
				client->track[cur_type].payload_type = pt;
			}
		} else if (strncmp(line, "m=audio", 7) == 0) {
			cur_type = FRAME_TYPE_AUDIO;
			client->track[cur_type].enable = 1;
			client->track[cur_type].type = cur_type;
			int32_t pt = 0;
			if (sscanf(line, "m=audio %*s %*s %d", &pt) == 1) {
				client->track[cur_type].payload_type = pt;
			}
		} else if (strncmp(line, "a=rtpmap:", 9) == 0 && cur_type != FRAME_TYPE_BUTT) {
			int32_t pt = 0;
			char encoding[64] = {0};
			int32_t clock_rate = 0;
			if (sscanf(line, "a=rtpmap:%d %63[^/]/%d", &pt, encoding, &clock_rate) >= 2) {
				if (pt == client->track[cur_type].payload_type) {
					if (cur_type == FRAME_TYPE_VIDEO) {
						client->track[cur_type].codec = rtsp_client_codec_from_rtpmap_video(encoding);
						client->track[cur_type].clock_rate = clock_rate > 0 ? clock_rate : 90000;
					} else if (cur_type == FRAME_TYPE_AUDIO) {
						client->track[cur_type].codec = rtsp_client_codec_from_rtpmap_audio(encoding);
						client->track[cur_type].clock_rate = clock_rate;
					}
				}
			}
		} else if (strncmp(line, "a=control:", 10) == 0 && cur_type != FRAME_TYPE_BUTT) {
			char *p = line + 10;
			while (*p == ' ') {
				p++;
			}
			rtsp_client_build_control_url(client->base_url, p, client->track[cur_type].control, sizeof(client->track[cur_type].control));
		}

	line = strtok(NULL, "\r\n");
	}

	free(dup);

	if (client->track[FRAME_TYPE_VIDEO].enable) {
		int32_t pt = client->track[FRAME_TYPE_VIDEO].payload_type;
		if (client->track[FRAME_TYPE_VIDEO].codec == 0) {
			if (pt == H264_PAYLOAD_TYPE) {
				client->track[FRAME_TYPE_VIDEO].codec = RTSP_VIDEO_H264;
			} else if (pt == H265_PAYLOAD_TYPE) {
				client->track[FRAME_TYPE_VIDEO].codec = RTSP_VIDEO_H265;
			} else if (pt == MJPEG_PAYLOAD_TYPE) {
				client->track[FRAME_TYPE_VIDEO].codec = RTSP_VIDEO_MJPEG;
			}
		}
		if (client->track[FRAME_TYPE_VIDEO].clock_rate == 0) {
			client->track[FRAME_TYPE_VIDEO].clock_rate = 90000;
		}
		if (client->track[FRAME_TYPE_VIDEO].control[0] == '\0' && client->base_url[0] != '\0') {
			rtsp_client_build_control_url(client->base_url, "trackID=0", client->track[FRAME_TYPE_VIDEO].control,
										  sizeof(client->track[FRAME_TYPE_VIDEO].control));
		}
	}

	if (client->track[FRAME_TYPE_AUDIO].enable) {
		int32_t pt = client->track[FRAME_TYPE_AUDIO].payload_type;
		if (client->track[FRAME_TYPE_AUDIO].codec == 0) {
			if (pt == PCM_PAYLOAD_TYPE) {
				client->track[FRAME_TYPE_AUDIO].codec = RTSP_AUDIO_PCM;
			} else if (pt == AAC_PAYLOAD_TYPE) {
				client->track[FRAME_TYPE_AUDIO].codec = RTSP_AUDIO_AAC;
			}
		}
		if (client->track[FRAME_TYPE_AUDIO].control[0] == '\0' && client->base_url[0] != '\0') {
			rtsp_client_build_control_url(client->base_url, "trackID=1", client->track[FRAME_TYPE_AUDIO].control,
										  sizeof(client->track[FRAME_TYPE_AUDIO].control));
		}
	}
	return 0;
}

/*
 * 解析 RTP 头部并定位 payload。
 */
static int32_t rtsp_client_parse_rtp(const uint8_t *buf, uint32_t len, RTSP_RTP_PKT_S *pkt)
{
	if (len < 12 || buf == NULL || pkt == NULL) {
		return -1;
	}
	uint8_t vpxcc = buf[0];
	uint8_t mpt = buf[1];
	uint8_t cc = vpxcc & 0x0F;
	uint32_t hdr_len = 12 + (cc * 4);
	if (vpxcc & 0x10) {
		if (len < hdr_len + 4) {
			return -1;
		}
		uint16_t ext_len = (uint16_t)((buf[hdr_len + 2] << 8) | buf[hdr_len + 3]);
		hdr_len += 4 + (ext_len * 4);
	}
	if (len < hdr_len) {
		return -1;
	}

	pkt->marker = (mpt >> 7) & 0x1;
	pkt->payload_type = mpt & 0x7F;
	pkt->seq = (uint16_t)((buf[2] << 8) | buf[3]);
	pkt->ts = (uint32_t)((buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | buf[7]);
	pkt->payload = (uint8_t *)(buf + hdr_len);
	pkt->payload_len = len - hdr_len;
	return 0;
}

/*
 * 将重组完成的帧写入 rbuf。
 */
static int32_t rtsp_client_push_frame(RTSP_CLIENT_S *client, RTSP_CLIENT_TRACK_S *track, uint64_t pts, int32_t key)
{
	if (client == NULL || track == NULL || track->rbuf == NULL) {
		return -1;
	}
	if (track->reasm.used == 0) {
		return -1;
	}

	RTSP_FRAME_INFO_S frame_info;
	frame_info.hmagic = 0x5a5a5a5a;
	frame_info.tmagic = 0x5a5a5a5a;
	frame_info.type = track->type;
	frame_info.key = key;
	frame_info.len = track->reasm.used;
	frame_info.pts = pts;
	frame_info.totalsize = sizeof(RTSP_FRAME_INFO_S) + frame_info.len;

	uint32_t unused = CVI_RBUF_Unused(track->rbuf);
	if (unused < (uint32_t)frame_info.totalsize) {
		CVI_LOGE("rtsp client 缓存不足 unused=%u bytes need=%u bytes", unused, frame_info.totalsize);
		return -1;
	}

	CVI_RBUF_Copy_In(track->rbuf, &frame_info, sizeof(RTSP_FRAME_INFO_S), 0);
	CVI_RBUF_Copy_In(track->rbuf, track->reasm.buf, frame_info.len, sizeof(RTSP_FRAME_INFO_S));
	CVI_RBUF_Refresh_In(track->rbuf, frame_info.totalsize);
	return 0;
}

/*
 * 简单扫描 H264 帧内是否包含 IDR。
 */
static int32_t rtsp_client_is_h264_key(const uint8_t *buf, uint32_t len)
{
	if (buf == NULL || len < 5) {
		return 0;
	}
	uint32_t i = 0;
	while (i + 4 < len) {
		if (buf[i] == 0x00 && buf[i + 1] == 0x00 && buf[i + 2] == 0x00 && buf[i + 3] == 0x01) {
			uint8_t nal = buf[i + 4] & 0x1F;
			if (nal == 5) {
				return 1;
			}
		}
		i++;
	}
	return 0;
}

/*
 * 简单扫描 H265 帧内是否包含 IDR。
 */
static int32_t rtsp_client_is_h265_key(const uint8_t *buf, uint32_t len)
{
	if (buf == NULL || len < 6) {
		return 0;
	}
	uint32_t i = 0;
	while (i + 5 < len) {
		if (buf[i] == 0x00 && buf[i + 1] == 0x00 && buf[i + 2] == 0x00 && buf[i + 3] == 0x01) {
			uint8_t nal_type = (buf[i + 4] >> 1) & 0x3F;
			if (nal_type == 19 || nal_type == 20) {
				return 1;
			}
		}
		i++;
	}
	return 0;
}

/*
 * 处理 H264 RTP 包（单包或 FU-A 分片）。
 */
static int32_t rtsp_client_handle_h264(RTSP_CLIENT_S *client, RTSP_CLIENT_TRACK_S *track, RTSP_RTP_PKT_S *pkt)
{
	uint8_t start_code[4] = {0x00, 0x00, 0x00, 0x01};
	if (pkt->payload_len < 1) {
		return -1;
	}

	uint8_t nal_type = pkt->payload[0] & 0x1F;
	if (nal_type == 28) {
		if (pkt->payload_len < 2) {
			return -1;
		}
		uint8_t fu_hdr = pkt->payload[1];
		uint8_t s = (fu_hdr >> 7) & 0x1;
		uint8_t e = (fu_hdr >> 6) & 0x1;
		uint8_t nal_hdr = (pkt->payload[0] & 0xE0) | (fu_hdr & 0x1F);
		if (s) {
			rtsp_client_reasm_reset(&track->reasm);
			if (rtsp_client_reasm_append(&track->reasm, start_code, sizeof(start_code)) != 0) {
				return -1;
			}
			if (rtsp_client_reasm_append(&track->reasm, &nal_hdr, 1) != 0) {
				return -1;
			}
			track->reasm.in_fragment = 1;
		}
		if (track->reasm.in_fragment) {
			if (rtsp_client_reasm_append(&track->reasm, pkt->payload + 2, pkt->payload_len - 2) != 0) {
				return -1;
			}
		}
		if (e) {
			track->reasm.in_fragment = 0;
		}
	} else {
		if (rtsp_client_reasm_append(&track->reasm, start_code, sizeof(start_code)) != 0) {
			return -1;
		}
		if (rtsp_client_reasm_append(&track->reasm, pkt->payload, pkt->payload_len) != 0) {
			return -1;
		}
	}

	if (pkt->marker) {
		int32_t key = rtsp_client_is_h264_key(track->reasm.buf, track->reasm.used);
		rtsp_client_push_frame(client, track, pkt->ts, key);
		rtsp_client_reasm_reset(&track->reasm);
	}
	return 0;
}

/*
 * 处理 H265 RTP 包（单包或 FU 分片）。
 */
static int32_t rtsp_client_handle_h265(RTSP_CLIENT_S *client, RTSP_CLIENT_TRACK_S *track, RTSP_RTP_PKT_S *pkt)
{
	uint8_t start_code[4] = {0x00, 0x00, 0x00, 0x01};
	if (pkt->payload_len < 2) {
		return -1;
	}

	uint8_t nal_type = (pkt->payload[0] >> 1) & 0x3F;
	if (nal_type == 49) {
		if (pkt->payload_len < 3) {
			return -1;
		}
		uint8_t fu_hdr = pkt->payload[2];
		uint8_t s = (fu_hdr >> 7) & 0x1;
		uint8_t e = (fu_hdr >> 6) & 0x1;
		uint8_t new_hdr[2];
		new_hdr[0] = (pkt->payload[0] & 0x81) | ((fu_hdr & 0x3F) << 1);
		new_hdr[1] = pkt->payload[1];
		if (s) {
			rtsp_client_reasm_reset(&track->reasm);
			if (rtsp_client_reasm_append(&track->reasm, start_code, sizeof(start_code)) != 0) {
				return -1;
			}
			if (rtsp_client_reasm_append(&track->reasm, new_hdr, sizeof(new_hdr)) != 0) {
				return -1;
			}
			track->reasm.in_fragment = 1;
		}
		if (track->reasm.in_fragment) {
			if (rtsp_client_reasm_append(&track->reasm, pkt->payload + 3, pkt->payload_len - 3) != 0) {
				return -1;
			}
		}
		if (e) {
			track->reasm.in_fragment = 0;
		}
	} else {
		if (rtsp_client_reasm_append(&track->reasm, start_code, sizeof(start_code)) != 0) {
			return -1;
		}
		if (rtsp_client_reasm_append(&track->reasm, pkt->payload, pkt->payload_len) != 0) {
			return -1;
		}
	}

	if (pkt->marker) {
		int32_t key = rtsp_client_is_h265_key(track->reasm.buf, track->reasm.used);
		rtsp_client_push_frame(client, track, pkt->ts, key);
		rtsp_client_reasm_reset(&track->reasm);
	}
	return 0;
}

/*
 * 处理 MJPEG RTP 包（简化，只拼 payload）。
 */
static int32_t rtsp_client_handle_mjpeg(RTSP_CLIENT_S *client, RTSP_CLIENT_TRACK_S *track, RTSP_RTP_PKT_S *pkt)
{
	if (pkt->payload_len <= 8) {
		return -1;
	}
	uint32_t frag_offset = ((uint32_t)pkt->payload[1] << 16) | ((uint32_t)pkt->payload[2] << 8) | pkt->payload[3];
	uint8_t *jpeg_data = pkt->payload + 8;
	uint32_t jpeg_len = pkt->payload_len - 8;
	if (frag_offset == 0) {
		rtsp_client_reasm_reset(&track->reasm);
	}
	if (frag_offset + jpeg_len > track->reasm.size) {
		rtsp_client_reasm_reset(&track->reasm);
		return -1;
	}
	memcpy(track->reasm.buf + frag_offset, jpeg_data, jpeg_len);
	if (frag_offset + jpeg_len > track->reasm.used) {
		track->reasm.used = frag_offset + jpeg_len;
	}
	if (pkt->marker) {
		rtsp_client_push_frame(client, track, pkt->ts, 1);
		rtsp_client_reasm_reset(&track->reasm);
	}
	return 0;
}

/*
 * 处理音频 RTP 包（payload 直接成帧）。
 */
static int32_t rtsp_client_handle_audio(RTSP_CLIENT_S *client, RTSP_CLIENT_TRACK_S *track, RTSP_RTP_PKT_S *pkt)
{
	if (pkt->payload_len == 0) {
		return -1;
	}
	if (pkt->payload_len > track->reasm.size) {
		rtsp_client_reasm_reset(&track->reasm);
		return -1;
	}
	memcpy(track->reasm.buf, pkt->payload, pkt->payload_len);
	track->reasm.used = pkt->payload_len;
	rtsp_client_push_frame(client, track, pkt->ts, 1);
	rtsp_client_reasm_reset(&track->reasm);
	return 0;
}

/*
 * 根据 payload type 或 interleaved 通道分发 RTP 包。
 */
static int32_t rtsp_client_dispatch_rtp(RTSP_CLIENT_S *client, RTSP_RTP_PKT_S *pkt, int32_t channel)
{
	RTSP_CLIENT_TRACK_S *track = NULL;
	if (client == NULL || pkt == NULL) {
		return -1;
	}

	if (client->transport == RTSP_TRANS_TCP) {
		if (client->track[FRAME_TYPE_VIDEO].enable && channel == client->track[FRAME_TYPE_VIDEO].rtp_channel) {
			track = &client->track[FRAME_TYPE_VIDEO];
		} else if (client->track[FRAME_TYPE_AUDIO].enable && channel == client->track[FRAME_TYPE_AUDIO].rtp_channel) {
			track = &client->track[FRAME_TYPE_AUDIO];
		}
	} else {
		if (client->track[FRAME_TYPE_VIDEO].enable && pkt->payload_type == client->track[FRAME_TYPE_VIDEO].payload_type) {
			track = &client->track[FRAME_TYPE_VIDEO];
		} else if (client->track[FRAME_TYPE_AUDIO].enable && pkt->payload_type == client->track[FRAME_TYPE_AUDIO].payload_type) {
			track = &client->track[FRAME_TYPE_AUDIO];
		}
	}

	if (track == NULL) {
		return -1;
	}

	if (track->type == FRAME_TYPE_VIDEO) {
		if (track->codec == RTSP_VIDEO_H264) {
			return rtsp_client_handle_h264(client, track, pkt);
		} else if (track->codec == RTSP_VIDEO_H265) {
			return rtsp_client_handle_h265(client, track, pkt);
		} else if (track->codec == RTSP_VIDEO_MJPEG) {
			return rtsp_client_handle_mjpeg(client, track, pkt);
		}
	} else if (track->type == FRAME_TYPE_AUDIO) {
		return rtsp_client_handle_audio(client, track, pkt);
	}
	return -1;
}

/*
 * 周期输出 rbuf 使用情况（单位：bytes），用于观察缓存是否持续增长。
 */
static void rtsp_client_log_rbuf_stat(RTSP_CLIENT_S *client)
{
#if BUILT_LOG_LEVEL == CVI_LOG_DEBUG
	uint64_t now = 0;
	if (client == NULL) {
		return;
	}
	cvi_osal_get_boot_time_us(&now);
	if (client->last_stat_us == 0) {
		client->last_stat_us = now;
		return;
	}
	if (now - client->last_stat_us < 5 * 1000 * 1000) {
		return;
	}
	client->last_stat_us = now;

	uint32_t v_total = 0, v_unused = 0, v_cnt = 0;
	uint32_t a_total = 0, a_unused = 0, a_cnt = 0;
	RTSP_CLIENT_TRACK_S *v = &client->track[FRAME_TYPE_VIDEO];
	RTSP_CLIENT_TRACK_S *a = &client->track[FRAME_TYPE_AUDIO];

	if (v->enable && v->rbuf) {
		v_total = CVI_RBUF_Get_Totalsize(v->rbuf);
		v_unused = CVI_RBUF_Unused(v->rbuf);
		v_cnt = CVI_RBUF_DataCnt(v->rbuf, 0);
	}
	if (a->enable && a->rbuf) {
		a_total = CVI_RBUF_Get_Totalsize(a->rbuf);
		a_unused = CVI_RBUF_Unused(a->rbuf);
		a_cnt = CVI_RBUF_DataCnt(a->rbuf, 0);
	}
	uint32_t v_used = (v_total > v_unused) ? (v_total - v_unused) : 0;
	uint32_t a_used = (a_total > a_unused) ? (a_total - a_unused) : 0;

	CVI_LOGD("rtsp client rbuf统计 v_cnt:%u v_used:%u bytes v_total:%u bytes a_cnt:%u a_used:%u bytes a_total:%u bytes",
		v_cnt, v_used, v_total, a_cnt, a_used, a_total);
#else
	(void)client;
#endif
}

/*
 * TCP 读取指定长度数据。
 */
static int32_t rtsp_client_tcp_readn(RTSP_CLIENT_S *client, int32_t fd, uint8_t *buf, int32_t len)
{
	int32_t total = 0;
	while (total < len && !client->recv_exit) {
		fd_set rset;
		struct timeval tv;
		FD_ZERO(&rset);
		FD_SET(fd, &rset);
		tv.tv_sec = 0;
		tv.tv_usec = 200 * 1000;
		int32_t ret = select(fd + 1, &rset, NULL, NULL, &tv);
		if (ret <= 0) {
			continue;
		}
		int32_t n = recv(fd, buf + total, len - total, 0);
		if (n <= 0) {
			return -1;
		}
		total += n;
	}
	return total;
}

/*
 * TCP interleaved 接收循环。
 */
static void rtsp_client_recv_tcp(RTSP_CLIENT_S *client)
{
	uint8_t hdr[4];
	uint8_t rtp_buf[RTSP_CLIENT_MAX_RTP_PKT];
	while (!client->recv_exit) {
		int32_t ret = rtsp_client_tcp_readn(client, client->rtsp_fd, hdr, sizeof(hdr));
		if (ret != (int32_t)sizeof(hdr)) {
			continue;
		}
		if (hdr[0] != '$') {
			uint8_t discard[RTSP_CLIENT_BUF_SIZE];
			int32_t used = 0;
			memcpy(discard, hdr, sizeof(hdr));
			used = sizeof(hdr);
			while (!client->recv_exit && used < (int32_t)sizeof(discard)) {
				ret = rtsp_client_tcp_readn(client, client->rtsp_fd, discard + used, 1);
				if (ret <= 0) {
					break;
				}
				used += ret;
				if (used >= 4 && memcmp(discard + used - 4, "\r\n\r\n", 4) == 0) {
					break;
				}
			}
			continue;
		}
		int32_t channel = hdr[1];
		uint16_t pkt_len = (uint16_t)((hdr[2] << 8) | hdr[3]);
		if (pkt_len == 0 || pkt_len > RTSP_CLIENT_MAX_RTP_PKT) {
			int32_t skip = 0;
			uint8_t dummy[256];
			while (skip < pkt_len && !client->recv_exit) {
				int32_t need = pkt_len - skip;
				if (need > (int32_t)sizeof(dummy)) {
					need = sizeof(dummy);
				}
				int32_t n = rtsp_client_tcp_readn(client, client->rtsp_fd, dummy, need);
				if (n <= 0) {
					break;
				}
				skip += n;
			}
			continue;
		}
		ret = rtsp_client_tcp_readn(client, client->rtsp_fd, rtp_buf, pkt_len);
		if (ret != pkt_len) {
			continue;
		}
		RTSP_RTP_PKT_S pkt;
		if (rtsp_client_parse_rtp(rtp_buf, pkt_len, &pkt) == 0) {
			rtsp_client_dispatch_rtp(client, &pkt, channel);
		}
		rtsp_client_log_rbuf_stat(client);
	}
}

/*
 * UDP 接收循环（video/audio 双口）。
 */
static void rtsp_client_recv_udp(RTSP_CLIENT_S *client)
{
	uint8_t rtp_buf[RTSP_CLIENT_MAX_RTP_PKT];
	while (!client->recv_exit) {
		fd_set rset;
		int32_t maxfd = -1;
		struct timeval tv;
		FD_ZERO(&rset);
		if (client->track[FRAME_TYPE_VIDEO].enable && client->track[FRAME_TYPE_VIDEO].rtp_fd > 0) {
			FD_SET(client->track[FRAME_TYPE_VIDEO].rtp_fd, &rset);
			if (client->track[FRAME_TYPE_VIDEO].rtp_fd > maxfd) {
				maxfd = client->track[FRAME_TYPE_VIDEO].rtp_fd;
			}
		}
		if (client->track[FRAME_TYPE_AUDIO].enable && client->track[FRAME_TYPE_AUDIO].rtp_fd > 0) {
			FD_SET(client->track[FRAME_TYPE_AUDIO].rtp_fd, &rset);
			if (client->track[FRAME_TYPE_AUDIO].rtp_fd > maxfd) {
				maxfd = client->track[FRAME_TYPE_AUDIO].rtp_fd;
			}
		}
		if (maxfd < 0) {
			cvi_osal_task_sleep(50 * 1000);
			continue;
		}
		tv.tv_sec = 0;
		tv.tv_usec = 200 * 1000;
		int32_t ret = select(maxfd + 1, &rset, NULL, NULL, &tv);
		if (ret <= 0) {
			continue;
		}

		for (int32_t i = 0; i < FRAME_TYPE_BUTT; i++) {
			RTSP_CLIENT_TRACK_S *track = &client->track[i];
			if (!track->enable || track->rtp_fd <= 0) {
				continue;
			}
			if (!FD_ISSET(track->rtp_fd, &rset)) {
				continue;
			}
			struct sockaddr_in from;
			socklen_t from_len = sizeof(from);
			int32_t n = network_udp_read(track->rtp_fd, rtp_buf, sizeof(rtp_buf), &from, &from_len);
			if (n <= 0) {
				continue;
			}
			RTSP_RTP_PKT_S pkt;
			if (rtsp_client_parse_rtp(rtp_buf, n, &pkt) == 0) {
				rtsp_client_dispatch_rtp(client, &pkt, -1);
			}
		}
		rtsp_client_log_rbuf_stat(client);
	}
}

/*
 * 接收线程入口，按传输方式分发。
 */
static void rtsp_client_recv_task(void *arg)
{
	RTSP_CLIENT_S *client = (RTSP_CLIENT_S *)arg;
	if (client->transport == RTSP_TRANS_TCP) {
		rtsp_client_recv_tcp(client);
	} else {
		rtsp_client_recv_udp(client);
	}
}

/*
 * 为 UDP 模式分配 RTP/RTCP 端口并 bind。
 */
static int32_t rtsp_client_setup_udp_ports(RTSP_CLIENT_TRACK_S *track)
{
	int32_t try_cnt = 0;
	while (try_cnt++ < 10) {
		int32_t rtp_fd = network_udp_open();
		if (rtp_fd < 0) {
			return -1;
		}
		if (network_udp_bind(rtp_fd, 0) != 0) {
			network_udp_close(rtp_fd);
			continue;
		}
		struct sockaddr_in addr;
		socklen_t addrlen = sizeof(addr);
		if (getsockname(rtp_fd, (struct sockaddr *)&addr, &addrlen) != 0) {
			network_udp_close(rtp_fd);
			continue;
		}
		uint16_t rtp_port = ntohs(addr.sin_port);
		if (rtp_port % 2 != 0) {
			network_udp_close(rtp_fd);
			continue;
		}
		int32_t rtcp_fd = network_udp_open();
		if (rtcp_fd < 0) {
			network_udp_close(rtp_fd);
			continue;
		}
		if (network_udp_bind(rtcp_fd, rtp_port + 1) != 0) {
			network_udp_close(rtp_fd);
			network_udp_close(rtcp_fd);
			continue;
		}
		track->rtp_fd = rtp_fd;
		track->rtcp_fd = rtcp_fd;
		return rtp_port;
	}
	return -1;
}

/*
 * 初始化 track 的重组缓存与 rbuf。
 */
static int32_t rtsp_client_init_track(RTSP_CLIENT_S *client, RTSP_CLIENT_TRACK_S *track)
{
	if (!track->enable) {
		return 0;
	}
	track->reasm.size = client->max_frame_size;
	track->reasm.buf = (uint8_t *)malloc(track->reasm.size);
	if (track->reasm.buf == NULL) {
		return -1;
	}
	rtsp_client_reasm_reset(&track->reasm);
	uint32_t rbuf_size = client->max_frame_size * RTSP_CLIENT_RBUF_MULTI;
	int32_t ret = CVI_RBUF_Init(&track->rbuf, rbuf_size, (track->type == FRAME_TYPE_VIDEO) ? "rtsp_c_v" : "rtsp_c_a", 1, rtsp_client_malloc, rtsp_client_free);
	if (ret != 0) {
		free(track->reasm.buf);
		track->reasm.buf = NULL;
		return -1;
	}
	CVI_LOGI("rtsp client %s track init reasm=%u bytes rbuf=%u bytes",
		(track->type == FRAME_TYPE_VIDEO) ? "video" : "audio", track->reasm.size, rbuf_size);
	return 0;
}

/*
 * 释放 track 相关资源。
 */
static void rtsp_client_deinit_track(RTSP_CLIENT_TRACK_S *track)
{
	if (track->rtp_fd > 0) {
		network_udp_close(track->rtp_fd);
		track->rtp_fd = -1;
	}
	if (track->rtcp_fd > 0) {
		network_udp_close(track->rtcp_fd);
		track->rtcp_fd = -1;
	}
	if (track->rbuf) {
		CVI_RBUF_DeInit(track->rbuf);
		track->rbuf = NULL;
	}
	if (track->reasm.buf) {
		free(track->reasm.buf);
		track->reasm.buf = NULL;
	}
}

/*
 * 发送 RTSP 请求并等待响应。
 */
static int32_t rtsp_client_do_request(RTSP_CLIENT_S *client, const char *msg, char **resp, int32_t *resp_len)
{
	if (rtsp_client_send(client->rtsp_fd, msg) != 0) {
		return -1;
	}
	if (rtsp_client_read_response(client->rtsp_fd, resp, resp_len, client->timeout_ms) != 0) {
		return -1;
	}
	return 0;
}

/*
 * 发送 SETUP，建立 RTP 传输参数。
 */
static int32_t rtsp_client_setup_track(RTSP_CLIENT_S *client, RTSP_CLIENT_TRACK_S *track)
{
	char msg[RTSP_CLIENT_BUF_SIZE] = {0};
	char *resp = NULL;
	int32_t resp_len = 0;
	int32_t ret = 0;

	if (!track->enable || track->control[0] == '\0') {
		return 0;
	}

	if (client->transport == RTSP_TRANS_TCP) {
		if (track->type == FRAME_TYPE_VIDEO) {
			track->rtp_channel = 0;
			track->rtcp_channel = 1;
		} else {
			track->rtp_channel = 2;
			track->rtcp_channel = 3;
		}
		if (client->session_id[0] != '\0') {
			snprintf(msg, sizeof(msg),
					"SETUP %s RTSP/1.0\r\n"
					"CSeq: %d\r\n"
					"Session: %s\r\n"
					"Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n\r\n",
					track->control, client->cseq++, client->session_id, track->rtp_channel, track->rtcp_channel);
		} else {
			snprintf(msg, sizeof(msg),
					"SETUP %s RTSP/1.0\r\n"
					"CSeq: %d\r\n"
					"Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n\r\n",
					track->control, client->cseq++, track->rtp_channel, track->rtcp_channel);
		}
	} else {
		int32_t rtp_port = rtsp_client_setup_udp_ports(track);
		if (rtp_port <= 0) {
			return -1;
		}
		if (client->session_id[0] != '\0') {
			snprintf(msg, sizeof(msg),
					"SETUP %s RTSP/1.0\r\n"
					"CSeq: %d\r\n"
					"Session: %s\r\n"
					"Transport: RTP/AVP;unicast;client_port=%d-%d\r\n\r\n",
					track->control, client->cseq++, client->session_id, rtp_port, rtp_port + 1);
		} else {
			snprintf(msg, sizeof(msg),
					"SETUP %s RTSP/1.0\r\n"
					"CSeq: %d\r\n"
					"Transport: RTP/AVP;unicast;client_port=%d-%d\r\n\r\n",
					track->control, client->cseq++, rtp_port, rtp_port + 1);
		}
	}

	ret = rtsp_client_do_request(client, msg, &resp, &resp_len);
	if (ret != 0) {
		return -1;
	}
	int32_t status = rtsp_client_parse_status(resp);
	if (status != 200) {
		free(resp);
		return -1;
	}
	if (client->session_id[0] == '\0') {
		rtsp_client_parse_session(resp, client->session_id, sizeof(client->session_id));
	}
	free(resp);
	return 0;
}

/*
 * 发送 TEARDOWN，通知服务端停止推流。
 */
static void rtsp_client_send_teardown(RTSP_CLIENT_S *client)
{
	if (client == NULL || client->rtsp_fd < 0) {
		return;
	}
	if (client->session_id[0] == '\0') {
		return;
	}

	char msg[RTSP_CLIENT_BUF_SIZE] = {0};
	char *resp = NULL;
	int32_t resp_len = 0;
	snprintf(msg, sizeof(msg),
			"TEARDOWN %s RTSP/1.0\r\n"
			"CSeq: %d\r\n"
			"Session: %s\r\n\r\n",
			client->stream_url, client->cseq++, client->session_id);
	if (rtsp_client_send(client->rtsp_fd, msg) == 0) {
		rtsp_client_read_response(client->rtsp_fd, &resp, &resp_len, client->timeout_ms);
		if (resp) {
			free(resp);
		}
	}
}

/*
 * 创建 RTSP 客户端：完成握手并启动接收线程。
 */
int32_t RTSP_ClientCreate(void **hdl, RTSP_CLIENT_ATTR_S *attr)
{
	if (hdl == NULL || attr == NULL) {
		return -1;
	}
	if (attr->url[0] == '\0') {
		return -1;
	}

	RTSP_CLIENT_S *client = (RTSP_CLIENT_S *)malloc(sizeof(RTSP_CLIENT_S));
	if (client == NULL) {
		return -1;
	}
	memset(client, 0, sizeof(RTSP_CLIENT_S));
	client->rtsp_fd = -1;
	client->timeout_ms = attr->timeout_ms > 0 ? attr->timeout_ms : RTSP_CLIENT_DEFAULT_TIMEOUT_MS;
	if (attr->transport == RTSP_TRANS_UDP || attr->transport == RTSP_TRANS_TCP) {
		client->transport = attr->transport;
	} else {
		client->transport = RTSP_TRANS_TCP;
	}
	client->max_frame_size = attr->max_frame_size > 0 ? (uint32_t)attr->max_frame_size : RTSP_CLIENT_DEFAULT_MAX_FRAME;
	strncpy(client->stream_url, attr->url, sizeof(client->stream_url) - 1);
	strncpy(client->username, attr->username, sizeof(client->username) - 1);
	strncpy(client->password, attr->password, sizeof(client->password) - 1);

	char path[RTSP_CLIENT_MAX_URL] = {0};
	if (rtsp_client_parse_url(attr->url, client->server_ip, &client->server_port, path, sizeof(path)) != 0) {
		free(client);
		return -1;
	}

	client->rtsp_fd = network_tcp_connect(client->server_ip, client->server_port, client->timeout_ms);
	if (client->rtsp_fd < 0) {
		free(client);
		return -1;
	}

	client->cseq = 1;
	char msg[RTSP_CLIENT_BUF_SIZE] = {0};
	char *resp = NULL;
	int32_t resp_len = 0;

	snprintf(msg, sizeof(msg), "OPTIONS %s RTSP/1.0\r\nCSeq: %d\r\n\r\n", client->stream_url, client->cseq++);
	if (rtsp_client_do_request(client, msg, &resp, &resp_len) != 0) {
		goto err;
	}
	free(resp);

	snprintf(msg, sizeof(msg),
			"DESCRIBE %s RTSP/1.0\r\n"
			"CSeq: %d\r\n"
			"Accept: application/sdp\r\n\r\n",
			client->stream_url, client->cseq++);
	if (rtsp_client_do_request(client, msg, &resp, &resp_len) != 0) {
		goto err;
	}
	if (rtsp_client_parse_status(resp) != 200) {
		free(resp);
		goto err;
	}

	rtsp_client_parse_content_base(resp, client->base_url, sizeof(client->base_url));
	if (client->base_url[0] == '\0') {
		strncpy(client->base_url, client->stream_url, sizeof(client->base_url) - 1);
		if (client->base_url[strlen(client->base_url) - 1] != '/') {
			strncat(client->base_url, "/", sizeof(client->base_url) - strlen(client->base_url) - 1);
		}
	}

	char *body = strstr(resp, "\r\n\r\n");
	if (body == NULL) {
		free(resp);
		goto err;
	}
	body += 4;
	rtsp_client_parse_sdp(client, body);
	free(resp);

	for (int32_t i = 0; i < FRAME_TYPE_BUTT; i++) {
		if (rtsp_client_init_track(client, &client->track[i]) != 0) {
			goto err;
		}
	}

	if (rtsp_client_setup_track(client, &client->track[FRAME_TYPE_VIDEO]) != 0) {
		goto err;
	}
	if (rtsp_client_setup_track(client, &client->track[FRAME_TYPE_AUDIO]) != 0) {
		goto err;
	}

	snprintf(msg, sizeof(msg),
			"PLAY %s RTSP/1.0\r\n"
			"CSeq: %d\r\n"
			"Session: %s\r\n\r\n",
			client->stream_url, client->cseq++, client->session_id);
	if (rtsp_client_do_request(client, msg, &resp, &resp_len) != 0) {
		goto err;
	}
	if (rtsp_client_parse_status(resp) != 200) {
		free(resp);
		goto err;
	}
	free(resp);

	cvi_osal_task_attr_t ta;
	ta.name = "rtsp_recv";
	ta.entry = rtsp_client_recv_task;
	ta.param = (void *)client;
	ta.priority = CVI_OSAL_PRI_NORMAL;
	ta.detached = false;
	ta.stack_size = 256 * 1024;
	cvi_osal_task_create(&ta, &client->recv_task);

	*hdl = client;
	return 0;

err:
	if (resp) {
		free(resp);
	}
	for (int32_t i = 0; i < FRAME_TYPE_BUTT; i++) {
		rtsp_client_deinit_track(&client->track[i]);
	}
	if (client->rtsp_fd >= 0) {
		network_tcp_close(client->rtsp_fd);
	}
	free(client);
	return -1;
}

/*
 * 合规关闭客户端：停止接收线程，发送 TEARDOWN（VLC 也是这样做），再释放资源。
 */
int32_t RTSP_ClientDestroy(void *hdl)
{
	RTSP_CLIENT_S *client = (RTSP_CLIENT_S *)hdl;
	if (client == NULL) {
		return -1;
	}

	client->recv_exit = 1;

	if (client->recv_task) {
		cvi_osal_task_join(client->recv_task);
		cvi_osal_task_destroy(&client->recv_task);
	}

	rtsp_client_send_teardown(client);

	for (int32_t i = 0; i < FRAME_TYPE_BUTT; i++) {
		rtsp_client_deinit_track(&client->track[i]);
	}

	if (client->rtsp_fd >= 0) {
		network_tcp_close(client->rtsp_fd);
		client->rtsp_fd = -1;
	}

	free(client);

	return 0;
}

/*
 * 从 rbuf 拉取一帧数据（会分配内存）。
 */
int32_t RTSP_RecvFrame(void *hdl, RTSP_FRAME_S *frame, int32_t timeout_ms)
{
	RTSP_CLIENT_S *client = (RTSP_CLIENT_S *)hdl;
	if (client == NULL || frame == NULL) {
		return -1;
	}
	int32_t wait_ms = timeout_ms >= 0 ? timeout_ms : client->timeout_ms;
	uint64_t start = 0;
	cvi_osal_get_boot_time_us(&start);

	while (1) {
		FRAME_TYPE_E types[2] = {FRAME_TYPE_VIDEO, FRAME_TYPE_AUDIO};
		int32_t type_cnt = 2;
		if (frame->type == FRAME_TYPE_VIDEO || frame->type == FRAME_TYPE_AUDIO) {
			types[0] = frame->type;
			type_cnt = 1;
		}

		for (int32_t i = 0; i < type_cnt; i++) {
			FRAME_TYPE_E t = types[i];
			RTSP_CLIENT_TRACK_S *track = &client->track[t];
			if (!track->enable || track->rbuf == NULL) {
				continue;
			}
			if (CVI_RBUF_DataCnt(track->rbuf, 0) == 0) {
				continue;
			}

			RTSP_FRAME_INFO_S finfo;
			if (CVI_RBUF_Copy_Out(track->rbuf, &finfo, sizeof(RTSP_FRAME_INFO_S), 0, 0) != 0) {
				continue;
			}
			if (finfo.hmagic != 0x5a5a5a5a || finfo.tmagic != 0x5a5a5a5a ||
				finfo.totalsize < (int32_t)sizeof(RTSP_FRAME_INFO_S) ||
				finfo.totalsize > (int32_t)(client->max_frame_size + sizeof(RTSP_FRAME_INFO_S))) {
				CVI_RBUF_Reset(track->rbuf);
				continue;
			}
			if (finfo.len <= 0 || (uint32_t)finfo.len > client->max_frame_size) {
				CVI_RBUF_Refresh_Out(track->rbuf, finfo.totalsize, 0);
				continue;
			}
			unsigned char *data = (unsigned char *)malloc(finfo.len);
			if (data == NULL) {
				return -1;
			}
			if (CVI_RBUF_Copy_Out(track->rbuf, data, finfo.len, sizeof(RTSP_FRAME_INFO_S), 0) != 0) {
				free(data);
				return -1;
			}
			CVI_RBUF_Refresh_Out(track->rbuf, finfo.totalsize, 0);
			memset(frame, 0, sizeof(*frame));
			frame->type = t;
			frame->data[0] = data;
			frame->len[0] = finfo.len;
			frame->iskey[0] = finfo.key;
			frame->vi_pts[0] = finfo.pts;
			return 0;
		}

		uint64_t now = 0;
		cvi_osal_get_boot_time_us(&now);
		if (wait_ms >= 0 && (now - start) >= (uint64_t)wait_ms * 1000) {
			return -1;
		}
		cvi_osal_task_sleep(10 * 1000);
	}
	return -1;
}

/*
 * 释放 RTSP_RecvFrame 返回的帧内存。
 */
void RTSP_ReleaseFrame(void *hdl, RTSP_FRAME_S *frame)
{
	(void)hdl;
	if (frame == NULL) {
		return;
	}
	for (int32_t i = 0; i < MAX_RTSP_VIDEO_FRAME; i++) {
		if (frame->data[i]) {
			free(frame->data[i]);
			frame->data[i] = NULL;
		}
		frame->len[i] = 0;
	}
}
