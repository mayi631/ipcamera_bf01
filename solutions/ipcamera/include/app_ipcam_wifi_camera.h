#ifndef APP_IPCAM_WIFI_CAMERA_H
#define APP_IPCAM_WIFI_CAMERA_H

#include "cJSON.h"

int app_ipcam_WifiCamera_Start(void);
void app_ipcam_WifiCamera_Poll(void);
void app_ipcam_WifiCamera_Stop(void);
int app_ipcam_WifiCamera_Request(const cJSON *request, cJSON **reply);

#endif
