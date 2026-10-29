#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include "app_ipcam_mp4.h"

struct app_mp4 {
    AVFormatContext *format;
    AVStream *stream;
    AVPacket *pending;
    uint64_t first_pts;
    uint64_t pending_pts;
    int header_written;
    AVCodecContext *audio_codec;
    AVStream *audio_stream;
    AVFrame *audio_frame;
    AVPacket *audio_packet;
    unsigned int audio_filled;
    int64_t audio_samples;
    int64_t audio_limit;
    int audio_started;
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

int app_mp4_enable_audio(struct app_mp4 *muxer, unsigned int sample_rate, unsigned int seconds)
{
    if (!muxer || muxer->header_written || muxer->audio_codec || sample_rate != 16000 ||
        !seconds || seconds > 300)
        return -1;
    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!encoder)
        return -1;
    muxer->audio_codec = avcodec_alloc_context3(encoder);
    muxer->audio_frame = av_frame_alloc();
    muxer->audio_packet = av_packet_alloc();
    muxer->audio_stream = avformat_new_stream(muxer->format, NULL);
    if (!muxer->audio_codec || !muxer->audio_frame || !muxer->audio_packet || !muxer->audio_stream)
        return -1;
    AVCodecContext *codec = muxer->audio_codec;
    codec->sample_rate = sample_rate;
    codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
    codec->channel_layout = AV_CH_LAYOUT_MONO;
    codec->channels = 1;
    codec->bit_rate = 48000;
    codec->profile = FF_PROFILE_AAC_LOW;
    codec->time_base = (AVRational){1, (int)sample_rate};
    codec->thread_count = 1;
    if (muxer->format->oformat->flags & AVFMT_GLOBALHEADER)
        codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(codec, encoder, NULL) < 0 || codec->frame_size <= 0 ||
        avcodec_parameters_from_context(muxer->audio_stream->codecpar, codec) < 0)
        return -1;
    muxer->audio_stream->time_base = codec->time_base;
    AVFrame *frame = muxer->audio_frame;
    frame->format = codec->sample_fmt;
    frame->channel_layout = codec->channel_layout;
    frame->sample_rate = codec->sample_rate;
    frame->nb_samples = codec->frame_size;
    muxer->audio_limit = (int64_t)sample_rate * seconds;
    return av_frame_get_buffer(frame, 0) < 0 ? -1 : 0;
}

static int receive_audio(struct app_mp4 *muxer)
{
    while (1) {
        int result = avcodec_receive_packet(muxer->audio_codec, muxer->audio_packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return 0;
        if (result < 0)
            return -1;
        av_packet_rescale_ts(muxer->audio_packet, muxer->audio_codec->time_base,
            muxer->audio_stream->time_base);
        muxer->audio_packet->stream_index = muxer->audio_stream->index;
        result = av_interleaved_write_frame(muxer->format, muxer->audio_packet);
        av_packet_unref(muxer->audio_packet);
        if (result < 0)
            return -1;
    }
}

static int send_audio(struct app_mp4 *muxer)
{
    muxer->audio_frame->nb_samples = muxer->audio_filled;
    muxer->audio_frame->pts = muxer->audio_samples - muxer->audio_filled;
    if (avcodec_send_frame(muxer->audio_codec, muxer->audio_frame) < 0 || receive_audio(muxer) < 0)
        return -1;
    muxer->audio_filled = 0;
    return 0;
}

int app_mp4_write_pcm(struct app_mp4 *muxer, const int16_t *samples, unsigned int count, uint64_t pts)
{
    if (!muxer || !muxer->header_written || !muxer->audio_codec || !samples || !count || count > 1024 ||
        pts > INT64_MAX || muxer->first_pts > INT64_MAX)
        return -1;
    int64_t first = av_rescale_q((int64_t)pts - (int64_t)muxer->first_pts,
        (AVRational){1, 1000000}, muxer->audio_codec->time_base);
    unsigned int skip = 0;
    if (first < 0) {
        if (first + count <= 0)
            return 0;
        skip = (unsigned int)-first;
        first = 0;
    }
    if (first >= muxer->audio_limit)
        return 0;
    if (!muxer->audio_started) {
        if (first > muxer->audio_codec->sample_rate / 4) {
            fprintf(stderr, "[audio] first PCM arrives too late: offset_samples=%lld\n", (long long)first);
            return -1;
        }
        muxer->audio_samples = first;
        muxer->audio_started = 1;
        fprintf(stderr, "[audio] MP4 AAC started: rate=%d channels=1 offset_samples=%lld\n",
            muxer->audio_codec->sample_rate, (long long)first);
    }
    if (llabs(first - muxer->audio_samples) > 2) {
        fprintf(stderr, "[audio] PCM timestamp discontinuity: expected=%lld received=%lld\n",
            (long long)muxer->audio_samples, (long long)first);
        return -1;
    }
    while (skip < count && muxer->audio_samples < muxer->audio_limit) {
        if (!muxer->audio_filled && av_frame_make_writable(muxer->audio_frame) < 0)
            return -1;
        unsigned int available = muxer->audio_codec->frame_size - muxer->audio_filled;
        unsigned int remaining = count - skip;
        if (remaining > available)
            remaining = available;
        if (remaining > muxer->audio_limit - muxer->audio_samples)
            remaining = (unsigned int)(muxer->audio_limit - muxer->audio_samples);
        float *output = (float *)muxer->audio_frame->data[0];
        for (unsigned int index = 0; index < remaining; index++)
            output[muxer->audio_filled + index] = samples[skip + index] / 32768.0f;
        muxer->audio_filled += remaining;
        muxer->audio_samples += remaining;
        skip += remaining;
        if (muxer->audio_filled == (unsigned int)muxer->audio_codec->frame_size && send_audio(muxer) < 0)
            return -1;
    }
    return 0;
}

int app_mp4_audio_complete(const struct app_mp4 *muxer)
{
    return muxer && muxer->audio_started && muxer->audio_samples >= muxer->audio_limit;
}

static int finish_audio(struct app_mp4 *muxer)
{
    if (!muxer->audio_codec)
        return 0;
    if (!app_mp4_audio_complete(muxer))
        return -1;
    if (muxer->audio_filled && send_audio(muxer) < 0)
        return -1;
    if (avcodec_send_frame(muxer->audio_codec, NULL) < 0)
        return -1;
    return receive_audio(muxer);
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
        write_pending(muxer, end_pts) >= 0 && finish_audio(muxer) >= 0 &&
        av_write_trailer(muxer->format) >= 0) {
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
    av_packet_free(&muxer->audio_packet);
    av_frame_free(&muxer->audio_frame);
    avcodec_free_context(&muxer->audio_codec);
    if (muxer->format) {
        if (muxer->format->pb)
            avio_closep(&muxer->format->pb);
        avformat_free_context(muxer->format);
    }
    free(muxer);
}
