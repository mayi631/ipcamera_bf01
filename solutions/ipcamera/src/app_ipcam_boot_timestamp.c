#include "app_ipcam_boot_timestamp.h"

#ifdef BF01_BOOT_CAPTURE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "bf01_boot_timestamp.h"
#include "cvi_ipcm.h"

static uint32_t expected_clock_seconds;
static int clock_reply_error;
static int ipc_lock_fd = -1;

int app_ipcam_BootTimestampLockIpc(void)
{
    if (ipc_lock_fd >= 0)
        return 0;
    if (mkdir("/run/bf01-preview", 0700) != 0 && errno != EEXIST)
        return errno;
    int descriptor = open("/run/bf01-preview/ipcm.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (descriptor < 0)
        return errno;
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        int error = errno;
        close(descriptor);
        fprintf(stderr, "[boot-timestamp] IPC is in use; stop ipcamera before maintenance RTC sync\n");
        return error;
    }
    ipc_lock_fd = descriptor;
    return 0;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int system_clock_seconds(uint32_t *seconds)
{
    int64_t current = (int64_t)time(NULL);
    if (current < 0 || (uint64_t)current < BF01_TIMESTAMP_MIN_SECONDS ||
        (uint64_t)current >= BF01_TIMESTAMP_MAX_SECONDS)
        return ENODATA;
    *seconds = (uint32_t)current;
    return 0;
}

void app_ipcam_BootTimestampPoll(void)
{
    static uint64_t last_poll_ms;
    static int clock_warned;
    uint64_t now_ms = monotonic_ms();
    if (last_poll_ms && now_ms - last_poll_ms < 60000)
        return;
    last_poll_ms = now_ms;
    uint32_t seconds = 0;
    if (system_clock_seconds(&seconds)) {
        if (!clock_warned) {
            fprintf(stderr, "[boot-timestamp] Linux clock invalid; waiting for external time synchronization\n");
            clock_warned = 1;
        }
        return;
    }
    clock_warned = 0;
    int result = CVI_IPCM_AnonSendParam(BF01_TIMESTAMP_CLOCK_PORT, BF01_TIMESTAMP_CLOCK_SET, seconds);
    if (result != CVI_SUCCESS)
        fprintf(stderr, "[boot-timestamp] RTC sync send failed: %d; capture continues\n", result);
    else
        fprintf(stderr, "[boot-timestamp] RTC sync queued: utc_seconds=%u\n", seconds);
}

static int receive_clock_reply(void *private_data, IPCM_ANON_MSG_S *message)
{
    (void)private_data;
    if (!message || message->u8PortID != BF01_TIMESTAMP_CLOCK_REPLY_PORT ||
        message->u8MsgID != BF01_TIMESTAMP_CLOCK_SET_ACK ||
        message->u8DataType != IPCM_MSG_TYPE_RAW_PARAM)
        return -EINVAL;
    uint32_t observed = message->u32Param;
    uint32_t expected = __atomic_load_n(&expected_clock_seconds, __ATOMIC_ACQUIRE);
    if (observed && (observed < expected || observed - expected > 1))
        return -EPROTO;
    __atomic_store_n(&clock_reply_error, observed ? 0 : EIO, __ATOMIC_RELEASE);
    return 0;
}

int app_ipcam_BootTimestampSyncRtc(void)
{
    uint32_t seconds = 0;
    int error = system_clock_seconds(&seconds);
    if (error) {
        fprintf(stderr, "[boot-timestamp] set the correct Linux UTC time before --sync-rtc\n");
        return error;
    }
    error = app_ipcam_BootTimestampLockIpc();
    if (error)
        return error;
    error = CVI_IPCM_Init();
    if (error != CVI_SUCCESS) {
        error = EIO;
        goto unlock;
    }
    int anonymous_ready = 0;
    int reply_registered = 0;
    error = EIO;
    if (CVI_IPCM_AnonInit() != CVI_SUCCESS)
        goto done;
    anonymous_ready = 1;
    __atomic_store_n(&expected_clock_seconds, seconds, __ATOMIC_RELEASE);
    __atomic_store_n(&clock_reply_error, EINPROGRESS, __ATOMIC_RELEASE);
    if (CVI_IPCM_RegisterAnonHandle(BF01_TIMESTAMP_CLOCK_REPLY_PORT, receive_clock_reply, NULL) != CVI_SUCCESS)
        goto done;
    reply_registered = 1;
    if (CVI_IPCM_AnonSendParam(BF01_TIMESTAMP_CLOCK_PORT, BF01_TIMESTAMP_CLOCK_SET_ACK, seconds) != CVI_SUCCESS)
        goto done;
    uint64_t expires_ms = monotonic_ms() + 1000;
    do {
        error = __atomic_load_n(&clock_reply_error, __ATOMIC_ACQUIRE);
        if (error != EINPROGRESS)
            break;
        usleep(1000);
    } while (monotonic_ms() < expires_ms);
    if (error == EINPROGRESS)
        error = ETIMEDOUT;
done:
    if (reply_registered)
        CVI_IPCM_DeregisterAnonHandle(BF01_TIMESTAMP_CLOCK_REPLY_PORT);
    if (anonymous_ready)
        CVI_IPCM_AnonUninit();
    CVI_IPCM_Uninit();
unlock:
    close(ipc_lock_fd);
    ipc_lock_fd = -1;
    fprintf(stderr, "[boot-timestamp] RTC sync %s: utc_seconds=%u error=%d\n",
        error ? "failed" : "confirmed", seconds, error);
    return error;
}
#endif
