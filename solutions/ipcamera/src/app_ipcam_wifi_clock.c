#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "app_ipcam_wifi_link.h"

#define CLOCK_MIN_UTC 1577836800LL
#define CLOCK_MAX_UTC 4102444800LL
#define RFC868_UNIX_OFFSET 2208988800ULL

static uint64_t clock_monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int clock_valid(int64_t seconds)
{
    return seconds >= CLOCK_MIN_UTC && seconds < CLOCK_MAX_UTC;
}

static int clock_wait(int descriptor, short events, uint64_t deadline)
{
    for (;;) {
        uint64_t now = clock_monotonic_ms();
        if (now >= deadline)
            return ETIMEDOUT;
        struct pollfd pending = { .fd = descriptor, .events = events };
        int result = poll(&pending, 1, (int)(deadline - now));
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return result ? errno : ETIMEDOUT;
        return pending.revents & POLLNVAL ? EBADF : 0;
    }
}

static int clock_receive(const struct addrinfo *address, uint64_t deadline, uint32_t *network_time)
{
    int descriptor = socket(address->ai_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_TCP);
    if (descriptor < 0)
        return errno;
    int error = 0;
    if (connect(descriptor, address->ai_addr, address->ai_addrlen)) {
        error = errno == EINPROGRESS ? clock_wait(descriptor, POLLOUT, deadline) : errno;
        if (!error) {
            socklen_t size = sizeof(error);
            if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &error, &size))
                error = errno;
        }
    }
    size_t received = 0;
    while (!error && received < sizeof(*network_time)) {
        ssize_t length = recv(descriptor, (char *)network_time + received,
                              sizeof(*network_time) - received, 0);
        if (length > 0)
            received += (size_t)length;
        else if (!length)
            error = EPROTO;
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
            error = clock_wait(descriptor, POLLIN, deadline);
        else if (errno != EINTR)
            error = errno;
    }
    close(descriptor);
    return error;
}

int app_ipcam_Wifi_SyncClock(void)
{
    if (geteuid() != 0)
        return EXIT_FAILURE;
    if (clock_valid(time(NULL))) {
        fprintf(stderr, "[wifi-clock] existing UTC clock valid; no adjustment\n");
        return EXIT_SUCCESS;
    }
    const char *hostname = "time.nist.gov";
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM, .ai_protocol = IPPROTO_TCP };
    struct addrinfo *addresses = NULL;
    uint64_t deadline = clock_monotonic_ms() + 5000;
    int resolved = getaddrinfo(hostname, "37", &hints, &addresses);
    if (resolved) {
        fprintf(stderr, "[wifi-clock] TCP time DNS failed: host=%s error=%s\n", hostname, gai_strerror(resolved));
        return EXIT_FAILURE;
    }
    int error = EHOSTUNREACH;
    time_t candidate = 0;
    for (const struct addrinfo *address = addresses; address; address = address->ai_next) {
        if (clock_monotonic_ms() >= deadline) {
            error = ETIMEDOUT;
            break;
        }
        uint32_t network_time = 0;
        error = clock_receive(address, deadline, &network_time);
        if (error)
            continue;
        uint64_t seconds1900 = ntohl(network_time);
        if (seconds1900 < RFC868_UNIX_OFFSET ||
            seconds1900 - RFC868_UNIX_OFFSET < CLOCK_MIN_UTC) {
            error = ERANGE;
            continue;
        }
        candidate = (time_t)(seconds1900 - RFC868_UNIX_OFFSET);
        char server[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &((const struct sockaddr_in *)address->ai_addr)->sin_addr, server, sizeof(server));
        fprintf(stderr, "[wifi-clock] source=RFC868/TCP host=%s server=%s port=37 received_hex=%08x utc_seconds=%lld\n",
                hostname, server, (unsigned int)seconds1900, (long long)candidate);
        break;
    }
    freeaddrinfo(addresses);
    if (error || !clock_valid(candidate)) {
        fprintf(stderr, "[wifi-clock] no valid TCP time: errno=%d; clock unchanged\n", error ? error : ERANGE);
        return EXIT_FAILURE;
    }
    if (clock_valid(time(NULL))) {
        fprintf(stderr, "[wifi-clock] another time source synchronized UTC; no adjustment\n");
        return EXIT_SUCCESS;
    }
    struct timespec target = { .tv_sec = candidate };
    if (clock_settime(CLOCK_REALTIME, &target)) {
        fprintf(stderr, "[wifi-clock] set UTC failed: errno=%d\n", errno);
        return EXIT_FAILURE;
    }
    time_t observed = time(NULL);
    if (!clock_valid(observed) || observed < candidate || observed - candidate > 5) {
        fprintf(stderr, "[wifi-clock] UTC readback mismatch\n");
        return EXIT_FAILURE;
    }
    struct tm utc;
    char formatted[32];
    gmtime_r(&observed, &utc);
    strftime(formatted, sizeof(formatted), "%Y-%m-%dT%H:%M:%SZ", &utc);
    fprintf(stderr, "[wifi-clock] synchronized UTC=%s source=RFC868/TCP\n", formatted);
    return EXIT_SUCCESS;
}
