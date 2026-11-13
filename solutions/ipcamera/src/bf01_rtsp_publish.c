#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "bf01_rtsp_publish.h"

struct bf01_rtsp_publish {
    int descriptor;
    int codec;
    unsigned int cseq;
    unsigned int channel;
    unsigned int rtcp_channel;
    uint16_t sequence;
    uint32_t ssrc;
    uint32_t timestamp;
    uint64_t first_pts;
    int have_pts;
    uint64_t clock_pts;
    uint64_t clock_realtime_us;
    uint64_t clock_monotonic_us;
    int have_clock;
    uint64_t last_report;
    unsigned char report_header[4];
    size_t report_header_length;
    size_t report_remaining;
    uint32_t packets;
    uint32_t octets;
    char url[1600];
    char track[1632];
    char session[128];
};

static uint64_t publish_clock(void)
{
    struct timespec timestamp;
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    return (uint64_t)timestamp.tv_sec * 1000 + timestamp.tv_nsec / 1000000;
}

static int transfer(int descriptor, void *buffer, size_t length, int writing, uint64_t deadline)
{
    unsigned char *cursor = buffer;
    while (length) {
        uint64_t now = publish_clock();
        if (now >= deadline)
            return ETIMEDOUT;
        struct pollfd pending = { .fd = descriptor, .events = writing ? POLLOUT : POLLIN };
        int ready = poll(&pending, 1, (int)(deadline - now));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            return ready ? errno : ETIMEDOUT;
        ssize_t completed = writing ? send(descriptor, cursor, length, MSG_NOSIGNAL) :
            recv(descriptor, cursor, length, 0);
        if (completed < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (completed <= 0)
            return completed ? errno : ECONNRESET;
        cursor += completed;
        length -= (size_t)completed;
    }
    return 0;
}

static int connect_server(const char *url, int *descriptor)
{
    if (!url || strncmp(url, "rtsp://", 7) || strlen(url) >= 1600 || strpbrk(url, "\r\n "))
        return EINVAL;
    const char *authority = url + 7;
    const char *path = strchr(authority, '/');
    if (!path || path == authority || (size_t)(path - authority) >= 320)
        return EINVAL;
    char host[320];
    memcpy(host, authority, (size_t)(path - authority));
    host[path - authority] = 0;
    if (strchr(host, '@'))
        return ENOTSUP;
    char *port = NULL;
    char *hostname = host;
    if (*hostname == '[') {
        char *closing = strchr(hostname, ']');
        if (!closing || (closing[1] && closing[1] != ':'))
            return EINVAL;
        port = closing[1] ? closing + 2 : NULL;
        *closing = 0;
        hostname++;
    } else {
        port = strchr(hostname, ':');
        if (port)
            *port++ = 0;
    }
    if (port) {
        char *end = NULL;
        long number = strtol(port, &end, 10);
        if (!*port || *end || number < 1 || number > 65535)
            return EINVAL;
    }
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(hostname, port ? port : "554", &hints, &addresses))
        return EHOSTUNREACH;
    int error = EHOSTUNREACH;
    uint64_t deadline = publish_clock() + 2000;
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        int candidate = socket(address->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (candidate < 0) {
            error = errno;
            continue;
        }
        int connected = connect(candidate, address->ai_addr, address->ai_addrlen);
        error = connected ? errno : 0;
        if (connected && error == EINPROGRESS) {
            struct pollfd pending = { .fd = candidate, .events = POLLOUT };
            uint64_t now = publish_clock();
            int ready = now < deadline ? poll(&pending, 1, (int)(deadline - now)) : 0;
            socklen_t size = sizeof(error);
            error = ready <= 0 ? (ready ? errno : ETIMEDOUT) :
                getsockopt(candidate, SOL_SOCKET, SO_ERROR, &error, &size) ? errno : error;
        }
        if (!error) {
            int capacity = 32768;
            int enabled = 1;
            if (setsockopt(candidate, SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) ||
                setsockopt(candidate, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled))) {
                error = errno;
                close(candidate);
                break;
            }
            *descriptor = candidate;
            break;
        }
        close(candidate);
        if (publish_clock() >= deadline)
            break;
    }
    freeaddrinfo(addresses);
    return error;
}

static int header_value(const char *response, const char *name, char *value, size_t capacity)
{
    size_t name_length = strlen(name);
    const char *line = strstr(response, "\r\n");
    while (line && line[2]) {
        line += 2;
        const char *end = strstr(line, "\r\n");
        if (!end)
            break;
        if (!strncasecmp(line, name, name_length) && line[name_length] == ':') {
            line += name_length + 1;
            while (line < end && (*line == ' ' || *line == '\t'))
                line++;
            if ((size_t)(end - line) >= capacity)
                return EOVERFLOW;
            memcpy(value, line, (size_t)(end - line));
            value[end - line] = 0;
            return 0;
        }
        line = end;
    }
    return ENOENT;
}

static int finish_report(struct bf01_rtsp_publish *publisher, uint64_t deadline)
{
    if (publisher->report_header_length) {
        int error = transfer(publisher->descriptor, publisher->report_header + publisher->report_header_length,
            sizeof(publisher->report_header) - publisher->report_header_length, 0, deadline);
        if (error)
            return error;
        if (publisher->report_header[0] != '$' || publisher->report_header[1] != publisher->rtcp_channel)
            return EPROTO;
        publisher->report_remaining = ((size_t)publisher->report_header[2] << 8) |
            publisher->report_header[3];
        publisher->report_header_length = 0;
        if (publisher->report_remaining < 4)
            return EPROTO;
    }
    unsigned char discard[1024];
    while (publisher->report_remaining) {
        size_t chunk = publisher->report_remaining < sizeof(discard) ?
            publisher->report_remaining : sizeof(discard);
        int error = transfer(publisher->descriptor, discard, chunk, 0, deadline);
        if (error)
            return error;
        publisher->report_remaining -= chunk;
    }
    return 0;
}

static int request(struct bf01_rtsp_publish *publisher, const char *method,
    const char *target, const char *headers, const char *body, unsigned int timeout)
{
    char message[8192];
    char session[160] = "";
    if (*publisher->session)
        snprintf(session, sizeof(session), "Session: %s\r\n", publisher->session);
    unsigned int cseq = ++publisher->cseq;
    int count = snprintf(message, sizeof(message),
        "%s %s RTSP/1.0\r\nCSeq: %u\r\nUser-Agent: BF01-IPCamera\r\n%s%sContent-Length: %zu\r\n\r\n%s",
        method, target, cseq, session, headers ? headers : "", body ? strlen(body) : 0, body ? body : "");
    if (count < 0 || (size_t)count >= sizeof(message))
        return EOVERFLOW;
    uint64_t started = publish_clock();
    uint64_t deadline = started + timeout;
    int error = transfer(publisher->descriptor, message, (size_t)count, 1, deadline);
    if (error) {
        fprintf(stderr, "[live-rtsp] %s cseq=%u phase=send errno=%d elapsed_ms=%llu\n",
            method, cseq, error, (unsigned long long)(publish_clock() - started));
        return error;
    }
    fprintf(stderr, "[live-rtsp] %s cseq=%u phase=sent bytes=%d elapsed_ms=%llu\n",
        method, cseq, count, (unsigned long long)(publish_clock() - started));
    size_t length = 0;
    error = finish_report(publisher, deadline);
    while (!error && length + 1 < sizeof(message)) {
        error = transfer(publisher->descriptor, message + length, 1, 0, deadline);
        if (error)
            break;
        if (!length && message[0] == '$') {
            unsigned char packet[4] = {0};
            error = transfer(publisher->descriptor, packet + 1, 3, 0, deadline);
            size_t remaining = ((size_t)packet[2] << 8) | packet[3];
            while (!error && remaining) {
                size_t chunk = remaining < sizeof(message) ? remaining : sizeof(message);
                error = transfer(publisher->descriptor, message, chunk, 0, deadline);
                remaining -= chunk;
            }
            continue;
        }
        message[++length] = 0;
        if (length >= 4 && !memcmp(message + length - 4, "\r\n\r\n", 4))
            break;
    }
    if (error) {
        fprintf(stderr, "[live-rtsp] %s cseq=%u phase=response errno=%d received=%zu elapsed_ms=%llu\n",
            method, cseq, error, length, (unsigned long long)(publish_clock() - started));
        return error;
    }
    if (length + 1 >= sizeof(message))
        return EOVERFLOW;
    char value[256];
    unsigned int received_cseq = 0;
    int status = 0;
    if (sscanf(message, "RTSP/1.0 %d", &status) != 1 ||
        header_value(message, "CSeq", value, sizeof(value)) ||
        sscanf(value, "%u", &received_cseq) != 1 || received_cseq != cseq)
        return EPROTO;
    fprintf(stderr, "[live-rtsp] %s cseq=%u status=%d\n", method, cseq, status);
    if (!header_value(message, "Content-Length", value, sizeof(value))) {
        char *end = NULL;
        unsigned long remaining = strtoul(value, &end, 10);
        if (!*value || *end || remaining > 65536)
            return EPROTO;
        char discard[1024];
        while (!error && remaining) {
            size_t chunk = remaining < sizeof(discard) ? remaining : sizeof(discard);
            error = transfer(publisher->descriptor, discard, chunk, 0, deadline);
            remaining -= chunk;
        }
    }
    if (error)
        return error;
    if (status != 200)
        return status == 401 || status == 403 ? EACCES : EPROTO;
    if (!strcmp(method, "SETUP")) {
        if (header_value(message, "Session", value, sizeof(value)))
            return EPROTO;
        char *separator = strchr(value, ';');
        if (separator)
            *separator = 0;
        if (!*value || strlen(value) >= sizeof(publisher->session))
            return EPROTO;
        strcpy(publisher->session, value);
        if (header_value(message, "Transport", value, sizeof(value)) ||
            !strcasestr(value, "RTP/AVP/TCP"))
            return EPROTO;
        const char *channels = strcasestr(value, "interleaved=");
        if (!channels || sscanf(channels + 12, "%u-%u", &publisher->channel, &publisher->rtcp_channel) != 2 ||
            publisher->channel > 255 || publisher->rtcp_channel > 255 || publisher->channel == publisher->rtcp_channel)
            return EPROTO;
    }
    return 0;
}

static size_t start_code(const unsigned char *frame, size_t length, size_t offset, size_t *prefix)
{
    for (; offset + 3 <= length; offset++) {
        if (!frame[offset] && !frame[offset + 1]) {
            if (frame[offset + 2] == 1) {
                *prefix = 3;
                return offset;
            }
            if (offset + 4 <= length && !frame[offset + 2] && frame[offset + 3] == 1) {
                *prefix = 4;
                return offset;
            }
        }
    }
    *prefix = 0;
    return length;
}

int bf01_rtsp_publish_is_idr(int codec, const unsigned char *frame, size_t length)
{
    if (!frame || (codec != 264 && codec != 265))
        return 0;
    size_t prefix = 0;
    size_t offset = start_code(frame, length, 0, &prefix);
    while (offset < length) {
        size_t begin = offset + prefix;
        offset = start_code(frame, length, begin, &prefix);
        size_t size = offset - begin;
        if (codec == 264 && size > 1) {
            unsigned int type = frame[begin] & 31;
            if (type >= 1 && type <= 5)
                return type == 5;
        } else if (codec == 265 && size > 2) {
            unsigned int type = (frame[begin] >> 1) & 63;
            if (type < 32)
                return type == 19 || type == 20;
        }
    }
    return 0;
}

static int base64(const unsigned char *data, size_t length, char *encoded, size_t capacity)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t needed = (length + 2) / 3 * 4;
    if (needed + 1 > capacity)
        return EOVERFLOW;
    size_t written = 0;
    for (size_t offset = 0; offset < length; offset += 3) {
        uint32_t group = (uint32_t)data[offset] << 16;
        if (offset + 1 < length)
            group |= (uint32_t)data[offset + 1] << 8;
        if (offset + 2 < length)
            group |= data[offset + 2];
        encoded[written++] = alphabet[(group >> 18) & 63];
        encoded[written++] = alphabet[(group >> 12) & 63];
        encoded[written++] = offset + 1 < length ? alphabet[(group >> 6) & 63] : '=';
        encoded[written++] = offset + 2 < length ? alphabet[group & 63] : '=';
    }
    encoded[written] = 0;
    return 0;
}

int bf01_rtsp_publish_open(struct bf01_rtsp_publish **output, const char *url,
    int codec, unsigned int width, unsigned int height, const unsigned char *frame, size_t length, uint64_t pts)
{
    if (!output || !url || strlen(url) >= 1600 || !frame || !length || (codec != 264 && codec != 265))
        return EINVAL;
    *output = NULL;
    char track[1632];
    const char *query = strchr(url, '?');
    size_t base_length = query ? (size_t)(query - url) : strlen(url);
    snprintf(track, sizeof(track), "%.*s%strackID=0%s",
        (int)base_length, url, base_length && url[base_length - 1] == '/' ? "" : "/", query ? query : "");
    char parameters[3][1400] = {{0}};
    char profile[7] = "";
    size_t prefix = 0;
    size_t offset = start_code(frame, length, 0, &prefix);
    while (offset < length) {
        size_t begin = offset + prefix;
        offset = start_code(frame, length, begin, &prefix);
        size_t size = offset - begin;
        if (!size)
            continue;
        unsigned int type = codec == 264 ? frame[begin] & 31 : (frame[begin] >> 1) & 63;
        int index = codec == 264 ? (type == 7 ? 1 : type == 8 ? 2 : -1) :
            (type >= 32 && type <= 34 ? (int)type - 32 : -1);
        if (index >= 0) {
            int error = base64(frame + begin, size, parameters[index], sizeof(parameters[index]));
            if (error)
                return error;
            if (codec == 264 && type == 7 && size >= 4)
                snprintf(profile, sizeof(profile), "%02X%02X%02X", frame[begin + 1], frame[begin + 2], frame[begin + 3]);
        }
    }
    if (!*parameters[1] || !*parameters[2] || (codec == 265 && !*parameters[0]))
        return ENODATA;
    char format[4400];
    if (codec == 264)
        snprintf(format, sizeof(format), "a=fmtp:96 packetization-mode=1;profile-level-id=%s;sprop-parameter-sets=%s,%s\r\n",
            profile, parameters[1], parameters[2]);
    else
        snprintf(format, sizeof(format), "a=fmtp:96 sprop-vps=%s;sprop-sps=%s;sprop-pps=%s\r\n",
            parameters[0], parameters[1], parameters[2]);
    char sdp[5000];
    int count = snprintf(sdp, sizeof(sdp),
        "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=BF01 live\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
        "a=control:*\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H%d/90000\r\n%sa=framesize:96 %u-%u\r\na=control:%s\r\n",
        codec, format, width, height, track);
    if (count < 0 || (size_t)count >= sizeof(sdp))
        return EOVERFLOW;
    struct bf01_rtsp_publish *publisher = calloc(1, sizeof(*publisher));
    if (!publisher)
        return ENOMEM;
    publisher->descriptor = -1;
    publisher->codec = codec;
    uint64_t started = publish_clock();
    int error = bf01_rtsp_publish_set_clock(publisher, pts);
    if (!error)
        error = connect_server(url, &publisher->descriptor);
    fprintf(stderr, "[live-rtsp] phase=connect errno=%d elapsed_ms=%llu\n",
        error, (unsigned long long)(publish_clock() - started));
    if (!error) {
        strcpy(publisher->url, url);
        strcpy(publisher->track, track);
        int random = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (random < 0 || read(random, &publisher->ssrc, sizeof(publisher->ssrc)) != sizeof(publisher->ssrc))
            publisher->ssrc = (uint32_t)(publish_clock() ^ getpid());
        if (random >= 0)
            close(random);
        publisher->sequence = (uint16_t)publisher->ssrc;
        publisher->timestamp = publisher->ssrc;
        error = request(publisher, "ANNOUNCE", publisher->url, "Content-Type: application/sdp\r\n", sdp, 10000);
    }
    if (!error)
        error = request(publisher, "SETUP", publisher->track,
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n", NULL, 2000);
    if (!error)
        error = request(publisher, "RECORD", publisher->url, "Range: npt=0.000-\r\n", NULL, 2000);
    if (error) {
        bf01_rtsp_publish_close(publisher);
        return error;
    }
    fprintf(stderr, "[live-rtsp] keepalive=interleaved-rtcp interval_ms=5000\n");
    *output = publisher;
    return 0;
}

static int packet(struct bf01_rtsp_publish *publisher, const unsigned char *payload,
    size_t length, const unsigned char *fragment, size_t fragment_length,
    uint32_t timestamp, int marker, uint64_t deadline)
{
    unsigned char data[1440];
    size_t rtp_length = 12 + fragment_length + length;
    if (rtp_length + 4 > sizeof(data))
        return EOVERFLOW;
    data[0] = '$';
    data[1] = (unsigned char)publisher->channel;
    data[2] = (unsigned char)(rtp_length >> 8);
    data[3] = (unsigned char)rtp_length;
    data[4] = 0x80;
    data[5] = (unsigned char)(96 | (marker ? 0x80 : 0));
    uint16_t sequence = htons(publisher->sequence++);
    uint32_t network_timestamp = htonl(timestamp);
    uint32_t ssrc = htonl(publisher->ssrc);
    memcpy(data + 6, &sequence, sizeof(sequence));
    memcpy(data + 8, &network_timestamp, sizeof(network_timestamp));
    memcpy(data + 12, &ssrc, sizeof(ssrc));
    if (fragment_length)
        memcpy(data + 16, fragment, fragment_length);
    memcpy(data + 16 + fragment_length, payload, length);
    uint64_t idle_deadline = publish_clock() + 2000;
    if (idle_deadline > deadline)
        idle_deadline = deadline;
    int error = transfer(publisher->descriptor, data, rtp_length + 4, 1, idle_deadline);
    if (!error) {
        publisher->packets++;
        publisher->octets += (uint32_t)(fragment_length + length);
    }
    return error;
}

int bf01_rtsp_publish_set_clock(struct bf01_rtsp_publish *publisher, uint64_t pts)
{
    if (!publisher || publisher->have_clock || publisher->have_pts)
        return EINVAL;
    struct timespec monotonic;
    struct timespec realtime;
    if (clock_gettime(CLOCK_MONOTONIC, &monotonic) || clock_gettime(CLOCK_REALTIME, &realtime))
        return errno;
    publisher->clock_pts = pts;
    publisher->clock_monotonic_us = (uint64_t)monotonic.tv_sec * 1000000 + monotonic.tv_nsec / 1000;
    publisher->clock_realtime_us = (uint64_t)realtime.tv_sec * 1000000 + realtime.tv_nsec / 1000;
    publisher->have_clock = 1;
    return 0;
}

static uint32_t rtp_timestamp(const struct bf01_rtsp_publish *publisher, uint64_t pts)
{
    uint64_t relative = pts >= publisher->first_pts ? pts - publisher->first_pts : publisher->first_pts - pts;
    uint32_t ticks = (uint32_t)(relative / 1000 * 90 + relative % 1000 * 90 / 1000);
    return pts >= publisher->first_pts ? publisher->timestamp + ticks : publisher->timestamp - ticks;
}

static int sender_report(struct bf01_rtsp_publish *publisher)
{
    unsigned char report[48] = { '$', (unsigned char)publisher->rtcp_channel, 0, 44, 0x80, 200, 0, 6 };
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return errno;
    uint64_t monotonic_us = (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
    if (!publisher->have_clock || monotonic_us < publisher->clock_monotonic_us)
        return EINVAL;
    uint64_t elapsed_us = monotonic_us - publisher->clock_monotonic_us;
    uint64_t realtime_us = publisher->clock_realtime_us + elapsed_us;
    uint32_t timestamp = rtp_timestamp(publisher, publisher->clock_pts + elapsed_us);
    uint32_t fields[] = {
        htonl(publisher->ssrc), htonl((uint32_t)(realtime_us / 1000000) + 2208988800U),
        htonl((uint32_t)(((realtime_us % 1000000) << 32) / 1000000)),
        htonl(timestamp), htonl(publisher->packets), htonl(publisher->octets)
    };
    memcpy(report + 8, fields, sizeof(fields));
    report[32] = 0x81;
    report[33] = 202;
    report[35] = 3;
    memcpy(report + 36, fields, sizeof(uint32_t));
    report[40] = 1;
    report[41] = 4;
    memcpy(report + 42, "BF01", 4);
    return transfer(publisher->descriptor, report, sizeof(report), 1, publish_clock() + 1000);
}

static int receive_reports(struct bf01_rtsp_publish *publisher)
{
    unsigned char incoming[1024];
    size_t budget = 4096;
    while (budget) {
        size_t capacity = budget < sizeof(incoming) ? budget : sizeof(incoming);
        ssize_t received = recv(publisher->descriptor, incoming, capacity, MSG_DONTWAIT);
        if (received < 0) {
            if (errno == EINTR)
                continue;
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : errno;
        }
        if (!received)
            return ECONNRESET;
        budget -= (size_t)received;
        size_t offset = 0;
        while (offset < (size_t)received) {
            size_t available = (size_t)received - offset;
            if (publisher->report_remaining) {
                size_t chunk = available < publisher->report_remaining ? available : publisher->report_remaining;
                publisher->report_remaining -= chunk;
                offset += chunk;
                continue;
            }
            size_t needed = sizeof(publisher->report_header) - publisher->report_header_length;
            size_t chunk = available < needed ? available : needed;
            memcpy(publisher->report_header + publisher->report_header_length, incoming + offset, chunk);
            publisher->report_header_length += chunk;
            offset += chunk;
            if (publisher->report_header[0] != '$')
                return EPROTO;
            if (publisher->report_header_length < sizeof(publisher->report_header))
                continue;
            if (publisher->report_header[1] != publisher->rtcp_channel)
                return EPROTO;
            publisher->report_remaining = ((size_t)publisher->report_header[2] << 8) |
                publisher->report_header[3];
            publisher->report_header_length = 0;
            if (publisher->report_remaining < 4)
                return EPROTO;
        }
    }
    return 0;
}

int bf01_rtsp_publish_frame(struct bf01_rtsp_publish *publisher,
    const unsigned char *frame, size_t length, uint64_t pts)
{
    if (!publisher || !frame || !length)
        return EINVAL;
    int receive_error = receive_reports(publisher);
    if (receive_error) {
        fprintf(stderr, "[live-rtsp] phase=receive errno=%d\n", receive_error);
        return receive_error;
    }
    if (!publisher->have_pts) {
        if (!publisher->have_clock)
            return EINVAL;
        publisher->first_pts = pts;
        publisher->have_pts = 1;
        fprintf(stderr, "[live-rtsp] clock frame_pts=%llu sample_pts=%llu ntp_unix_us=%llu\n",
            (unsigned long long)pts, (unsigned long long)publisher->clock_pts,
            (unsigned long long)publisher->clock_realtime_us);
    }
    if (pts < publisher->first_pts)
        return EINVAL;
    uint32_t timestamp = rtp_timestamp(publisher, pts);
    size_t prefix = 0;
    size_t offset = start_code(frame, length, 0, &prefix);
    if (offset == length)
        return EPROTO;
    uint64_t started = publish_clock();
    uint64_t deadline = started + 5000;
    while (offset < length) {
        size_t begin = offset + prefix;
        offset = start_code(frame, length, begin, &prefix);
        size_t size = offset - begin;
        if (!size)
            continue;
        int error = 0;
        if (size <= 1200) {
            error = packet(publisher, frame + begin, size, NULL, 0, timestamp, offset == length, deadline);
        } else {
            size_t header_size = publisher->codec == 264 ? 1 : 2;
            if (size <= header_size)
                return EPROTO;
            for (size_t sent = header_size; !error && sent < size;) {
                size_t chunk = size - sent < 1200 ? size - sent : 1200;
                int last = sent + chunk == size;
                unsigned char fragment[3];
                if (publisher->codec == 264) {
                    fragment[0] = (frame[begin] & 0xe0) | 28;
                    fragment[1] = (frame[begin] & 31) | (sent == header_size ? 0x80 : 0) | (last ? 0x40 : 0);
                } else {
                    fragment[0] = (frame[begin] & 0x81) | (49 << 1);
                    fragment[1] = frame[begin + 1];
                    fragment[2] = ((frame[begin] >> 1) & 63) | (sent == header_size ? 0x80 : 0) | (last ? 0x40 : 0);
                }
                error = packet(publisher, frame + begin + sent, chunk, fragment, header_size + 1,
                    timestamp, last && offset == length, deadline);
                sent += chunk;
            }
        }
        if (error) {
            fprintf(stderr, "[live-rtsp] phase=rtp-send errno=%d frame_bytes=%zu elapsed_ms=%llu\n",
                error, length, (unsigned long long)(publish_clock() - started));
            return error;
        }
    }
    if (!publisher->last_report || publish_clock() - publisher->last_report >= 5000) {
        int error = sender_report(publisher);
        if (error) {
            fprintf(stderr, "[live-rtsp] phase=rtcp-send errno=%d elapsed_ms=%llu\n",
                error, (unsigned long long)(publish_clock() - started));
            return error;
        }
        publisher->last_report = publish_clock();
    }
    return 0;
}

void bf01_rtsp_publish_close(struct bf01_rtsp_publish *publisher)
{
    if (!publisher)
        return;
    if (publisher->descriptor >= 0) {
        if (*publisher->session)
            request(publisher, "TEARDOWN", publisher->url, NULL, NULL, 250);
        close(publisher->descriptor);
    }
    free(publisher);
}

void bf01_rtsp_publish_abort(struct bf01_rtsp_publish *publisher)
{
    if (!publisher)
        return;
    if (publisher->descriptor >= 0) {
        struct linger discard = { .l_onoff = 1, .l_linger = 0 };
        if (setsockopt(publisher->descriptor, SOL_SOCKET, SO_LINGER, &discard, sizeof(discard)))
            fprintf(stderr, "[live-rtsp] phase=abort errno=%d\n", errno);
        close(publisher->descriptor);
    }
    free(publisher);
}
