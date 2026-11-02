#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <linux/msdos_fs.h>
#include <mntent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "app_ipcam_capture.h"
#include "app_ipcam_mp4.h"
#include "app_ipcam_storage.h"
#include "app_ipcam_boot_capture.h"
#include "app_ipcam_paramparse.h"
#include "cvi_mbuf.h"
#include "minIni.h"

#define CAPTURE_RUN "/run/ipcamera-capture"
#define CAPTURE_CFG "/mnt/cfg/ipcamera"
#define CAPTURE_STORAGE APP_STORAGE_MOUNT
#define CAPTURE_TASK_ROOT CAPTURE_STORAGE "/.capture"
#define CAPTURE_MEDIA_ROOT CAPTURE_STORAGE "/DCIM"
#define CAPTURE_PHOTO_DIR CAPTURE_MEDIA_ROOT "/PHOTO"
#define CAPTURE_MOVIE_DIR CAPTURE_MEDIA_ROOT "/MOVIE"

static pthread_t capture_thread;
static pthread_mutex_t capture_mutex = PTHREAD_MUTEX_INITIALIZER;
static int capture_started;
static int capture_stopping;

struct capture_task {
    int seconds;
    int channel;
    uint64_t storage_generation;
    uint64_t started_ms;
    uint64_t storage_ready_ms;
    uint64_t photo_started_ms;
    uint64_t photo_ready_ms;
    uint64_t recording_started_ms;
    uint64_t completed_ms;
    uint64_t recorded_ms;
    uint64_t video_bytes;
    char directory[PATH_MAX];
    char photo[PATH_MAX];
    char video[PATH_MAX];
#ifdef BF01_BOOT_CAPTURE
    char photos[BF01_BURST_COUNT][PATH_MAX];
    struct bf01_burst_reply burst;
#endif
};

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int capture_cancelled(void)
{
    pthread_mutex_lock(&capture_mutex);
    int stopping = capture_stopping;
    pthread_mutex_unlock(&capture_mutex);
    return stopping;
}

static int mounted_at(const char *path, int writable)
{
    FILE *mounts = setmntent("/proc/mounts", "r");
    if (!mounts)
        return 0;
    struct mntent entry;
    struct mntent *result;
    char buffer[4096];
    int found = 0;
    while ((result = getmntent_r(mounts, &entry, buffer, sizeof(buffer))) != NULL) {
        if (!strcmp(result->mnt_dir, path) && (!writable || !hasmntopt(result, "ro"))) {
            found = 1;
            break;
        }
    }
    endmntent(mounts);
    return found;
}

static int parse_number(const char *text, int minimum, int maximum, int *value)
{
    char *end;
    errno = 0;
    long number = strtol(text, &end, 10);
    if (errno || !*text || *end || number < minimum || number > maximum)
        return -1;
    *value = (int)number;
    return 0;
}

static int sync_directory(const char *path)
{
    char directory[PATH_MAX];
    if (strlen(path) >= sizeof(directory))
        return -1;
    strcpy(directory, path);
    char *name = strrchr(directory, '/');
    if (!name || name == directory)
        return -1;
    *name = '\0';
    int descriptor = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0)
        return -1;
    int result = fsync(descriptor);
    if (close(descriptor) != 0)
        result = -1;
    return result;
}

static int sync_file(const char *path)
{
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return -1;
    struct stat info;
    int result = fstat(descriptor, &info);
    if (result == 0 && (!S_ISREG(info.st_mode) || info.st_size <= 0)) {
        errno = EIO;
        result = -1;
    }
    if (result == 0)
        result = fsync(descriptor);
    if (close(descriptor) != 0)
        result = -1;
    return result;
}

static int publish_file(const char *temporary, const char *path)
{
    if (rename(temporary, path) != 0 || sync_file(path) != 0)
        return -1;
    return sync_directory(path);
}

static int save_file(const char *path, const char *text)
{
    char temporary[PATH_MAX];
    int length = snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
    if (length < 0 || (size_t)length >= sizeof(temporary))
        return -1;
    int descriptor = mkstemp(temporary);
    if (descriptor < 0)
        return -1;
    FILE *file = fdopen(descriptor, "w");
    if (!file) {
        close(descriptor);
        unlink(temporary);
        return -1;
    }
    int result = fputs(text, file) < 0 ? -1 : 0;
    if (fflush(file) != 0 || fsync(fileno(file)) != 0)
        result = -1;
    if (fclose(file) != 0)
        result = -1;
    if (result == 0 && publish_file(temporary, path) == 0)
        return 0;
    unlink(temporary);
    return -1;
}

static int hide_task_directory(void)
{
    int descriptor = open(CAPTURE_TASK_ROOT, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return -1;
    __u32 attributes;
    int result = ioctl(descriptor, FAT_IOCTL_GET_ATTRIBUTES, &attributes);
    if (result == 0) {
        attributes |= ATTR_HIDDEN | ATTR_SYS;
        result = ioctl(descriptor, FAT_IOCTL_SET_ATTRIBUTES, &attributes);
    }
    if (result == 0) {
        result = ioctl(descriptor, FAT_IOCTL_GET_ATTRIBUTES, &attributes);
        if (result == 0 && (attributes & (ATTR_HIDDEN | ATTR_SYS)) != (ATTR_HIDDEN | ATTR_SYS)) {
            errno = EIO;
            result = -1;
        }
    }
    if (result == 0)
        result = fsync(descriptor);
    int error = errno;
    if (close(descriptor) != 0 && result == 0)
        return -1;
    errno = error;
    return result;
}

static int prepare_task_directory(void)
{
    struct stat info;
    if (mkdir(CAPTURE_TASK_ROOT, 0700) != 0 && errno != EEXIST)
        return -1;
    if (lstat(CAPTURE_TASK_ROOT, &info) != 0 || !S_ISDIR(info.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    if (lstat(CAPTURE_STORAGE "/capture", &info) == 0) {
        if (!S_ISDIR(info.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
        char legacy[PATH_MAX];
        int moved = 0;
        for (unsigned int index = 0; index < 1000; index++) {
            int length = index == 0
                ? snprintf(legacy, sizeof(legacy), "%s/legacy_capture", CAPTURE_TASK_ROOT)
                : snprintf(legacy, sizeof(legacy), "%s/legacy_capture_%03u", CAPTURE_TASK_ROOT, index);
            if (length < 0 || (size_t)length >= sizeof(legacy)) {
                errno = ENAMETOOLONG;
                return -1;
            }
            if (lstat(legacy, &info) == 0)
                continue;
            if (errno != ENOENT)
                return -1;
            if (rename(CAPTURE_STORAGE "/capture", legacy) != 0)
                return -1;
            moved = 1;
            break;
        }
        if (!moved) {
            errno = EEXIST;
            return -1;
        }
        if (sync_directory(legacy) != 0)
            return -1;
    } else if (errno != ENOENT) {
        return -1;
    }
    if (hide_task_directory() != 0)
        fprintf(stderr, "[capture] warning: cannot set FAT hidden/system attributes on %s: %s; dot-directory remains\n",
            CAPTURE_TASK_ROOT, strerror(errno));
    return sync_directory(CAPTURE_TASK_ROOT);
}

static int prepare_media_directory(const char *path)
{
    struct stat info;
    if (mkdir(CAPTURE_MEDIA_ROOT, 0755) != 0 && errno != EEXIST)
        return -1;
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return -1;
    if (stat(path, &info) != 0 || !S_ISDIR(info.st_mode))
        return -1;
    if (sync_directory(CAPTURE_MEDIA_ROOT) != 0)
        return -1;
    return sync_directory(path);
}

static int next_media_path(const char *directory, const char *extension, char *path, size_t path_size)
{
    DIR *entries = opendir(directory);
    unsigned int next = 1;
    int length;

    if (!entries)
        return -1;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(entries);
        if (!entry) {
            int error = errno;
            if (closedir(entries) != 0)
                return -1;
            if (error) {
                errno = error;
                return -1;
            }
            break;
        }
        if (strncasecmp(entry->d_name, "DSCF", 4) ||
            entry->d_name[4] < '0' || entry->d_name[4] > '9')
            continue;
        char *suffix;
        unsigned long index = strtoul(entry->d_name + 4, &suffix, 10);
        int overflow = errno == ERANGE || index >= UINT_MAX;
        size_t extensionLength = strlen(extension);
        if (strcasecmp(suffix, extension) &&
            (strncasecmp(suffix, extension, extensionLength) ||
            strcasecmp(suffix + extensionLength, ".part")))
            continue;
        if (overflow) {
            closedir(entries);
            errno = EOVERFLOW;
            return -1;
        }
        if (index >= next)
            next = (unsigned int)index + 1;
    }
    length = snprintf(path, path_size, "%s/DSCF%05u%s", directory, next, extension);
    if (length < 0 || (size_t)length >= path_size)
        return -1;
    return 0;
}

static int update_status(const struct capture_task *task, const char *state, int error, int persistent)
{
    char text[PATH_MAX * 7 + 2048];
    uint64_t photoElapsed = task->photo_ready_ms ? task->photo_ready_ms - task->photo_started_ms : 0;
    uint64_t elapsed = (task->completed_ms ? task->completed_ms : monotonic_ms()) - task->started_ms;
    int length = snprintf(text, sizeof(text),
        "[task]\nstate=%s\nerror=%d\nduration_seconds=%d\nchannel=%d\n"
        "directory=%s\nphoto=%s\nvideo=%s\nai_state=%s\nupload_decision=pending\n"
        "task_started_uptime_ms=%llu\nstorage_ready_uptime_ms=%llu\nphoto_elapsed_ms=%llu\n"
        "photo_ready_uptime_ms=%llu\nrecording_started_uptime_ms=%llu\ncompleted_uptime_ms=%llu\n"
        "recorded_ms=%llu\nvideo_bytes=%llu\nelapsed_ms=%llu\n",
        state, error, task->seconds, task->channel, task->directory, task->photo, task->video,
        !strcmp(state, "awaiting_ai") ? "pending" : "not_submitted",
        (unsigned long long)task->started_ms, (unsigned long long)task->storage_ready_ms,
        (unsigned long long)photoElapsed, (unsigned long long)task->photo_ready_ms,
        (unsigned long long)task->recording_started_ms, (unsigned long long)task->completed_ms,
        (unsigned long long)task->recorded_ms, (unsigned long long)task->video_bytes,
        (unsigned long long)elapsed);
    if (length < 0 || (size_t)length >= sizeof(text))
        return -1;
#ifdef BF01_BOOT_CAPTURE
    for (unsigned int index = 0; index < BF01_BURST_COUNT; index++) {
        const struct bf01_burst_image *image = &task->burst.images[index];
        uint64_t pts = ((uint64_t)image->pts_high << 32) | image->pts_low;
        int appended = snprintf(text + length, sizeof(text) - length,
            "photo%u=%s\nphoto%u_rtos_ms=%u\nphoto%u_pts_us=%llu\n",
            index + 1, task->photos[index], index + 1, image->captured_ms,
            index + 1, (unsigned long long)pts);
        if (appended < 0 || (size_t)appended >= sizeof(text) - length)
            return -1;
        length += appended;
    }
#endif
    int result = 0;
    if (persistent && *task->directory) {
        char manifest[PATH_MAX];
        length = snprintf(manifest, sizeof(manifest), "%s/task.ini", task->directory);
        if (app_ipcam_Storage_Check(task->storage_generation) != 0 ||
            length < 0 || (size_t)length >= sizeof(manifest) || save_file(manifest, text) != 0) {
            fprintf(stderr, "[capture] cannot commit task manifest: %s\n", task->directory);
            result = -1;
        }
    }
    if (result != 0 && !strcmp(state, "awaiting_ai"))
        return -1;
    if (save_file(CAPTURE_RUN "/status.ini", text) != 0) {
        fprintf(stderr, "[capture] cannot save runtime status\n");
        result = -1;
    }
    fprintf(stderr, "[capture] state=%s error=%d channel=%d duration=%ds recorded_ms=%llu "
        "video_bytes=%llu elapsed_ms=%llu task=%s\n", state, error, task->channel, task->seconds,
        (unsigned long long)task->recorded_ms, (unsigned long long)task->video_bytes,
        (unsigned long long)elapsed, task->directory);
    return result;
}

static int task_status(const struct capture_task *task, const char *state, int error)
{
    return update_status(task, state, error, 1);
}

static int load_settings(struct capture_task *task, int *enabled)
{
    char config[PATH_MAX];
    strcpy(config, CAPTURE_CFG "/capture.ini");
    if (access(config, F_OK) != 0) {
        if (errno != ENOENT)
            return -1;
        ssize_t length = readlink("/proc/self/exe", config, sizeof(config) - 1);
        if (length <= 0 || (size_t)length >= sizeof(config) - 1)
            return -1;
        config[length] = '\0';
        char *name = strrchr(config, '/');
        if (!name || (size_t)(name - config) + sizeof("/capture.ini") > sizeof(config))
            return -1;
        strcpy(name, "/capture.ini");
    }
    if (access(config, F_OK) != 0 && errno == ENOENT) {
        *enabled = 1;
        task->seconds = 10;
        task->channel = 0;
        return 0;
    }
    if (access(config, R_OK) != 0)
        return -1;
    char text[32];
    ini_gets("capture", "enabled", "1", text, sizeof(text), config);
    if (parse_number(text, 0, 1, enabled) != 0)
        return -1;
    ini_gets("capture", "duration_seconds", "10", text, sizeof(text), config);
    if (parse_number(text, 1, 300, &task->seconds) != 0)
        return -1;
    ini_gets("capture", "channel", "0", text, sizeof(text), config);
    return parse_number(text, 0, 2, &task->channel);
}

int app_ipcam_Capture_Configure(const char *seconds, const char *channel)
{
    int duration;
    int selected;
    if (parse_number(seconds, 1, 300, &duration) != 0 ||
        parse_number(channel, 0, 2, &selected) != 0) {
        fprintf(stderr, "Usage: ipcamera --capture-config SECONDS(1-300) CHANNEL(0-2)\n");
        return -1;
    }
    if (!mounted_at("/mnt/cfg", 1) ||
        (mkdir(CAPTURE_CFG, 0700) != 0 && errno != EEXIST)) {
        fprintf(stderr, "[capture] writable /mnt/cfg must be mounted before saving settings\n");
        return -1;
    }
    char text[128];
    snprintf(text, sizeof(text), "[capture]\nenabled=1\nduration_seconds=%d\nchannel=%d\n", duration, selected);
    if (save_file(CAPTURE_CFG "/capture.ini", text) != 0)
        return -1;
    printf("[capture] saved duration=%ds channel=%d; applies on the next application start\n", duration, selected);
    return 0;
}

#ifdef AUDIO_SUPPORT
static int record_audio(struct app_mp4 *output, int64_t clock_offset, int boot_video,
    uint64_t first_pts, uint64_t through_pts)
{
    for (unsigned int index = 0; index < 64; index++) {
        APP_AUDIO_PCM_FRAME frame;
        int received;
#ifdef BF01_BOOT_CAPTURE
        if (boot_video) {
            received = app_ipcam_BootVideo_ReadAudio(frame.samples,
                sizeof(frame.samples) / sizeof(frame.samples[0]), &frame.pts, first_pts, through_pts);
            if (received == -EAGAIN)
                break;
            if (received > 0)
                frame.count = (unsigned int)received;
        } else
#endif
        {
            received = app_ipcam_Audio_CaptureRead(&frame);
        }
#ifndef BF01_BOOT_CAPTURE
        (void)boot_video;
        (void)first_pts;
        (void)through_pts;
#endif
        if (received < 0)
            return -received;
        if (!received)
            break;
        if (frame.pts > INT64_MAX || (clock_offset > 0 && frame.pts > (uint64_t)(INT64_MAX - clock_offset)) ||
            (clock_offset < 0 && frame.pts < (uint64_t)-clock_offset))
            return EIO;
        uint64_t pts = (uint64_t)((int64_t)frame.pts + clock_offset);
        if (app_mp4_write_pcm(output, frame.samples, frame.count, pts) != 0)
            return EIO;
        if (app_mp4_audio_complete(output))
            break;
    }
    return 0;
}
#endif

static int record_clip(struct capture_task *task)
{
    APP_PARAM_VENC_CTX_S *context = app_ipcam_Venc_Param_Get();
    if (task->channel >= context->s32VencChnCnt)
        return EINVAL;
    APP_VENC_CHN_CFG_S *encoder = &context->astVencChnCfg[task->channel];
    int boot_video = 0;
#ifdef BF01_BOOT_CAPTURE
    boot_video = app_ipcam_BootVideo_Available();
#endif
    if (!encoder->bEnable || !encoder->bStart || (!boot_video && encoder->enType != PT_H264) ||
        (!boot_video && !(encoder->StreamTo & 1)) || encoder->enGopMode == VENC_GOPMODE_BIPREDB)
        return EINVAL;
    if (boot_video)
        fprintf(stderr, "[capture] using RTOS early video ring before Linux VENC stream\n");
    CVI_MBUF_HANDLE *reader = boot_video ? NULL : app_ipcam_Mbuf_CreateReader(encoder->VencChn, 1);
    CVI_MEDIA_FRAME_INFO_T frame = {0};
    frame.frameBuf = malloc(CVI_MBUF_STREAM_MAX_SIZE);
    struct app_mp4 *output = NULL;
    int error = 0;
    int started = 0;
    int complete = 0;
    int audio = 0;
    int64_t audioClockOffset = 0;
    uint64_t firstPts = 0;
    uint64_t previousPts = 0;
    uint64_t lastProgressMs = 0;
    struct timespec lastFrame;
    clock_gettime(CLOCK_MONOTONIC, &lastFrame);
    struct timespec recordingBegin = lastFrame;
    if ((!boot_video && !reader) || !frame.frameBuf) {
        error = ENOMEM;
        goto done;
    }
    char partial[PATH_MAX];
    int length = snprintf(partial, sizeof(partial), "%s.part", task->video);
    if (length < 0 || (size_t)length >= sizeof(partial)) {
        error = ENAMETOOLONG;
        goto done;
    }
    unsigned int output_width = encoder->u32Width;
    unsigned int output_height = encoder->u32Height;
#ifdef BF01_BOOT_CAPTURE
    if (boot_video && app_ipcam_BootVideo_GetSize(&output_width, &output_height) != 0) {
        error = ENODEV;
        goto done;
    }
#endif
    output = boot_video ? app_mp4_open_h265(partial, output_width, output_height) :
        app_mp4_open(partial, output_width, output_height);
    if (!output) {
        error = EIO;
        goto done;
    }
#ifdef AUDIO_SUPPORT
    if (boot_video || app_ipcam_Audio_Param_Get()->bCaptureOnly) {
        error = boot_video ? 0 : app_ipcam_Audio_CaptureBegin();
        if (error)
            goto done;
        audio = 1;
        if (app_mp4_enable_audio(output, 16000, task->seconds) != 0) {
            error = EIO;
            goto done;
        }
    }
#else
    if (boot_video) {
        error = ENOTSUP;
        goto done;
    }
#endif
    frame.frameBufLen = CVI_MBUF_STREAM_MAX_SIZE;
    if (!boot_video) {
        app_ipcam_Mbuf_ReadFrame(reader, 0, &frame, 0);
        if (CVI_VENC_RequestIDR(encoder->VencChn, CVI_TRUE) != CVI_SUCCESS) {
            error = EIO;
            goto done;
        }
    }
    while (!capture_cancelled()) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - lastFrame.tv_sec >= 10 ||
            now.tv_sec - recordingBegin.tv_sec >= task->seconds + 15) {
            error = ETIMEDOUT;
            break;
        }
        error = app_ipcam_Storage_Check(task->storage_generation);
        if (error) {
            break;
        }
        int received;
#ifdef BF01_BOOT_CAPTURE
        if (boot_video) {
            struct bf01_video_frame boot_frame;
            received = app_ipcam_BootVideo_ReadFrame(frame.frameBuf, CVI_MBUF_STREAM_MAX_SIZE,
                &boot_frame, 100);
            if (received == -EAGAIN) {
                usleep(10 * 1000);
                continue;
            }
            if (received == -ENODATA) {
                error = EIO;
                break;
            }
            if (received < 0) {
                error = -received;
                break;
            }
            frame.frameParam.frameLen = (unsigned int)received;
            frame.frameParam.frameType = (boot_frame.flags & BF01_VIDEO_FLAG_KEY) ?
                CVI_MEDIA_VFRAME_I : CVI_MEDIA_VFRAME_P;
            frame.frameParam.frameCodec = PT_H265;
            frame.frameParam.framePts = ((uint64_t)boot_frame.pts_high << 32) | boot_frame.pts_low;
            frame.frameParam.frameIndex = boot_frame.sequence;
        } else
#endif
        {
            frame.frameBufLen = CVI_MBUF_STREAM_MAX_SIZE;
            received = app_ipcam_Mbuf_ReadFrame(reader, 0, &frame, 100);
            if (received <= 0 || frame.frameParam.frameType == CVI_MEDIA_AFRAME_A) {
                usleep(10 * 1000);
                continue;
            }
        }
        if (!frame.frameParam.frameLen || frame.frameParam.frameLen > CVI_MBUF_STREAM_MAX_SIZE) {
            error = EIO;
            break;
        }
        if (!started && frame.frameParam.frameType != CVI_MEDIA_VFRAME_I)
            continue;
        uint64_t pts = frame.frameParam.framePts;
        if (started && pts <= previousPts) {
            error = EIO;
            break;
        }
        if (!started) {
            firstPts = pts;
#ifdef AUDIO_SUPPORT
            if (audio && !boot_video) {
                CVI_U64 audioClock = 0;
                if (CVI_SYS_GetCurPTS(&audioClock) != CVI_SUCCESS || audioClock > INT64_MAX || pts > INT64_MAX) {
                    error = EIO;
                    break;
                }
                audioClockOffset = (int64_t)pts - (int64_t)audioClock;
                fprintf(stderr, "[audio] video/PCM clock mapping: offset_us=%lld\n", (long long)audioClockOffset);
            }
#endif
        }
        task->recorded_ms = (pts - firstPts) / 1000;
        if (started && pts - firstPts >= (uint64_t)task->seconds * 1000000) {
#ifdef AUDIO_SUPPORT
            uint64_t deadline = monotonic_ms() + 2000;
            while (audio && !app_mp4_audio_complete(output)) {
                error = app_ipcam_Storage_Check(task->storage_generation);
                if (!error && capture_cancelled())
                    error = ECANCELED;
                if (!error)
                    error = record_audio(output, audioClockOffset, boot_video, firstPts,
                        firstPts + (uint64_t)task->seconds * 1000000);
                if (!error && monotonic_ms() >= deadline)
                    error = ETIMEDOUT;
                if (error)
                    break;
                if (!app_mp4_audio_complete(output))
                    usleep(10 * 1000);
            }
            if (error)
                break;
#endif
            int finished = app_mp4_finish(output, firstPts + (uint64_t)task->seconds * 1000000);
            output = NULL;
            if (finished != 0) {
                error = EIO;
                break;
            }
            if (sync_file(partial) != 0 || publish_file(partial, task->video) != 0) {
                error = EIO;
                break;
            }
            struct stat info;
            if (stat(task->video, &info) != 0 || info.st_size <= 0) {
                error = EIO;
                break;
            }
            task->recorded_ms = (uint64_t)task->seconds * 1000;
            task->video_bytes = (uint64_t)info.st_size;
            task->completed_ms = monotonic_ms();
            complete = 1;
            break;
        }
        if (app_mp4_write(output, frame.frameBuf, frame.frameParam.frameLen, pts) != 0) {
            error = EIO;
            break;
        }
#ifdef AUDIO_SUPPORT
        if (audio && (error = record_audio(output, audioClockOffset, boot_video, firstPts, pts)) != 0)
            break;
#endif
        if (!started) {
            task->recording_started_ms = monotonic_ms();
            fprintf(stderr, "[capture] recording channel %d from first keyframe; uptime_ms=%llu\n",
                task->channel, (unsigned long long)task->recording_started_ms);
        }
        started = 1;
        previousPts = pts;
        lastFrame = now;
        if (task->recorded_ms >= lastProgressMs + 1000) {
            struct stat info;
            if (stat(partial, &info) == 0)
                task->video_bytes = (uint64_t)info.st_size;
            if (update_status(task, "recording", 0, 0) != 0) {
                error = EIO;
                break;
            }
            lastProgressMs = task->recorded_ms;
        }
    }
    if (!complete && !error)
        error = ECANCELED;
done:
#ifdef BF01_BOOT_CAPTURE
    if (boot_video)
        app_ipcam_BootVideo_Stop();
#endif
#ifdef AUDIO_SUPPORT
    if (audio && !boot_video)
        app_ipcam_Audio_CaptureEnd();
#else
    (void)audio;
    (void)audioClockOffset;
#endif
    app_mp4_abort(output);
    if (reader)
        app_ipcam_Mbuf_DestoryReader(reader);
    free(frame.frameBuf);
    return error;
}

#ifdef BF01_BOOT_CAPTURE
static int save_boot_burst(struct capture_task *task)
{
    if (!task->burst.count || task->burst.count > BF01_BURST_COUNT)
        return EPROTO;
    unsigned char buffer[16384];
    for (unsigned int index = 0; index < task->burst.count; index++) {
        char source[128];
        char partial[PATH_MAX];
        snprintf(source, sizeof(source), APP_BOOT_CAPTURE_DIR "/photo%u.jpg", index);
        if (capture_cancelled())
            return ECANCELED;
        if (app_ipcam_Storage_Check(task->storage_generation) != 0)
            return ENODEV;
        if (next_media_path(CAPTURE_PHOTO_DIR, ".jpg", task->photos[index], sizeof(task->photos[index])) != 0)
            return EIO;
        int length = snprintf(partial, sizeof(partial), "%s.part", task->photos[index]);
        if (length < 0 || (size_t)length >= sizeof(partial))
            return ENAMETOOLONG;
        FILE *input = fopen(source, "rb");
        if (!input)
            return errno;
        FILE *output = fopen(partial, "wbx");
        if (!output) {
            int error = errno;
            fclose(input);
            return error;
        }
        uint32_t crc = UINT32_MAX;
        size_t total = 0;
        int error = 0;
        for (;;) {
            if (capture_cancelled() || app_ipcam_Storage_Check(task->storage_generation) != 0) {
                error = capture_cancelled() ? ECANCELED : ENODEV;
                break;
            }
            size_t count = fread(buffer, 1, sizeof(buffer), input);
            if (!count) {
                if (ferror(input))
                    error = EIO;
                break;
            }
            if (count > task->burst.images[index].size - total || fwrite(buffer, 1, count, output) != count) {
                error = EIO;
                break;
            }
            total += count;
            crc = bf01_burst_crc(crc, buffer, count);
        }
        if (fclose(input) != 0 || total != task->burst.images[index].size ||
            (crc ^ UINT32_MAX) != task->burst.images[index].crc)
            error = EIO;
        if (fflush(output) != 0 || fsync(fileno(output)) != 0)
            error = EIO;
        if (fclose(output) != 0)
            error = EIO;
        if (!error && (app_ipcam_Storage_Check(task->storage_generation) != 0 ||
            publish_file(partial, task->photos[index]) != 0))
            error = EIO;
        if (error) {
            unlink(partial);
            return error;
        }
        if (index == 0)
            strcpy(task->photo, task->photos[0]);
        fprintf(stderr, "[boot-capture] SD committed photo=%u path=%s\n", index + 1, task->photos[index]);
    }
    return 0;
}
#endif

static void *capture_boot_task(void *opaque)
{
    struct capture_task task = { .seconds = 10, .channel = 0, .started_ms = monotonic_ms() };
    int enabled;
    int error = 0;
#ifdef BF01_BOOT_CAPTURE
    int photos_saved = 0;
    int transfer_error = 0;
#endif
    (void)opaque;
    if (load_settings(&task, &enabled) != 0) {
        task_status(&task, "failed", EINVAL);
        return NULL;
    }
    if (!enabled) {
#ifdef BF01_BOOT_CAPTURE
        app_ipcam_BootVideo_Stop();
#endif
        task_status(&task, "disabled", 0);
        return NULL;
    }
#ifdef BF01_BOOT_CAPTURE
    task_status(&task, "receiving_rtos_photos", 0);
    for (;;) {
        if (capture_cancelled()) {
            task_status(&task, "interrupted", ECANCELED);
            return NULL;
        }
        error = app_ipcam_BootCapture_Stage(&task.burst, capture_cancelled);
        if (!error)
            break;
        if (task.burst.state != BF01_BURST_FAILED &&
            (error == ENOMEM || error == EAGAIN || error == ETIMEDOUT)) {
            if (error != transfer_error) {
                fprintf(stderr, "[boot-capture] RTOS transfer pending: error=%d; retaining photos and retrying\n", error);
                task_status(&task, "receiving_rtos_photos", error);
                transfer_error = error;
            }
            usleep(200 * 1000);
            continue;
        }
        fprintf(stderr, "[boot-capture] RTOS photo staging failed: %d; no Linux recapture\n", error);
        task_status(&task, error == EALREADY ? "photos_already_saved" :
            error == ECANCELED ? "interrupted" : "failed", error);
        return NULL;
    }
wait_for_storage:
#endif
    task_status(&task, "waiting_for_storage", 0);
    while (app_ipcam_Storage_Acquire(&task.storage_generation) != 0) {
        if (capture_cancelled()) {
            task_status(&task, "interrupted", ECANCELED);
            return NULL;
        }
        usleep(200 * 1000);
    }
    if (capture_cancelled()) {
        task_status(&task, "interrupted", ECANCELED);
        app_ipcam_Storage_Release();
        return NULL;
    }
    task.storage_ready_ms = monotonic_ms();
    if (prepare_task_directory() != 0) {
        error = EIO;
        goto failed;
    }
    if (prepare_media_directory(CAPTURE_PHOTO_DIR) != 0 ||
        prepare_media_directory(CAPTURE_MOVIE_DIR) != 0) {
        error = EIO;
        goto failed;
    }
    strcpy(task.directory, CAPTURE_TASK_ROOT "/task_XXXXXX");
    if (!mkdtemp(task.directory)) {
        task.directory[0] = '\0';
        error = errno;
        goto failed;
    }
    if (next_media_path(CAPTURE_PHOTO_DIR, ".jpg", task.photo, sizeof(task.photo)) != 0 ||
        next_media_path(CAPTURE_MOVIE_DIR, ".mp4", task.video, sizeof(task.video)) != 0) {
        error = EIO;
        goto failed;
    }
    if (sync_directory(task.directory) != 0) {
        error = EIO;
        goto failed;
    }
    if (task_status(&task, "capturing_photo", 0) != 0) {
        error = EIO;
        goto failed;
    }
#ifdef BF01_BOOT_CAPTURE
    task.photo_started_ms = monotonic_ms();
    error = save_boot_burst(&task);
    if (error)
        goto failed;
    task.photo_ready_ms = monotonic_ms();
    if (task_status(&task, "photos_saved", 0) != 0) {
        error = EIO;
        goto failed;
    }
    photos_saved = 1;
    error = app_ipcam_BootCapture_Commit(&task.burst);
    if (error)
        fprintf(stderr, "[boot-capture] SD photos saved; RTOS release not acknowledged: %d\n", error);
#else
    char partial[PATH_MAX];
    int length = snprintf(partial, sizeof(partial), "%s.part", task.photo);
    if (length < 0 || (size_t)length >= sizeof(partial)) {
        error = ENAMETOOLONG;
        goto failed;
    }
    task.photo_started_ms = monotonic_ms();
    if (app_ipcam_Jpeg_CaptureTo(partial) != CVI_SUCCESS ||
        app_ipcam_Storage_Check(task.storage_generation) != 0 || publish_file(partial, task.photo) != 0) {
        error = EIO;
        unlink(partial);
        goto failed;
    }
    task.photo_ready_ms = monotonic_ms();
#endif
    if (capture_cancelled()) {
        error = ECANCELED;
        goto failed;
    }
    if (task_status(&task, "recording", 0) != 0) {
        error = EIO;
        goto failed;
    }
    error = record_clip(&task);
    if (error)
        goto failed;
    if (task_status(&task, "awaiting_ai", 0) == 0) {
        app_ipcam_Storage_Release();
        return NULL;
    }
    error = EIO;
failed:
#ifdef BF01_BOOT_CAPTURE
    if (!photos_saved && !capture_cancelled() &&
        (error == EIO || error == ENODEV || error == EROFS || error == ENOSPC)) {
        fprintf(stderr, "[boot-capture] SD save pending: error=%d; retaining photos until storage is writable\n", error);
        app_ipcam_Storage_ReportError(task.storage_generation, error);
        app_ipcam_Storage_Release();
        task.directory[0] = '\0';
        task.photo[0] = '\0';
        task.video[0] = '\0';
        memset(task.photos, 0, sizeof(task.photos));
        task.storage_ready_ms = 0;
        task.photo_started_ms = 0;
        task.photo_ready_ms = 0;
        task_status(&task, "waiting_for_storage", error);
        usleep(500 * 1000);
        goto wait_for_storage;
    }
#endif
    task_status(&task, error == ECANCELED ? "interrupted" : "failed", error);
    app_ipcam_Storage_Release();
    return NULL;
}

int app_ipcam_Capture_Start(void)
{
    if (mkdir(CAPTURE_RUN, 0700) != 0 && errno != EEXIST)
        return -1;
    pthread_mutex_lock(&capture_mutex);
    if (capture_started) {
        pthread_mutex_unlock(&capture_mutex);
        return 0;
    }
    capture_stopping = 0;
    int result = pthread_create(&capture_thread, NULL, capture_boot_task, NULL);
    if (!result)
        capture_started = 1;
    pthread_mutex_unlock(&capture_mutex);
    return result;
}

void app_ipcam_Capture_Stop(void)
{
    pthread_mutex_lock(&capture_mutex);
    int started = capture_started;
    capture_stopping = 1;
    pthread_mutex_unlock(&capture_mutex);
    if (started) {
        pthread_join(capture_thread, NULL);
        pthread_mutex_lock(&capture_mutex);
        capture_started = 0;
        pthread_mutex_unlock(&capture_mutex);
    }
}
