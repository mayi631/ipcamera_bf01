#ifndef BF01_WIFI_LINK_H
#define BF01_WIFI_LINK_H

#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#define BF_WIFI_RECEIVE_MAX 1600U
#define BF_WIFI_SEND_MAX 1595U
#define BF_WIFI_PARTS_MAX 32U
#define BF_WIFI_PART_TIMEOUT_MS 15000U

struct bf_wifi_link;

struct bf_wifi_ops {
    int (*receive)(void *context, char *message, size_t capacity);
    int (*exchange)(void *context, const char *request, char *response, size_t capacity);
    int (*apply)(void *context, const cJSON *changes, int full);
    int (*stream)(void *context, const cJSON *request);
    int (*snapshot)(void *context, const char *name, cJSON **value);
    void (*completed)(void *context, const cJSON *request, const cJSON *reply);
};

struct bf_wifi_link *bf_wifi_create(const struct bf_wifi_ops *ops, void *context);
int bf_wifi_resume_id(struct bf_wifi_link *link, unsigned int next_id);
unsigned int bf_wifi_next_id(const struct bf_wifi_link *link);
void bf_wifi_destroy(struct bf_wifi_link *link);
void bf_wifi_reset(struct bf_wifi_link *link);
int bf_wifi_step(struct bf_wifi_link *link, uint64_t monotonic_ms, uint64_t realtime_ms);
int bf_wifi_enqueue(struct bf_wifi_link *link, const cJSON *request);
int bf_wifi_retry_http(struct bf_wifi_link *link);
const cJSON *bf_wifi_http(const struct bf_wifi_link *link, uint64_t realtime_ms);
int bf_wifi_http_refresh_due(const struct bf_wifi_link *link, uint64_t realtime_ms);

struct bf_wifi_sdio {
    int descriptor;
    unsigned int status;
};

struct bf_wifi_filter {
    uint32_t status;
    uint16_t flags;
    uint8_t source_count;
    uint8_t destination_count;
    uint16_t source_ports[4];
    uint16_t destination_ports[4];
};

int bf_wifi_sdio_receive(void *context, char *message, size_t capacity);
int bf_wifi_sdio_exchange(void *context, const char *request, char *response, size_t capacity);
int bf_wifi_sdio_filter(struct bf_wifi_sdio *sdio, struct bf_wifi_filter *filter);

struct bf_wifi_time {
    uint32_t status;
    int32_t second;
    int32_t minute;
    int32_t hour;
    int32_t day;
    int32_t month;
    int32_t year;
    int32_t weekday;
    int32_t yearday;
    int32_t daylight;
};

int bf_wifi_sdio_time(struct bf_wifi_sdio *sdio, struct bf_wifi_time *value);

#endif
