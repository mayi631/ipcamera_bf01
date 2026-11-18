#ifndef APP_IPCAM_SETTINGS_H
#define APP_IPCAM_SETTINGS_H

#include <stddef.h>
#include "cJSON.h"

int app_settings_path(char *path, size_t capacity);
int app_settings_init(void);
int app_settings_show(void);
int app_settings_load(cJSON **settings);
int app_settings_apply(const cJSON *changes);
int app_settings_lock(int operation);
int app_settings_load_locked(cJSON **settings);
int app_settings_apply_locked(const cJSON *changes);
int app_settings_sync_locked(void);
int app_settings_sync_boot(void);

#endif
