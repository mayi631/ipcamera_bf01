#ifndef APP_IPCAM_BOOT_SETTINGS_H
#define APP_IPCAM_BOOT_SETTINGS_H

#include "cJSON.h"

int app_boot_settings_sync(const cJSON *settings);
int app_boot_settings_status(cJSON *status, const cJSON *settings);

#endif
