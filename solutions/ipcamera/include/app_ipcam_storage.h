#ifndef APP_IPCAM_STORAGE_H
#define APP_IPCAM_STORAGE_H

#include <stdint.h>

#define APP_STORAGE_MOUNT "/mnt/sd"

struct app_storage_status {
    char state[32];
    char device[64];
    char format_status[16];
    uint64_t generation;
    uint64_t total_bytes;
    uint64_t free_bytes;
    int ready;
    int error;
};

int app_ipcam_Storage_Start(void);
void app_ipcam_Storage_Stop(void);
int app_ipcam_Storage_Acquire(uint64_t *generation);
int app_ipcam_Storage_Check(uint64_t generation);
void app_ipcam_Storage_ReportError(uint64_t generation, int error);
void app_ipcam_Storage_Release(void);
int app_ipcam_Storage_Lock(int *descriptor);
int app_ipcam_Storage_Status(struct app_storage_status *status);
int app_ipcam_Storage_ReportStatus(struct app_storage_status *status);
int app_ipcam_Storage_Format(void);
void app_ipcam_Storage_Reported(const char *format_status);
void app_ipcam_Storage_ReportReset(void);
void app_ipcam_Storage_FormatStop(void);

#endif
