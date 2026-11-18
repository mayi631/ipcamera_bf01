#ifndef APP_IPCAM_BOOT_TIMESTAMP_H
#define APP_IPCAM_BOOT_TIMESTAMP_H

#ifdef BF01_BOOT_CAPTURE
int app_ipcam_BootTimestampLockIpc(void);
void app_ipcam_BootTimestampPoll(void);
int app_ipcam_BootTimestampSyncRtc(void);
int app_ipcam_BootTimestampApplyPolicy(void);
#endif

#endif
