#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include "app_ipcam_mp4.h"

struct app_mp4 {
    AVFormatContext *format;
    AVStream *stream;
    AVPacket *pending;
    uint64_t first_pts;
    uint64_t pending_pts;
    int header_written;
};

static size_t next_nal(const unsigned char *data, size_t size, size_t offset, size_t *prefix)
{
    for (; offset + 3 < size; offset++) {
        if (!data[offset] && !data[offset + 1]) {
            if (data[offset + 2] == 1) {
                *prefix = 3;
                return offset;
            }
            if (!data[offset + 2] && data[offset + 3] == 1) {
                *prefix = 4;
                return offset;
            }
        }
    }
    *prefix = 0;
    return size;
}

static int inspect_frame(const unsigned char *data, size_t size, int *idr, unsigned char *extra, size_t *extra_size)
{
    size_t prefix;
    size_t offset = next_nal(data, size, 0, &prefix);
    int parameters = 0;
    *idr = 0;
    *extra_size = 0;
    if (offset == size)
        return -1;
    while (offset < size) {
        size_t payload = offset + prefix;
        if (payload >= size)
            return -1;
        size_t following_prefix;
        size_t end = next_nal(data, size, payload + 1, &following_prefix);
        unsigned int type = data[payload] & 31;
        if (type == 5)
            *idr = 1;
        if ((type == 7 || type == 8) && extra) {
            size_t length = end - payload;
            if (length > 4096 - 4 || *extra_size > 4096 - length - 4)
                return -1;
            memcpy(extra + *extra_size, "\0\0\0\1", 4);
            memcpy(extra + *extra_size + 4, data + payload, length);
            *extra_size += length + 4;
            parameters |= type == 7 ? 1 : 2;
        }
        offset = end;
        prefix = following_prefix;
    }
    return extra && parameters != 3 ? -1 : 0;
}

struct app_mp4 *app_mp4_open(const char *path, unsigned int width, unsigned int height)
{
    struct app_mp4 *muxer = calloc(1, sizeof(*muxer));
    if (!muxer)
        return NULL;
    if (avformat_alloc_output_context2(&muxer->format, NULL, "mp4", path) < 0 || !muxer->format)
        goto failed;
    muxer->stream = avformat_new_stream(muxer->format, NULL);
    muxer->pending = av_packet_alloc();
    if (!muxer->stream || !muxer->pending)
        goto failed;
    muxer->stream->time_base = (AVRational){1, 1000000};
    AVCodecParameters *codec = muxer->stream->codecpar;
    codec->codec_type = AVMEDIA_TYPE_VIDEO;
    codec->codec_id = AV_CODEC_ID_H264;
    codec->width = (int)width;
    codec->height = (int)height;
    codec->video_delay = 0;
    if (avio_open(&muxer->format->pb, path, AVIO_FLAG_WRITE) < 0)
        goto failed;
    return muxer;
failed:
    app_mp4_abort(muxer);
    return NULL;
}

static int write_pending(struct app_mp4 *muxer, uint64_t next_pts)
{
    if (next_pts <= muxer->pending_pts || next_pts - muxer->first_pts > INT64_MAX)
        return -1;
    muxer->pending->pts = (int64_t)(muxer->pending_pts - muxer->first_pts);
    muxer->pending->dts = muxer->pending->pts;
    muxer->pending->duration = (int64_t)(next_pts - muxer->pending_pts);
    av_packet_rescale_ts(muxer->pending, (AVRational){1, 1000000}, muxer->stream->time_base);
    muxer->pending->stream_index = muxer->stream->index;
    return av_interleaved_write_frame(muxer->format, muxer->pending);
}

int app_mp4_write(struct app_mp4 *muxer, const unsigned char *data, size_t size, uint64_t pts)
{
    int idr;
    unsigned char extra[4096];
    size_t extra_size;
    if (!muxer || !data || !size || size > INT_MAX)
        return -1;
    if (inspect_frame(data, size, &idr, muxer->header_written ? NULL : extra, &extra_size) != 0)
        return -1;
    if (!muxer->header_written) {
        if (!idr)
            return -1;
        AVCodecParameters *codec = muxer->stream->codecpar;
        codec->extradata = av_mallocz(extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!codec->extradata)
            return -1;
        memcpy(codec->extradata, extra, extra_size);
        codec->extradata_size = (int)extra_size;
        if (avformat_write_header(muxer->format, NULL) < 0)
            return -1;
        muxer->header_written = 1;
        muxer->first_pts = pts;
    } else if (write_pending(muxer, pts) < 0) {
        return -1;
    }
    av_packet_unref(muxer->pending);
    if (av_new_packet(muxer->pending, (int)size) < 0)
        return -1;
    memcpy(muxer->pending->data, data, size);
    if (idr)
        muxer->pending->flags |= AV_PKT_FLAG_KEY;
    muxer->pending_pts = pts;
    return 0;
}

int app_mp4_finish(struct app_mp4 *muxer, uint64_t end_pts)
{
    int result = -1;
    if (muxer && muxer->header_written && muxer->pending->size > 0 &&
        write_pending(muxer, end_pts) >= 0 && av_write_trailer(muxer->format) >= 0) {
        avio_flush(muxer->format->pb);
        result = muxer->format->pb->error < 0 ? -1 : 0;
    }
    if (muxer && muxer->format && muxer->format->pb && avio_closep(&muxer->format->pb) < 0)
        result = -1;
    app_mp4_abort(muxer);
    return result;
}

void app_mp4_abort(struct app_mp4 *muxer)
{
    if (!muxer)
        return;
    av_packet_free(&muxer->pending);
    if (muxer->format) {
        if (muxer->format->pb)
            avio_closep(&muxer->format->pb);
        avformat_free_context(muxer->format);
    }
    free(muxer);
}
