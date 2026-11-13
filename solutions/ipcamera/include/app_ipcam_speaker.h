#ifndef APP_IPCAM_SPEAKER_H
#define APP_IPCAM_SPEAKER_H

struct app_speaker_state {
    unsigned int left_gain;
    unsigned int right_gain;
    unsigned int left_muted;
    unsigned int right_muted;
    int configured;
    unsigned int percent;
};

int app_ipcam_Speaker_Status(struct app_speaker_state *state);
int app_ipcam_Speaker_Apply(unsigned int percent, struct app_speaker_state *state);
int app_ipcam_Speaker_Restore(void);

#endif
