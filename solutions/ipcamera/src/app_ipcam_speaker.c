#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "app_ipcam_speaker.h"

#define SPEAKER_DIRECTORY "/mnt/data/ipcamera-wifi"
#define SPEAKER_SETTING SPEAKER_DIRECTORY "/speaker-volume"

struct speaker_native_volume {
    unsigned int gain;
    unsigned int muted;
};

#define SPEAKER_SET_LEFT _IOWR('A', 12, struct speaker_native_volume)
#define SPEAKER_SET_RIGHT _IOWR('A', 13, struct speaker_native_volume)
#define SPEAKER_GET_LEFT _IOWR('A', 24, struct speaker_native_volume)
#define SPEAKER_GET_RIGHT _IOWR('A', 25, struct speaker_native_volume)

static int speaker_lock(void)
{
    if (mkdir(SPEAKER_DIRECTORY, 0700) && errno != EEXIST)
        return -1;
    int descriptor = open(SPEAKER_DIRECTORY "/speaker.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor < 0)
        return -1;
    if (flock(descriptor, LOCK_EX | LOCK_NB)) {
        int error = errno;
        close(descriptor);
        errno = error;
        return -1;
    }
    return descriptor;
}

static int speaker_load(unsigned int *percent)
{
    int descriptor = open(SPEAKER_SETTING, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return errno;
    char buffer[32];
    ssize_t length = read(descriptor, buffer, sizeof(buffer) - 1);
    int error = length < 0 ? errno : 0;
    close(descriptor);
    if (error)
        return error;
    if (length <= 0 || (size_t)length == sizeof(buffer) - 1)
        return EPROTO;
    buffer[length] = 0;
    if (buffer[0] < '0' || buffer[0] > '9')
        return EPROTO;
    char *end;
    errno = 0;
    unsigned long parsed = strtoul(buffer, &end, 10);
    if (errno || parsed > 100 || parsed % 5 || (*end && strcmp(end, "\n")))
        return EPROTO;
    *percent = (unsigned int)parsed;
    return 0;
}

static int speaker_save(unsigned int percent)
{
    char temporary[128];
    snprintf(temporary, sizeof(temporary), SPEAKER_SETTING ".%ld", (long)getpid());
    int descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor < 0)
        return errno;
    char buffer[16];
    int length = snprintf(buffer, sizeof(buffer), "%u\n", percent);
    int error = write(descriptor, buffer, (size_t)length) == length ? 0 : errno ? errno : EIO;
    if (!error && fsync(descriptor))
        error = errno;
    if (close(descriptor) && !error)
        error = errno;
    if (!error && rename(temporary, SPEAKER_SETTING))
        error = errno;
    if (!error) {
        int directory = open(SPEAKER_DIRECTORY, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0)
            error = errno;
        else {
            if (fsync(directory))
                error = errno;
            close(directory);
        }
    }
    if (error)
        unlink(temporary);
    return error;
}

static int speaker_read(int descriptor, struct speaker_native_volume *left, struct speaker_native_volume *right)
{
    memset(left, 0, sizeof(*left));
    memset(right, 0, sizeof(*right));
    if (ioctl(descriptor, SPEAKER_GET_LEFT, left) || ioctl(descriptor, SPEAKER_GET_RIGHT, right))
        return errno;
    if (left->gain > 32 || right->gain > 32 || left->muted > 1 || right->muted > 1)
        return EPROTO;
    return 0;
}

static int speaker_matches(const struct speaker_native_volume *observed, unsigned int gain, unsigned int muted)
{
    return observed->muted == muted && (muted || observed->gain == gain);
}

static void speaker_state(struct app_speaker_state *state, const struct speaker_native_volume *left,
                          const struct speaker_native_volume *right)
{
    state->left_gain = left->gain;
    state->left_muted = left->muted;
    state->right_gain = right->gain;
    state->right_muted = right->muted;
}

static int speaker_apply_locked(unsigned int percent, struct app_speaker_state *state, int persist)
{
    _Static_assert(sizeof(struct speaker_native_volume) == 8, "CV181x DAC volume ABI mismatch");
    int descriptor = open("/dev/cvitekadac", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return errno;
    struct speaker_native_volume previous_left;
    struct speaker_native_volume previous_right;
    int error = speaker_read(descriptor, &previous_left, &previous_right);
    if (error) {
        close(descriptor);
        return error;
    }
    struct speaker_native_volume target = { .gain = (percent * 32 + 50) / 100, .muted = percent == 0 };
    int changed = !speaker_matches(&previous_left, target.gain, target.muted) ||
        !speaker_matches(&previous_right, target.gain, target.muted);
    if (changed && (ioctl(descriptor, SPEAKER_SET_LEFT, &target) || ioctl(descriptor, SPEAKER_SET_RIGHT, &target)))
        error = errno;
    struct speaker_native_volume left;
    struct speaker_native_volume right;
    if (!error)
        error = speaker_read(descriptor, &left, &right);
    if (!error && (!speaker_matches(&left, target.gain, target.muted) ||
                   !speaker_matches(&right, target.gain, target.muted)))
        error = EIO;
    unsigned int saved = 0;
    if (!error && persist && (speaker_load(&saved) || saved != percent))
        error = speaker_save(percent);
    if (error && changed) {
        int restored_left = ioctl(descriptor, SPEAKER_SET_LEFT, &previous_left);
        int restored_right = ioctl(descriptor, SPEAKER_SET_RIGHT, &previous_right);
        if (restored_left || restored_right)
            fprintf(stderr, "[speaker] rollback failed; left=%d right=%d\n", restored_left, restored_right);
    }
    if (!error) {
        speaker_state(state, &left, &right);
        state->percent = percent;
        state->configured = 1;
        if (changed)
            fprintf(stderr, "[speaker] applied percent=%u previous_gain=%u/%u previous_mute=%u/%u gain=%u mute=%u readback=%u/%u; ADC and amplifier unchanged\n",
                    percent, previous_left.gain, previous_right.gain, previous_left.muted, previous_right.muted,
                    target.gain, target.muted, left.gain, right.gain);
    }
    close(descriptor);
    return error;
}

int app_ipcam_Speaker_Status(struct app_speaker_state *state)
{
    memset(state, 0, sizeof(*state));
    int lock = speaker_lock();
    if (lock < 0)
        return errno;
    int descriptor = open("/dev/cvitekadac", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    int error = descriptor < 0 ? errno : 0;
    struct speaker_native_volume left;
    struct speaker_native_volume right;
    if (!error)
        error = speaker_read(descriptor, &left, &right);
    if (!error) {
        speaker_state(state, &left, &right);
        int loaded = speaker_load(&state->percent);
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

int app_ipcam_Speaker_Apply(unsigned int percent, struct app_speaker_state *state)
{
    memset(state, 0, sizeof(*state));
    if (percent > 100 || percent % 5)
        return EINVAL;
    int lock = speaker_lock();
    if (lock < 0)
        return errno;
    int error = speaker_apply_locked(percent, state, 1);
    close(lock);
    return error;
}

int app_ipcam_Speaker_Restore(void)
{
    unsigned int percent = 0;
    int error = speaker_load(&percent);
    if (error == ENOENT)
        return 0;
    if (error)
        return error;
    int lock = speaker_lock();
    if (lock < 0)
        return errno;
    error = speaker_load(&percent);
    if (!error) {
        struct app_speaker_state state = {0};
        error = speaker_apply_locked(percent, &state, 0);
    } else if (error == ENOENT)
        error = 0;
    close(lock);
    return error;
}
