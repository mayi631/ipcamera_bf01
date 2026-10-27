/*
 * 网络基础封装，供 RTSP 服务端/客户端共用。
 *
 * 服务端：TCP listen/accept + TCP read/write；
 * 客户端：TCP connect（带超时）、UDP bind/read/write；
 * 结合 rtsp_client.c 完成 RTSP/RTP 连接与数据收发。
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "cvi_log.h"
#include "network.h"

char* network_sock_ntop_host(const struct sockaddr* sa, socklen_t salen, char* str, size_t len)
{
    switch (sa->sa_family) {
    case AF_INET: {
        struct sockaddr_in* sin = (struct sockaddr_in*)sa;
        if (inet_ntop(AF_INET, &sin->sin_addr, str, len) == NULL)
            return (NULL);
        return (str);
    }

    default:
        snprintf(str, len, "sock_ntop_host: unknown AF_xxx: %d, len %d",
            sa->sa_family, salen);
        return (str);
    }
    return (NULL);
}

static int32_t network_tcp_SetKeepAlive(int32_t sockfd)
{
    int32_t s32Ret = 0;
    int32_t keepalive = 1, keepidle = 3, keepinterval = 1, keepcount = 3;
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500 * 1000;

    s32Ret = setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, (void*)&keepalive, sizeof(keepalive));
    if (s32Ret < 0) {
        return s32Ret;
    }

    s32Ret = setsockopt(sockfd, SOL_TCP, TCP_KEEPIDLE, (void*)&keepidle, sizeof(keepidle));
    if (s32Ret < 0) {
        return s32Ret;
    }

    s32Ret = setsockopt(sockfd, SOL_TCP, TCP_KEEPINTVL, (void*)&keepinterval, sizeof(keepinterval));
    if (s32Ret < 0) {
        return s32Ret;
    }

    s32Ret = setsockopt(sockfd, SOL_TCP, TCP_KEEPCNT, (void*)&keepcount, sizeof(keepcount));
    if (s32Ret < 0) {
        return s32Ret;
    }

    s32Ret = setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof tv);
    if (s32Ret < 0) {
        return s32Ret;
    }

    s32Ret = setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof tv);
    if (s32Ret < 0) {
        return s32Ret;
    }

    return s32Ret;
}

int32_t network_tcp_open(unsigned short port, int32_t max_conn)
{
    int32_t f = -1;
    struct sockaddr_in s;
    int32_t v = 1;

    /*创建套接字*/
    if ((f = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
        CVI_LOGE("socket() error in tcp_listen");
        return -1;
    }

    struct linger so_linger;
    so_linger.l_onoff = 1;
    so_linger.l_linger = 0;
    setsockopt(f, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(so_linger));

    /*设置socket的可选参数 SO_REUSEPORT SO_REUSEADDR*/
    setsockopt(f, SOL_SOCKET, SO_REUSEPORT, (void*)&v, sizeof(int32_t));
    network_tcp_SetKeepAlive(f);

    s.sin_family = AF_INET;
    s.sin_addr.s_addr = htonl(INADDR_ANY);
    s.sin_port = htons(port);

    /*绑定socket*/
    if (bind(f, (struct sockaddr*)&s, sizeof(s))) {
        CVI_LOGE("bind() error in tcp_listen with %s", strerror(errno));
        return -1;
    }

    /*监听*/
    if (listen(f, max_conn) < 0) {
        CVI_LOGE("listen() error in tcp_listen.");
        return -1;
    }

    return f;
}

#if defined(RTSP_ENABLE_RECV)
int32_t network_tcp_connect(const char* ip, unsigned short port, int32_t timeout_ms)
{
    int32_t fd = -1;
    int32_t flags = 0;
    struct sockaddr_in s;
    struct timeval tv;

    if ((fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) {
        CVI_LOGE("socket() error in tcp_connect");
        return -1;
    }

    memset(&s, 0, sizeof(s));
    s.sin_family = AF_INET;
    s.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &s.sin_addr) <= 0) {
        CVI_LOGE("inet_pton failed %s", ip);
        close(fd);
        return -1;
    }

    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    if (connect(fd, (struct sockaddr*)&s, sizeof(s)) < 0) {
        if (errno != EINPROGRESS) {
            CVI_LOGE("connect error %s", strerror(errno));
            close(fd);
            return -1;
        }
    }

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(fd + 1, NULL, &wset, NULL, &tv) <= 0) {
        CVI_LOGE("connect timeout");
        close(fd);
        return -1;
    }

    int32_t err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
        CVI_LOGE("connect failed %d", err);
        close(fd);
        return -1;
    }

    return fd;
}
#endif

void network_tcp_close(int32_t fd)
{
    // shutdown(fd, SHUT_RDWR);
    close(fd);
}

int32_t network_tcp_accept(int32_t fd)
{
    int32_t f;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    addrlen = sizeof(addr);
    /*接收连接，创建一个新的socket,返回其描述符*/
    f = accept(fd, (struct sockaddr*)&addr, &addrlen);
    return f;
}

int32_t network_get_sockaddr(int32_t fd, uint32_t* port, char* ip)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr*)&addr, &addr_len) == 0) {
        inet_ntop(AF_INET, &addr.sin_addr, ip, INET_ADDRSTRLEN);
        *port = ntohs(addr.sin_port);
        return 0;
    }

    return -1;
}

int32_t network_tcp_read(int32_t fd, void* buffer, int32_t nbytes)
{
    return recv(fd, buffer, nbytes, 0);
}

int32_t network_tcp_write(int32_t fd, char* dataBuf, int32_t dataSize)
{
    int32_t actDataSize = 0;
    int32_t again_cnt = 0;
    while (dataSize > 0) {
        actDataSize = send(fd, dataBuf, dataSize, 0);
        if (actDataSize <= 0) {
            if (errno == EAGAIN && again_cnt++ < 20) {
                usleep(10 * 1000);
                continue;
            }
            break;
        }
        dataBuf += actDataSize;
        dataSize -= actDataSize;
    }

    if (dataSize > 0) {
        return -1;
    }
    return 0;
}

int32_t network_udp_open(void)
{
    return socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
}

void network_udp_close(int32_t fd)
{
    close(fd);
}

int32_t network_udp_write(int32_t fd, char* dataBuf, int32_t dataSize, void* addr, size_t alen)
{
    if (dataSize > 0) {
        int32_t again_cnt = 0;
    again:
        if (sendto(fd, dataBuf, dataSize, 0, (struct sockaddr*)addr, alen) < 0) {
            if (errno == EAGAIN && again_cnt++ < 20) {
                usleep(10 * 1000);
                goto again;
            }
            return -1;
        }
    }
    return 0;
}

int32_t network_udp_read(int32_t fd, void* buffer, int32_t nbytes, void* addr, socklen_t* addrlen)
{
    return recvfrom(fd, buffer, nbytes, 0, (struct sockaddr*)addr, addrlen);
}

int32_t network_udp_bind(int32_t fd, unsigned short port)
{
    int32_t on = 1;
    struct sockaddr_in s;
    memset(&s, 0, sizeof(s));
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = htonl(INADDR_ANY);
    s.sin_port = htons(port);

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void*)&on, sizeof(on));
    if (bind(fd, (struct sockaddr*)&s, sizeof(s)) < 0) {
        CVI_LOGE("udp bind error %s", strerror(errno));
        return -1;
    }
    return 0;
}

int32_t network_enable_broadcast(int32_t fd)
{
    int32_t on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (void*)&on, sizeof(on)) < 0) {
        return -1;
    }
    return 0;
}
