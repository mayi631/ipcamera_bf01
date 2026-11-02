#ifndef APP_IPCAM_BOOT_CAPTURE_H
#define APP_IPCAM_BOOT_CAPTURE_H

#ifdef BF01_BOOT_CAPTURE
#include "bf01_boot_capture.h"
#include "app_ipcam_boot_video.h"

#ifndef APP_BOOT_CAPTURE_DIR
#define APP_BOOT_CAPTURE_DIR "/run/ipcamera-burst"
#endif

int app_ipcam_BootCapture_Init(void);
void app_ipcam_BootCapture_UnInit(void);
int app_ipcam_BootCapture_Ready(void);
int app_ipcam_BootCapture_Stage(struct bf01_burst_reply *metadata, int (*cancelled)(void));
int app_ipcam_BootCapture_Commit(const struct bf01_burst_reply *metadata);
#endif

#endif
