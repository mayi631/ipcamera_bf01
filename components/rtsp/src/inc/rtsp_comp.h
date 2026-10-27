#ifndef _RTSP_H_
#define _RTSP_H_

/*standard header file*/
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

/*external header file*/
#include "cvi_log.h"
#include "cvi_osal.h"
#include "cvi_rbuf.h"
#include "rtsp.h"

/*internal header file*/
#include "network.h"
#include "rtp.h"

#ifndef COMMIT_ID
#define COMMIT_ID "ff7c7ac141e25444704b667b7dc82379651d2a45"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RTSP_VER "RTSP/1.0"
#define RTSP_EL "\r\n"
#define PACKAGE "sophgo"
#define VERSION "1.11"
#define SDP_EL "\r\n"
#define RTSP_RTP_AVP "RTP/AVP"
#define RTSP_STREAM_URL "trackID="

/* msg header string */
#define HDR_CONTENTLENGTH "Content-Length"
#define HDR_ACCEPT "Accept"
#define HDR_ALLOW "Allow"
#define HDR_BLOCKSIZE "Blocksize"
#define HDR_CONTENTTYPE "Content-Type"
#define HDR_DATE "Date"
#define HDR_REQUIRE "Require"
#define HDR_TRANSPORTREQUIRE "Transport-Require"
#define HDR_SEQUENCENO "SequenceNo"
#define HDR_CSEQ "CSeq"
#define HDR_STREAM "Stream"
#define HDR_SESSION "Session"
#define HDR_TRANSPORT "Transport"
#define HDR_RANGE "Range"
#define HDR_USER_AGENT "User-Agent"

/*rtsp method string*/
#define RTSP_METHOD_MAXLEN 15
#define RTSP_METHOD_DESCRIBE "DESCRIBE"
#define RTSP_METHOD_ANNOUNCE "ANNOUNCE"
#define RTSP_METHOD_GET_PARAMETERS "GET_PARAMETER"
#define RTSP_METHOD_OPTIONS "OPTIONS"
#define RTSP_METHOD_PAUSE "PAUSE"
#define RTSP_METHOD_PLAY "PLAY"
#define RTSP_METHOD_RECORD "RECORD"
#define RTSP_METHOD_REDIRECT "REDIRECT"
#define RTSP_METHOD_SETUP "SETUP"
#define RTSP_METHOD_SET_PARAMETER "SET_PARAMETER"
#define RTSP_METHOD_TEARDOWN "TEARDOWN"

/*rtsp method ID*/
#define RTSP_ID_DESCRIBE 0
#define RTSP_ID_ANNOUNCE 1
#define RTSP_ID_GET_PARAMETERS 2
#define RTSP_ID_OPTIONS 3
#define RTSP_ID_PAUSE 4
#define RTSP_ID_PLAY 5
#define RTSP_ID_RECORD 6
#define RTSP_ID_REDIRECT 7
#define RTSP_ID_SETUP 8
#define RTSP_ID_SET_PARAMETER 9
#define RTSP_ID_TEARDOWN 10
#define RTSP_ID_RTCP 11

#define RTSP_BUFFERSIZE 4096

/*error codes define*/
#define ERR_NOERROR 0
#define ERR_GENERIC -1
#define ERR_NOT_FOUND -2
#define ERR_PARSE -3
#define ERR_ALLOC -4
#define ERR_INPUT_PARAM -5
#define ERR_NOT_SD -6
#define ERR_UNSUPPORTED_PT -7
#define ERR_EOF -8
#define ERR_FATAL -9
#define ERR_CONNECTION_CLOSE -10

/*		RTSP MSG		*/
#define RTSP_not_full 0
#define RTSP_method_rcvd 1
#define RTSP_interlvd_rcvd 2

/* Stati della macchina a stati del server rtsp*/
#define INIT_STATE 0
#define READY_STATE 1
#define PLAY_STATE 2
#define PAUSE_STATE 3
#define TEARDOWN_STATE 4
#define DESTROY_STATE 0xFF

#define RTSP_VIDEO_DEFAULT_PORT (10000)
#define RTSP_AUDIO_DEFAULT_PORT (20000)
#define RTSP_SESSION_DEFAULT_TIMEOUT (120 * 1000 * 1000)
#define RTSP_ALIGN(s, n) (((s) + (n) - 1) & (~((n) - 1)))

#define RTSP_THREAD_POOL_NUM (RTSP_INSTANCE_NUM)
#define RTSP_THREAD_POOL_PREFIX "rtsp_pool_"

typedef struct RTSP_FRAME_INFO {
	uint32_t hmagic;
	FRAME_TYPE_E type;
	int32_t key;
	int32_t len;
	uint64_t pts;
	int32_t totalsize;
	uint32_t tmagic;
} RTSP_FRAME_INFO_S;

typedef struct RTSP_CLIENT_INFO {
	int32_t fd;
	uint32_t port;
	char ip[32];
	int32_t timeout;
	uint32_t cseq;
	uint64_t stime;
	int32_t overtcp;
	int32_t cur_state;
	int32_t session_id;
} RTSP_CLIENT_INFO_S;

typedef struct RTSP_SESSION {
	char url[MAX_RTSP_NAME_LEN];
	char localip[32];
	RTSP_CLIENT_INFO_S client_info;
	char in_buffer[RTSP_BUFFERSIZE];
	uint32_t in_size;
	char out_buffer[RTSP_BUFFERSIZE * 2];
	char msg_buffer[RTSP_BUFFERSIZE];
	int32_t out_size;
	void *rtp_session[FRAME_TYPE_BUTT]; /*session, 0:video, 1:audio*/
	cvi_osal_mutex_handle_t mutex;
	char username[MAX_RTSP_NAME_LEN];
	char password[MAX_RTSP_NAME_LEN];
	struct RTSP_SESSION *next;
} RTSP_SESSION_S;

typedef struct RTSP_INSTANCE {
	RTSP_MEDIA_INFO_S info;
	void *rbuf[FRAME_TYPE_BUTT];
	RTSP_SESSION_S *session;
	cvi_osal_mutex_handle_t mutex;
	cvi_osal_task_handle_t send_task;
	int32_t send_exit;
} RTSP_INSTANCE_S;

typedef struct RTSP_TASK_ARG {
	int32_t id;
	int32_t pool_exit;
	void *arg;
} RTSP_TASK_ARG_S;

typedef struct RTSP_SERVER {
	RTSP_INFO_S info;
	int32_t listen_fd;
	cvi_osal_task_handle_t listen_task;
	int32_t listen_exit;
	cvi_osal_task_handle_t pool_task[RTSP_THREAD_POOL_NUM];
	RTSP_TASK_ARG_S pool_task_arg[RTSP_THREAD_POOL_NUM];
	cvi_osal_mutex_handle_t mutex;
} RTSP_SERVER_S;

#ifdef __cplusplus
}
#endif

#endif
