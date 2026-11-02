#include "app_ipcam_playback.h"

#ifdef BF01_AUDIO_DUPLEX
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include "app_ipcam_audio.h"
#include "cvi_audio.h"

#define PLAYBACK_RATE 16000
#define PLAYBACK_SAMPLES 320
#define PLAYBACK_AO_CHN 1
#define PLAYBACK_AMP_START_US 250000
#define PLAYBACK_AMP_STOP_US 100000

struct pcm_output {
    struct timespec started;
    uint64_t samples;
};

static struct {
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    pthread_t thread;
    int running;
    int busy;
    int cancel;
    int pending_fd;
    int error;
} playback = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .wake = PTHREAD_COND_INITIALIZER,
    .pending_fd = -1
};

static int interrupted(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&playback.mutex);
    int stopped = !playback.running || playback.cancel;
    pthread_mutex_unlock(&playback.mutex);
    return stopped;
}

static int read_audio(void *opaque, uint8_t *buffer, int size)
{
    if (interrupted(NULL))
        return AVERROR_EXIT;
    ssize_t count;
    do {
        count = read(*(int *)opaque, buffer, size);
    } while (count < 0 && errno == EINTR && !interrupted(NULL));
    return count < 0 ? AVERROR(errno) : (count ? (int)count : AVERROR_EOF);
}

static int64_t seek_audio(void *opaque, int64_t offset, int whence)
{
    int descriptor = *(int *)opaque;
    if (whence == AVSEEK_SIZE) {
        struct stat status;
        return fstat(descriptor, &status) ? AVERROR(errno) : status.st_size;
    }
    off_t position = lseek(descriptor, offset, whence & ~AVSEEK_FORCE);
    return position < 0 ? AVERROR(errno) : position;
}

static int send_pcm(struct pcm_output *output, const uint8_t *buffer, int samples)
{
    while (samples > 0) {
        if (interrupted(NULL))
            return AVERROR_EXIT;
        int count = samples > PLAYBACK_SAMPLES ? PLAYBACK_SAMPLES : samples;
        AUDIO_FRAME_S frame = {0};
        frame.enBitwidth = AUDIO_BIT_WIDTH_16;
        frame.enSoundmode = AUDIO_SOUND_MODE_MONO;
        frame.u32Len = count;
        frame.u64VirAddr[0] = (CVI_U8 *)buffer;
        if (!output->samples && clock_gettime(CLOCK_MONOTONIC, &output->started))
            return AVERROR(errno);
        if (CVI_AO_SendFrame(0, PLAYBACK_AO_CHN, &frame, 200) != CVI_SUCCESS)
            return AVERROR(EIO);
        output->samples += count;
        buffer += count * sizeof(int16_t);
        samples -= count;
    }
    return 0;
}

static int send_silence(struct pcm_output *output, int frames)
{
    int16_t silence[PLAYBACK_SAMPLES] = {0};
    for (int index = 0; index < frames; ++index) {
        int result = send_pcm(output, (const uint8_t *)silence, PLAYBACK_SAMPLES);
        if (result < 0)
            return result;
    }
    return 0;
}

static int convert_pcm(struct pcm_output *output, SwrContext *resampler, AVFrame *frame)
{
    int capacity = swr_get_out_samples(resampler, frame ? frame->nb_samples : 0);
    if (capacity < 0)
        return capacity;
    if (!capacity)
        return 0;
    uint8_t *buffer = NULL;
    int result = av_samples_alloc(&buffer, NULL, 1, capacity, AV_SAMPLE_FMT_S16, 0);
    if (result < 0)
        return result;
    int count = swr_convert(resampler, &buffer, capacity,
        frame ? (const uint8_t **)frame->extended_data : NULL, frame ? frame->nb_samples : 0);
    result = count < 0 ? count : send_pcm(output, buffer, count);
    av_freep(&buffer);
    return result < 0 ? result : count;
}

static int receive_pcm(struct pcm_output *output, AVCodecContext *decoder, SwrContext *resampler, AVFrame *frame)
{
    int result;
    while ((result = avcodec_receive_frame(decoder, frame)) >= 0) {
        if (frame->sample_rate != decoder->sample_rate || frame->format != decoder->sample_fmt ||
            frame->channels != decoder->channels) {
            av_frame_unref(frame);
            return AVERROR(EINVAL);
        }
        result = convert_pcm(output, resampler, frame);
        av_frame_unref(frame);
        if (result < 0)
            return result;
    }
    return result == AVERROR(EAGAIN) || result == AVERROR_EOF ? 0 : result;
}

static void close_output(int *ao_enabled, int *channel_enabled)
{
    app_ipcam_Audio_SpeakerDisable();
    if (*channel_enabled) {
        usleep(PLAYBACK_AMP_STOP_US);
        CVI_AO_SetMute(0, CVI_TRUE, NULL);
        CVI_AO_ClearChnBuf(0, PLAYBACK_AO_CHN);
        CVI_AO_DisableChn(0, PLAYBACK_AO_CHN);
        *channel_enabled = 0;
    }
    if (*ao_enabled) {
        int result = CVI_AO_Disable(0);
        fprintf(stderr, "[audio] AO device shutdown: result=%d\n", result);
        *ao_enabled = 0;
    }
}

static int play_file(int descriptor, int *ao_enabled, int *channel_enabled)
{
    int result = AVERROR(ENOMEM);
    struct pcm_output output = {0};
    AVFormatContext *format = avformat_alloc_context();
    AVCodecContext *decoder = NULL;
    SwrContext *resampler = NULL;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    uint8_t *io_buffer = av_malloc(32768);
    AVIOContext *io = NULL;
    if (!format || !packet || !frame || !io_buffer)
        goto done;
    io = avio_alloc_context(io_buffer, 32768, 0, &descriptor, read_audio, NULL, seek_audio);
    if (!io)
        goto done;
    io_buffer = NULL;
    format->pb = io;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    format->interrupt_callback.callback = interrupted;
    AVDictionary *options = NULL;
    av_dict_set(&options, "format_whitelist", "mp3,wav,aac,mov", 0);
    result = avformat_open_input(&format, NULL, NULL, &options);
    av_dict_free(&options);
    if (result < 0)
        goto done;
    result = avformat_find_stream_info(format, NULL);
    if (result < 0)
        goto done;
    AVCodec *codec = NULL;
    int stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (stream < 0) {
        result = stream;
        goto done;
    }
    decoder = avcodec_alloc_context3(codec);
    if (!decoder) {
        result = AVERROR(ENOMEM);
        goto done;
    }
    result = avcodec_parameters_to_context(decoder, format->streams[stream]->codecpar);
    if (result < 0)
        goto done;
    decoder->pkt_timebase = format->streams[stream]->time_base;
    result = avcodec_open2(decoder, codec, NULL);
    if (result < 0)
        goto done;
    if (decoder->sample_rate <= 0 || decoder->channels <= 0 || decoder->channels > 8) {
        result = AVERROR(EINVAL);
        goto done;
    }
    int64_t layout = decoder->channel_layout;
    if (!layout)
        layout = av_get_default_channel_layout(decoder->channels);
    resampler = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_MONO, AV_SAMPLE_FMT_S16,
        PLAYBACK_RATE, layout, decoder->sample_fmt, decoder->sample_rate, 0, NULL);
    if (!resampler) {
        result = AVERROR(ENOMEM);
        goto done;
    }
    result = swr_init(resampler);
    if (result < 0 || interrupted(NULL)) {
        if (result >= 0)
            result = AVERROR_EXIT;
        goto done;
    }
    if (!*channel_enabled) {
        AIO_ATTR_S attr = {0};
        attr.enSamplerate = AUDIO_SAMPLE_RATE_16000;
        attr.enBitwidth = AUDIO_BIT_WIDTH_16;
        attr.enWorkmode = AIO_MODE_I2S_MASTER;
        attr.enSoundmode = AUDIO_SOUND_MODE_MONO;
        attr.u32FrmNum = 4;
        attr.u32PtNumPerFrm = PLAYBACK_SAMPLES;
        attr.u32ChnCnt = 2;
        attr.enI2sType = AIO_I2STYPE_INNERCODEC;
        result = AVERROR(EIO);
        if (!*ao_enabled) {
            if (CVI_AO_SetPubAttr(0, &attr) != CVI_SUCCESS || CVI_AO_Enable(0) != CVI_SUCCESS)
                goto done;
            *ao_enabled = 1;
            fprintf(stderr, "[audio] AO device enabled: retained until playback shutdown\n");
        }
        if (CVI_AO_EnableChn(0, PLAYBACK_AO_CHN) != CVI_SUCCESS)
            goto done;
        *channel_enabled = 1;
        if (CVI_AO_SetMute(0, CVI_TRUE, NULL) != CVI_SUCCESS)
            goto done;
        result = send_silence(&output, 5);
        if (result < 0)
            goto done;
        usleep(100000);
        if (interrupted(NULL)) {
            result = AVERROR_EXIT;
            goto done;
        }
        result = AVERROR(EIO);
        if (CVI_AO_SetVolume(0, 32) != CVI_SUCCESS ||
            CVI_AO_SetMute(0, CVI_FALSE, NULL) != CVI_SUCCESS)
            goto done;
        result = send_silence(&output, 10);
        if (result < 0)
            goto done;
        usleep(200000);
        if (interrupted(NULL)) {
            result = AVERROR_EXIT;
            goto done;
        }
        result = app_ipcam_Audio_SpeakerSet(1);
        if (result < 0)
            goto done;
        usleep(PLAYBACK_AMP_START_US);
        if (interrupted(NULL)) {
            result = AVERROR_EXIT;
            goto done;
        }
        fprintf(stderr, "[audio] speaker held on: AO and amplifier retained between prompts\n");
    }
    fprintf(stderr, "[audio] playback started: 16000 Hz mono PCM, DAC volume=32\n");
    while (!interrupted(NULL)) {
        result = av_read_frame(format, packet);
        if (result < 0)
            break;
        if (packet->stream_index == stream) {
            result = avcodec_send_packet(decoder, packet);
            if (result >= 0)
                result = receive_pcm(&output, decoder, resampler, frame);
        }
        av_packet_unref(packet);
        if (result < 0)
            break;
    }
    if (interrupted(NULL))
        result = AVERROR_EXIT;
    if (result == AVERROR_EOF) {
        result = avcodec_send_packet(decoder, NULL);
        if (result >= 0)
            result = receive_pcm(&output, decoder, resampler, frame);
        if (result >= 0) {
            do {
                result = convert_pcm(&output, resampler, NULL);
            } while (result > 0);
        }
        if (result == 0)
            result = send_silence(&output, 20);
        if (result == 0) {
            result = AVERROR(ETIMEDOUT);
            for (int attempt = 0; attempt < 200 && !interrupted(NULL); ++attempt) {
                AO_CHN_STATE_S state = {0};
                if (CVI_AO_QueryChnStat(0, PLAYBACK_AO_CHN, &state) != CVI_SUCCESS) {
                    result = AVERROR(EIO);
                    break;
                }
                if (!state.u32ChnBusyNum) {
                    struct timespec now;
                    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
                        result = AVERROR(errno);
                        break;
                    }
                    int64_t elapsed = (int64_t)(now.tv_sec - output.started.tv_sec) * 1000000 +
                        (now.tv_nsec - output.started.tv_nsec) / 1000;
                    uint64_t duration = output.samples * 1000000 / PLAYBACK_RATE + 200000;
                    if (elapsed >= 0 && (uint64_t)elapsed >= duration) {
                        result = 0;
                        break;
                    }
                }
                usleep(10000);
            }
            if (interrupted(NULL))
                result = AVERROR_EXIT;
        }
    }
done:
    swr_free(&resampler);
    avcodec_free_context(&decoder);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avformat_close_input(&format);
    if (io) {
        av_freep(&io->buffer);
        avio_context_free(&io);
    }
    av_free(io_buffer);
    return result;
}

static void *playback_worker(void *unused)
{
    (void)unused;
    int ao_enabled = 0;
    int channel_enabled = 0;
    pthread_mutex_lock(&playback.mutex);
    while (playback.running) {
        while (playback.running && playback.pending_fd < 0)
            pthread_cond_wait(&playback.wake, &playback.mutex);
        if (!playback.running)
            break;
        int descriptor = playback.pending_fd;
        playback.pending_fd = -1;
        pthread_mutex_unlock(&playback.mutex);
        int result = play_file(descriptor, &ao_enabled, &channel_enabled);
        close(descriptor);
        if (result < 0)
            close_output(&ao_enabled, &channel_enabled);
        pthread_mutex_lock(&playback.mutex);
        playback.error = result == AVERROR_EXIT ? 0 : result;
        playback.busy = 0;
        char message[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(result, message, sizeof(message));
        fprintf(stderr, "[audio] playback finished: result=%d (%s)\n", result, message);
    }
    if (playback.pending_fd >= 0) {
        close(playback.pending_fd);
        playback.pending_fd = -1;
    }
    playback.busy = 0;
    pthread_mutex_unlock(&playback.mutex);
    close_output(&ao_enabled, &channel_enabled);
    return NULL;
}

int app_ipcam_Playback_Start(void)
{
    APP_PARAM_AUDIO_CFG_T *parameters = app_ipcam_Audio_Param_Get();
    if (!parameters->bInit || !parameters->bCaptureOnly)
        return -ENODEV;
    pthread_mutex_lock(&playback.mutex);
    if (playback.running) {
        pthread_mutex_unlock(&playback.mutex);
        return 0;
    }
    playback.running = 1;
    playback.cancel = 0;
    playback.error = 0;
    int result = pthread_create(&playback.thread, NULL, playback_worker, NULL);
    if (result)
        playback.running = 0;
    pthread_mutex_unlock(&playback.mutex);
    return -result;
}

void app_ipcam_Playback_Stop(void)
{
    pthread_mutex_lock(&playback.mutex);
    int running = playback.running;
    playback.running = 0;
    playback.cancel = 1;
    pthread_cond_signal(&playback.wake);
    pthread_mutex_unlock(&playback.mutex);
    if (running)
        pthread_join(playback.thread, NULL);
    app_ipcam_Audio_SpeakerDisable();
}

int app_ipcam_Playback_Play(const char *filename)
{
    char path[PATH_MAX];
    if (!filename || !realpath(filename, path))
        return filename ? -errno : -EINVAL;
    if (strncmp(path, "/mnt/data/", 10) && strncmp(path, "/mnt/sd/", 8))
        return -EACCES;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (descriptor < 0)
        return -errno;
    struct stat status;
    int result = fstat(descriptor, &status) ? -errno : 0;
    if (!result && (!S_ISREG(status.st_mode) || !status.st_size))
        result = -EINVAL;
    pthread_mutex_lock(&playback.mutex);
    if (!result && !playback.running)
        result = -ENODEV;
    if (!result && playback.busy)
        result = -EBUSY;
    if (!result) {
        playback.pending_fd = descriptor;
        playback.busy = 1;
        playback.cancel = 0;
        playback.error = 0;
        pthread_cond_signal(&playback.wake);
    }
    pthread_mutex_unlock(&playback.mutex);
    if (result)
        close(descriptor);
    return result;
}

void app_ipcam_Playback_Cancel(void)
{
    pthread_mutex_lock(&playback.mutex);
    playback.cancel = 1;
    pthread_mutex_unlock(&playback.mutex);
}

static int query_value(const char *query, const char *name, char *value, size_t size)
{
    size_t length = strlen(name);
    while (query && *query) {
        const char *end = strchr(query, '&');
        if (!end)
            end = query + strlen(query);
        if ((size_t)(end - query) > length && !strncmp(query, name, length) && query[length] == '=') {
            size_t used = 0;
            for (const char *cursor = query + length + 1; cursor < end; ++cursor) {
                unsigned char character = *cursor;
                if (character == '%') {
                    if (end - cursor < 3 || !strchr("0123456789abcdefABCDEF", cursor[1]) ||
                        !strchr("0123456789abcdefABCDEF", cursor[2]))
                        return -EINVAL;
                    char hex[3] = {cursor[1], cursor[2], 0};
                    character = strtoul(hex, NULL, 16);
                    cursor += 2;
                } else if (character == '+') {
                    character = ' ';
                }
                if (!character || used + 1 >= size)
                    return -EINVAL;
                value[used++] = character;
            }
            value[used] = 0;
            return 0;
        }
        query = *end ? end + 1 : end;
    }
    return -ENOENT;
}

void app_ipcam_Playback_Control(const char *query, char *response, unsigned int size)
{
    char action[16];
    char filename[PATH_MAX];
    int result = query_value(query, "action", action, sizeof(action));
    if (result == -ENOENT) {
        strcpy(action, "status");
        result = 0;
    }
    if (!result) {
        if (!strcmp(action, "play")) {
            result = query_value(query, "file", filename, sizeof(filename));
            if (!result)
                result = app_ipcam_Playback_Play(filename);
        } else if (!strcmp(action, "stop")) {
            app_ipcam_Playback_Cancel();
        } else if (strcmp(action, "status")) {
            result = -EINVAL;
        }
    }
    pthread_mutex_lock(&playback.mutex);
    snprintf(response, size, "{\"result\":%d,\"info\":{\"available\":%s,\"busy\":%s,\"error\":%d}}",
        result, playback.running ? "true" : "false", playback.busy ? "true" : "false", playback.error);
    pthread_mutex_unlock(&playback.mutex);
}

#else
#include <errno.h>
#include <stdio.h>

int app_ipcam_Playback_Start(void) { return 0; }
void app_ipcam_Playback_Stop(void) {}
int app_ipcam_Playback_Play(const char *filename) { (void)filename; return -ENOTSUP; }
void app_ipcam_Playback_Cancel(void) {}
void app_ipcam_Playback_Control(const char *query, char *response, unsigned int size)
{
    (void)query;
    snprintf(response, size, "{\"result\":%d}", -ENOTSUP);
}
#endif
