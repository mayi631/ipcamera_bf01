#ifndef APP_IPCAM_UPLOAD_H
#define APP_IPCAM_UPLOAD_H

#include "bf01_wifi_link.h"

int app_upload_start(void);
void app_upload_stop(void);
void app_upload_poll(struct bf_wifi_link *link);
void app_upload_completed(const cJSON *request, const cJSON *reply);
void app_upload_status(cJSON *reply);
int app_upload_refresh_needed(void);
void app_upload_refresh_started(void);

#endif
