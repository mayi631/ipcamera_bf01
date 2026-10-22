#ifndef APP_IPCAM_PREVIEW_H
#define APP_IPCAM_PREVIEW_H

#ifdef BF01_PREVIEW_SUPPORT
int app_ipcam_Preview_Enabled(void);
int app_ipcam_Preview_Start(void);
void app_ipcam_Preview_Stop(void);
#else
static inline int app_ipcam_Preview_Enabled(void) { return 0; }
static inline int app_ipcam_Preview_Start(void) { return 0; }
static inline void app_ipcam_Preview_Stop(void) { }
#endif

#endif
