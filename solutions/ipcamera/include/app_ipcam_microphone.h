#ifndef APP_IPCAM_MICROPHONE_H
#define APP_IPCAM_MICROPHONE_H

#include <stddef.h>
#include <stdint.h>

struct app_microphone_state {
    unsigned int left_gain;
    unsigned int right_gain;
    int left_muted;
    int right_muted;
    int configured;
    int enabled;
};

int app_ipcam_Microphone_Apply(int enabled, struct app_microphone_state *state);
int app_ipcam_Microphone_Status(struct app_microphone_state *state);
int app_ipcam_Microphone_GuardFrame(int16_t *samples, size_t count);

#endif
