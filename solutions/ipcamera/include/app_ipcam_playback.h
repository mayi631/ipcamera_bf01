#ifndef APP_IPCAM_PLAYBACK_H
#define APP_IPCAM_PLAYBACK_H

int app_ipcam_Playback_Start(void);
void app_ipcam_Playback_Stop(void);
int app_ipcam_Playback_Play(const char *filename);
void app_ipcam_Playback_Cancel(void);
void app_ipcam_Playback_Control(const char *query, char *response, unsigned int size);

#endif
