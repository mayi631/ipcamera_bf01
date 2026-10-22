#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "bf01_preview.h"
#include "cJSON.h"

#define PREVIEW_CLIENTS 16
#define REQUEST_SIZE 4096
#define RESPONSE_SIZE 8192

struct preview_client {
    int fd;
    int kind;
    size_t received;
    size_t queued;
    size_t sent;
    long long deadline;
    long long heartbeat;
    char request[REQUEST_SIZE];
    char response[RESPONSE_SIZE];
};

static struct bf01_preview_config settings;
static char interface_name[IFNAMSIZ];
static int listeners[4] = {-1, -1, -1, -1};
static struct preview_client clients[PREVIEW_CLIENTS];
static pthread_t service_thread;
static int started;
static int clients_initialized;
static atomic_int stopping;
static struct sockaddr_in last_peer;

static long long milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void close_client(struct preview_client *client)
{
    if (client->fd >= 0)
        close(client->fd);
    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

static int interface_info(struct in_addr *address, struct in_addr *mask, char *mac)
{
    struct ifreq request;
    int socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int result = -1;
    if (socket_fd < 0)
        return -1;
    memset(&request, 0, sizeof(request));
    snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", interface_name);
    if (ioctl(socket_fd, SIOCGIFADDR, &request) != 0)
        goto done;
    *address = ((struct sockaddr_in *)&request.ifr_addr)->sin_addr;
    if (ioctl(socket_fd, SIOCGIFNETMASK, &request) != 0)
        goto done;
    *mask = ((struct sockaddr_in *)&request.ifr_netmask)->sin_addr;
    if (ioctl(socket_fd, SIOCGIFHWADDR, &request) != 0)
        goto done;
    if (mac) {
        unsigned char *bytes = (unsigned char *)request.ifr_hwaddr.sa_data;
        snprintf(mac, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
            bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    }
    result = address->s_addr != INADDR_ANY ? 0 : -1;
done:
    close(socket_fd);
    return result;
}

static int open_listener(unsigned short port, int datagram)
{
    struct sockaddr_in local;
    int enabled = 1;
    int socket_fd = socket(AF_INET,
        (datagram ? SOCK_DGRAM : SOCK_STREAM) | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (socket_fd < 0)
        return -1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    if (setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE,
            interface_name, strlen(interface_name) + 1) != 0)
        goto fail;
    if (datagram && setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST,
            &enabled, sizeof(enabled)) != 0)
        goto fail;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    if (bind(socket_fd, (struct sockaddr *)&local, sizeof(local)) != 0 ||
        (!datagram && listen(socket_fd, 8) != 0))
        goto fail;
    return socket_fd;
fail:
    close(socket_fd);
    return -1;
}

static void queue_json(struct preview_client *client, const char *body)
{
    int length = snprintf(client->response, sizeof(client->response),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n%s",
        strlen(body), body);
    if (length < 0 || (size_t)length >= sizeof(client->response)) {
        close_client(client);
        return;
    }
    client->queued = (size_t)length;
    client->sent = 0;
    client->deadline = milliseconds() + 3000;
}

static int query_value(const char *query, const char *name, const char *value)
{
    size_t name_length = strlen(name);
    size_t value_length = strlen(value);
    while (*query) {
        const char *end = strchr(query, '&');
        size_t length = end ? (size_t)(end - query) : strlen(query);
        if (length == name_length + value_length + 1 &&
            !strncasecmp(query, name, name_length) && query[name_length] == '=' &&
            !strncasecmp(query + name_length + 1, value, value_length))
            return 1;
        if (!end)
            break;
        query = end + 1;
    }
    return 0;
}

static void handle_http(struct preview_client *client, int video_ready)
{
    char method[8];
    char target[1024];
    char version[16];
    char *query;
    const char *command;
    struct in_addr address;
    struct in_addr mask;
    char ip[INET_ADDRSTRLEN];
    char ssid[34] = "";
    const char *ssid_file;
    char mac[18];
    char url[128];
    char *body;
    cJSON *root;
    cJSON *info;
    if (sscanf(client->request, "%7s %1023s %15s", method, target, version) != 3 ||
        strcmp(method, "GET") || strncmp(version, "HTTP/1.", 7)) {
        queue_json(client, "{\"result\":-1,\"info\":\"GET required\"}");
        return;
    }
    query = strchr(target, '?');
    if (query)
        *query++ = '\0';
    else
        query = "";
    fprintf(stderr, "[preview] GET %s\n", target);
    if (!strcmp(target, "/Misc")) {
        if (!strcmp(query, "Info") || !strcmp(query, "GetInfo"))
            queue_json(client, "{\"Result\":0,\"ProductName\":\"SOPHCAM\",\"ModelName\":\"BF01\","
                "\"ChipID\":\"CV1815JA\",\"ProtocolVer\":1,\"WifiChip\":\"ATBM6461\"}");
        else if (!strcmp(query, "AliveAck"))
            queue_json(client, "{\"Result\":0}");
        else if (!strcmp(query, "GetCurrentWorkMode"))
            queue_json(client, "{\"Result\":0,\"Mode\":\"VideoCapture\"}");
        else
            queue_json(client, "{\"Result\":-1,\"Info\":\"preview only\"}");
        return;
    }
    if (!strcmp(target, "/SetMode")) {
        if (!strcmp(query, "VideoCapture") && video_ready) {
            queue_json(client, "{\"Result\":0,\"Mode\":\"VideoCapture\"}");
        } else {
            queue_json(client, "{\"Result\":-1}");
        }
        return;
    }
    if (!strcmp(target, "/health")) {
        queue_json(client, video_ready ? "{\"result\":0,\"video_ready\":true}" :
            "{\"result\":-1,\"video_ready\":false}");
        return;
    }
    if (strncmp(target, "/app/", 5)) {
        queue_json(client, "{\"result\":-1,\"info\":\"unsupported; live preview only\"}");
        return;
    }
    command = target + 5;
    if (!strcasecmp(command, "getproductinfo")) {
        queue_json(client, "{\"result\":0,\"info\":{\"model\":\"cardv\","
            "\"company\":\"sophgo\",\"soc\":\"sophgo\",\"sp\":\"cv\"}}");
    } else if (!strcasecmp(command, "capability")) {
        queue_json(client, "{\"result\":0,\"info\":{\"format\":0,\"reset\":0,"
            "\"rec_resolution\":0,\"image_size\":0,\"photo_resolution\":0,\"wifi\":0,"
            "\"ssid\":0,\"password\":0,\"wifireboot\":0,\"support_change_wifi_name_pwd\":0,"
            "\"value\":\"00000000300000100\"}}");
    } else if (!strcasecmp(command, "getcamerainfo")) {
        queue_json(client, "{\"result\":0,\"info\":{\"camnum\":1,\"online\":\"0\",\"curcamid\":0}}");
    } else if (!strcasecmp(command, "getsdinfo") || !strcasecmp(command, "getstorageinfo")) {
        queue_json(client, "{\"result\":0,\"info\":{\"status\":12}}");
    } else if (!strcasecmp(command, "getparamitems")) {
        queue_json(client, "{\"result\":0,\"info\":[]}");
    } else if (!strcasecmp(command, "playstate") &&
        (query_value(query, "state", "0") || query_value(query, "state", "1"))) {
        queue_json(client, "{\"result\":0,\"info\":\"preview navigation acknowledged\"}");
    } else if (!strcasecmp(command, "getmediainfo") || !strcasecmp(command, "getdeviceattr")) {
        if (interface_info(&address, &mask, mac) != 0 ||
            (!strcasecmp(command, "getmediainfo") && !video_ready)) {
            queue_json(client, "{\"result\":-1,\"info\":\"preview not ready\"}");
            return;
        }
        inet_ntop(AF_INET, &address, ip, sizeof(ip));
        root = cJSON_CreateObject();
        if (!root) {
            close_client(client);
            return;
        }
        cJSON_AddNumberToObject(root, "result", 0);
        info = cJSON_AddObjectToObject(root, "info");
        if (!info) {
            cJSON_Delete(root);
            close_client(client);
            return;
        }
        if (!strcasecmp(command, "getmediainfo")) {
            snprintf(url, sizeof(url), "rtsp://%s:%u/live%u", ip, settings.rtsp_port, settings.session);
            cJSON_AddStringToObject(info, "rtsp", url);
            cJSON_AddStringToObject(info, "transport", "tcp");
            cJSON_AddNumberToObject(info, "port", 6035);
            cJSON_AddNumberToObject(info, "page", 1);
            cJSON_AddNumberToObject(info, "rectime", 0);
        } else {
            ssid_file = getenv("BF01_PREVIEW_SSID_FILE");
            FILE *file = ssid_file ? fopen(ssid_file, "r") : NULL;
            if (file) {
                if (fgets(ssid, sizeof(ssid), file))
                    ssid[strcspn(ssid, "\r\n")] = '\0';
                fclose(file);
            }
            cJSON_AddStringToObject(info, "uuid", mac);
            cJSON_AddStringToObject(info, "bssid", mac);
            cJSON_AddStringToObject(info, "ssid", ssid);
            cJSON_AddStringToObject(info, "softver", "BF01-preview-1");
            cJSON_AddStringToObject(info, "hwver", "BF01");
            cJSON_AddNumberToObject(info, "camnum", 1);
            cJSON_AddNumberToObject(info, "curcamid", 0);
        }
        body = cJSON_PrintUnformatted(root);
        if (body) {
            queue_json(client, body);
            free(body);
        } else {
            close_client(client);
        }
        cJSON_Delete(root);
    } else {
        queue_json(client, "{\"result\":-1,\"info\":\"unsupported; live preview only\"}");
    }
}

static void accept_client(int kind)
{
    struct sockaddr_in peer;
    socklen_t peer_size = sizeof(peer);
    int accepted = accept4(listeners[kind], (struct sockaddr *)&peer,
        &peer_size, SOCK_NONBLOCK | SOCK_CLOEXEC);
    int enabled = 1;
    int idle = 15;
    int interval = 5;
    int probes = 3;
    int index;
    if (accepted < 0)
        return;
    for (index = 0; index < PREVIEW_CLIENTS; index++) {
        if (clients[index].fd < 0)
            break;
    }
    if (index == PREVIEW_CLIENTS) {
        close(accepted);
        return;
    }
    setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    setsockopt(accepted, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
    setsockopt(accepted, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(accepted, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(accepted, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof(probes));
    clients[index].fd = accepted;
    clients[index].kind = kind;
    clients[index].deadline = milliseconds() + (kind ? 60000 : 3000);
    last_peer = peer;
    last_peer.sin_port = htons(49142);
}

static void discovery_send(void)
{
    static const char message[] = "CAM_NUM_CHANGE = 1\r\nSD_STATUS = OFF\r\n";
    struct in_addr address;
    struct in_addr mask;
    struct sockaddr_in destination;
    if (interface_info(&address, &mask, NULL) != 0)
        return;
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(49142);
    destination.sin_addr.s_addr = address.s_addr | ~mask.s_addr;
    sendto(listeners[3], message, sizeof(message) - 1, MSG_DONTWAIT,
        (struct sockaddr *)&destination, sizeof(destination));
    if (last_peer.sin_family == AF_INET &&
        (last_peer.sin_addr.s_addr & mask.s_addr) == (address.s_addr & mask.s_addr))
        sendto(listeners[3], message, sizeof(message) - 1, MSG_DONTWAIT,
            (struct sockaddr *)&last_peer, sizeof(last_peer));
}

static void service_client(struct preview_client *client, short events, int ready)
{
    ssize_t length;
    long long now = milliseconds();
    if (events & (POLLERR | POLLNVAL)) {
        close_client(client);
        return;
    }
    if (events & POLLIN) {
        if (!client->kind && !client->queued) {
            length = recv(client->fd, client->request + client->received,
                sizeof(client->request) - client->received - 1, 0);
            if (length > 0) {
                client->received += (size_t)length;
                client->request[client->received] = '\0';
                if (memchr(client->request, '\0', client->received)) {
                    close_client(client);
                    return;
                }
                if (strstr(client->request, "\r\n\r\n"))
                    handle_http(client, ready);
                else if (client->received == sizeof(client->request) - 1)
                    close_client(client);
            } else if (!length || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
                close_client(client);
            }
        } else {
            char discarded[512];
            length = recv(client->fd, discarded, sizeof(discarded), 0);
            if (!length || (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                close_client(client);
        }
    }
    if (client->fd < 0)
        return;
    if (client->kind == 1 && now >= client->heartbeat && !client->queued) {
        int size = snprintf(client->response, sizeof(client->response),
            "{\"msgid\":\"sd\",\"info\":{\"status\":12},\"time\":%lld}"
            "{\"msgid\":\"cam_plugin\",\"info\":{\"action\":0,\"curcamid\":0,\"camnum\":1},\"time\":%lld}",
            (long long)time(NULL), (long long)time(NULL));
        client->queued = (size_t)size;
        client->heartbeat = now + 1500;
        client->deadline = now + 5000;
    }
    if (client->queued && (events & POLLOUT)) {
        length = send(client->fd, client->response + client->sent,
            client->queued - client->sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (length > 0) {
            client->sent += (size_t)length;
            if (client->sent == client->queued) {
                if (!client->kind) {
                    close_client(client);
                    return;
                }
                client->queued = client->sent = 0;
            }
        } else if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close_client(client);
            return;
        }
    }
    if ((events & POLLHUP) ||
        ((!client->kind || client->queued) && now >= client->deadline))
        close_client(client);
}

static void *service_loop(void *opaque)
{
    struct pollfd polling[4 + PREVIEW_CLIENTS];
    long long next_discovery = 0;
    (void)opaque;
    while (!atomic_load(&stopping)) {
        int index;
        int result;
        int ready = settings.video_ready(settings.opaque);
        for (index = 0; index < 4 + PREVIEW_CLIENTS; index++) {
            polling[index].fd = index < 4 ? listeners[index] : clients[index - 4].fd;
            polling[index].events = POLLIN;
            polling[index].revents = 0;
            if (index >= 4 && clients[index - 4].queued)
                polling[index].events |= POLLOUT;
        }
        result = poll(polling, 4 + PREVIEW_CLIENTS, 40);
        if (result < 0 && errno != EINTR)
            break;
        for (index = 0; index < PREVIEW_CLIENTS; index++) {
            if (clients[index].fd >= 0)
                service_client(&clients[index], polling[index + 4].revents, ready);
        }
        for (index = 0; index < 3; index++) {
            if (polling[index].revents & POLLIN)
                accept_client(index);
        }
        if (polling[3].revents & POLLIN) {
            char request[256];
            struct sockaddr_in sender;
            socklen_t size = sizeof(sender);
            if (recvfrom(listeners[3], request, sizeof(request), 0,
                    (struct sockaddr *)&sender, &size) > 0)
                last_peer = sender;
            last_peer.sin_port = htons(49142);
        }
        if (milliseconds() >= next_discovery) {
            discovery_send();
            next_discovery = milliseconds() + 1500;
        }
    }
    return NULL;
}

int bf01_preview_start(const struct bf01_preview_config *config)
{
    unsigned short ports[4];
    int index;
    int result;
    sigset_t blocked;
    sigset_t previous;
    if (started || !config || !config->video_ready || !config->interface_name ||
        strlen(config->interface_name) >= sizeof(interface_name) ||
        !config->http_port || !config->rtsp_port)
        return -1;
    settings = *config;
    snprintf(interface_name, sizeof(interface_name), "%s", config->interface_name);
    ports[0] = config->http_port;
    ports[1] = 6035;
    ports[2] = 8222;
    ports[3] = 49142;
    memset(&last_peer, 0, sizeof(last_peer));
    memset(clients, 0, sizeof(clients));
    for (index = 0; index < PREVIEW_CLIENTS; index++)
        clients[index].fd = -1;
    clients_initialized = 1;
    for (index = 0; index < 4; index++) {
        listeners[index] = open_listener(ports[index], index == 3);
        if (listeners[index] < 0) {
            fprintf(stderr, "[preview] cannot bind %s port %u: %s\n",
                interface_name, ports[index], strerror(errno));
            bf01_preview_stop();
            return -1;
        }
    }
    atomic_store(&stopping, 0);
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0) {
        bf01_preview_stop();
        return -1;
    }
    result = pthread_create(&service_thread, NULL, service_loop, NULL);
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (result != 0) {
        bf01_preview_stop();
        return -1;
    }
    started = 1;
    fprintf(stderr, "[preview] live-only HTTP:%u APP:6035 event:8222 UDP:49142 RTSP:%u/live%u\n",
        config->http_port, config->rtsp_port, config->session);
    return 0;
}

void bf01_preview_stop(void)
{
    int index;
    atomic_store(&stopping, 1);
    if (started) {
        pthread_join(service_thread, NULL);
        started = 0;
    }
    for (index = 0; index < 4; index++) {
        if (listeners[index] >= 0)
            close(listeners[index]);
        listeners[index] = -1;
    }
    for (index = 0; clients_initialized && index < PREVIEW_CLIENTS; index++) {
        if (clients[index].fd >= 0)
            close_client(&clients[index]);
    }
    clients_initialized = 0;
}
