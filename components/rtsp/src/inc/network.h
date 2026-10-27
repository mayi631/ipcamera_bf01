#ifndef _RTSP_NET_H
#define _RTSP_NET_H

#ifdef __cplusplus
extern "C" {
#endif
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

int32_t network_tcp_write(int32_t fd, char *dataBuf, int32_t dataSize);
int32_t network_tcp_read(int32_t fd, void *buffer, int32_t nbytes);
int32_t network_tcp_accept(int32_t fd);
void network_tcp_close(int32_t fd);
int32_t network_tcp_open(unsigned short port, int32_t max_conn);
#if defined(RTSP_ENABLE_RECV)
int32_t network_tcp_connect(const char *ip, unsigned short port, int32_t timeout_ms);
#endif
char *network_sock_ntop_host(const struct sockaddr *sa, socklen_t salen, char *str, size_t len);

int32_t network_udp_open(void);
void network_udp_close(int32_t fd);
int32_t network_udp_write(int32_t fd, char *dataBuf, int32_t dataSize, void *addr, size_t alen);
int32_t network_udp_read(int32_t fd, void *buffer, int32_t nbytes, void *addr, socklen_t *addrlen);
int32_t network_udp_bind(int32_t fd, unsigned short port);
int32_t network_enable_broadcast(int32_t fd);
int32_t network_get_sockaddr(int32_t fd, uint32_t *port, char *ip);

#ifdef __cplusplus
}
#endif
#endif
