#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "atbm_tool.h"
#include "atbm_ioctl_ext.h"

#define RUN_DIR "/run/bf01-preview"

static volatile sig_atomic_t stopping;
static volatile sig_atomic_t event_pending;

static void handle_signal(int signal_number)
{
    if (signal_number == SIGIO)
        event_pending = 1;
    else
        stopping = 1;
}

static int read_line(FILE *file, char *text, size_t size)
{
    size_t length;
    if (!fgets(text, size, file))
        return -1;
    length = strlen(text);
    if (!length || text[length - 1] != '\n')
        return -1;
    text[--length] = '\0';
    if (length && text[length - 1] == '\r')
        text[--length] = '\0';
    return 0;
}

static int read_config(const char *path, struct ap_cfg *config, int *channel)
{
    char ssid[35];
    char key[67];
    char channel_text[8];
    char *end;
    size_t index;
    FILE *file = fopen(path, "r");
    int result = -1;
    long number;
    if (!file)
        return -1;
    if (read_line(file, ssid, sizeof(ssid)) || read_line(file, key, sizeof(key)) ||
        read_line(file, channel_text, sizeof(channel_text)) || fgetc(file) != EOF)
        goto done;
    number = strtol(channel_text, &end, 10);
    if (!*channel_text || *end || number < 1 || number > 13 ||
        !strlen(ssid) || strlen(ssid) > 32 || strlen(key) < 8 || strlen(key) > 63)
        goto done;
    for (index = 0; ssid[index]; index++) {
        if ((unsigned char)ssid[index] < 32 || (unsigned char)ssid[index] == 127)
            goto done;
    }
    for (index = 0; key[index]; index++) {
        if ((unsigned char)key[index] < 32 || (unsigned char)key[index] > 126)
            goto done;
    }
    memset(config, 0, sizeof(*config));
    config->ap_parameter.ssidLength = strlen(ssid);
    memcpy(config->ap_parameter.ssid, ssid, strlen(ssid));
    config->ap_parameter.keyLength = strlen(key);
    memcpy(config->ap_parameter.key, key, strlen(key));
    config->ap_parameter.keyMgmt = KEY_MGMT_WPA2;
    *channel = (int)number;
    result = 0;
done:
    fclose(file);
    memset(key, 0, sizeof(key));
    return result;
}

static int inherited_control_lock(void)
{
    struct stat inherited;
    struct stat expected;
    if (fstat(9, &inherited) || stat("/run/atbm_wifi/control.lock", &expected) ||
        inherited.st_dev != expected.st_dev || inherited.st_ino != expected.st_ino ||
        flock(9, LOCK_EX | LOCK_NB)) {
        fprintf(stderr, "AP requires inherited Wi-Fi control lock on fd 9; use bf01-preview\n");
        return -1;
    }
    return 0;
}

static int set_interface(const struct HostConnectEvent *event)
{
    struct ifreq request;
    struct sockaddr_in *address;
    int socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int result = -1;
    if (socket_fd < 0)
        return -1;
    memset(&request, 0, sizeof(request));
    strcpy(request.ifr_name, "wlan0");
    address = (struct sockaddr_in *)&request.ifr_addr;
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = event ? event->ipaddr : INADDR_ANY;
    if (ioctl(socket_fd, SIOCSIFADDR, &request))
        goto done;
    if (event) {
        address->sin_addr.s_addr = event->ipmask;
        if (ioctl(socket_fd, SIOCSIFNETMASK, &request))
            goto done;
        address->sin_addr.s_addr = event->ipaddr | ~event->ipmask;
        if (ioctl(socket_fd, SIOCSIFBRDADDR, &request))
            goto done;
    }
    if (ioctl(socket_fd, SIOCGIFFLAGS, &request))
        goto done;
    if (event)
        request.ifr_flags |= IFF_UP;
    else
        request.ifr_flags &= ~IFF_UP;
    if (ioctl(socket_fd, SIOCSIFFLAGS, &request))
        goto done;
    result = 0;
done:
    close(socket_fd);
    return result;
}

static int valid_ap_event(const struct HostConnectEvent *event, const struct ap_cfg *config)
{
    uint32_t address = ntohl(event->ipaddr);
    uint32_t mask = ntohl(event->ipmask);
    uint32_t hosts = ~mask;
    return event->ssidlen == config->ap_parameter.ssidLength &&
        !memcmp(event->ssid, config->ap_parameter.ssid, event->ssidlen) &&
        mask && hosts && !(hosts & (hosts + 1)) &&
        (address >> 24) != 0 && (address >> 24) != 127 && (address >> 24) < 224 &&
        (address & hosts) && (address & hosts) != hosts;
}

static int mark_ready(const struct HostConnectEvent *event, const struct ap_cfg *config)
{
    char address[INET_ADDRSTRLEN];
    char mask[INET_ADDRSTRLEN];
    struct in_addr converted;
    FILE *file;
    if (set_interface(event))
        return -1;
    converted.s_addr = event->ipaddr;
    inet_ntop(AF_INET, &converted, address, sizeof(address));
    converted.s_addr = event->ipmask;
    inet_ntop(AF_INET, &converted, mask, sizeof(mask));
    file = fopen(RUN_DIR "/ap.ssid", "w");
    if (!file)
        return -1;
    fprintf(file, "%.*s\n", config->ap_parameter.ssidLength, config->ap_parameter.ssid);
    if (fclose(file))
        return -1;
    file = fopen(RUN_DIR "/ap.ready", "w");
    if (!file)
        return -1;
    fprintf(file, "ap_address=%s\nap_netmask=%s\n", address, mask);
    if (fclose(file))
        return -1;
    printf("AP firmware ready; Linux wlan0=%s/%s; verify phone DHCP and RTSP separately\n",
        address, mask);
    fflush(stdout);
    return 0;
}

static int restore_sta(int device_fd)
{
    int result = ioctl(device_fd, ATBM_WIFI_MODE, 0);
    if (set_interface(NULL))
        result = -1;
    unlink(RUN_DIR "/ap.ready");
    unlink(RUN_DIR "/ap.ssid");
    return result;
}

static int watch_ap(int device_fd, struct ap_cfg *config, int channel)
{
    struct sigaction action;
    struct status_info status;
    struct HostConnectEvent applied;
    struct status_async event;
    struct timespec now;
    long started_at;
    long last_status_at = -1;
    int flags;
    int watch_fd;
    int ready = 0;
    int failures = 0;
    int result = -1;
    watch_fd = open("/run/atbm_wifi/watch.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (watch_fd < 0 || flock(watch_fd, LOCK_EX | LOCK_NB)) {
        if (watch_fd >= 0)
            close(watch_fd);
        fprintf(stderr, "STA watcher still owns Wi-Fi; refusing AP startup\n");
        return -1;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGIO, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    flags = fcntl(device_fd, F_GETFL);
    if (flags < 0 || fcntl(device_fd, F_SETOWN, getpid()) ||
        fcntl(device_fd, F_SETFL, flags | FASYNC))
        goto done;
    if (ioctl(device_fd, ATBM_WIFI_MODE, 1) || ioctl(device_fd, ATBM_AP_CFG, config) ||
        ioctl(device_fd, ATBM_WIFI_CHANNEL, channel)) {
        fprintf(stderr, "AP configuration/channel rejected by firmware\n");
        goto done;
    }
    memset(&applied, 0, sizeof(applied));
    clock_gettime(CLOCK_MONOTONIC, &now);
    started_at = now.tv_sec;
    while (!stopping) {
        if (event_pending) {
            int attempts;
            event_pending = 0;
            for (attempts = 0; attempts < 64; attempts++) {
                if (read(device_fd, &event, sizeof(event)) != (ssize_t)sizeof(event))
                    break;
                if (event.type == 1 && !event.driver_mode)
                    goto done;
                if (event.list_empty)
                    break;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec != last_status_at) {
            last_status_at = now.tv_sec;
            memset(&status, 0, sizeof(status));
            if (!ioctl(device_fd, ATBM_STATUS, &status) && status.wifimode &&
                valid_ap_event(&status.con_event, config)) {
                failures = 0;
                if (!ready || applied.ipaddr != status.con_event.ipaddr ||
                    applied.ipmask != status.con_event.ipmask) {
                    if (mark_ready(&status.con_event, config))
                        goto done;
                    memcpy(&applied, &status.con_event, sizeof(applied));
                    ready = 1;
                }
            } else if (ready && ++failures >= 3) {
                fprintf(stderr, "AP status lost; stopping instead of advertising a stale address\n");
                goto done;
            }
        }
        if (!ready && now.tv_sec - started_at > 25) {
            fprintf(stderr, "No matching AP status/IP from firmware; check AP capability and module log\n");
            goto done;
        }
        usleep(100000);
    }
    result = 0;
done:
    if (restore_sta(device_fd)) {
        fprintf(stderr, "Failed to restore STA mode\n");
        result = -1;
    }
    close(watch_fd);
    return result;
}

int main(int argc, char **argv)
{
    struct ap_cfg config;
    int channel;
    int device_fd;
    int result;
    umask(077);
    if (argc == 3 && !strcmp(argv[1], "check")) {
        result = read_config(argv[2], &config, &channel);
        memset(&config, 0, sizeof(config));
        if (result)
            fprintf(stderr, "AP config requires SSID(1-32 bytes), WPA2 key(8-63 printable ASCII), channel(1-13), one per line\n");
        return result ? 1 : 0;
    }
    if (!((argc == 3 && !strcmp(argv[1], "run")) ||
        (argc == 2 && !strcmp(argv[1], "sta")))) {
        fprintf(stderr, "Use bf01-preview; internal usage: bf01_atbm_ap {check FILE|run FILE|sta}\n");
        return 1;
    }
    if (argc == 3 && read_config(argv[2], &config, &channel))
        return 1;
    if (inherited_control_lock())
        return 1;
    device_fd = open("/dev/atbm_ioctl", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (device_fd < 0) {
        perror("open /dev/atbm_ioctl");
        return 1;
    }
    if (!strcmp(argv[1], "sta"))
        result = restore_sta(device_fd);
    else
        result = watch_ap(device_fd, &config, channel);
    memset(&config, 0, sizeof(config));
    close(device_fd);
    return result ? 1 : 0;
}
