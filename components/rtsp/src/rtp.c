#include <errno.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "cvi_log.h"
#include "network.h"
#include "rtp.h"

typedef struct RTP_FIXED_HDR {
    unsigned char u4CSrcLen : 4;    /* expect 0 */
    unsigned char u1Externsion : 1; /* expect 1, see RTP_OP below */
    unsigned char u1Padding : 1;    /* expect 0 */
    unsigned char u2Version : 2;    /* expect 2 */

    unsigned char u7Payload : 7; /* RTP_PAYLOAD_RTSP */
    unsigned char u1Marker : 1;  /* expect 1 */

    unsigned short u16SeqNum;

    uint32_t u32TimeStamp;

    uint32_t u32SSrc; /* stream number is used here. */
} __attribute__((packed)) RTP_FIXED_HDR_S;

typedef struct H264_FU_INDICATOR {
    unsigned char u5Type : 5;
    unsigned char u2Nri : 2;
    unsigned char u1F : 1;
} __attribute__((packed)) H264_FU_INDICATOR_S;

typedef struct H264_FU_HDR {
    unsigned char u5Type : 5;
    unsigned char u1R : 1;
    unsigned char u1E : 1;
    unsigned char u1S : 1;
} __attribute__((packed)) H264_FU_HDR_S;

typedef struct H265_FU_INDICATOR {
    uint16_t u3Tid : 3;
    uint16_t u6Layer : 6;
    uint16_t u6Type : 6;
    uint16_t u1F : 1;
} __attribute__((packed)) H265_FU_INDICATOR_S;

typedef struct H265_FU_HDR {
    unsigned char u6Type : 6;
    unsigned char u1E : 1;
    unsigned char u1S : 1;
} H265_FU_HDR_S;

typedef struct RTP_TCP_HDR {
    unsigned char u8Magic;
    unsigned char u8Chn;
    unsigned short u16Len;
} __attribute__((packed)) RTP_TCP_HDR_S;

/* RFC 2435 JPEG main header (8B), prepended to every fragment payload */
typedef struct JPEG_MAIN_HDR {
    uint8_t u8TypeSpec; /* type-specific, always 0 */
    uint8_t u8Off2;     /* fragment offset, high byte (24-bit big-endian) */
    uint8_t u8Off1;     /* fragment offset, mid byte */
    uint8_t u8Off0;     /* fragment offset, low byte */
    uint8_t u8Type;     /* 0: 4:2:2, 1: 4:2:0 */
    uint8_t u8Q;        /* quant factor; 255 = dynamic tables (sent in first packet) */
    uint8_t u8Width;    /* width/8 (in units of 8 pixels, <=255) */
    uint8_t u8Height;   /* height/8 */
} __attribute__((packed)) JPEG_MAIN_HDR_S;

/* RFC 2435 quant-table header (4B), sent once in the first packet when Q>=128 */
typedef struct JPEG_QT_HDR {
    uint8_t u8Mbz;       /* always 0 */
    uint8_t u8Precision; /* 0: all quant tables are 8-bit */
    uint16_t u16Length;  /* total quant-table bytes, big-endian */
} __attribute__((packed)) JPEG_QT_HDR_S;

typedef struct RTP_SOCK_INFO {
    int32_t s32Sock;
    struct sockaddr_in cli_addr;
    unsigned short u16SeqNum;
    uint32_t u32TimeStampCurr;
    uint32_t u32SSrc;
    char *sendbuf;
    RTP_FIXED_HDR_S *rtp_fixed_hdr;
    H264_FU_INDICATOR_S *h264_fu_ind;
    H264_FU_HDR_S *h264_fu_hdr;
    H265_FU_INDICATOR_S *h265_fu_ind;
    H265_FU_HDR_S *h265_fu_hdr;
} RTP_SOCK_INFO_S;

typedef struct RTP_SESSION {
    RTP_SOCK_INFO_S rtp_sock;
    RTP_TRANSPORT_INFO_S transport;
    int32_t state;
} RTP_SESSION_S;

#define RTP_SENDBUFF_SIZE (MAX_RTP_PKT_LENGTH * 2)

static void rtp_pcm_le2be(char *buf, int32_t len)
{
    uint16_t *tmp = (uint16_t *)buf;
    for (int32_t i = 0; i < len / 2; i++) {
        tmp[i] = htons(tmp[i]);
    }
}

static int32_t rtp_send_pcm(RTP_SESSION_S *rtp, char *buf, int32_t len, uint64_t pts)
{
    RTP_SOCK_INFO_S *sock = &rtp->rtp_sock;
    char *pSendBuf;
    int32_t s32Bytes = 0;
    int32_t offset = 0;
    char *audioBe = NULL;
    char *tmp = NULL;
    int32_t remainLen = 0;
    RTP_TCP_HDR_S *tcp_hdr = NULL;
    pSendBuf = sock->sendbuf;
    memset(pSendBuf, 0x0, RTP_SENDBUFF_SIZE);

    int32_t overtcp = RTP_OVER_UDP;
    if (rtp->transport.type == RTP_OVER_TCP) {
        overtcp = RTP_OVER_TCP;
    }

    if (overtcp) {
        tcp_hdr = (RTP_TCP_HDR_S *)pSendBuf;
        tcp_hdr->u8Magic = '$';
        tcp_hdr->u8Chn = rtp->transport.u.tcp.interleaved.rtp;
        tcp_hdr->u16Len = 0;
        offset += sizeof(RTP_TCP_HDR_S);
    }

    sock->u32TimeStampCurr += pts;
    sock->rtp_fixed_hdr = (RTP_FIXED_HDR_S *)(pSendBuf + offset);
    sock->rtp_fixed_hdr->u7Payload = PCM_PAYLOAD_TYPE;
    sock->rtp_fixed_hdr->u2Version = 2;
    sock->rtp_fixed_hdr->u1Marker = 0;
    sock->rtp_fixed_hdr->u32SSrc = htonl(sock->u32SSrc + 1);
    sock->rtp_fixed_hdr->u32TimeStamp = htonl(sock->u32TimeStampCurr);
    offset += sizeof(RTP_FIXED_HDR_S);

    audioBe = malloc(len);
    memcpy(audioBe, buf, len);
    rtp_pcm_le2be(audioBe, len);

    remainLen = len + offset;

    if (remainLen <= MAX_RTP_PKT_LENGTH) {
        sock->rtp_fixed_hdr->u1Marker = 1;
        sock->rtp_fixed_hdr->u16SeqNum = htons(sock->u16SeqNum++);
        memcpy(pSendBuf + offset, audioBe, len);
        if (overtcp == RTP_OVER_TCP) {
            tcp_hdr->u16Len = htons(remainLen - sizeof(RTP_TCP_HDR_S));
            if (network_tcp_write(sock->s32Sock, pSendBuf, remainLen) != 0) {
                CVI_LOGE("tcp sock %d %d Failed to send!", sock->s32Sock, errno);
                free(audioBe);
                return -1;
            }
        } else {
            if (network_udp_write(sock->s32Sock, pSendBuf, remainLen, (void *)&sock->cli_addr, sizeof(sock->cli_addr)) != 0) {
                CVI_LOGE("udp sock %d %d Failed to send!", sock->s32Sock, errno);
                free(audioBe);
                return -1;
            }
        }
    } else {
        tmp = audioBe;
        remainLen -= offset;
        while (remainLen > 0) {
            sock->rtp_fixed_hdr->u16SeqNum = htons(sock->u16SeqNum++);
            sock->rtp_fixed_hdr->u1Marker = (remainLen <= MAX_RTP_PKT_LENGTH) ? 1 : 0;

            s32Bytes = (remainLen < MAX_RTP_PKT_LENGTH) ? remainLen : MAX_RTP_PKT_LENGTH;

            memcpy(pSendBuf + offset, tmp, s32Bytes);
            s32Bytes += offset;

            if (overtcp == RTP_OVER_TCP) {
                tcp_hdr->u16Len = htons(s32Bytes - sizeof(RTP_TCP_HDR_S));
                if (network_tcp_write(sock->s32Sock, pSendBuf, s32Bytes) != 0) {
                    CVI_LOGE("tcp sock %d %d Failed to send!", sock->s32Sock, errno);
                    free(audioBe);
                    return -1;
                }
            } else {
                if (network_udp_write(sock->s32Sock, pSendBuf, s32Bytes, (void *)&sock->cli_addr, sizeof(sock->cli_addr)) != 0) {
                    CVI_LOGE("udp sock %d %d Failed to send!", sock->s32Sock, errno);
                    free(audioBe);
                    return -1;
                }
            }
            tmp += MAX_RTP_PKT_LENGTH;
            remainLen -= MAX_RTP_PKT_LENGTH;
        }
    }
    if (audioBe) {
        free(audioBe);
    }
    return 0;
}

static int32_t rtp_send_h264(RTP_SESSION_S *rtp, char *buf, int32_t len, uint64_t pts)
{
    RTP_SOCK_INFO_S *sock = &rtp->rtp_sock;
    if (buf[0] == 0x0 && buf[1] == 0x0 && buf[2] == 0x0 && buf[3] == 0x01) {
        len -= 4;
        buf += 4;
    } else if (buf[0] == 0x0 && buf[1] == 0x0 && buf[2] == 0x1) {
        len -= 3;
        buf += 3;
    }

    int32_t overtcp = RTP_OVER_UDP;
    if (rtp->transport.type == RTP_OVER_TCP) {
        overtcp = RTP_OVER_TCP;
    }
    char *pNaluPayload;
    char *pSendBuf;
    int32_t s32Bytes = 0;
    int32_t offset = 0;
    char *pNaluCurr;
    int32_t s32NaluRemain;
    unsigned char u8NaluBytes;
    RTP_TCP_HDR_S *tcp_hdr = NULL;
    pSendBuf = sock->sendbuf;
    memset(pSendBuf, 0x0, RTP_SENDBUFF_SIZE);

    if (overtcp == RTP_OVER_TCP) {
        tcp_hdr = (RTP_TCP_HDR_S *)pSendBuf;
        tcp_hdr->u8Magic = '$';
        tcp_hdr->u8Chn = rtp->transport.u.tcp.interleaved.rtp;
        tcp_hdr->u16Len = 0;
        offset += sizeof(RTP_TCP_HDR_S);
    }

    sock->u32TimeStampCurr += pts;
    sock->rtp_fixed_hdr = (RTP_FIXED_HDR_S *)(pSendBuf + offset);
    sock->rtp_fixed_hdr->u7Payload = H264_PAYLOAD_TYPE;
    sock->rtp_fixed_hdr->u2Version = 2;
    sock->rtp_fixed_hdr->u1Marker = 0;
    sock->rtp_fixed_hdr->u32SSrc = htonl(sock->u32SSrc);
    sock->rtp_fixed_hdr->u32TimeStamp = htonl(sock->u32TimeStampCurr);
    offset += sizeof(RTP_FIXED_HDR_S);

    u8NaluBytes = *buf;
    pNaluCurr = buf + 1;
    s32NaluRemain = len - 1;
    if (s32NaluRemain <= MAX_RTP_PKT_LENGTH) {
        sock->rtp_fixed_hdr->u1Marker = 1;
        sock->rtp_fixed_hdr->u16SeqNum = htons(sock->u16SeqNum++);
        sock->h264_fu_ind = (H264_FU_INDICATOR_S *)(pSendBuf + offset);
        sock->h264_fu_ind->u1F = (u8NaluBytes & 0x80) >> 7;
        sock->h264_fu_ind->u2Nri = (u8NaluBytes & 0x60) >> 5;
        sock->h264_fu_ind->u5Type = u8NaluBytes & 0x1f;
        ;
        offset += sizeof(H264_FU_INDICATOR_S);

        pNaluPayload = pSendBuf + offset;
        memcpy(pNaluPayload, pNaluCurr, s32NaluRemain);
        s32Bytes = s32NaluRemain + offset;

        if (overtcp == RTP_OVER_TCP) {
            tcp_hdr->u16Len = htons(s32Bytes - sizeof(RTP_TCP_HDR_S));
            if (network_tcp_write(sock->s32Sock, pSendBuf, s32Bytes) != 0) {
                CVI_LOGE("tcp sock %d %d Failed to send!", sock->s32Sock, errno);
                return -1;
            }
        } else {
            if (network_udp_write(sock->s32Sock, pSendBuf, s32Bytes, (void *)&sock->cli_addr, sizeof(sock->cli_addr)) != 0) {
                CVI_LOGE("udp sock %d %d Failed to send!", sock->s32Sock, errno);
                return -1;
            }
        }
    } else {
        sock->h264_fu_ind = (H264_FU_INDICATOR_S *)(pSendBuf + offset);
        sock->h264_fu_ind->u1F = (u8NaluBytes & 0x80) >> 7;
        sock->h264_fu_ind->u2Nri = (u8NaluBytes & 0x60) >> 5;
        sock->h264_fu_ind->u5Type = 28;
        offset += sizeof(H264_FU_INDICATOR_S);

        sock->h264_fu_hdr = (H264_FU_HDR_S *)(pSendBuf + offset);
        sock->h264_fu_hdr->u1R = 0;
        sock->h264_fu_hdr->u5Type = u8NaluBytes & 0x1f;
        offset += sizeof(H264_FU_HDR_S);

        pNaluPayload = pSendBuf + offset;

        while (s32NaluRemain > 0) {
            sock->rtp_fixed_hdr->u16SeqNum = htons(sock->u16SeqNum++);
            sock->rtp_fixed_hdr->u1Marker = (s32NaluRemain <= MAX_RTP_PKT_LENGTH) ? 1 : 0;

            sock->h264_fu_hdr->u1E = (s32NaluRemain <= MAX_RTP_PKT_LENGTH) ? 1 : 0;
            sock->h264_fu_hdr->u1S = (s32NaluRemain == (len - 1)) ? 1 : 0;

            s32Bytes = (s32NaluRemain < MAX_RTP_PKT_LENGTH) ? s32NaluRemain : MAX_RTP_PKT_LENGTH;

            memcpy(pNaluPayload, pNaluCurr, s32Bytes);
            s32Bytes += offset;

            if (overtcp == RTP_OVER_TCP) {
                tcp_hdr->u16Len = htons(s32Bytes - sizeof(RTP_TCP_HDR_S));
                if (network_tcp_write(sock->s32Sock, pSendBuf, s32Bytes) != 0) {
                    CVI_LOGE("tcp sock %d %d Failed to send!", sock->s32Sock, errno);
                    return -1;
                }
            } else {
                if (network_udp_write(sock->s32Sock, pSendBuf, s32Bytes, (void *)&sock->cli_addr, sizeof(sock->cli_addr)) != 0) {
                    CVI_LOGE("udp sock %d %d Failed to send!", sock->s32Sock, errno);
                    return -1;
                }
            }
            pNaluCurr += MAX_RTP_PKT_LENGTH;
            s32NaluRemain -= MAX_RTP_PKT_LENGTH;
        }
    }
    return 0;
}

static int32_t rtp_send_h265(RTP_SESSION_S *rtp, char *buf, int32_t len, uint64_t pts)
{
    RTP_SOCK_INFO_S *sock = &rtp->rtp_sock;
    if (buf[0] == 0x0 && buf[1] == 0x0 && buf[2] == 0x0 && buf[3] == 0x01) {
        len -= 4;
        buf += 4;
    } else if (buf[0] == 0x0 && buf[1] == 0x0 && buf[2] == 0x1) {
        len -= 3;
        buf += 3;
    }

    int32_t overtcp = RTP_OVER_UDP;
    if (rtp->transport.type == RTP_OVER_TCP) {
        overtcp = RTP_OVER_TCP;
    }
    char *pNaluPayload;
    char *pSendBuf;
    int32_t offset = 0;
    char *pNaluCurr;
    int32_t s32Bytes = 0;
    int32_t s32NaluRemain;
    unsigned char u8NaluBytes;
    RTP_TCP_HDR_S *tcp_hdr = NULL;
    pSendBuf = sock->sendbuf;
    memset(pSendBuf, 0x0, RTP_SENDBUFF_SIZE);

    if (overtcp == RTP_OVER_TCP) {
        tcp_hdr = (RTP_TCP_HDR_S *)pSendBuf;
        tcp_hdr->u8Magic = '$';
        tcp_hdr->u8Chn = rtp->transport.u.tcp.interleaved.rtp;
        tcp_hdr->u16Len = 0;
        offset += sizeof(RTP_TCP_HDR_S);
    }

    sock->u32TimeStampCurr += pts;
    sock->rtp_fixed_hdr = (RTP_FIXED_HDR_S *)(pSendBuf + offset);
    sock->rtp_fixed_hdr->u7Payload = H265_PAYLOAD_TYPE;
    sock->rtp_fixed_hdr->u2Version = 2;
    sock->rtp_fixed_hdr->u1Marker = 0;
    sock->rtp_fixed_hdr->u32SSrc = htonl(sock->u32SSrc);
    sock->rtp_fixed_hdr->u32TimeStamp = htonl(sock->u32TimeStampCurr);
    offset += sizeof(RTP_FIXED_HDR_S);

    u8NaluBytes = *buf;
    pNaluCurr = buf + sizeof(H265_FU_INDICATOR_S);
    s32NaluRemain = len - sizeof(H265_FU_INDICATOR_S);

    if (s32NaluRemain <= MAX_RTP_PKT_LENGTH) {
        if ((0x26 == u8NaluBytes) || (0x02 == u8NaluBytes)) {
            sock->rtp_fixed_hdr->u1Marker = 1;
        } else {
            sock->rtp_fixed_hdr->u1Marker = 0;
        }
        sock->rtp_fixed_hdr->u16SeqNum = htons(sock->u16SeqNum++);
        sock->h265_fu_ind = (H265_FU_INDICATOR_S *)(pSendBuf + offset);
        sock->h265_fu_ind->u1F = (u8NaluBytes & 0x80) >> 7;
        sock->h265_fu_ind->u6Type = (u8NaluBytes >> 1) & 0x3F;
        sock->h265_fu_ind->u6Layer = 0;
        sock->h265_fu_ind->u3Tid = 0;
        offset += sizeof(H265_FU_INDICATOR_S);

        pNaluPayload = pSendBuf + offset;
        memcpy(pNaluPayload, pNaluCurr, s32NaluRemain);
        s32Bytes = s32NaluRemain + offset;

        if (overtcp == RTP_OVER_TCP) {
            tcp_hdr->u16Len = htons(s32Bytes - sizeof(RTP_TCP_HDR_S));
            if (network_tcp_write(sock->s32Sock, pSendBuf, s32Bytes) != 0) {
                CVI_LOGE("tcp sock %d %d Failed to send!", sock->s32Sock, errno);
                return -1;
            }
        } else {
            if (network_udp_write(sock->s32Sock, pSendBuf, s32Bytes, (void *)&sock->cli_addr, sizeof(sock->cli_addr)) != 0) {
                CVI_LOGE("udp sock %d %d Failed to send!", sock->s32Sock, errno);
                return -1;
            }
        }
    } else {
        sock->h265_fu_ind = (H265_FU_INDICATOR_S *)(pSendBuf + offset);
        *((char *)sock->h265_fu_ind) = 1;
        *((char *)sock->h265_fu_ind + 1) = 49 << 1;

        *(uint16_t *)sock->h265_fu_ind = htons(*(uint16_t *)sock->h265_fu_ind);
        offset += sizeof(H265_FU_INDICATOR_S);

        sock->h265_fu_hdr = (H265_FU_HDR_S *)(pSendBuf + offset);
        sock->h265_fu_hdr->u6Type = (u8NaluBytes >> 1) & 0x3F;
        offset += sizeof(H265_FU_HDR_S);

        pNaluPayload = pSendBuf + offset;

        while (s32NaluRemain > 0) {
            sock->rtp_fixed_hdr->u16SeqNum = htons(sock->u16SeqNum++);
            sock->rtp_fixed_hdr->u1Marker = (s32NaluRemain <= MAX_RTP_PKT_LENGTH) ? 1 : 0;

            sock->h265_fu_hdr->u1E = (s32NaluRemain <= MAX_RTP_PKT_LENGTH) ? 1 : 0;
            sock->h265_fu_hdr->u1S = (s32NaluRemain == (int32_t)(len - sizeof(H265_FU_INDICATOR_S))) ? 1 : 0;

            s32Bytes = (s32NaluRemain < MAX_RTP_PKT_LENGTH) ? s32NaluRemain : MAX_RTP_PKT_LENGTH;

            memcpy(pNaluPayload, pNaluCurr, s32Bytes);
            s32Bytes += offset;

            if (overtcp == RTP_OVER_TCP) {
                tcp_hdr->u16Len = htons(s32Bytes - sizeof(RTP_TCP_HDR_S));
                if (network_tcp_write(sock->s32Sock, pSendBuf, s32Bytes) != 0) {
                    CVI_LOGE("tcp sock %d %d Failed to send!", sock->s32Sock, errno);
                    return -1;
                }
            } else {
                if (network_udp_write(sock->s32Sock, pSendBuf, s32Bytes, (void *)&sock->cli_addr, sizeof(sock->cli_addr)) != 0) {
                    CVI_LOGE("udp sock %d %d Failed to send!", sock->s32Sock, errno);
                    return -1;
                }
            }
            pNaluCurr += MAX_RTP_PKT_LENGTH;
            s32NaluRemain -= MAX_RTP_PKT_LENGTH;
        }
    }

    return 0;
}

/* RFC 2435 JPEG/RTP packetizer. Parses SOF0 (dimensions), DQT (quant tables)
 * and locates the scan data after SOS, builds the 8B JPEG main header (plus a
 * 4B quant-table header + tables in the first packet), strips the JFIF header
 * and fragments the scan data into 1400B RTP packets. */
static int32_t rtp_send_mjpeg(RTP_SESSION_S *rtp, char *buf, int32_t len, uint64_t pts)
{
    RTP_SOCK_INFO_S *sock = &rtp->rtp_sock;
    uint8_t *jpg = (uint8_t *)buf;
    int32_t width = 0, height = 0;
    int32_t jpeg_type = 0; /* 0:4:2:2  1:4:2:0 */
    uint8_t qtable[128];   /* up to two 8-bit quant tables (64*2) */
    int32_t qtlen = 0;
    int32_t scan_off = -1; /* offset of scan data (after SOS) in the stream */
    int32_t i = 0;

    /* Parse JPEG markers: read SOF0 (dimensions/sampling), DQT (quant tables), locate scan data after SOS */
    if (len < 2 || jpg[0] != 0xFF || jpg[1] != 0xD8) {
        CVI_LOGE("mjpeg: no SOI marker (head=%02x %02x), drop frame", jpg[0], jpg[1]);
        return -1;
    }
    i = 2;
    while (i + 4 < len) {
        if (jpg[i] != 0xFF) {
            i++;
            continue;
        }
        uint8_t marker = jpg[i + 1];
        /* Skip 0xFF fill bytes before a marker. This HW encoder emits FF FF FF
         * padding between SOF0 and SOS; without this the FF FF is misread as a
         * segment length of 0xFFFF and the whole scan is skipped. */
        if (marker == 0xFF) {
            i++;
            continue;
        }
        if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        int32_t seglen = (jpg[i + 2] << 8) | jpg[i + 3];
        if (seglen < 2 || i + 2 + seglen > len) {
            break;
        }
        if (marker == 0xDB) { /* DQT: collect quant tables (drop the 1B Pq/Tq prefix) */
            int32_t p = i + 4;
            int32_t segend = i + 2 + seglen;
            while (p < segend && qtlen + 64 <= (int32_t)sizeof(qtable)) {
                p++; /* skip precision/table-id byte, take 8-bit table only */
                memcpy(qtable + qtlen, jpg + p, 64);
                qtlen += 64;
                p += 64;
            }
        } else if (marker == 0xC0 || marker == 0xC1) { /* SOF0/SOF1: dimensions and chroma sampling */
            height = (jpg[i + 5] << 8) | jpg[i + 6];
            width = (jpg[i + 7] << 8) | jpg[i + 8];
            /* component-1 sampling factor: 0x22->4:2:0, 0x21->4:2:2 */
            uint8_t samp = jpg[i + 11];
            jpeg_type = (samp == 0x22) ? 1 : 0;
        } else if (marker == 0xDA) { /* SOS: scan data follows */
            scan_off = i + 2 + seglen;
            break;
        }
        i += 2 + seglen;
    }

    if (scan_off < 0 || width == 0 || height == 0 || qtlen == 0) {
        CVI_LOGE("mjpeg: parse fail w=%d h=%d qt=%d scan=%d, drop", width, height, qtlen, scan_off);
        return -1;
    }
    if ((width / 8) > 255 || (height / 8) > 255) {
        CVI_LOGE("mjpeg: %dx%d exceeds RFC2435 8160 limit, drop", width, height);
        return -1;
    }

    /* scan length: up to EOI(FFD9); if no explicit EOI, use frame end */
    int32_t scan_len = len - scan_off;
    if (scan_len >= 2 && jpg[len - 2] == 0xFF && jpg[len - 1] == 0xD9) {
        scan_len -= 2;
    }
    uint8_t *pScan = jpg + scan_off;

    int32_t overtcp = RTP_OVER_UDP;
    if (rtp->transport.type == RTP_OVER_TCP) {
        overtcp = RTP_OVER_TCP;
    }

    sock->u32TimeStampCurr += pts;

    int32_t frag_off = 0;
    while (scan_len > 0) {
        char *pSendBuf = sock->sendbuf;
        int32_t offset = 0;
        RTP_TCP_HDR_S *tcp_hdr = NULL;
        memset(pSendBuf, 0x0, RTP_SENDBUFF_SIZE);

        if (overtcp == RTP_OVER_TCP) {
            tcp_hdr = (RTP_TCP_HDR_S *)pSendBuf;
            tcp_hdr->u8Magic = '$';
            tcp_hdr->u8Chn = rtp->transport.u.tcp.interleaved.rtp;
            tcp_hdr->u16Len = 0;
            offset += sizeof(RTP_TCP_HDR_S);
        }

        /* RTP fixed header */
        sock->rtp_fixed_hdr = (RTP_FIXED_HDR_S *)(pSendBuf + offset);
        sock->rtp_fixed_hdr->u7Payload = MJPEG_PAYLOAD_TYPE;
        sock->rtp_fixed_hdr->u2Version = 2;
        sock->rtp_fixed_hdr->u1Marker = 0;
        sock->rtp_fixed_hdr->u32SSrc = htonl(sock->u32SSrc);
        sock->rtp_fixed_hdr->u32TimeStamp = htonl(sock->u32TimeStampCurr);
        sock->rtp_fixed_hdr->u16SeqNum = htons(sock->u16SeqNum++);
        offset += sizeof(RTP_FIXED_HDR_S);

        /* JPEG 8B main header (Q=255 means dynamic quant tables) */
        JPEG_MAIN_HDR_S *jhdr = (JPEG_MAIN_HDR_S *)(pSendBuf + offset);
        jhdr->u8TypeSpec = 0;
        jhdr->u8Off2 = (frag_off >> 16) & 0xFF;
        jhdr->u8Off1 = (frag_off >> 8) & 0xFF;
        jhdr->u8Off0 = frag_off & 0xFF;
        jhdr->u8Type = jpeg_type;
        jhdr->u8Q = 255;
        jhdr->u8Width = width / 8;
        jhdr->u8Height = height / 8;
        offset += sizeof(JPEG_MAIN_HDR_S);

        /* first packet carries the quant-table header + tables */
        if (frag_off == 0) {
            JPEG_QT_HDR_S *qthdr = (JPEG_QT_HDR_S *)(pSendBuf + offset);
            qthdr->u8Mbz = 0;
            qthdr->u8Precision = 0;
            qthdr->u16Length = htons(qtlen);
            offset += sizeof(JPEG_QT_HDR_S);
            memcpy(pSendBuf + offset, qtable, qtlen);
            offset += qtlen;
        }

        int32_t payload = MAX_RTP_PKT_LENGTH - offset;
        if (payload > scan_len) {
            payload = scan_len;
        }
        if (scan_len - payload <= 0) {
            sock->rtp_fixed_hdr->u1Marker = 1; /* last packet of the frame */
        }
        memcpy(pSendBuf + offset, pScan + frag_off, payload);
        int32_t s32Bytes = offset + payload;

        if (overtcp == RTP_OVER_TCP) {
            tcp_hdr->u16Len = htons(s32Bytes - sizeof(RTP_TCP_HDR_S));
            if (network_tcp_write(sock->s32Sock, pSendBuf, s32Bytes) != 0) {
                CVI_LOGE("tcp sock %d %d Failed to send!", sock->s32Sock, errno);
                return -1;
            }
        } else {
            if (network_udp_write(sock->s32Sock, pSendBuf, s32Bytes, (void *)&sock->cli_addr, sizeof(sock->cli_addr)) != 0) {
                CVI_LOGE("udp sock %d %d Failed to send!", sock->s32Sock, errno);
                return -1;
            }
        }
        frag_off += payload;
        scan_len -= payload;
    }
    return 0;
}

int32_t rtp_send_data(void *rtp_session, char *buf, int32_t len, uint64_t pts, int32_t type)
{
    RTP_SESSION_S *rtp = (RTP_SESSION_S *)rtp_session;
    int32_t res = 0;
    if (rtp) {
        switch (type) {
        case H264_PAYLOAD_TYPE:
            res = rtp_send_h264(rtp, buf, len, pts);
            break;

        case H265_PAYLOAD_TYPE:
            res = rtp_send_h265(rtp, buf, len, pts);
            break;

        /* MJPEG (payload 26): dispatch to the RFC 2435 packetizer */
        case MJPEG_PAYLOAD_TYPE:
            res = rtp_send_mjpeg(rtp, buf, len, pts);
            break;

        case PCM_PAYLOAD_TYPE:
            res = rtp_send_pcm(rtp, buf, len, pts);
            break;

        default:
            break;
        }
    }
    return res;
}

int32_t rtp_session_create(void **rtp_session, RTP_TRANSPORT_INFO_S *info)
{
    RTP_SESSION_S *rtp = (RTP_SESSION_S *)malloc(sizeof(RTP_SESSION_S));
    if (rtp == NULL) {
        CVI_LOGE("rtp malloc failed");
        return -1;
    }
    memset(rtp, 0, sizeof(RTP_SESSION_S));
    if (info->type == RTP_OVER_UDP) {
        if ((rtp->rtp_sock.s32Sock = network_udp_open()) < 0) {
            CVI_LOGE("Failed to open udp socket");
            free(rtp);
            return -1;
        }

        if (0xFF000000 == (inet_addr(info->u.udp.cli_ip) & 0xFF000000)) {
            network_enable_broadcast(rtp->rtp_sock.s32Sock);
        }
        rtp->rtp_sock.cli_addr.sin_port = htons(info->u.udp.cli_ports.rtp);
    } else {
        rtp->rtp_sock.s32Sock = info->u.tcp.fd;
    }

    rtp->rtp_sock.cli_addr.sin_family = AF_INET;
    rtp->rtp_sock.cli_addr.sin_addr.s_addr = inet_addr(info->u.udp.cli_ip);
    bzero(&(rtp->rtp_sock.cli_addr.sin_zero), 8);
    rtp->rtp_sock.u16SeqNum = 0;
    rtp->rtp_sock.u32TimeStampCurr = 0;
    srand(time(0));
    rtp->rtp_sock.u32SSrc = rand();
    memcpy(&rtp->transport, info, sizeof(RTP_TRANSPORT_INFO_S));
    rtp->rtp_sock.sendbuf = (char *)malloc(RTP_SENDBUFF_SIZE);
    if (rtp->rtp_sock.sendbuf == NULL) {
        if (info->type == RTP_OVER_UDP) {
            network_udp_close(rtp->rtp_sock.s32Sock);
        }
        free(rtp);
        return -1;
    }
    rtp->state = 0;
    *rtp_session = rtp;
    return 0;
}

void rtp_session_destroy(void *rtp_session)
{
    RTP_SESSION_S *rtp = (RTP_SESSION_S *)rtp_session;
    if (rtp) {
        CVI_LOGD("rtp_session_destroy %d start", rtp->transport.type);
        if (rtp->transport.type == RTP_OVER_UDP) {
            network_udp_close(rtp->rtp_sock.s32Sock);
        }
        if (rtp->rtp_sock.sendbuf) {
            free(rtp->rtp_sock.sendbuf);
        }
        CVI_LOGD("rtp_session_destroy %d end", rtp->transport.type);
        free(rtp);
    }
}
