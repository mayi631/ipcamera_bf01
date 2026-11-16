#ifndef APP_IPCAM_WIFI_CAMERA_H
#define APP_IPCAM_WIFI_CAMERA_H

#include "cJSON.h"

int app_ipcam_WifiCamera_Start(void);
int app_ipcam_WifiCamera_Descriptor(void);
void app_ipcam_WifiCamera_Poll(void);
void app_ipcam_WifiCamera_Stop(void);
int app_ipcam_WifiCamera_Request(const cJSON *request, cJSON **reply);
int app_ipcam_WifiCamera_RequestTimed(const cJSON *request, cJSON **reply, int timeout_ms);

#endif
