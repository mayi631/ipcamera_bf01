
#ifndef _RTP_H
#define _RTP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_RTP_PKT_LENGTH 1400

#define H264_PAYLOAD_TYPE 96
#define H265_PAYLOAD_TYPE 98
#define MJPEG_PAYLOAD_TYPE 26
#define PCM_PAYLOAD_TYPE 8
#define AAC_PAYLOAD_TYPE 97

typedef struct RTP_PORT {
    int32_t rtp;
    int32_t rtcp;
} RTP_PORT_S;

typedef enum RTP_TYPE {
    RTP_OVER_UDP,
    RTP_OVER_TCP,
    RTP_OVER_BUTT
} RTP_TYPE_E;

typedef struct RTP_TRANSPORT_INFO {
    RTP_TYPE_E type;
    union {
        struct {
            RTP_PORT_S cli_ports;
            RTP_PORT_S ser_ports;
            char cli_ip[32];
            unsigned char is_multicast;
            int32_t ttl;
        } udp;
        struct {
            int32_t fd;
            RTP_PORT_S interleaved;
        } tcp;
    } u;
} RTP_TRANSPORT_INFO_S;

int32_t rtp_session_create(void **rtp_session, RTP_TRANSPORT_INFO_S *info);
void rtp_session_destroy(void *rtp_session);
int32_t rtp_send_data(void *rtp_session, char *buf, int32_t len, uint64_t pts, int32_t type);


#ifdef __cplusplus
}
#endif

#endif /* _RTPUTILS_H */
