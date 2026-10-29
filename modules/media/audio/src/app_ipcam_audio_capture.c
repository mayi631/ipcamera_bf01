#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "app_ipcam_audio.h"
#include "cvi_audio.h"
#include "cvi_sys.h"

#define PCM_QUEUE_FRAMES 64

static struct {
    pthread_mutex_t mutex;
    pthread_t thread;
    int running;
    int recording;
    int error;
    unsigned int head;
    unsigned int count;
    uint64_t generation;
    APP_AUDIO_PCM_FRAME frames[PCM_QUEUE_FRAMES];
} pcm = { .mutex = PTHREAD_MUTEX_INITIALIZER };

static void *capture_pcm(void *unused)
{
    (void)unused;
    APP_AUDIO_CFG_S *config = &app_ipcam_Audio_Param_Get()->astAudioCfg;
    uint64_t origin = 0;
    uint64_t samples = 0;
    uint64_t last_received = 0;
    uint64_t generation = 0;
    while (1) {
        pthread_mutex_lock(&pcm.mutex);
        int running = pcm.running;
        pthread_mutex_unlock(&pcm.mutex);
        if (!running)
            break;
        AUDIO_FRAME_S frame = {0};
        AEC_FRAME_S reference = {0};
        if (CVI_AI_GetFrame(config->u32AiDevId, config->u32AiChn, &frame, &reference, 100) != CVI_SUCCESS) {
            usleep(10000);
            continue;
        }
        CVI_U64 now = 0;
        int error = 0;
        if (!frame.u64VirAddr[0] || !frame.u32Len || frame.u32Len > APP_AUDIO_PCM_SAMPLES ||
            frame.enBitwidth != AUDIO_BIT_WIDTH_16 || frame.enSoundmode != AUDIO_SOUND_MODE_MONO ||
            CVI_SYS_GetCurPTS(&now) != CVI_SUCCESS) {
            error = EIO;
        } else {
            uint64_t duration = (uint64_t)frame.u32Len * 1000000 / config->enSamplerate;
            pthread_mutex_lock(&pcm.mutex);
            if (generation != pcm.generation) {
                generation = pcm.generation;
                origin = 0;
                samples = 0;
            }
            if (!origin) {
                origin = now > duration ? now - duration : 0;
            } else if (now <= last_received || now - last_received > 500000) {
                error = EIO;
            }
            if (pcm.recording && !pcm.error) {
                if (error || pcm.count == PCM_QUEUE_FRAMES) {
                    pcm.error = error ? error : EOVERFLOW;
                } else {
                    unsigned int index = (pcm.head + pcm.count) % PCM_QUEUE_FRAMES;
                    APP_AUDIO_PCM_FRAME *output = &pcm.frames[index];
                    output->pts = origin + samples * 1000000 / config->enSamplerate;
                    output->count = frame.u32Len;
                    memcpy(output->samples, frame.u64VirAddr[0], frame.u32Len * sizeof(int16_t));
                    pcm.count++;
                }
            }
            pthread_mutex_unlock(&pcm.mutex);
            samples += frame.u32Len;
            last_received = now;
        }
        if (CVI_AI_ReleaseFrame(config->u32AiDevId, config->u32AiChn, &frame, &reference) != CVI_SUCCESS)
            error = EIO;
        if (error) {
            pthread_mutex_lock(&pcm.mutex);
            if (pcm.recording)
                pcm.error = error;
            pthread_mutex_unlock(&pcm.mutex);
            origin = 0;
            samples = 0;
        }
    }
    return NULL;
}

int app_ipcam_Audio_CaptureInit(void)
{
    APP_PARAM_AUDIO_CFG_T *parameters = app_ipcam_Audio_Param_Get();
    APP_AUDIO_CFG_S *config = &parameters->astAudioCfg;
    if (parameters->bInit)
        return 0;
    if (config->enSamplerate != AUDIO_SAMPLE_RATE_16000 || config->enBitwidth != AUDIO_BIT_WIDTH_16 ||
        config->enSoundmode != AUDIO_SOUND_MODE_MONO || config->u32PtNumPerFrm == 0 ||
        config->u32PtNumPerFrm > APP_AUDIO_PCM_SAMPLES || config->u32ChnCnt == 0 ||
        config->u32ChnCnt > 2 || config->u32AiChn >= config->u32ChnCnt)
        return -EINVAL;
    AIO_ATTR_S attr = {0};
    attr.enSamplerate = config->enSamplerate;
    attr.enBitwidth = config->enBitwidth;
    attr.enWorkmode = config->enWorkmode;
    attr.enSoundmode = config->enSoundmode;
    attr.u32EXFlag = config->u32EXFlag;
    attr.u32FrmNum = config->u32FrmNum;
    attr.u32PtNumPerFrm = config->u32PtNumPerFrm;
    attr.u32ChnCnt = config->u32ChnCnt;
    attr.u32ClkSel = config->u32ClkSel;
    attr.enI2sType = config->enI2sType;
    int result = CVI_AUDIO_INIT();
    if (result != CVI_SUCCESS)
        return result;
    result = CVI_AI_SetPubAttr(config->u32AiDevId, &attr);
    if (result != CVI_SUCCESS)
        goto deinit;
    result = CVI_AI_Enable(config->u32AiDevId);
    if (result != CVI_SUCCESS)
        goto deinit;
    result = CVI_AI_EnableChn(config->u32AiDevId, config->u32AiChn);
    if (result != CVI_SUCCESS)
        goto disable;
    result = CVI_AI_SetVolume(config->u32AiDevId, parameters->astAudioVol.iAdcLVol);
    if (result != CVI_SUCCESS)
        goto channel;
    pthread_mutex_lock(&pcm.mutex);
    pcm.running = 1;
    pcm.recording = 0;
    pcm.error = 0;
    pcm.head = 0;
    pcm.count = 0;
    pthread_mutex_unlock(&pcm.mutex);
    result = pthread_create(&pcm.thread, NULL, capture_pcm, NULL);
    if (result) {
        pthread_mutex_lock(&pcm.mutex);
        pcm.running = 0;
        pthread_mutex_unlock(&pcm.mutex);
        result = -result;
        goto channel;
    }
    parameters->bInit = true;
    fprintf(stderr, "[audio] MIC ready: PCM 16000 Hz mono 16-bit; capture only, AO/playback disabled\n");
    return 0;
channel:
    CVI_AI_DisableChn(config->u32AiDevId, config->u32AiChn);
disable:
    CVI_AI_Disable(config->u32AiDevId);
deinit:
    CVI_AUDIO_DEINIT();
    return result;
}

int app_ipcam_Audio_CaptureUnInit(void)
{
    APP_PARAM_AUDIO_CFG_T *parameters = app_ipcam_Audio_Param_Get();
    if (!parameters->bInit)
        return 0;
    pthread_mutex_lock(&pcm.mutex);
    pcm.recording = 0;
    pcm.running = 0;
    pthread_mutex_unlock(&pcm.mutex);
    pthread_join(pcm.thread, NULL);
    APP_AUDIO_CFG_S *config = &parameters->astAudioCfg;
    int result = CVI_AI_DisableChn(config->u32AiDevId, config->u32AiChn);
    int disabled = CVI_AI_Disable(config->u32AiDevId);
    int deinit = CVI_AUDIO_DEINIT();
    parameters->bInit = false;
    return result ? result : (disabled ? disabled : deinit);
}

int app_ipcam_Audio_CaptureBegin(void)
{
    pthread_mutex_lock(&pcm.mutex);
    int result = !pcm.running ? ENODEV : (pcm.recording ? EBUSY : 0);
    if (!result) {
        pcm.head = 0;
        pcm.count = 0;
        pcm.error = 0;
        pcm.generation++;
        pcm.recording = 1;
    }
    pthread_mutex_unlock(&pcm.mutex);
    return result;
}

int app_ipcam_Audio_CaptureRead(APP_AUDIO_PCM_FRAME *frame)
{
    if (!frame)
        return -EINVAL;
    pthread_mutex_lock(&pcm.mutex);
    int result = pcm.error ? -pcm.error : (!pcm.running || !pcm.recording ? -ENODEV : 0);
    if (!result && pcm.count) {
        *frame = pcm.frames[pcm.head];
        pcm.head = (pcm.head + 1) % PCM_QUEUE_FRAMES;
        pcm.count--;
        result = 1;
    }
    pthread_mutex_unlock(&pcm.mutex);
    return result;
}

void app_ipcam_Audio_CaptureEnd(void)
{
    pthread_mutex_lock(&pcm.mutex);
    pcm.recording = 0;
    pcm.count = 0;
    pthread_mutex_unlock(&pcm.mutex);
}
