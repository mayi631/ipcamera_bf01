#include "app_ipcam_boot_capture.h"

#ifdef BF01_BOOT_CAPTURE
#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
#include "cvi_ipcm.h"

static pthread_mutex_t reply_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reply_condition = PTHREAD_COND_INITIALIZER;
static struct bf01_burst_reply response;
static uint32_t sequence;
static int received;
static int initialized;

static uint64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int receive_reply(void *private_data, IPCM_ANON_MSG_S *message)
{
    (void)private_data;
    if (!message || message->u8PortID != BF01_BURST_PORT ||
        message->u8MsgID != BF01_BURST_VERSION || message->u8DataType != IPCM_MSG_TYPE_SHM ||
        message->stData.u32Size != sizeof(response) || !message->stData.pData)
        return -EINVAL;
    struct bf01_burst_reply incoming;
    memcpy(&incoming, message->stData.pData, sizeof(incoming));
    if (incoming.magic != BF01_BURST_MAGIC || incoming.version != BF01_BURST_VERSION)
        return -EPROTO;
    pthread_mutex_lock(&reply_mutex);
    if (incoming.sequence == sequence) {
        response = incoming;
        received = 1;
        pthread_cond_signal(&reply_condition);
    }
    pthread_mutex_unlock(&reply_mutex);
    return 0;
}

static int exchange(uint32_t command, uint32_t index, uint32_t offset, struct bf01_burst_reply *reply)
{
    if (!initialized)
        return ENODEV;
    for (unsigned int attempt = 0; attempt < 3; attempt++) {
        struct bf01_burst_request *request = CVI_IPCM_GetBuff(sizeof(*request));
        if (!request) {
            if (attempt == 2)
                return ENOMEM;
            usleep(100 * 1000);
            continue;
        }
        pthread_mutex_lock(&reply_mutex);
        sequence++;
        received = 0;
        *request = (struct bf01_burst_request) {
            .magic = BF01_BURST_MAGIC, .version = BF01_BURST_VERSION, .sequence = sequence,
            .command = command, .index = index, .offset = offset
        };
        pthread_mutex_unlock(&reply_mutex);
        int result = CVI_IPCM_AnonSendMsg(BF01_BURST_PORT, BF01_BURST_VERSION, request, sizeof(*request));
        if (result < 0) {
            CVI_IPCM_ReleaseBuff(request);
            return EIO;
        }
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec++;
        pthread_mutex_lock(&reply_mutex);
        while (!received) {
            result = pthread_cond_timedwait(&reply_condition, &reply_mutex, &deadline);
            if (result)
                break;
        }
        if (received) {
            *reply = response;
            pthread_mutex_unlock(&reply_mutex);
            if (reply->command != command || reply->index != index || reply->offset != offset ||
                reply->length > BF01_BURST_CHUNK || reply->count > BF01_BURST_COUNT ||
                reply->width != BF01_BURST_WIDTH || reply->height != BF01_BURST_HEIGHT)
                return EPROTO;
            return reply->error > 0 ? reply->error : (reply->error ? EIO : 0);
        }
        pthread_mutex_unlock(&reply_mutex);
    }
    return ETIMEDOUT;
}

int app_ipcam_BootCapture_Init(void)
{
    if (initialized)
        return 0;
    int result = CVI_IPCM_Init();
    if (result != 0)
        return result;
    result = CVI_IPCM_RegisterAnonHandle(BF01_BURST_PORT, receive_reply, NULL);
    if (result != 0) {
        CVI_IPCM_Uninit();
        return result;
    }
    initialized = 1;
    return 0;
}

void app_ipcam_BootCapture_UnInit(void)
{
    if (initialized) {
        CVI_IPCM_DeregisterAnonHandle(BF01_BURST_PORT);
        CVI_IPCM_Uninit();
        initialized = 0;
    }
}

int app_ipcam_BootCapture_Ready(void)
{
    pthread_mutex_lock(&reply_mutex);
    int ready = response.state == BF01_BURST_READY || response.state == BF01_BURST_RELEASED;
    pthread_mutex_unlock(&reply_mutex);
    return ready;
}

static int stage_image(unsigned int index, const struct bf01_burst_reply *metadata, int (*cancelled)(void))
{
    const struct bf01_burst_image *image = &metadata->images[index];
    if (image->size < 4 || image->size > BF01_BURST_MAX_JPEG)
        return EOVERFLOW;
    char temporary[128];
    char path[128];
    snprintf(path, sizeof(path), APP_BOOT_CAPTURE_DIR "/photo%u.jpg", index);
    snprintf(temporary, sizeof(temporary), APP_BOOT_CAPTURE_DIR "/photo%u.part", index);
    int descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0)
        return errno;
    int error = 0;
    uint32_t crc = UINT32_MAX;
    unsigned char tail[2] = {0};
    for (uint32_t offset = 0; offset < image->size;) {
        if (cancelled()) {
            error = ECANCELED;
            break;
        }
        struct bf01_burst_reply reply;
        error = exchange(BF01_BURST_READ, index, offset, &reply);
        if (error)
            break;
        uint32_t remaining = image->size - offset;
        uint32_t expected = remaining < BF01_BURST_CHUNK ? remaining : BF01_BURST_CHUNK;
        if (reply.state != BF01_BURST_READY || reply.length != expected ||
            reply.images[index].size != image->size || reply.images[index].crc != image->crc ||
            (offset == 0 && (reply.data[0] != 0xff || reply.data[1] != 0xd8))) {
            error = EBADMSG;
            break;
        }
        size_t written = 0;
        while (written < reply.length) {
            ssize_t count = write(descriptor, reply.data + written, reply.length - written);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0) {
                error = count < 0 ? errno : EIO;
                break;
            }
            written += (size_t)count;
        }
        if (error)
            break;
        if (reply.length >= 2) {
            tail[0] = reply.data[reply.length - 2];
            tail[1] = reply.data[reply.length - 1];
        } else {
            tail[0] = tail[1];
            tail[1] = reply.data[0];
        }
        crc = bf01_burst_crc(crc, reply.data, reply.length);
        offset += reply.length;
    }
    if (!error && ((crc ^ UINT32_MAX) != image->crc || tail[0] != 0xff || tail[1] != 0xd9))
        error = EBADMSG;
    if (close(descriptor) != 0 && !error)
        error = errno;
    if (!error && rename(temporary, path) != 0)
        error = errno;
    if (error)
        unlink(temporary);
    return error;
}

int app_ipcam_BootCapture_Stage(struct bf01_burst_reply *metadata, int (*cancelled)(void))
{
    struct statfs filesystem;
    if (statfs("/run", &filesystem) != 0 || filesystem.f_type != TMPFS_MAGIC)
        return ENODEV;
    if (mkdir(APP_BOOT_CAPTURE_DIR, 0700) != 0 && errno != EEXIST)
        return errno;
    uint64_t deadline = now_ms() + 10000;
    for (;;) {
        if (cancelled())
            return ECANCELED;
        int error = exchange(BF01_BURST_STATUS, 0, 0, metadata);
        if (error)
            return error;
        if (metadata->state == BF01_BURST_RELEASED)
            return EALREADY;
        if (metadata->state == BF01_BURST_READY)
            break;
        if (metadata->state == BF01_BURST_FAILED)
            return EIO;
        if (now_ms() >= deadline)
            return ETIMEDOUT;
        usleep(20 * 1000);
    }
    if (!metadata->count || metadata->count > BF01_BURST_COUNT)
        return EPROTO;
    for (unsigned int index = 0; index < metadata->count; index++) {
        int error = stage_image(index, metadata, cancelled);
        if (error)
            return error;
        fprintf(stderr, "[boot-capture] staged photo=%u bytes=%u RTOS_ms=%u\n",
            index + 1, metadata->images[index].size, metadata->images[index].captured_ms);
    }
    fprintf(stderr, "[boot-capture] %u photos retained in RAM at %s; waiting for SD commit\n",
        metadata->count, APP_BOOT_CAPTURE_DIR);
    return 0;
}

int app_ipcam_BootCapture_Commit(const struct bf01_burst_reply *metadata)
{
    if (!metadata->count || metadata->count > BF01_BURST_COUNT)
        return EPROTO;
    uint32_t crc = 0;
    for (unsigned int index = 0; index < metadata->count; index++)
        crc ^= metadata->images[index].crc;
    struct bf01_burst_reply reply;
    int error = exchange(BF01_BURST_COMMIT, 0, crc, &reply);
    if (!error && reply.state != BF01_BURST_RELEASED)
        error = EPROTO;
    if (!error) {
        for (unsigned int index = 0; index < metadata->count; index++) {
            char path[128];
            snprintf(path, sizeof(path), APP_BOOT_CAPTURE_DIR "/photo%u.jpg", index);
            unlink(path);
        }
        fprintf(stderr, "[boot-capture] SD commit acknowledged; RTOS images released\n");
    }
    return error;
}
#endif
