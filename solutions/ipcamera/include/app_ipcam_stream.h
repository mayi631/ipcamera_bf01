#ifndef APP_IPCAM_STREAM_H
#define APP_IPCAM_STREAM_H

#include "cJSON.h"

int app_ipcam_Stream_Apply(const cJSON *request, cJSON *reply);
void app_ipcam_Stream_Status(cJSON *reply);
void app_ipcam_Stream_Stop(void);

#endif
