#ifndef APP_IPCAM_CAPTURE_H
#define APP_IPCAM_CAPTURE_H

int app_ipcam_Capture_Start(void);
void app_ipcam_Capture_Stop(void);
int app_ipcam_Capture_Configure(const char *seconds, const char *channel);

#endif
