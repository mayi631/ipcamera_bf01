#ifndef APP_IPCAM_EVENT_H
#define APP_IPCAM_EVENT_H

#include <stdint.h>
#include <stddef.h>
#include "cJSON.h"

struct app_event_policy {
    int ai_enabled;
    int bird_alarm;
    int squirrel_alarm;
    int other_alarm;
    int record_seconds;
    int confidence_percent;
    int datetime_enabled;
    int datetime_format;
    int ai_configured;
    int record_configured;
};

int app_event_read_json(const char *path, cJSON **value);
int app_event_write_json(const char *path, const cJSON *value);
int app_event_config_path(const char *name, char *path, size_t capacity);
int app_event_config_prepare(void);
int app_event_policy_load(struct app_event_policy *policy);
int app_event_policy_field(const char *name);
int app_event_live(void);
int app_event_boot_cancelled(void);
int app_event_set_live(int enabled);
int app_event_begin(cJSON **event);
int app_event_media(const char *directory, cJSON *event, const char *purpose, const char *path);
const char *app_event_text(const cJSON *object, const char *name);
uint64_t app_event_realtime(void);

#endif
