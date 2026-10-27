

#include <errno.h>
#include <poll.h>

#include "rtsp_comp.h"

static RTSP_SERVER_S *gstRtspServer = NULL;
static cvi_osal_mutex_t rtsp_server_mutex = CVI_OSAL_MUTEX_INITIALIZER;
static RTSP_INSTANCE_S *gstRtspInstance[RTSP_INSTANCE_NUM];
static cvi_osal_mutex_t rtsp_instance_mutex = CVI_OSAL_MUTEX_INITIALIZER;

static void *rtsp_malloc(uint32_t size, const char *name)
{
	(void)name;
	if (size > 0) {
		return malloc(RTSP_ALIGN(size, 4096));
	}
	return NULL;
}

static void rtsp_free(void *p)
{
	if (p) {
		free(p);
	}
}

static const char *rtsp_msg_get_errstring(int32_t err)
{
	struct
	{
		const char *token;
		int32_t code;
	} status[] =
		{
			{"Continue", 100},
			{"OK", 200},
			{"Created", 201},
			{"Accepted", 202},
			{"Non-Authoritative Information", 203},
			{"No Content", 204},
			{"Reset Content", 205},
			{"Partial Content", 206},
			{"Multiple Choices", 300},
			{"Moved Permanently", 301},
			{"Moved Temporarily", 302},
			{"Bad Request", 400},
			{"Unauthorized", 401},
			{"Payment Required", 402},
			{"Forbidden", 403},
			{"Not Found", 404},
			{"Method Not Allowed", 405},
			{"Not Acceptable", 406},
			{"Proxy Authentication Required", 407},
			{"Request Time-out", 408},
			{"Conflict", 409},
			{"Gone", 410},
			{"Length Required", 411},
			{"Precondition Failed", 412},
			{"Request Entity Too Large", 413},
			{"Request-URI Too Large", 414},
			{"Unsupported Media Type", 415},
			{"Bad Extension", 420},
			{"Invalid Parameter", 450},
			{"Parameter Not Understood", 451},
			{"Conference Not Found", 452},
			{"Not Enough Bandwidth", 453},
			{"Session Not Found", 454},
			{"Method Not Valid In This State", 455},
			{"Header Field Not Valid for Resource", 456},
			{"Invalid Range", 457},
			{"Parameter Is Read-Only", 458},
			{"Unsupported transport", 461},
			{"Internal Server Error", 500},
			{"Not Implemented", 501},
			{"Bad Gateway", 502},
			{"Service Unavailable", 503},
			{"Gateway Time-out", 504},
			{"RTSP Version Not Supported", 505},
			{"Option not supported", 551},
			{"Extended Error:", 911},
			{NULL, -1}};

	int32_t i = 0;
	for (i = 0; status[i].code != err && status[i].code != -1; ++i)
		;

	return status[i].token;
}

static int32_t rtsp_msg_bwrite(RTSP_SESSION_S *session, char *buffer, int32_t len)
{
	if ((session->out_size + len) > (int32_t)sizeof(session->out_buffer)) {
		CVI_LOGE("rtsp_msg not enough free space in out message buffer");
		memset(session->in_buffer, 0x0, session->in_size);
		session->in_size = 0;
		return -1;
	}

	memcpy(&(session->out_buffer[session->out_size]), buffer, len);
	session->out_buffer[session->out_size + len] = '\0';
	session->out_size += len;
	return 0;
}

static int32_t rtsp_msg_send_reply(RTSP_SESSION_S *session, int32_t err)
{
	memset(session->msg_buffer, 0, RTSP_BUFFERSIZE);
	snprintf(session->msg_buffer, RTSP_BUFFERSIZE, "%s %d %s" RTSP_EL "CSeq: %d" RTSP_EL RTSP_EL,
			 RTSP_VER, err, rtsp_msg_get_errstring(err), session->client_info.cseq);
	return rtsp_msg_bwrite(session, session->msg_buffer, strlen(session->msg_buffer));
}

static int32_t rtsp_msg_discard_rtcp(int32_t fd, char *buf, int32_t size)
{
	if (size > 0) {
		uint16_t *intlvd_len = (uint16_t *)&buf[2];
		int32_t len = ntohs(*intlvd_len);
		if (buf[0] == '$' && size == 4) {
			char tmp[128] = {0};
			while (len > 0) {
				int32_t n = network_tcp_read(fd, tmp, len);
				if (n > 0) {
					len -= n;
				}
			}
			return size;
		}

		if (buf[0] == '$' && len + 4 == size) {
			return size;
		}
	}
	return 0;
}

static int32_t rtsp_msg_validate_method(RTSP_SESSION_S *session)
{
	char method[32] = {0};
	char url[256] = {0};
	char ver[32] = {0};
	char hdr[32] = {0};
	uint32_t seq = 0;
	int32_t mid = ERR_GENERIC;
	char *p = NULL;
	char trash[255] = {0};

	if (session->in_buffer[0] == '$') {
		session->client_info.cseq = seq;
		mid = RTSP_ID_RTCP;
		return mid;
	}

	if (sscanf(session->in_buffer, " %31s %255s %31s\n%15s\n%u", method, url, ver, hdr, &seq) != 5 && sscanf(session->in_buffer, " %31s %255s %31s\r\n", method, url, ver) != 3) {
		CVI_LOGD("%s ", method);
		CVI_LOGD("%s ", url);
		CVI_LOGD("%s ", ver);
		return ERR_GENERIC;
	}

	if ((p = strstr(session->in_buffer, "CSeq")) == NULL) {
		return ERR_GENERIC;
	} else {
		if (sscanf(p, "%254s %d", trash, &seq) != 2) {
			return ERR_GENERIC;
		}
	}

	if (strcmp(method, RTSP_METHOD_DESCRIBE) == 0) {
		mid = RTSP_ID_DESCRIBE;
	} else if (strcmp(method, RTSP_METHOD_ANNOUNCE) == 0) {
		mid = RTSP_ID_ANNOUNCE;
	} else if (strcmp(method, RTSP_METHOD_GET_PARAMETERS) == 0) {
		mid = RTSP_ID_GET_PARAMETERS;
	} else if (strcmp(method, RTSP_METHOD_OPTIONS) == 0) {
		mid = RTSP_ID_OPTIONS;
	} else if (strcmp(method, RTSP_METHOD_PAUSE) == 0) {
		mid = RTSP_ID_PAUSE;
	} else if (strcmp(method, RTSP_METHOD_PLAY) == 0) {
		mid = RTSP_ID_PLAY;
	} else if (strcmp(method, RTSP_METHOD_RECORD) == 0) {
		mid = RTSP_ID_RECORD;
	} else if (strcmp(method, RTSP_METHOD_REDIRECT) == 0) {
		mid = RTSP_ID_REDIRECT;
	} else if (strcmp(method, RTSP_METHOD_SETUP) == 0) {
		mid = RTSP_ID_SETUP;
	} else if (strcmp(method, RTSP_METHOD_SET_PARAMETER) == 0) {
		mid = RTSP_ID_SET_PARAMETER;
	} else if (strcmp(method, RTSP_METHOD_TEARDOWN) == 0) {
		mid = RTSP_ID_TEARDOWN;
	}

	session->client_info.cseq = seq;
	return mid;
}

static int32_t rtsp_msg_url_match(RTSP_INSTANCE_S **instance, char *url)
{
	cvi_osal_mutex_lock(&rtsp_instance_mutex);
	for (int32_t i = 0; i < RTSP_INSTANCE_NUM; i++) {
		if (instance[i] && strcmp(instance[i]->info.rtsp_name, url) == 0) {
			cvi_osal_mutex_unlock(&rtsp_instance_mutex);
			return i;
		}
	}
	cvi_osal_mutex_unlock(&rtsp_instance_mutex);
	return -1;
}

static int32_t rtsp_msg_parse_url(const char *pUrl, char *pServer, unsigned short *port, char *pFileName, size_t FileNameLen)
{
	/* expects format [rtsp://server[:port/]]filename RTSP/1.0*/
	int32_t s32NoValUrl = 0;
	char *pFull = (char *)malloc(strlen(pUrl) + 1);
	strcpy(pFull, pUrl);

	if (strncmp(pFull, "rtsp://", 7) == 0) {
		char *pSuffix;
		if ((pSuffix = strchr(&pFull[7], '/')) != NULL) {
			char *pPort;
			char pSubPort[128];
			pPort = strchr(&pFull[7], ':');
			if (pPort != NULL) {
				strncpy(pServer, &pFull[7], pPort - pFull - 7);
				strncpy(pSubPort, pPort + 1, pSuffix - pPort - 1);
				pSubPort[pSuffix - pPort - 1] = '\0';
				if (strlen(pSubPort) > 0) {
					*port = (unsigned short)atol(pSubPort);
				} else {
					*port = 5554;
				}
			} else {
				*port = 5554;
			}
			pSuffix++;
			while (*pSuffix == ' ' || *pSuffix == '\t') {
				pSuffix++;
			}
			snprintf(pFileName, FileNameLen, "%s", pSuffix);
			CVI_LOGD("server:%s port:%d url:%s", pServer, *port, pFileName);
			s32NoValUrl = 0;
		} else {
			*port = 5554;
			*pFileName = '\0';
			s32NoValUrl = 1;
		}
	} else {
		*pFileName = '\0';
		s32NoValUrl = 1;
	}
	free(pFull);
	return s32NoValUrl;
}

static char *rtsp_msg_get_sdp_id(char *buffer)
{
	time_t t;
	buffer[0] = '\0';
	t = time(NULL);
	sprintf(buffer, "%.0f", (float)t + 2208988800U);
	return buffer;
}

static void rtsp_msg_get_sdp_descr(RTSP_SESSION_S *session, RTSP_MEDIA_INFO_S *media_info, char *msg)
{
	char sdp_time[128] = {0};
	int32_t len = 0;

	rtsp_msg_get_sdp_id(sdp_time);
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "v=0\r\n");
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "o=- %s 0 IN IP4 %s\r\n", sdp_time, session->localip);
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "s=session\r\n");
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "i=N/A\r\n");
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "c=IN IP4 %s\r\n", session->client_info.ip);
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "t=0 0\r\n");
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=recvonly\r\n");

	if (media_info->video_codec == RTSP_VIDEO_H264) {
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "m=video 0 RTP/AVP %d\r\n", H264_PAYLOAD_TYPE);
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=rtpmap:%d H264/90000\r\n", H264_PAYLOAD_TYPE);
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=fmtp:%d packetization-mode=1;\r\n", H264_PAYLOAD_TYPE);
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=control:%s%d\r\n", RTSP_STREAM_URL, FRAME_TYPE_VIDEO);
	} else if (media_info->video_codec == RTSP_VIDEO_H265) {
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "m=video 0 RTP/AVP %d\r\n", H265_PAYLOAD_TYPE);
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=rtpmap:%d H265/90000\r\n", H265_PAYLOAD_TYPE);
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=fmtp:%d packetization-mode=1;\r\n", H265_PAYLOAD_TYPE);
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=control:%s%d\r\n", RTSP_STREAM_URL, FRAME_TYPE_VIDEO);
	} else if (media_info->video_codec == RTSP_VIDEO_MJPEG) {
		/* MJPEG SDP per RFC 2435: static payload 26, standard JPEG/90000 rtpmap, no H264 fmtp */
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "m=video 0 RTP/AVP %d\r\n", MJPEG_PAYLOAD_TYPE);
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=rtpmap:%d JPEG/90000\r\n", MJPEG_PAYLOAD_TYPE);
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=control:%s%d\r\n", RTSP_STREAM_URL, FRAME_TYPE_VIDEO);
	} else {
		CVI_LOGE("unsupport video codec:%d", media_info->video_codec);
	}

	if (media_info->audio_codec == RTSP_AUDIO_PCM) {
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "m=audio 0 RTP/AVP %d\r\n", PCM_PAYLOAD_TYPE);
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=rtpmap:%d L16/%d\r\n", PCM_PAYLOAD_TYPE, media_info->audio_sample_rate);
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=fmtp:%d\r\n", PCM_PAYLOAD_TYPE);
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=control:%s%d\r\n", RTSP_STREAM_URL, FRAME_TYPE_AUDIO);
	} else if (media_info->audio_codec == RTSP_AUDIO_AAC) {
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "m=audio 0 RTP/AVP %d\r\n", AAC_PAYLOAD_TYPE);
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=rtpmap:%d mpeg4-generic/%d/2\r\n", AAC_PAYLOAD_TYPE, media_info->audio_sample_rate);
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=fmtp:%d profile-level-id=1; mode=AAC-hbr; config=1408; SizeLength=13; IndexLength=3; IndexDeltaLength=3;\r\n", AAC_PAYLOAD_TYPE);
		len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=control:%s%d\r\n", RTSP_STREAM_URL, FRAME_TYPE_AUDIO);
	} else {
		CVI_LOGE("unsupport audio codec:%d", media_info->audio_codec);
	}
}

static void rtsp_msg_add_time_stamp(char *b, int32_t crlf)
{
	struct tm *t;
	time_t now;

	/*
	 * concatenates a null terminated string with a
	 * time stamp in the format of "Date: 23 Jan 1997 15:35:06 GMT"
	 */
	now = time(NULL);
	t = gmtime(&now);
	// 输出时间格式：Date: Fri, 15 Jul 2011 09:23:26 GMT
	strftime(b + strlen(b), 38, "Date: %a, %d %b %Y %H:%M:%S GMT" RTSP_EL, t);
	// 是否是消息结束，添加回车换行符
	if (crlf)
		strcat(b, "\r\n"); /* add a message header terminator (CRLF) */
}

static int32_t rtsp_msg_send_desc_replay(RTSP_SESSION_S *session, char *descr)
{
	memset(session->msg_buffer, 0, RTSP_BUFFERSIZE);
	char *pMsgBuf = session->msg_buffer;
	uint64_t u64DescrLen = strlen(descr);

	sprintf(pMsgBuf, "%s %d %s" RTSP_EL "CSeq: %d" RTSP_EL "Server: %s/%s" RTSP_EL, RTSP_VER, 200, rtsp_msg_get_errstring(200), session->client_info.cseq, PACKAGE, VERSION);
	rtsp_msg_add_time_stamp(pMsgBuf, 0);
	strcat(pMsgBuf, "Content-Type: application/sdp" RTSP_EL);
	sprintf(pMsgBuf + strlen(pMsgBuf), "Content-Base: rtsp://%s/%s/" RTSP_EL, session->localip, session->url);
	sprintf(pMsgBuf + strlen(pMsgBuf), "Content-Length: %" PRIu64 "" RTSP_EL, u64DescrLen);
	strcat(pMsgBuf, RTSP_EL);
	strcat(pMsgBuf, descr);
	rtsp_msg_bwrite(session, pMsgBuf, strlen(pMsgBuf));
	return ERR_NOERROR;
}

static int32_t rtsp_msg_describe(RTSP_INSTANCE_S **instance, RTSP_SESSION_S *session)
{
	char object[255] = {0}, trash[255] = {0};
	char *p = NULL;
	unsigned short port = 0;
	char s8Url[255] = {0};
	char s8Descr[RTSP_BUFFERSIZE] = {0};
	char server[128] = {0};
	int32_t inst_index = 0;
	CVI_LOGD("client ip %s port %u fd %d cseq %u", session->client_info.ip, session->client_info.port, session->client_info.fd, session->client_info.cseq);

	if (!sscanf(session->in_buffer, " %*s %254s ", s8Url)) {
		CVI_LOGE("sscanf Error:%s", session->in_buffer);
		rtsp_msg_send_reply(session, 400); /* bad request */
		return ERR_GENERIC;
	}

	switch (rtsp_msg_parse_url(s8Url, server, &port, object, sizeof(object))) {
	case 1: /*请求错误*/
		CVI_LOGE("rtsp_parse_url Error %s", session->in_buffer);
		rtsp_msg_send_reply(session, 400);
		return ERR_GENERIC;
		break;

	case -1: /*内部错误*/
		CVI_LOGE("rtsp_parse_url Error %s", session->in_buffer);
		rtsp_msg_send_reply(session, 500);
		return ERR_FATAL;
		break;

	default:
		break;
	}

	inst_index = rtsp_msg_url_match(instance, object);
	if (inst_index == -1) {
		CVI_LOGE("rtsp_url_match Error \n%s", session->in_buffer);
		rtsp_msg_send_reply(session, 404);
		return ERR_NOT_FOUND;
	}
	CVI_LOGD("match instance inst_index %d", inst_index);

	/*取得序列号,并且必须有这个选项*/
	if ((p = strstr(session->in_buffer, HDR_CSEQ)) == NULL) {
		CVI_LOGE("rtsp_parse_url Error %s", session->in_buffer);
		rtsp_msg_send_reply(session, 400); /* Bad Request */
		return ERR_GENERIC;
	} else {
		if (sscanf(p, "%254s %d", trash, &(session->client_info.cseq)) != 2) {
			CVI_LOGE("rtsp_parse_url Error %s", p);
			rtsp_msg_send_reply(session, 400);
			return ERR_GENERIC;
		}
	}

	cvi_osal_get_boot_time_us(&session->client_info.stime);
	strncpy(session->url, object, sizeof(session->url));
	strncpy(session->localip, server, sizeof(session->localip));
	CVI_LOGD("match instance inst_index %d %s %s", inst_index, session->url, session->localip);
	RTSP_MEDIA_INFO_S *media_info = &instance[inst_index]->info;
	rtsp_msg_get_sdp_descr(session, media_info, s8Descr);
	rtsp_msg_send_desc_replay(session, s8Descr);
	return ERR_NOERROR;
}

static int32_t rtsp_msg_send_options_reply(RTSP_SESSION_S *session)
{
	memset(session->msg_buffer, 0, RTSP_BUFFERSIZE);
	char *pMsgBuf = session->msg_buffer;
	sprintf(pMsgBuf, "%s %d %s" RTSP_EL "CSeq: %d" RTSP_EL, RTSP_VER, 200, rtsp_msg_get_errstring(200), session->client_info.cseq);
	strcat(pMsgBuf, "Public: OPTIONS,DESCRIBE,GET_PARAMETER,SETUP,PLAY,TEARDOWN" RTSP_EL);
	strcat(pMsgBuf, RTSP_EL);
	rtsp_msg_bwrite(session, pMsgBuf, strlen(pMsgBuf));
	return ERR_NOERROR;
}

static int32_t rtsp_msg_options(RTSP_SESSION_S *session)
{
	char *p;
	char trash[255];
	CVI_LOGD("client ip %s port %u fd %d cseq %u", session->client_info.ip, session->client_info.port, session->client_info.fd, session->client_info.cseq);
	if ((p = strstr(session->in_buffer, HDR_CSEQ)) == NULL) {
		CVI_LOGE("error %s", session->in_buffer);
		rtsp_msg_send_reply(session, 400); /* Bad Request */
		return ERR_GENERIC;
	} else {
		if (sscanf(p, "%254s %d", trash, &(session->client_info.cseq)) != 2) {
			CVI_LOGE("error %s", session->in_buffer);
			rtsp_msg_send_reply(session, 400); /* Bad Request */
			return ERR_GENERIC;
		}
	}

	cvi_osal_get_boot_time_us(&session->client_info.stime);
	rtsp_msg_send_options_reply(session);
	return ERR_NOERROR;
}

static int32_t rtsp_msg_send_setup_reply(RTSP_SESSION_S *session, RTP_TRANSPORT_INFO_S t)
{
	memset(session->msg_buffer, 0, RTSP_BUFFERSIZE);
	char *pMsgBuf = session->msg_buffer;
	sprintf(pMsgBuf, "%s %d %s" RTSP_EL "CSeq: %u" RTSP_EL "Server: %s/%s" RTSP_EL, RTSP_VER,
			200, rtsp_msg_get_errstring(200), session->client_info.cseq, PACKAGE, VERSION);
	rtsp_msg_add_time_stamp(pMsgBuf, 0);
	sprintf(pMsgBuf + strlen(pMsgBuf), "Session: %d;timeout=%d" RTSP_EL "Transport: ", session->client_info.session_id, session->client_info.timeout);

	switch (t.type) {
	case RTP_OVER_UDP:
		if (t.u.udp.is_multicast) {
			sprintf(pMsgBuf + strlen(pMsgBuf), "RTP/AVP;multicast;ttl=%d;destination=%s;port=", t.u.udp.ttl, t.u.udp.cli_ip);
		} else {
			sprintf(pMsgBuf + strlen(pMsgBuf), "RTP/AVP;unicast;client_port=%d-%d;server_port=",
					t.u.udp.cli_ports.rtp, t.u.udp.cli_ports.rtcp);
		}
		sprintf(pMsgBuf + strlen(pMsgBuf), "%d-%d" RTSP_EL, t.u.udp.ser_ports.rtp, t.u.udp.ser_ports.rtcp);
		break;

	case RTP_OVER_TCP:
		sprintf(pMsgBuf + strlen(pMsgBuf), "RTP/AVP/TCP;interleaved=%d-%d" RTSP_EL,
				t.u.tcp.interleaved.rtp, t.u.tcp.interleaved.rtcp);
		break;

	default:
		break;
	}

	strcat(pMsgBuf, RTSP_EL);
	rtsp_msg_bwrite(session, pMsgBuf, (unsigned short)strlen(pMsgBuf));
	return ERR_NOERROR;
}

static int32_t rtsp_msg_setup(RTSP_INSTANCE_S **instance, RTSP_SESSION_S *session)
{
	char s8TranStr[128], *s8Str;
	char *pStr;
	RTP_TRANSPORT_INFO_S trans_info;
	int32_t session_id = -1;
	int32_t trackID = -1;
	int32_t inst_index = 0;
	CVI_LOGD("client %s ip %s port %u fd %d cseq %u", session->url, session->client_info.ip, session->client_info.port, session->client_info.fd, session->client_info.cseq);

	if ((s8Str = strstr(session->in_buffer, HDR_TRANSPORT)) == NULL) {
		CVI_LOGE("error %s", session->in_buffer);
		rtsp_msg_send_reply(session, 406);
		return ERR_GENERIC;
	}

	if (sscanf(s8Str, "%*10s %255s", s8TranStr) != 1) {
		CVI_LOGE("SETUP request malformed: Transport string is empty");
		rtsp_msg_send_reply(session, 400);
		return ERR_GENERIC;
	}

	CVI_LOGD("*** transport: %s ***", s8TranStr);

	if ((s8Str = strstr(session->in_buffer, RTSP_STREAM_URL)) != NULL) {
		s8Str += strlen(RTSP_STREAM_URL);
		if (sscanf(s8Str, "%d", &trackID) != 1 || trackID >= FRAME_TYPE_BUTT) {
			CVI_LOGE("SETUP request malformed: trackID string is empty");
			rtsp_msg_send_reply(session, 400);
			return ERR_GENERIC;
		}
	}

	if (strlen(session->url) <= 0) {
		CVI_LOGE("describe request first");
		rtsp_msg_send_reply(session, 400);
		return ERR_GENERIC;
	}

	inst_index = rtsp_msg_url_match(instance, session->url);
	if (inst_index == -1) {
		CVI_LOGE("rtsp_url_match Error \n%s", session->url);
		rtsp_msg_send_reply(session, 404);
		return ERR_NOT_FOUND;
	}

	trans_info.type = RTP_OVER_BUTT;

	if ((pStr = strstr(s8TranStr, RTSP_RTP_AVP))) {
		// Transport: RTP/AVP
		pStr += strlen(RTSP_RTP_AVP);
		if (!*pStr || (*pStr == ';') || (*pStr == ' ') || (!strncmp(pStr, "/UDP", 4))) {
			trans_info.type = RTP_OVER_UDP;
			if (strstr(s8TranStr, "unicast")) {
				if ((pStr = strstr(s8TranStr, "client_port"))) {
					pStr = strstr(s8TranStr, "=");
					sscanf(pStr + 1, "%d", &(trans_info.u.udp.cli_ports.rtp));
					pStr = strstr(s8TranStr, "-");
					sscanf(pStr + 1, "%d", &(trans_info.u.udp.cli_ports.rtcp));
				} else {
					trans_info.u.udp.cli_ports.rtp = instance[inst_index]->info.id * 10 + instance[inst_index]->info.rtp_port;
					trans_info.u.udp.cli_ports.rtcp = trans_info.u.udp.cli_ports.rtp + 1;
				}

				trans_info.u.udp.ser_ports.rtp = instance[inst_index]->info.id * 2 + instance[inst_index]->info.rtp_port;
				trans_info.u.udp.ser_ports.rtcp = trans_info.u.udp.ser_ports.rtp + 1;
				trans_info.u.udp.is_multicast = 0;
				strcpy(trans_info.u.udp.cli_ip, session->client_info.ip);
			} else if (strstr(s8TranStr, "multicast")) {
				trans_info.u.udp.is_multicast = 1;
				strcpy(trans_info.u.udp.cli_ip, "224.2.0.1");
				trans_info.u.udp.ttl = 16;
			}
		} else if (!strncmp(pStr, "/TCP", 4)) {
			if ((pStr = strstr(s8TranStr, "interleaved"))) {
				pStr = strstr(s8TranStr, "=");
				sscanf(pStr + 1, "%d", &(trans_info.u.tcp.interleaved.rtp));
				if ((pStr = strstr(pStr, "-"))) {
					sscanf(pStr + 1, "%d", &(trans_info.u.tcp.interleaved.rtcp));
				} else {
					trans_info.u.tcp.interleaved.rtcp = trans_info.u.tcp.interleaved.rtp + 1;
				}
			}
			trans_info.u.tcp.fd = session->client_info.fd;
			trans_info.type = RTP_OVER_TCP;
			session->client_info.overtcp = 1;
		}
	}

	if (trans_info.type == RTP_OVER_BUTT) {
		CVI_LOGE("AAAAAAAAAAA Unsupported Transport");
		rtsp_msg_send_reply(session, 461);
		return ERR_GENERIC;
	}

	if ((pStr = strstr(session->in_buffer, HDR_SESSION)) != NULL) {
		if (sscanf(pStr, "%*s %d", &session_id) != 1) {
			CVI_LOGE("error %s", session->in_buffer);
			rtsp_msg_send_reply(session, 454); // Session Not Found
			return ERR_GENERIC;
		}
	} else {
		// 产生一个非0的随机的会话序号
		struct timeval stNowTmp;
		gettimeofday(&stNowTmp, 0);
		srand((stNowTmp.tv_sec * 1000) + (stNowTmp.tv_usec / 1000));
		session_id = 1 + (int32_t)(10.0 * rand() / (100000 + 1.0));
		if (session_id == 0) {
			session_id++;
		}
	}

	session->client_info.session_id = session_id;
	if (session->rtp_session[trackID] == NULL) {
		CVI_LOGD("create rtp session %s trackID %d", session->url, trackID);
		rtp_session_create(&session->rtp_session[trackID], &trans_info);
	}

	rtsp_msg_send_setup_reply(session, trans_info);
	return ERR_NOERROR;
}

static int32_t rtsp_msg_send_play_reply(RTSP_SESSION_S *session)
{
	memset(session->msg_buffer, 0, RTSP_BUFFERSIZE);
	char *pMsgBuf = session->msg_buffer;
	sprintf(pMsgBuf, "%s %d %s" RTSP_EL "CSeq: %d" RTSP_EL "Server: %s/%s" RTSP_EL, RTSP_VER, 200,
			rtsp_msg_get_errstring(200), session->client_info.cseq, PACKAGE, VERSION);
	rtsp_msg_add_time_stamp(pMsgBuf, 0);
	sprintf(pMsgBuf + strlen(pMsgBuf), "Session: %d" RTSP_EL RTSP_EL, session->client_info.session_id);
	rtsp_msg_bwrite(session, pMsgBuf, strlen(pMsgBuf));
	return ERR_NOERROR;
}

static int32_t rtsp_msg_play(RTSP_SESSION_S *session)
{
	char *pStr;
	char pTrash[128];
	long int session_id = 0;
	CVI_LOGD("client ip %s port %u fd %d cseq %u", session->client_info.ip, session->client_info.port, session->client_info.fd, session->client_info.cseq);

	if ((pStr = strstr(session->in_buffer, HDR_CSEQ)) == NULL) {
		rtsp_msg_send_reply(session, 400); /* Bad Request */
		CVI_LOGD("get CSeq!!400");
		return ERR_GENERIC;
	} else {
		if (sscanf(pStr, "%254s %d", pTrash, &(session->client_info.cseq)) != 2) {
			rtsp_msg_send_reply(session, 400); /* Bad Request */
			CVI_LOGD("get CSeq!! 2 400");
			return ERR_GENERIC;
		}
	}

	if ((pStr = strstr(session->in_buffer, HDR_SESSION)) != NULL) {
		if (sscanf(pStr, "%254s %ld", pTrash, &session_id) != 2) {
			rtsp_msg_send_reply(session, 454); // Session Not Found
			CVI_LOGD("Session Not Found");
			return ERR_GENERIC;
		}
	} else {
		rtsp_msg_send_reply(session, 400); // bad request
		CVI_LOGD("Session Not Found bad request");
		return ERR_GENERIC;
	}

	if (strlen(session->url) <= 0) {
		CVI_LOGE("describe request first");
		rtsp_msg_send_reply(session, 400); // Bad Request
		return ERR_GENERIC;
	}

	if (session->client_info.session_id != session_id) {
		rtsp_msg_send_reply(session, 454); // Session not found
		return ERR_GENERIC;
	}
	rtsp_msg_send_play_reply(session);
	cvi_osal_get_boot_time_us(&session->client_info.stime);
	return ERR_NOERROR;
}

static int32_t rtsp_msg_send_teardown_reply(RTSP_SESSION_S *session)
{
	memset(session->msg_buffer, 0, RTSP_BUFFERSIZE);
	char *pMsgBuf = session->msg_buffer;
	sprintf(pMsgBuf, "%s %d %s" RTSP_EL "CSeq: %u" RTSP_EL "Server: %s/%s" RTSP_EL, RTSP_VER,
			200, rtsp_msg_get_errstring(200), session->client_info.cseq, PACKAGE, VERSION);
	rtsp_msg_add_time_stamp(pMsgBuf, 0);
	sprintf(pMsgBuf + strlen(pMsgBuf), "Session: %d" RTSP_EL RTSP_EL, session->client_info.session_id);
	rtsp_msg_bwrite(session, pMsgBuf, strlen(pMsgBuf));
	return ERR_NOERROR;
}

static int32_t rtsp_msg_teardown(RTSP_SESSION_S *session)
{
	char *pStr = NULL;
	char pTrash[128] = {0};
	long int session_id = -1;
	CVI_LOGD("client ip %s port %u fd %d cseq %u", session->client_info.ip, session->client_info.port, session->client_info.fd, session->client_info.cseq);

	if ((pStr = strstr(session->in_buffer, HDR_CSEQ)) == NULL) {
		rtsp_msg_send_reply(session, 400); // Bad Request
		CVI_LOGD("get CSeq error");
		return ERR_GENERIC;
	} else {
		if (sscanf(pStr, "%254s %d", pTrash, &(session->client_info.cseq)) != 2) {
			rtsp_msg_send_reply(session, 400); // Bad Request
			CVI_LOGD("get CSeq 2 error");
			return ERR_GENERIC;
		}
	}

	if ((pStr = strstr(session->in_buffer, HDR_SESSION)) != NULL) {
		if (sscanf(pStr, "%254s %ld", pTrash, &session_id) != 2) {
			rtsp_msg_send_reply(session, 454); // Session Not Found
			return ERR_GENERIC;
		}
	}

	if (session->client_info.session_id != session_id) {
		rtsp_msg_send_reply(session, 454); // Session not found
		return ERR_GENERIC;
	}

	rtsp_msg_send_teardown_reply(session);
	return ERR_NOERROR;
}

static int32_t rtsp_msg_send_get_parameters_reply(RTSP_SESSION_S *session)
{
	memset(session->msg_buffer, 0, RTSP_BUFFERSIZE);
	char *pMsgBuf = session->msg_buffer;
	sprintf(pMsgBuf, "%s %d %s" RTSP_EL "CSeq: %d" RTSP_EL "Server: %s/%s" RTSP_EL, RTSP_VER,
			200, rtsp_msg_get_errstring(200), session->client_info.cseq, PACKAGE, VERSION);
	rtsp_msg_add_time_stamp(pMsgBuf, 0);
	strcat(pMsgBuf, RTSP_EL);
	rtsp_msg_bwrite(session, pMsgBuf, strlen(pMsgBuf));
	return ERR_NOERROR;
}

static int32_t rtsp_msg_get_parameters(RTSP_SESSION_S *session)
{
	char *pStr;
	char pTrash[128];
	int32_t session_id = -1;
	CVI_LOGD("client ip %s port %u fd %d cseq %u", session->client_info.ip, session->client_info.port, session->client_info.fd, session->client_info.cseq);

	if ((pStr = strstr(session->in_buffer, HDR_CSEQ)) == NULL) {
		rtsp_msg_send_reply(session, 400); // Bad Request
		CVI_LOGD("get CSeq error");
		return ERR_GENERIC;
	} else {
		if (sscanf(pStr, "%254s %d", pTrash, &(session->client_info.cseq)) != 2) {
			rtsp_msg_send_reply(session, 400); // Bad Request
			CVI_LOGD("get CSeq 2 error");
			return ERR_GENERIC;
		}
	}

	if ((pStr = strstr(session->in_buffer, HDR_SESSION)) != NULL) {
		if (sscanf(pStr, "%254s %d", pTrash, &session_id) != 2) {
			rtsp_msg_send_reply(session, 454); // Session Not Found
			return ERR_GENERIC;
		}
	}

	if (session->client_info.session_id != session_id) {
		rtsp_msg_send_reply(session, 454); // Session not found
		return ERR_GENERIC;
	}

	cvi_osal_get_boot_time_us(&session->client_info.stime);
	rtsp_msg_send_get_parameters_reply(session);
	return ERR_NOERROR;
}

static int32_t rtsp_msg_state_machine(RTSP_INSTANCE_S **instance, RTSP_SESSION_S *session, int32_t method)
{
	char *s;
	long int session_id;
	char trash[255];
	int32_t ret = ERR_NOERROR;

	if (method == RTSP_ID_RTCP) {
		return ret;
	}

	/*找到会话位置*/
	if ((s = strstr(session->in_buffer, HDR_SESSION)) != NULL) {
		if (sscanf(s, "%254s %ld", trash, &session_id) != 2) {
			CVI_LOGE("Invalid Session number %s,%i\n", __FILE__, __LINE__);
			rtsp_msg_send_reply(session, 454); /* 没有此会话*/
			return ERR_GENERIC;
		}
	}

	if (session->client_info.cur_state == DESTROY_STATE) {
		return ERR_GENERIC;
	}

	/*根据状态迁移规则，从当前状态开始迁移*/
	switch (session->client_info.cur_state) {
	case INIT_STATE: /*初始态*/
	{
		switch (method) {
		case RTSP_ID_DESCRIBE: // 状态不变
			ret = rtsp_msg_describe(instance, session);
			break;

		case RTSP_ID_SETUP: // 状态变为就绪态
			if ((ret = rtsp_msg_setup(instance, session)) == ERR_NOERROR) {
				session->client_info.cur_state = READY_STATE;
				CVI_LOGD("TRANSFER TO READY STATE!\n");
			}
			break;

		case RTSP_ID_TEARDOWN: // 状态不变
			ret = rtsp_msg_teardown(session);
			session->client_info.cur_state = TEARDOWN_STATE;
			break;

		case RTSP_ID_OPTIONS:
			if ((ret = rtsp_msg_options(session)) == ERR_NOERROR) {
				session->client_info.cur_state = INIT_STATE;
			}
			break;

		case RTSP_ID_GET_PARAMETERS:
			ret = rtsp_msg_get_parameters(session);
			break;

		case RTSP_ID_PLAY: // method not valid this state.
			break;
		case RTSP_ID_PAUSE:
			ret = rtsp_msg_send_reply(session, 455);
			break;

		default:
			ret = rtsp_msg_send_reply(session, 501);
			break;
		}
		break;
	}

	case READY_STATE: {
		switch (method) {
		case RTSP_ID_PLAY: // 状态迁移为播放态
			if ((ret = rtsp_msg_play(session)) == ERR_NOERROR) {
				CVI_LOGD("\tStart Playing!\n");
				session->client_info.cur_state = PLAY_STATE;
			}
			break;

		case RTSP_ID_SETUP:
			if ((ret = rtsp_msg_setup(instance, session)) == ERR_NOERROR) // 状态不变
			{
				session->client_info.cur_state = READY_STATE;
			}
			break;

		case RTSP_ID_TEARDOWN:
			ret = rtsp_msg_teardown(session);
			session->client_info.cur_state = TEARDOWN_STATE;
			break;

		case RTSP_ID_OPTIONS:
			if ((ret = rtsp_msg_options(session)) == ERR_NOERROR) {
				session->client_info.cur_state = INIT_STATE; // 状态不变
			}
			break;

		case RTSP_ID_PAUSE: // method not valid this state.
			ret = rtsp_msg_send_reply(session, 455);
			break;

		case RTSP_ID_DESCRIBE:
			ret = rtsp_msg_describe(instance, session);
			break;

		case RTSP_ID_GET_PARAMETERS:
			ret = rtsp_msg_get_parameters(session);
			break;

		default:
			ret = rtsp_msg_send_reply(session, 501);
			break;
		}

		break;
	}

	case PLAY_STATE: {
		switch (method) {
		case RTSP_ID_PLAY:
			// Feature not supported
			CVI_LOGE("UNSUPPORTED: Play while playing.\n");
			ret = rtsp_msg_send_reply(session, 551); // Option not supported
			break;
		case RTSP_ID_PAUSE:
			// rtsp_pause(c);
			session->client_info.cur_state = PAUSE_STATE;
			break;
		case RTSP_ID_TEARDOWN:
			ret = rtsp_msg_teardown(session);
			session->client_info.cur_state = TEARDOWN_STATE;
			break;

		case RTSP_ID_OPTIONS:
			break;

		case RTSP_ID_DESCRIBE:
			ret = rtsp_msg_describe(instance, session);
			break;

		case RTSP_ID_GET_PARAMETERS:
			ret = rtsp_msg_get_parameters(session);
			break;

		case RTSP_ID_SETUP:
			break;
		}

		break;
	} /* PLAY state */

	default: {
		/* invalid/unexpected current state. */
		CVI_LOGE("State error: unknown state=%d, method code=%d\n", session->client_info.cur_state, method);
		ret = ERR_GENERIC;
	} break;
	} /* end of current state switch */
	return ret;
}

/*根据缓冲区的内容，填充后边两个长度数据,检查缓冲区中消息的完整性
 * return -1 on ERROR
 * return RTSP_not_full (0) if a full RTSP message is NOT present in the in_buffer yet.
 * return RTSP_method_rcvd (1) if a full RTSP message is present in the in_buffer and is
 *                     ready to be handled.
 * return RTSP_interlvd_rcvd (2) if a complete RTP/RTCP interleaved packet is present.
 * terminate on really ugly cases.
 */
static int32_t rtsp_msg_full_msg_rcvd(RTSP_SESSION_S *session, int32_t *hdr_len, int32_t *body_len)
{
	int32_t eomh; /* end of message header found */
	int32_t mb;	  /* message body exists */
	int32_t tc;	  /* terminator count */
	int32_t ws;	  /* white space */
	uint32_t ml;  /* total message length including any message body */
	int32_t bl;	  /* message body length */
	char c;		  /* character */
	int32_t control;
	char *p;

	/*是否存在交叉存取的二进制rtp/rtcp数据包，参考RFC2326-10.12*/
	if (session->in_buffer[0] == '$') {
		/*暂不处理RTCP消息*/
		uint16_t *intlvd_len = (uint16_t *)&session->in_buffer[2]; /*跳过通道标志符*/
		/*转化为主机字节序，因为长度是网络字节序*/
		if ((bl = ntohs(*intlvd_len)) <= session->in_size) {
			CVI_LOGE("Interleaved RTP or RTCP packet arrived (len: %hu).\n", bl);
			if (hdr_len)
				*hdr_len = 4;
			if (body_len)
				*body_len = bl;
		} else {
			/*缓冲区不能完全存放数据*/
			if (hdr_len)
				*hdr_len = 0;
			if (body_len)
				*body_len = RTSP_BUFFERSIZE;
			CVI_LOGD("Non-complete Interleaved RTP or RTCP packet arrived.\n");
		}
		return RTSP_interlvd_rcvd;
	}

	eomh = mb = ml = bl = 0;
	while (ml <= session->in_size) {
		/* look for eol. */
		/*计算不包含回车、换行在内的所有字符数*/
		control = strcspn(&(session->in_buffer[ml]), "\r\n");
		if (control > 0)
			ml += control;
		else
			return ERR_GENERIC;

		/* haven't received the entire message yet. */
		if (ml > session->in_size)
			return RTSP_not_full;

		/* 处理终结符，判读是否是消息头的结束*/
		tc = ws = 0;
		while (!eomh && ((ml + tc + ws) < session->in_size)) {
			c = session->in_buffer[ml + tc + ws];
			/*统计回车换行*/
			if (c == '\r' || c == '\n')
				tc++;
			else if ((tc < 3) && ((c == ' ') || (c == '\t'))) {
				ws++; /*回车、换行之间的空格或者TAB，也是可以接受的 */
			} else {
				break;
			}
		}

		/*
		 *一对回车、换行符仅仅被统计为一个行终结符
		 * 双行可以被接受，并将其认为是消息头的结束标识
		 * 这与RFC2068中的描述一致，参考rfc2068 19.3
		 *否则，对所有的HTTP/1.1兼容协议消息元素来说，
		 *回车、换行被认为是合法的行终结符
		 */

		/* must be the end of the message header */
		if ((tc > 2) || ((tc == 2) && (session->in_buffer[ml] == session->in_buffer[ml + 1])))
			eomh = 1;
		ml += tc + ws;

		if (eomh) {
			ml += bl; /* 加入消息体长度 */
			if (ml <= session->in_size)
				break; /* all done finding the end of the message. */
		}

		if (ml >= session->in_size)
			return RTSP_not_full; /* 还没有完全接收消息 */

		/*检查每一行的第一个记号，确定是否有消息体存在 */
		if (!mb) {
			/* content length token not yet encountered. */
			if (!strncmp(&(session->in_buffer[ml]), HDR_CONTENTLENGTH, strlen(HDR_CONTENTLENGTH))) {
				mb = 1; /* 存在消息体. */
				ml += strlen(HDR_CONTENTLENGTH);
				/*跳过:和空格，找到长度字段*/
				while (ml < session->in_size) {
					c = session->in_buffer[ml];
					if ((c == ':') || (c == ' '))
						ml++;
					else
						break;
				}
				// Content-Length:后面是消息体长度值
				if (sscanf(&(session->in_buffer[ml]), "%d", &bl) != 1) {
					CVI_LOGE("RTSP_full_msg_rcvd(): Invalid ContentLength encountered in message.\n");
					return ERR_GENERIC;
				}
			}
		}
	}

	if (hdr_len)
		*hdr_len = ml - bl;

	if (body_len) {
		/*
		 * go through any trailing nulls.  Some servers send null terminated strings
		 * following the body part of the message.  It is probably not strictly
		 * legal when the null byte is not included in the Content-Length count.
		 * However, it is tolerated here.
		 * 减去可能存在的\0，它没有被计算在Content-Length中
		 */
		for (tc = session->in_size - ml, p = &(session->in_buffer[ml]); tc && (*p == '\0'); p++, bl++, tc--)
			;
		*body_len = bl;
	}

	return RTSP_method_rcvd;
}

static void rtsp_msg_remove_msg(RTSP_SESSION_S *session, int32_t len)
{
	session->in_size -= len;
	if (session->in_size && len) {
		// 删除指定长度的消息
		memmove(session->in_buffer, &(session->in_buffer[len]), RTSP_BUFFERSIZE - len);
		memset(&(session->in_buffer[RTSP_BUFFERSIZE - len]), 0, len);
	}
}

static void rtsp_msg_discard_msg(RTSP_SESSION_S *session)
{
	int32_t hlen = 0, blen = 0;
	// 找出缓冲区中首个消息的长度，然后删除
	if (rtsp_msg_full_msg_rcvd(session, &hlen, &blen) > 0)
		rtsp_msg_remove_msg(session, hlen + blen);
}

static int32_t rtsp_msg_do_msg(RTSP_INSTANCE_S **instance, RTSP_SESSION_S *session)
{
	int32_t s32Meth;
	int32_t ret = ERR_NOERROR;
	while (session->in_size > 0) {
		s32Meth = rtsp_msg_validate_method(session);
		if (s32Meth < 0) {
			CVI_LOGE("bad request, requestion not exit %d", s32Meth);
			rtsp_msg_send_reply(session, 400);
			memset(session->in_buffer, 0x0, RTSP_BUFFERSIZE);
			session->in_size = 0;
			ret = ERR_UNSUPPORTED_PT;
		} else {
			cvi_osal_mutex_lock(session->mutex);
			ret = rtsp_msg_state_machine(instance, session, s32Meth);
			cvi_osal_mutex_unlock(session->mutex);
			CVI_LOGD("exit Rtsp_state_machine %d", s32Meth);
		}
		rtsp_msg_discard_msg(session);
	}
	return ret;
}

static int32_t rtsp_session_create(RTSP_SESSION_S **session, int32_t fd)
{
	RTSP_SESSION_S *rtsp_session = (RTSP_SESSION_S *)malloc(sizeof(RTSP_SESSION_S));
	if (rtsp_session == NULL) {
		return -1;
	}
	memset(rtsp_session, 0, sizeof(RTSP_SESSION_S));
	rtsp_session->client_info.fd = fd;
	network_get_sockaddr(fd, &rtsp_session->client_info.port, rtsp_session->client_info.ip);
	cvi_osal_mutex_attr_t mutex_attr;
	mutex_attr.name = "rtsp_mutex";
	mutex_attr.type = PTHREAD_MUTEX_NORMAL;
	cvi_osal_mutex_create(&mutex_attr, &rtsp_session->mutex);
	rtsp_session->next = NULL;
	cvi_osal_get_boot_time_us(&rtsp_session->client_info.stime);
	rtsp_session->client_info.timeout = RTSP_SESSION_DEFAULT_TIMEOUT;
	rtsp_session->client_info.cur_state = INIT_STATE;
	*session = rtsp_session;
	return 0;
}

static void rtsp_session_destroy(RTSP_SESSION_S *session)
{
	CVI_LOGD("rtsp_session_destroy start %p", session);
	if (session) {
		CVI_LOGD("rtsp_session_destroy %s start", session->client_info.ip);
		cvi_osal_mutex_lock(session->mutex);
		for (int32_t i = 0; i < FRAME_TYPE_BUTT; i++) {
			if (session->rtp_session[i]) {
				rtp_session_destroy(session->rtp_session[i]);
				session->rtp_session[i] = NULL;
			}
		}
		shutdown(session->client_info.fd, SHUT_RDWR);
		network_tcp_close(session->client_info.fd);
		cvi_osal_mutex_unlock(session->mutex);
		cvi_osal_mutex_destroy(session->mutex);
		CVI_LOGD("rtsp_session_destroy %s end", session->client_info.ip);
		free(session);
	}
	CVI_LOGD("rtsp_session_destroy end");
}

static int32_t rtsp_session_timeout(RTSP_SESSION_S *session)
{
	if (session) {
		if (session->client_info.overtcp == 1) {
			return 0;
		}
		uint64_t cur = 0;
		cvi_osal_get_boot_time_us(&cur);
		if (session->client_info.stime + session->client_info.timeout < cur) {
			return 1;
		}

		if (session->client_info.cur_state == INIT_STATE && cur - session->client_info.stime > 500 * 1000) {
			CVI_LOGD("rtsp_session_timeout %s", session->client_info.ip);
			return 1;
		}
		return 0;
	}
	return 1;
}

static int32_t rtsp_session_socket_read(RTSP_SESSION_S *rtsp_session, int sec, int us)
{
	struct pollfd pfd;

	pfd.fd = rtsp_session->client_info.fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	int32_t ret = poll(&pfd, 1, sec * 1000 + us / 1000);
	if (ret < 0) {
		if (errno == EINTR) {
			return 0;
		}
		CVI_LOGE("client close by peer %d\n", rtsp_session->client_info.fd);
		return -1;
	} else if (ret == 0) {
		return 0;
	}

	char msg_buf[RTSP_BUFFERSIZE + 1] = {0};
	int32_t n = network_tcp_read(rtsp_session->client_info.fd, msg_buf, sizeof(msg_buf));
	if (n == 0) {
		return n;
	}

	if (n < 0) {
		rtsp_msg_send_reply(rtsp_session, 500);
		return -1;
	}

	if (rtsp_msg_discard_rtcp(rtsp_session->client_info.fd, msg_buf, n) != 0) {
		return 0;
	}

	if (rtsp_session->in_size + n > RTSP_BUFFERSIZE) {
		CVI_LOGE("RTSP buffer overflow (input RTSP message is most likely invalid) %d\n", n);
		rtsp_msg_send_reply(rtsp_session, 500);
		return -1;
	}

	memcpy(&(rtsp_session->in_buffer[rtsp_session->in_size]), msg_buf, n);
	rtsp_session->in_size += n;
	return n;
}

static int32_t rtsp_session_socket_write(RTSP_SESSION_S *rtsp_session)
{
	if (rtsp_session->out_size > 0) {
		network_tcp_write(rtsp_session->client_info.fd, rtsp_session->out_buffer, rtsp_session->out_size);
		memset(rtsp_session->out_buffer, 0x0, rtsp_session->out_size);
		rtsp_session->out_size = 0;
	}
	return 0;
}

static int32_t rtsp_session_get_state(RTSP_SESSION_S *rtsp_session)
{
	if (rtsp_session == NULL) {
		return TEARDOWN_STATE;
	}
	return rtsp_session->client_info.cur_state;
}

static int32_t rtsp_session_set_attr(RTSP_SESSION_S *rtsp_session, char *usrname, char *passwd, int32_t timeout)
{
	rtsp_session->client_info.timeout = timeout;
	strcpy(rtsp_session->username, usrname);
	strcpy(rtsp_session->password, passwd);
	return 0;
}

static int32_t rtsp_instance_get_exist(void)
{
	cvi_osal_mutex_lock(&rtsp_instance_mutex);
	for (int32_t i = 0; i < RTSP_INSTANCE_NUM; i++) {
		if (gstRtspInstance[i] != NULL) {
			cvi_osal_mutex_unlock(&rtsp_instance_mutex);
			return 1;
		}
	}
	cvi_osal_mutex_unlock(&rtsp_instance_mutex);
	return 0;
}

static void rtsp_instance_send_task(void *arg)
{
	RTSP_INSTANCE_S *instance = (RTSP_INSTANCE_S *)arg;
	char *buff = NULL;
	int32_t size = 0;
	uint32_t frame_cnt = 0;
	uint32_t vpts = (1000 / instance->info.framerate) * (90000 / 1000);
	uint32_t apts = instance->info.audio_pernum;
	uint32_t vcnt = 0, acnt = 0;
	// Adaptive send-interval: sleep min(10ms, frame_period/2) instead of a fixed 10ms.
	uint32_t send_interval = 10 * 1000;
	if (instance->info.framerate > 0) {
		uint32_t adaptive = (uint32_t)(1000000.0f / instance->info.framerate / 2.0f);
		if (adaptive < send_interval) {
			send_interval = adaptive;
		}
		if (send_interval < 1000) {
			send_interval = 1000;
		}
	}

	CVI_LOGD("rtsp_instance_send_task %s start", instance->info.rtsp_name);
	while (!instance->send_exit) {
		RTSP_FRAME_INFO_S finfo;
		int32_t ret = -1;
		FRAME_TYPE_E type = FRAME_TYPE_BUTT;
		cvi_osal_mutex_lock(instance->mutex);
		RTSP_SESSION_S *rtsp_session = instance->session;
		while (rtsp_session) {
			if (rtsp_session->client_info.cur_state == PLAY_STATE) {
				ret = 0;
				break;
			}
			rtsp_session = rtsp_session->next;
		}

		if (ret != 0) {
			frame_cnt = 0;
			cvi_osal_mutex_unlock(instance->mutex);
			cvi_osal_task_sleep(200 * 1000);
			continue;
		}

		if (instance->rbuf[FRAME_TYPE_VIDEO]) {
			vcnt = CVI_RBUF_DataCnt(instance->rbuf[FRAME_TYPE_VIDEO], 0);
		}
		if (instance->rbuf[FRAME_TYPE_AUDIO]) {
			acnt = CVI_RBUF_DataCnt(instance->rbuf[FRAME_TYPE_AUDIO], 0);
		}

		if (vcnt > acnt && vcnt > 0) {
			//When the video queue backlog is greater than the audio queue backlog, the video frame is sent out first.
			ret = CVI_RBUF_Copy_Out(instance->rbuf[FRAME_TYPE_VIDEO], (void *)&finfo, sizeof(RTSP_FRAME_INFO_S), 0, 0);
			type = FRAME_TYPE_VIDEO;
		} else if (acnt > vcnt && acnt > 0) {
			//When the audio queue backlog is greater than the video queue backlog, the audio frame is output.
			ret = CVI_RBUF_Copy_Out(instance->rbuf[FRAME_TYPE_AUDIO], (void *)&finfo, sizeof(RTSP_FRAME_INFO_S), 0, 0);
			type = FRAME_TYPE_AUDIO;
		} else if (vcnt == acnt && vcnt > 0) {
			// When the audio queue backlog is equal to the video queue backlog, the video is given priority
			// (note: can be adjusted according to actual needs)
			ret = CVI_RBUF_Copy_Out(instance->rbuf[FRAME_TYPE_VIDEO], (void *)&finfo, sizeof(RTSP_FRAME_INFO_S), 0, 0);
			type = FRAME_TYPE_VIDEO;
		} else if (vcnt > 0) {
			// When only video queue are left
			ret = CVI_RBUF_Copy_Out(instance->rbuf[FRAME_TYPE_VIDEO], (void *)&finfo, sizeof(RTSP_FRAME_INFO_S), 0, 0);
			type = FRAME_TYPE_VIDEO;
		} else if (acnt > 0) {
			// When only audio queue are left
			ret = CVI_RBUF_Copy_Out(instance->rbuf[FRAME_TYPE_AUDIO], (void *)&finfo, sizeof(RTSP_FRAME_INFO_S), 0, 0);
			type = FRAME_TYPE_AUDIO;
		} else {
			// Both queues are empty, no processing is done
			ret = -1;
			type = -1;
		}

		if (type != FRAME_TYPE_BUTT && ret == 0 && finfo.hmagic == 0x5a5a5a5a && finfo.tmagic == 0x5a5a5a5a) {
			if (finfo.len > size) {
				if (buff)
					free(buff);
				size = finfo.len + 50 * 1024;
				buff = malloc(size);
				if (!buff)
					size = 0;
				CVI_LOGD("rtsp send buff increase to %d", size);
			}

			if (buff) {
				CVI_RBUF_Copy_Out(instance->rbuf[type], buff, finfo.len, sizeof(RTSP_FRAME_INFO_S), 0);
				CVI_RBUF_Refresh_Out(instance->rbuf[type], finfo.totalsize, 0);

				RTSP_SESSION_S *session = instance->session;
				while (session && session->client_info.cur_state == PLAY_STATE) {
					cvi_osal_mutex_lock(session->mutex);
					if (type == FRAME_TYPE_VIDEO) {
						if (instance->info.video_codec == RTSP_VIDEO_H264) {
							ret = rtp_send_data(session->rtp_session[type], buff, finfo.len, vpts, H264_PAYLOAD_TYPE);
						} else if (instance->info.video_codec == RTSP_VIDEO_H265) {
							ret = rtp_send_data(session->rtp_session[type], buff, finfo.len, vpts, H265_PAYLOAD_TYPE);
						} else if (instance->info.video_codec == RTSP_VIDEO_MJPEG) {
							ret = rtp_send_data(session->rtp_session[type], buff, finfo.len, vpts, MJPEG_PAYLOAD_TYPE);
						}
					} else if (type == FRAME_TYPE_AUDIO) {
						if (instance->info.audio_codec == RTSP_AUDIO_PCM) {
							ret = rtp_send_data(session->rtp_session[type], buff, finfo.len, apts, PCM_PAYLOAD_TYPE);
						} else if (instance->info.audio_codec == RTSP_AUDIO_AAC) {
							ret = rtp_send_data(session->rtp_session[type], buff, finfo.len, apts, AAC_PAYLOAD_TYPE);
						}
					}
					if (ret == 0) {
						cvi_osal_get_boot_time_us(&session->client_info.stime);
					}
					cvi_osal_mutex_unlock(session->mutex);
					session = session->next;
				}

				if (frame_cnt++ > 2000) {
					CVI_LOGI("show %s rbuf info:", instance->info.rtsp_name);
					CVI_RBUF_ShowLog(instance->rbuf[FRAME_TYPE_VIDEO]);
					CVI_RBUF_ShowLog(instance->rbuf[FRAME_TYPE_AUDIO]);
					frame_cnt = 0;
				}
			}
		}
		cvi_osal_mutex_unlock(instance->mutex);
		cvi_osal_task_sleep(send_interval);
	}
	if (buff) {
		free(buff);
	}
}

static void rtsp_instance_event_init(RTSP_EVENT_S *event, int32_t id, char *ip, char *name, RTSP_EVENT_TYPE_E event_type)
{
	event->e = event_type;
	event->rtsp_id = id;
	strcpy(event->cli_ipaddr, ip);
	strcpy(event->rtsp_name, name);
}

static int32_t rtsp_instance_create(RTSP_INSTANCE_S **instance, RTSP_MEDIA_INFO_S *media_info)
{
	int32_t ret = 0;
	RTSP_INSTANCE_S *new = (RTSP_INSTANCE_S *)malloc(sizeof(RTSP_INSTANCE_S));
	if (new == NULL) {
		CVI_LOGE("rtsp_instance create failed with OOM");
		return -1;
	}
	memset(new, 0, sizeof(RTSP_INSTANCE_S));
	new->info = *media_info;
	if (new->info.video_port == 0) {
		new->info.video_port = RTSP_VIDEO_DEFAULT_PORT;
	}

	if (new->info.audio_port == 0) {
		new->info.audio_port = RTSP_AUDIO_DEFAULT_PORT;
	}
	CVI_LOGD("rtsp_instance %s create success", media_info->rtsp_name);
	uint32_t bufsize = ((uint32_t)media_info->bitrate_kbps >> 3) * 1024 * 3;
	ret = CVI_RBUF_Init(&new->rbuf[FRAME_TYPE_VIDEO], bufsize, "rtsp_v", 1, rtsp_malloc, rtsp_free);
	if (ret != 0) {
		CVI_LOGE("rtsp_rbuf V create failed with OOM");
		free(new);
		return ret;
	}
	CVI_LOGD("rtsp_instance %s rbuf %u create success", media_info->rtsp_name, bufsize);

	if (media_info->audio_codec != RTSP_AUDIO_BUTT) {
		bufsize = media_info->audio_channels * media_info->audio_pernum * 2 * 200;
		ret = CVI_RBUF_Init(&new->rbuf[FRAME_TYPE_AUDIO], bufsize, "rtsp_a", 1, rtsp_malloc, rtsp_free);
		if (ret != 0) {
			CVI_LOGE("rtsp_rbuf A create failed with OOM");
			CVI_RBUF_DeInit(new->rbuf[FRAME_TYPE_VIDEO]);
			free(new);
			return ret;
		}
	}
	cvi_osal_mutex_attr_t mutex_attr;
	mutex_attr.name = "rtsp_mutex";
	mutex_attr.type = PTHREAD_MUTEX_NORMAL;
	cvi_osal_mutex_create(&mutex_attr, &new->mutex);
	CVI_LOGD("rtsp_instance %s rbuf %u create success", media_info->rtsp_name, bufsize);
	CVI_LOGD("CVI_RBUF_Init %p %p %p", new, new->rbuf[FRAME_TYPE_VIDEO], new->rbuf[FRAME_TYPE_AUDIO]);

	new->send_exit = 0;
	cvi_osal_task_attr_t ta;
	ta.name = "rtsp_send";
	ta.entry = rtsp_instance_send_task;
	ta.param = (void *)new;
	ta.priority = CVI_OSAL_PRI_RT_HIGH;
	ta.detached = false;
	ta.stack_size = 256 * 1024;
	CVI_LOGD("rtsp_instance %s send_task create start", media_info->rtsp_name);
	cvi_osal_task_create(&ta, &new->send_task);
	CVI_LOGD("rtsp_instance %s send_task create success\n", media_info->rtsp_name);
	*instance = new;
	return 0;
}

static void rtsp_instance_destroy(RTSP_INSTANCE_S *instance)
{
	if (instance) {
		CVI_LOGD("rtsp_instance_destroy %d start", instance->info.id);
		if (instance->send_task) {
			instance->send_exit = 1;
			cvi_osal_task_join(instance->send_task);
			cvi_osal_task_destroy(&instance->send_task);
		}

		cvi_osal_mutex_lock(instance->mutex);
		RTSP_SESSION_S *rtsp_session = instance->session;
		while (rtsp_session) {
			RTSP_EVENT_S event;
			rtsp_instance_event_init(&event, instance->info.id, rtsp_session->client_info.ip, rtsp_session->url, RTSP_EVENT_CLI_DISCONNECT);
			instance->info.cb(&event);
			CVI_LOGD("%s %s send CLI_DISCONNECT", rtsp_session->url, rtsp_session->client_info.ip);
			RTSP_SESSION_S *tmp = rtsp_session;
			rtsp_session = rtsp_session->next;
			tmp->client_info.cur_state = DESTROY_STATE;
			// rtsp_session_destroy(tmp);
		}

		for (int32_t i = 0; i < FRAME_TYPE_BUTT; i++) {
			CVI_LOGD("CVI_RBUF_Destroy %p %p", instance, instance->rbuf[i]);
			if (instance->rbuf[i]) {
				CVI_RBUF_DeInit(instance->rbuf[i]);
				instance->rbuf[i] = NULL;
			}
		}

		cvi_osal_mutex_unlock(instance->mutex);
		cvi_osal_mutex_destroy(instance->mutex);
		CVI_LOGD("rtsp_instance_destroy %d end", instance->info.id);
		free(instance);
	}
}

static int32_t rtsp_instance_add(RTSP_INSTANCE_S **instance, RTSP_INSTANCE_S *new)
{
	CVI_LOGD("rtsp_instance_add %s %d start", new->info.rtsp_name, new->info.id);
	if (new == NULL || (new->info.id >= RTSP_INSTANCE_NUM &&new->info.id < 0)) {
		CVI_LOGE("invalid input");
		return -1;
	}
	cvi_osal_mutex_lock(&rtsp_instance_mutex);
	if (instance[new->info.id] == NULL) {
		instance[new->info.id] = new;
		cvi_osal_mutex_unlock(&rtsp_instance_mutex);
		return 0;
	} else {
		CVI_LOGE("instance %s already exist", new->info.rtsp_name);
	}
	cvi_osal_mutex_unlock(&rtsp_instance_mutex);
	CVI_LOGD("rtsp_instance_add %s %d end", new->info.rtsp_name, new->info.id);
	return -1;
}

static void rtsp_instance_del(RTSP_INSTANCE_S **instance, RTSP_INSTANCE_S *del)
{
	if (del == NULL || (del->info.id >= RTSP_INSTANCE_NUM && del->info.id < 0)) {
		CVI_LOGE("invalid input");
		return;
	}
	CVI_LOGD("rtsp_instance_del %d start", del->info.id);
	int32_t id = del->info.id;
	cvi_osal_mutex_lock(&rtsp_instance_mutex);
	if (instance[id] != NULL) {
		instance[id] = NULL;
	}
	cvi_osal_mutex_unlock(&rtsp_instance_mutex);
	rtsp_instance_destroy(del);

	CVI_LOGD("rtsp_instance_del %d end", id);
}

static void rtsp_instance_remove_session(RTSP_INSTANCE_S **instance, RTSP_SESSION_S *session)
{
	CVI_LOGD("rtsp_instance start %s %s %d", session->url, session->client_info.ip, session->client_info.port);
	for (int32_t i = 0; i < RTSP_INSTANCE_NUM; i++) {
		if (instance[i] != NULL) {
			cvi_osal_mutex_lock(instance[i]->mutex);
			if (strcmp(instance[i]->info.rtsp_name, session->url) == 0) {
				RTSP_EVENT_S event;
				rtsp_instance_event_init(&event, instance[i]->info.id, session->client_info.ip, session->url, RTSP_EVENT_CLI_DISCONNECT);
				instance[i]->info.cb(&event);
				RTSP_SESSION_S *rtsp_session = instance[i]->session;
				if (instance[i]->session == session) {
					CVI_LOGD("rtsp_instance find %s %s %d", session->url, session->client_info.ip, session->client_info.port);
					instance[i]->session = instance[i]->session->next;
				} else {
					while (rtsp_session->next) {
						if (rtsp_session->next == session) {
							CVI_LOGD("rtsp_instance find %s %s %d", session->url, session->client_info.ip, session->client_info.port);
							rtsp_session->next = rtsp_session->next->next;
							break;
						}
						rtsp_session = rtsp_session->next;
					}
				}
				cvi_osal_mutex_unlock(instance[i]->mutex);
				break;
			}
			cvi_osal_mutex_unlock(instance[i]->mutex);
		}
	}
	CVI_LOGD("rtsp_instance end %s %s %d", session->url, session->client_info.ip, session->client_info.port);
}

static void rtsp_instance_add_session(RTSP_INSTANCE_S **instance, RTSP_SESSION_S *session)
{
	for (int32_t i = 0; i < RTSP_INSTANCE_NUM; i++) {
		if (instance[i] != NULL) {
			cvi_osal_mutex_lock(instance[i]->mutex);
			if (strcmp(instance[i]->info.rtsp_name, session->url) == 0) {
				RTSP_EVENT_S event;
				rtsp_instance_event_init(&event, instance[i]->info.id, session->client_info.ip, session->url, RTSP_EVENT_CLI_CONNECT);
				instance[i]->info.cb(&event);
				RTSP_SESSION_S *rtsp_session = instance[i]->session;
				if (rtsp_session == NULL) {
					instance[i]->session = session;
					cvi_osal_mutex_unlock(instance[i]->mutex);
					break;
				} else {
					while (rtsp_session->next) {
						rtsp_session = rtsp_session->next;
					}
					rtsp_session->next = session;
					cvi_osal_mutex_unlock(instance[i]->mutex);
					break;
				}
			}
			cvi_osal_mutex_unlock(instance[i]->mutex);
		}
	}
}

static void rtsp_server_listen_task(void *arg)
{
	RTSP_SERVER_S *rtsp_server = (RTSP_SERVER_S *)arg;

	CVI_LOGD("listen task start");
	while (!rtsp_server->listen_exit) {
		cvi_osal_mutex_lock(rtsp_server->mutex);
		if (!rtsp_instance_get_exist()) {
			cvi_osal_mutex_unlock(rtsp_server->mutex);
			cvi_osal_task_sleep(200 * 1000);
			continue;
		}

		if (rtsp_server->listen_fd == 0) {
			rtsp_server->listen_fd = network_tcp_open(rtsp_server->info.port, rtsp_server->info.max_conn);
			if (rtsp_server->listen_fd < 0) {
				CVI_LOGE("listen failed");
				rtsp_server->listen_fd = 0;
				cvi_osal_mutex_unlock(rtsp_server->mutex);
				cvi_osal_task_sleep(200 * 1000);
				continue;
			} else {
				CVI_LOGD("listen success %d", rtsp_server->listen_fd);
			}
		}

		int32_t fd = network_tcp_accept(rtsp_server->listen_fd);
		if (fd > 0) {
			CVI_LOGD("new connect %d coming", fd);
			/*设置通信socket非阻塞*/
			int32_t on = 1;
			if(ioctl(fd, FIONBIO, &on) < 0){
			    CVI_LOGE("connect(%d) ioctl error in tcp_accept.",fd);
			}
			/*Search for idle task threads*/
			int32_t found = 0;
			for (int32_t i = 0; i < RTSP_THREAD_POOL_NUM; i++) {
				if (rtsp_server->pool_task_arg[i].arg == NULL) {
					RTSP_SESSION_S *rtsp_session = NULL;
					rtsp_session_create(&rtsp_session, fd);
					rtsp_session_set_attr(rtsp_session, gstRtspServer->info.username, gstRtspServer->info.password, gstRtspServer->info.timeout);
					rtsp_server->pool_task_arg[i].arg = (void *)rtsp_session;
					found = 1;
					CVI_LOGD("pool_task %d start %d", i, fd);
					break;
				}
			}
			if (found == 0) {
				CVI_LOGI("The maximum task processing capacity has been reached %d", fd);
				close(fd);
			}
		}
		cvi_osal_mutex_unlock(rtsp_server->mutex);
		cvi_osal_task_sleep(50 * 1000);
	}
}

static void rtsp_server_task_pool(void *arg)
{
	RTSP_TASK_ARG_S *task = (RTSP_TASK_ARG_S *)arg;
	RTSP_SESSION_S *rtsp_session = NULL;
	int32_t ret = 0;
	int32_t laststate = INIT_STATE;
	CVI_LOGD("task_index %d start", task->id);
	while (!task->pool_exit) {
		if (rtsp_session == NULL && task->arg != NULL) {
			rtsp_session = (RTSP_SESSION_S *)task->arg;
			CVI_LOGI("rtsp_session ip %s port %u fd %d", rtsp_session->client_info.ip, rtsp_session->client_info.port, rtsp_session->client_info.fd);
		}

		if (rtsp_session == NULL) {
			cvi_osal_task_sleep(100 * 1000);
			continue;
		}

		if (rtsp_session_timeout(rtsp_session)) {
			CVI_LOGE("%p %s rtsp client timeout, close it", rtsp_session, rtsp_session->client_info.ip);
			cvi_osal_mutex_lock(&rtsp_instance_mutex);
			rtsp_instance_remove_session(gstRtspInstance, rtsp_session);
			cvi_osal_mutex_unlock(&rtsp_instance_mutex);
			rtsp_session_destroy(rtsp_session);
			rtsp_session = task->arg = NULL;
			laststate = INIT_STATE;
			continue;
		}

		ret = rtsp_session_socket_read(rtsp_session, 0, 120 * 1000);
		if (ret < 0) {
			CVI_LOGE("%p %s rtsp client close by peer", rtsp_session, rtsp_session->client_info.ip);
			cvi_osal_mutex_lock(&rtsp_instance_mutex);
			rtsp_instance_remove_session(gstRtspInstance, rtsp_session);
			cvi_osal_mutex_unlock(&rtsp_instance_mutex);
			rtsp_session_destroy(rtsp_session);
			rtsp_session = task->arg = NULL;
			laststate = INIT_STATE;
			continue;
		}

		if (ret > 0) {
			ret = rtsp_msg_do_msg(gstRtspInstance, rtsp_session);
			if (ret != ERR_NOERROR) {
				CVI_LOGE("%p %s Invalid input message", rtsp_session, rtsp_session->client_info.ip);
				cvi_osal_mutex_lock(&rtsp_instance_mutex);
				rtsp_instance_remove_session(gstRtspInstance, rtsp_session);
				cvi_osal_mutex_unlock(&rtsp_instance_mutex);
				rtsp_session_destroy(rtsp_session);
				rtsp_session = task->arg = NULL;
				laststate = INIT_STATE;
				continue;
			}
			CVI_LOGD("%p %s rtsp_do_msg finished", rtsp_session, rtsp_session->client_info.ip);
		}
		rtsp_session_socket_write(rtsp_session);

		int32_t state = rtsp_session_get_state(rtsp_session);
		if (laststate != TEARDOWN_STATE && (state == TEARDOWN_STATE || state == DESTROY_STATE)) {
			CVI_LOGD("%p laststate %d curstate %d", rtsp_session, laststate, state);
			cvi_osal_mutex_lock(&rtsp_instance_mutex);
			rtsp_instance_remove_session(gstRtspInstance, rtsp_session);
			cvi_osal_mutex_unlock(&rtsp_instance_mutex);
			rtsp_session_destroy(rtsp_session);
			rtsp_session = task->arg = NULL;
			laststate = TEARDOWN_STATE;
		} else if (laststate != PLAY_STATE && state == PLAY_STATE) {
			cvi_osal_mutex_lock(&rtsp_instance_mutex);
			rtsp_instance_add_session(gstRtspInstance, rtsp_session);
			cvi_osal_mutex_unlock(&rtsp_instance_mutex);
			CVI_LOGD("%p laststate %d curstate %d", rtsp_session, laststate, state);
			laststate = PLAY_STATE;
		}
		cvi_osal_task_sleep(20 * 1000);
	}

	if (rtsp_session) {
		cvi_osal_mutex_lock(&rtsp_instance_mutex);
		rtsp_instance_remove_session(gstRtspInstance, rtsp_session);
		cvi_osal_mutex_unlock(&rtsp_instance_mutex);
		rtsp_session_destroy(rtsp_session);
		rtsp_session = task->arg = NULL;
	}
}

static int32_t rtsp_server_create(RTSP_SERVER_S **c, RTSP_INFO_S *info)
{
	RTSP_SERVER_S *rtsp_server = (RTSP_SERVER_S *)malloc(sizeof(RTSP_SERVER_S));
	if (rtsp_server == NULL) {
		CVI_LOGE("rtsp_server create failed with OOM");
		return -1;
	}
	memset(rtsp_server, 0, sizeof(RTSP_SERVER_S));
	rtsp_server->info = *info;
	/*second convert to usecond*/
	rtsp_server->info.timeout = rtsp_server->info.timeout * 1000 * 1000;
	cvi_osal_mutex_attr_t mutex_attr;
	mutex_attr.name = "rtsp_mutex";
	mutex_attr.type = PTHREAD_MUTEX_NORMAL;
	cvi_osal_mutex_create(&mutex_attr, &rtsp_server->mutex);

	rtsp_server->listen_exit = 0;
	cvi_osal_task_attr_t ta;
	ta.name = "rtsp_listen";
	ta.entry = rtsp_server_listen_task;
	ta.param = (void *)rtsp_server;
	ta.priority = CVI_OSAL_PRI_NORMAL;
	ta.detached = false;
	ta.stack_size = 512 * 1024;
	cvi_osal_task_create(&ta, &rtsp_server->listen_task);
	CVI_LOGD("listen_task create success");

	static char pool_name[RTSP_THREAD_POOL_NUM][16];

	for (int32_t i = 0; i < RTSP_THREAD_POOL_NUM; i++) {
		rtsp_server->pool_task_arg[i].arg = NULL;
		rtsp_server->pool_task_arg[i].id = i;
		rtsp_server->pool_task_arg[i].pool_exit = 0;
		snprintf(pool_name[i], sizeof(pool_name[i]), RTSP_THREAD_POOL_PREFIX "%d", i);
		cvi_osal_task_attr_t pool;
		pool.name = pool_name[i];
		pool.entry = rtsp_server_task_pool;
		pool.param = (void *)&rtsp_server->pool_task_arg[i];
		pool.priority = CVI_OSAL_PRI_NORMAL;
		pool.detached = false;
		pool.stack_size = 512 * 1024;
		cvi_osal_task_create(&pool, &rtsp_server->pool_task[i]);
		CVI_LOGD("task_pool %d create success", i);
	}

	*c = rtsp_server;
	return 0;
}

static void rtsp_server_destroy(RTSP_SERVER_S *server)
{
	if (server) {
		cvi_osal_mutex_lock(server->mutex);

		if (server->listen_task) {
			server->listen_exit = 1;
			cvi_osal_task_join(server->listen_task);
			cvi_osal_task_destroy(&server->listen_task);
			network_tcp_close(server->listen_fd);
		}

		for (int32_t i = 0; i < RTSP_THREAD_POOL_NUM; i++) {
			if (server->pool_task[i]) {
				server->pool_task_arg[i].pool_exit = 1;
				cvi_osal_task_join(server->pool_task[i]);
				cvi_osal_task_destroy(&server->pool_task[i]);
			}
		}
		cvi_osal_mutex_unlock(server->mutex);
		cvi_osal_mutex_destroy(server->mutex);
		free(server);
	}
}

int32_t RTSP_Create(void** hdl, RTSP_INFO_S* info, RTSP_MEDIA_INFO_S* media_info)
{
    CVI_LOGI("%s build id: %s", __FUNCTION__, COMMIT_ID);
    CVI_LOGD("RTSP_Create %s start", media_info->rtsp_name);
    int32_t ret = 0;
    cvi_osal_mutex_lock(&rtsp_server_mutex);
    if (gstRtspServer == NULL) {
        ret = rtsp_server_create(&gstRtspServer, info);
        if (ret != 0) {
            cvi_osal_mutex_unlock(&rtsp_server_mutex);
            CVI_LOGE("rtsp_server_create failed");
            return ret;
        }
    }
    CVI_LOGD("rtsp_instance_create %s start", media_info->rtsp_name);
    RTSP_INSTANCE_S* instance = NULL;
    ret = rtsp_instance_create(&instance, media_info);
    if (ret != 0) {
        cvi_osal_mutex_unlock(&rtsp_server_mutex);
        CVI_LOGE("rtsp_instance_create failed");
        return ret;
    }
    CVI_LOGD("rtsp_instance_create %s finished", media_info->rtsp_name);
    ret = rtsp_instance_add(gstRtspInstance, instance);
    if (ret != 0) {
        rtsp_instance_destroy(instance);
        cvi_osal_mutex_unlock(&rtsp_server_mutex);
        CVI_LOGE("rtsp_server_add_instance failed");
        return ret;
    }

    *hdl = instance;
    cvi_osal_mutex_unlock(&rtsp_server_mutex);
    CVI_LOGD("RTSP_Create %s end", media_info->rtsp_name);
    return 0;
}

int32_t RTSP_Destroy(void *hdl)
{
	if (hdl == NULL) {
		return -1;
	}
	CVI_LOGD("RTSP_Destroy start");
	cvi_osal_mutex_lock(&rtsp_server_mutex);
	int32_t exist_instance = 0;
	rtsp_instance_del(gstRtspInstance, (RTSP_INSTANCE_S *)hdl);
	exist_instance = rtsp_instance_get_exist();

	if (gstRtspServer && exist_instance == 0) {
		rtsp_server_destroy(gstRtspServer);
		gstRtspServer = NULL;
	}
	cvi_osal_mutex_unlock(&rtsp_server_mutex);
	CVI_LOGD("RTSP_Destroy end");
	return 0;
}

int32_t RTSP_SendFrame(void *hdl, RTSP_FRAME_S *frame)
{
	RTSP_INSTANCE_S *instance = (RTSP_INSTANCE_S *)hdl;
	if (instance && instance->rbuf[frame->type]) {
		if (frame->type == FRAME_TYPE_AUDIO && CVI_RBUF_Get_InSize(instance->rbuf[FRAME_TYPE_VIDEO]) == 0) {
			return 0;
		}
		RTSP_FRAME_INFO_S frame_info;
		frame_info.hmagic = 0x5a5a5a5a;
		frame_info.tmagic = 0x5a5a5a5a;
		frame_info.type = frame->type;
		frame_info.len = 0;
		frame_info.totalsize = sizeof(RTSP_FRAME_INFO_S);
		for (int32_t i = 0; i < MAX_RTSP_VIDEO_FRAME && frame->len[i] > 0; i++) {
			frame_info.totalsize += frame->len[i];
			frame_info.len += frame->len[i];
			frame_info.key = frame->iskey[i];
			frame_info.pts = frame->vi_pts[i];
		}

		uint32_t unused = CVI_RBUF_Unused(instance->rbuf[frame_info.type]);
		if (unused >= (uint32_t)frame_info.totalsize) {
			int32_t off = 0;
			CVI_RBUF_Copy_In(instance->rbuf[frame_info.type], (void *)&frame_info, sizeof(RTSP_FRAME_INFO_S), off);
			off += sizeof(RTSP_FRAME_INFO_S);
			for (int32_t i = 0; i < MAX_RTSP_VIDEO_FRAME && frame->len[i] > 0; i++) {
				CVI_RBUF_Copy_In(instance->rbuf[frame_info.type], frame->data[i], frame->len[i], off);
				off += frame->len[i];
			}
			CVI_RBUF_Refresh_In(instance->rbuf[frame_info.type], frame_info.totalsize);
		} else {
			CVI_LOGE("rtsp_instance_send_frame failed %u %u", unused, frame_info.totalsize);
			return -1;
		}
	}
	return 0;
}
