#ifndef APP_IPCAM_BOOT_AI_H
#define APP_IPCAM_BOOT_AI_H

#ifdef BF01_BOOT_CAPTURE
int app_ipcam_BootAi_Run(void);
int app_ipcam_BootAi_Bypass(void);
int app_ipcam_BootAi_Worker(void);
int app_ipcam_BootAi_Allowed(void);
#endif

#endif
