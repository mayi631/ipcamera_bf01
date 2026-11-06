#include "app_ipcam_boot_video.h"

#ifdef BF01_BOOT_CAPTURE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "cvi_ipcm.h"
#include "cvi_sys.h"

static pthread_mutex_t video_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t command_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t video_condition = PTHREAD_COND_INITIALIZER;
static struct bf01_video_reply response;
static struct bf01_video_ring *ring;
static uint32_t sequence;
static uint32_t next_frame;
static int have_next_frame;
static uint32_t next_audio;
static int have_next_audio;
static int received;
static int initialized;
static uint32_t ring_size;
static uint32_t ring_paddr;

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int receive_reply(void *private_data, IPCM_ANON_MSG_S *message)
{
    (void)private_data;
    if (!message || message->u8PortID != BF01_VIDEO_PORT ||
        message->u8MsgID != BF01_VIDEO_VERSION || message->u8DataType != IPCM_MSG_TYPE_SHM ||
        message->stData.u32Size != sizeof(response) || !message->stData.pData)
        return -EINVAL;
    struct bf01_video_reply incoming;
    memcpy(&incoming, message->stData.pData, sizeof(incoming));
    if (incoming.magic != BF01_VIDEO_MAGIC || incoming.version != BF01_VIDEO_VERSION)
        return -EPROTO;
    pthread_mutex_lock(&video_mutex);
    if (incoming.sequence == sequence) {
        response = incoming;
        received = 1;
        pthread_cond_signal(&video_condition);
    }
    pthread_mutex_unlock(&video_mutex);
    return 0;
}

static int exchange_locked(uint32_t command, struct bf01_video_reply *reply, int timeout_ms)
{
    memset(reply, 0, sizeof(*reply));
    if (!initialized)
        return ENODEV;
    struct bf01_video_request *request = CVI_IPCM_GetBuff(sizeof(*request));
    if (!request)
        return ENOMEM;
    pthread_mutex_lock(&video_mutex);
    sequence++;
    received = 0;
    *request = (struct bf01_video_request) {
        .magic = BF01_VIDEO_MAGIC,
        .version = BF01_VIDEO_VERSION,
        .sequence = sequence,
        .command = command,
    };
    uint32_t request_sequence = sequence;
    pthread_mutex_unlock(&video_mutex);

    int result = CVI_IPCM_AnonSendMsg(BF01_VIDEO_PORT, BF01_VIDEO_VERSION, request, sizeof(*request));
    if (result < 0) {
        CVI_IPCM_ReleaseBuff(request);
        return EIO;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&video_mutex);
    while (!received && sequence == request_sequence) {
        result = pthread_cond_timedwait(&video_condition, &video_mutex, &deadline);
        if (result)
            break;
    }
    if (received && sequence == request_sequence) {
        *reply = response;
        pthread_mutex_unlock(&video_mutex);
        return reply->error > 0 ? reply->error : (reply->error ? EIO : 0);
    }
    pthread_mutex_unlock(&video_mutex);
    return ETIMEDOUT;
}

static int exchange(uint32_t command, struct bf01_video_reply *reply, int timeout_ms)
{
    pthread_mutex_lock(&command_mutex);
    int result = exchange_locked(command, reply, timeout_ms);
    pthread_mutex_unlock(&command_mutex);
    return result;
}

static int refresh(int timeout_ms)
{
    struct bf01_video_reply status;
    int result = exchange(BF01_VIDEO_STATUS, &status, timeout_ms);
    if (result)
        return result;
    if (status.state == BF01_VIDEO_FAILED)
        return status.error ? (int)status.error : EIO;
    if (!status.ring_paddr || status.ring_size != bf01_video_ring_size())
        return ENODEV;
    if (ring)
        return ring_paddr == status.ring_paddr ? 0 : EPROTO;
    struct bf01_video_ring *mapped = CVI_SYS_Mmap(status.ring_paddr, status.ring_size);
    if (!mapped)
        return ENXIO;
    if (mapped->magic != BF01_VIDEO_MAGIC || mapped->version != BF01_VIDEO_VERSION ||
        mapped->frame_capacity != BF01_VIDEO_FRAME_CAPACITY || mapped->data_size != BF01_VIDEO_DATA_SIZE ||
        mapped->codec != BF01_VIDEO_CODEC_H265 || mapped->width != BF01_VIDEO_WIDTH ||
        mapped->height != BF01_VIDEO_HEIGHT) {
        CVI_SYS_Munmap(mapped, status.ring_size);
        return EPROTO;
    }
    pthread_mutex_lock(&video_mutex);
    if (ring != mapped) {
        ring = mapped;
        ring_size = status.ring_size;
        ring_paddr = status.ring_paddr;
        have_next_frame = 0;
        have_next_audio = 0;
    }
    pthread_mutex_unlock(&video_mutex);
    return 0;
}

int app_ipcam_BootVideo_Init(void)
{
    if (initialized)
        return 0;
    int result = CVI_IPCM_RegisterAnonHandle(BF01_VIDEO_PORT, receive_reply, NULL);
    if (result)
        return result;
    initialized = 1;
    return 0;
}

void app_ipcam_BootVideo_UnInit(void)
{
    if (!initialized)
        return;
    app_ipcam_BootVideo_Stop();
    CVI_IPCM_DeregisterAnonHandle(BF01_VIDEO_PORT);
    initialized = 0;
}

int app_ipcam_BootVideo_Available(void)
{
    if (!initialized)
        return 0;
    uint64_t deadline = monotonic_ms() + 5000;
    do {
        if (ring && (ring->state == BF01_VIDEO_CAPTURING || ring->state == BF01_VIDEO_READY))
            return 1;
        int result = refresh(100);
        if (result != 0 && result != ENODEV && result != ETIMEDOUT)
            return 0;
        if (ring && (ring->state == BF01_VIDEO_CAPTURING || ring->state == BF01_VIDEO_READY))
            return 1;
        usleep(10 * 1000);
    } while (monotonic_ms() < deadline);
    return 0;
}

int app_ipcam_BootVideo_GetSize(unsigned int *width, unsigned int *height)
{
    if (!width || !height || !app_ipcam_BootVideo_Available())
        return -ENODEV;
    *width = ring->width;
    *height = ring->height;
    return 0;
}

static int find_first_keyframe(uint32_t oldest, uint32_t newest)
{
    for (uint32_t frame_number = oldest; frame_number < newest; frame_number++) {
        struct bf01_video_frame *frame = &ring->frames[frame_number % ring->frame_capacity];
        if (frame->sequence == frame_number && (frame->flags & BF01_VIDEO_FLAG_KEY)) {
            next_frame = frame_number;
            have_next_frame = 1;
            return 0;
        }
    }
    return EAGAIN;
}

int app_ipcam_BootVideo_ReadFrame(unsigned char *buffer, size_t capacity,
    struct bf01_video_frame *frame, int timeout_ms)
{
    if (!buffer || !frame || !capacity || !initialized)
        return -EINVAL;
    uint64_t deadline = monotonic_ms() + (timeout_ms < 0 ? 0 : (uint64_t)timeout_ms);
    for (;;) {
        if (!ring) {
            int result = refresh(timeout_ms > 100 ? 100 : timeout_ms);
            if (result && result != ETIMEDOUT && result != ENODEV)
                return -result;
        }
        if (!ring) {
            if (timeout_ms == 0 || monotonic_ms() >= deadline)
                return -EAGAIN;
            usleep(10 * 1000);
            continue;
        }
        if (ring->state == BF01_VIDEO_FAILED || ring->audio.state == BF01_VIDEO_FAILED)
            return -(ring->audio.error > 0 ? ring->audio.error : EIO);
        __sync_synchronize();
        uint32_t generation = *(volatile uint32_t *)&ring->generation;
        if (generation & 1U)
            goto waiting;
        uint32_t oldest = ring->oldest_sequence;
        uint32_t newest = ring->write_sequence;
        if (!have_next_frame) {
            if (oldest) {
                __sync_synchronize();
                if (*(volatile uint32_t *)&ring->generation != generation)
                    goto waiting;
                fprintf(stderr, "[boot-video] cache overrun before attach: oldest=%u; refusing incomplete startup video\n", oldest);
                return -EOVERFLOW;
            }
            find_first_keyframe(oldest, newest);
        }
        if (have_next_frame && next_frame < oldest) {
            fprintf(stderr, "[boot-video] reader overrun: wanted=%u oldest=%u\n", next_frame, oldest);
            return -EOVERFLOW;
        }
        if (have_next_frame && next_frame < newest) {
            struct bf01_video_frame candidate = ring->frames[next_frame % ring->frame_capacity];
            if (candidate.sequence != next_frame) {
                have_next_frame = 0;
                continue;
            }
            if (!candidate.size || candidate.size > capacity ||
                candidate.offset > ring->data_size || candidate.size > ring->data_size - candidate.offset)
                return -EOVERFLOW;
            memcpy(buffer, ring->data + candidate.offset, candidate.size);
            __sync_synchronize();
            if (*(volatile uint32_t *)&ring->generation != generation)
                goto waiting;
            *frame = candidate;
            next_frame++;
            return (int)candidate.size;
        }
        if ((ring->state == BF01_VIDEO_READY || ring->state == BF01_VIDEO_FAILED) &&
            (!have_next_frame || next_frame >= newest))
            return -ENODATA;
waiting:
        if (timeout_ms == 0 || monotonic_ms() >= deadline)
            return -EAGAIN;
        usleep(10 * 1000);
    }
}

int app_ipcam_BootVideo_ReadAudio(int16_t *samples, size_t capacity,
    uint64_t *pts, uint64_t first_pts, uint64_t through_pts)
{
    if (!samples || !pts || capacity < BF01_AUDIO_SAMPLES || !ring)
        return -EINVAL;
    struct bf01_audio_ring *audio = &ring->audio;
    if (audio->rate != BF01_AUDIO_RATE || audio->channels != 1 ||
        audio->frame_capacity != BF01_AUDIO_FRAME_CAPACITY)
        return -EPROTO;
    for (unsigned int attempt = 0; attempt < 4; attempt++) {
        __sync_synchronize();
        uint32_t generation = *(volatile uint32_t *)&audio->generation;
        if (generation & 1U)
            continue;
        if (audio->error || audio->state == BF01_VIDEO_FAILED)
            return -(audio->error > 0 ? audio->error : EIO);
        uint32_t oldest = audio->oldest_sequence;
        uint32_t newest = audio->write_sequence;
        uint32_t wanted = have_next_audio ? next_audio : oldest;
        if (wanted < oldest) {
            fprintf(stderr, "[boot-audio] reader overrun: wanted=%u oldest=%u\n", wanted, oldest);
            return -EOVERFLOW;
        }
        while (wanted < newest) {
            struct bf01_audio_frame frame = audio->frames[wanted % BF01_AUDIO_FRAME_CAPACITY];
            uint64_t frame_pts = ((uint64_t)frame.pts_high << 32) | frame.pts_low;
            uint64_t duration = (uint64_t)frame.count * 1000000 / BF01_AUDIO_RATE;
            __sync_synchronize();
            if (*(volatile uint32_t *)&audio->generation != generation)
                break;
            if (frame.sequence != wanted || frame.count != BF01_AUDIO_SAMPLES)
                return -EPROTO;
            if (!have_next_audio && frame_pts + duration <= first_pts) {
                wanted++;
                continue;
            }
            if (!have_next_audio && frame_pts > first_pts + 1000000 / BF01_AUDIO_RATE) {
                fprintf(stderr, "[boot-audio] missing audio at first video PTS: video=%llu audio=%llu\n",
                    (unsigned long long)first_pts, (unsigned long long)frame_pts);
                return -ENODATA;
            }
            if (frame_pts > through_pts)
                return -EAGAIN;
            memcpy(samples, frame.samples, frame.count * sizeof(*samples));
            *pts = frame_pts;
            next_audio = wanted + 1;
            have_next_audio = 1;
            return (int)frame.count;
        }
        __sync_synchronize();
        if (*(volatile uint32_t *)&audio->generation != generation)
            continue;
        if (audio->state == BF01_VIDEO_READY && wanted >= newest)
            return -ENODATA;
        return -EAGAIN;
    }
    return -EAGAIN;
}

static int stopped_reply(const struct bf01_video_reply *reply)
{
    return reply->magic == BF01_VIDEO_MAGIC && reply->version == BF01_VIDEO_VERSION &&
        (reply->state == BF01_VIDEO_READY || reply->state == BF01_VIDEO_FAILED ||
            reply->state == BF01_VIDEO_RELEASED);
}

void app_ipcam_BootVideo_Stop(void)
{
    if (!initialized)
        return;
    struct bf01_video_reply response_for_stop;
    exchange(BF01_VIDEO_STOP, &response_for_stop, 200);
    uint64_t deadline = monotonic_ms() + 5000;
    while (!stopped_reply(&response_for_stop) && monotonic_ms() < deadline) {
        usleep(20 * 1000);
        exchange(BF01_VIDEO_STATUS, &response_for_stop, 200);
    }
    if (ring) {
        CVI_SYS_Munmap(ring, ring_size);
        ring = NULL;
        ring_size = 0;
        ring_paddr = 0;
        have_next_frame = 0;
        have_next_audio = 0;
    }
    if (stopped_reply(&response_for_stop))
        exchange(BF01_VIDEO_RELEASE, &response_for_stop, 200);
    else
        fprintf(stderr, "[boot-video] stop not acknowledged; bounded RTOS cache retained\n");
}
#endif
