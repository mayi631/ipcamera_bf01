#ifndef APP_IPCAM_STORAGE_H
#define APP_IPCAM_STORAGE_H

#include <stdint.h>

#define APP_STORAGE_MOUNT "/mnt/sd"

int app_ipcam_Storage_Start(void);
void app_ipcam_Storage_Stop(void);
int app_ipcam_Storage_Acquire(uint64_t *generation);
int app_ipcam_Storage_Check(uint64_t generation);
void app_ipcam_Storage_ReportError(uint64_t generation, int error);
void app_ipcam_Storage_Release(void);

#endif
