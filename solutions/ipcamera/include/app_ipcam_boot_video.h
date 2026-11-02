#ifndef APP_IPCAM_BOOT_VIDEO_H
#define APP_IPCAM_BOOT_VIDEO_H

#ifdef BF01_BOOT_CAPTURE
#include "bf01_boot_video.h"

int app_ipcam_BootVideo_Init(void);
void app_ipcam_BootVideo_UnInit(void);
int app_ipcam_BootVideo_Available(void);
int app_ipcam_BootVideo_GetSize(unsigned int *width, unsigned int *height);
int app_ipcam_BootVideo_ReadFrame(unsigned char *buffer, size_t capacity,
    struct bf01_video_frame *frame, int timeout_ms);
int app_ipcam_BootVideo_ReadAudio(int16_t *samples, size_t capacity,
    uint64_t *pts, uint64_t first_pts, uint64_t through_pts);
void app_ipcam_BootVideo_Stop(void);
#endif

#endif
