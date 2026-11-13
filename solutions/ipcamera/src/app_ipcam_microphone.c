#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "app_ipcam_microphone.h"

#define MICROPHONE_DIRECTORY "/mnt/data/ipcamera-wifi"
#define MICROPHONE_SETTING MICROPHONE_DIRECTORY "/microphone-enabled"

struct microphone_native_volume {
    unsigned int gain;
    unsigned int muted;
};

#define MICROPHONE_MUTE_LEFT _IOWR('A', 16, unsigned int)
#define MICROPHONE_MUTE_RIGHT _IOWR('A', 17, unsigned int)
#define MICROPHONE_GET_LEFT _IOWR('A', 26, struct microphone_native_volume)
#define MICROPHONE_GET_RIGHT _IOWR('A', 27, struct microphone_native_volume)

static int microphone_lock(int operation)
{
    if (mkdir(MICROPHONE_DIRECTORY, 0700) && errno != EEXIST)
        return -1;
    int descriptor = open(MICROPHONE_DIRECTORY "/microphone.lock",
        O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (descriptor < 0)
        return -1;
    struct stat status;
    int error = fstat(descriptor, &status) ? errno : !S_ISREG(status.st_mode) ? EINVAL : 0;
    if (!error && flock(descriptor, operation))
        error = errno;
    if (error) {
        close(descriptor);
        errno = error;
        return -1;
    }
    return descriptor;
}

static int microphone_load(int *enabled)
{
    int descriptor = open(MICROPHONE_SETTING, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0)
        return errno;
    struct stat status;
    int error = fstat(descriptor, &status) ? errno : !S_ISREG(status.st_mode) ? EINVAL : 0;
    char buffer[4] = {0};
    ssize_t length = error ? -1 : read(descriptor, buffer, sizeof(buffer));
    if (!error && length < 0)
        error = errno;
    close(descriptor);
    if (error)
        return error;
    if ((length != 1 && length != 2) || (buffer[0] != '0' && buffer[0] != '1') ||
        (length == 2 && buffer[1] != '\n'))
        return EPROTO;
    *enabled = buffer[0] == '1';
    return 0;
}

static int microphone_sync(void)
{
    int descriptor = open(MICROPHONE_SETTING, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0)
        return errno;
    int error = fsync(descriptor) ? errno : 0;
    close(descriptor);
    if (error)
        return error;
    descriptor = open(MICROPHONE_DIRECTORY, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0)
        return errno;
    error = fsync(descriptor) ? errno : 0;
    close(descriptor);
    return error;
}

static int microphone_save(int enabled, int *installed)
{
    *installed = 0;
    char temporary[128];
    snprintf(temporary, sizeof(temporary), MICROPHONE_SETTING ".%ld", (long)getpid());
    int descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0)
        return errno;
    char buffer[] = { enabled ? '1' : '0', '\n' };
    ssize_t written;
    do {
        written = write(descriptor, buffer, sizeof(buffer));
    } while (written < 0 && errno == EINTR);
    int error = written < 0 ? errno : written != (ssize_t)sizeof(buffer) ? EIO : 0;
    if (!error && fsync(descriptor))
        error = errno;
    if (close(descriptor) && !error)
        error = errno;
    if (!error) {
        if (rename(temporary, MICROPHONE_SETTING))
            error = errno;
        else
            *installed = 1;
    }
    if (!error)
        error = microphone_sync();
    if (error)
        unlink(temporary);
    return error;
}

static int microphone_read(int descriptor, struct microphone_native_volume *left,
    struct microphone_native_volume *right)
{
    memset(left, 0, sizeof(*left));
    memset(right, 0, sizeof(*right));
    if (ioctl(descriptor, MICROPHONE_GET_LEFT, left) || ioctl(descriptor, MICROPHONE_GET_RIGHT, right))
        return errno;
    if (left->gain > 24 || right->gain > 24 || left->muted > 1 || (right->muted & ~2U))
        return EPROTO;
    return 0;
}

static int microphone_mute(int descriptor, unsigned int left, unsigned int right)
{
    int error = ioctl(descriptor, MICROPHONE_MUTE_LEFT, &left) ? errno : 0;
    if (ioctl(descriptor, MICROPHONE_MUTE_RIGHT, &right) && !error)
        error = errno;
    return error;
}

static void microphone_state(struct app_microphone_state *state,
    const struct microphone_native_volume *left, const struct microphone_native_volume *right)
{
    state->left_gain = left->gain;
    state->right_gain = right->gain;
    state->left_muted = left->muted != 0;
    state->right_muted = right->muted != 0;
}

int app_ipcam_Microphone_Status(struct app_microphone_state *state)
{
    if (!state)
        return EINVAL;
    memset(state, 0, sizeof(*state));
    int lock = microphone_lock(LOCK_SH | LOCK_NB);
    if (lock < 0)
        return errno;
    int descriptor = open("/dev/cvitekaadc", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    int error = descriptor < 0 ? errno : 0;
    struct microphone_native_volume left;
    struct microphone_native_volume right;
    if (!error)
        error = microphone_read(descriptor, &left, &right);
    if (!error) {
        microphone_state(state, &left, &right);
        int loaded = microphone_load(&state->enabled);
        if (!loaded)
            state->configured = 1;
        else if (loaded != ENOENT)
            error = loaded;
    }
    if (descriptor >= 0)
        close(descriptor);
    close(lock);
    return error;
}

int app_ipcam_Microphone_Apply(int enabled, struct app_microphone_state *state)
{
    if (!state || (enabled != 0 && enabled != 1))
        return EINVAL;
    memset(state, 0, sizeof(*state));
    int lock = microphone_lock(LOCK_EX | LOCK_NB);
    if (lock < 0)
        return errno;
    int descriptor = open("/dev/cvitekaadc", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    int error = descriptor < 0 ? errno : 0;
    struct microphone_native_volume previous_left;
    struct microphone_native_volume previous_right;
    struct microphone_native_volume left;
    struct microphone_native_volume right;
    int saved = 1;
    int loaded = microphone_load(&saved);
    if (!error && loaded && loaded != ENOENT)
        error = loaded;
    if (!error)
        error = microphone_read(descriptor, &previous_left, &previous_right);
    unsigned int muted = !enabled;
    int changed = !error && ((previous_left.muted != 0) != muted || (previous_right.muted != 0) != muted);
    if (!error && changed)
        error = microphone_mute(descriptor, muted, muted);
    if (!error)
        error = microphone_read(descriptor, &left, &right);
    if (!error && ((left.muted != 0) != muted || (right.muted != 0) != muted ||
        left.gain != previous_left.gain || right.gain != previous_right.gain))
        error = EIO;
    int verified = !error;
    int installed = 0;
    if (!error)
        error = loaded == ENOENT || saved != enabled ? microphone_save(enabled, &installed) : microphone_sync();
    if (error && changed && (!verified || (!installed && (loaded == ENOENT || saved != enabled)))) {
        int restored = microphone_mute(descriptor, previous_left.muted != 0, previous_right.muted != 0);
        if (!restored)
            restored = microphone_read(descriptor, &left, &right);
        if (!restored && (left.muted != previous_left.muted || right.muted != previous_right.muted ||
            left.gain != previous_left.gain || right.gain != previous_right.gain))
            restored = EIO;
        fprintf(stderr, "[microphone] apply failed errno=%d rollback_errno=%d\n", error, restored);
    }
    if (!error) {
        microphone_state(state, &left, &right);
        state->enabled = enabled;
        state->configured = 1;
        if (changed || loaded == ENOENT || saved != enabled)
            fprintf(stderr, "[microphone] applied enabled=%d previous_gain=%u/%u previous_mute=%u/%u gain=%u/%u mute=%u/%u; DAC and amplifier unchanged\n",
                enabled, previous_left.gain, previous_right.gain, previous_left.muted, previous_right.muted,
                left.gain, right.gain, left.muted, right.muted);
    }
    if (descriptor >= 0)
        close(descriptor);
    close(lock);
    return error;
}

int app_ipcam_Microphone_GuardFrame(int16_t *samples, size_t count)
{
    if (!samples || !count || count > SIZE_MAX / sizeof(*samples))
        return -EINVAL;
    int lock = microphone_lock(LOCK_SH);
    if (lock < 0)
        return -errno;
    int enabled = 1;
    int error = microphone_load(&enabled);
    if (error && error != ENOENT) {
        memset(samples, 0, count * sizeof(*samples));
        close(lock);
        return -error;
    }
    if (!enabled)
        memset(samples, 0, count * sizeof(*samples));
    return lock;
}
