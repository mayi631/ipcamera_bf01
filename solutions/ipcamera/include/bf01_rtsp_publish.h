#ifndef BF01_RTSP_PUBLISH_H
#define BF01_RTSP_PUBLISH_H

#include <stddef.h>
#include <stdint.h>

struct bf01_rtsp_publish;

int bf01_rtsp_publish_is_idr(int codec, const unsigned char *frame, size_t length);
int bf01_rtsp_publish_open(struct bf01_rtsp_publish **publisher, const char *url,
    int codec, unsigned int width, unsigned int height, const unsigned char *frame, size_t length, uint64_t pts);
int bf01_rtsp_publish_set_clock(struct bf01_rtsp_publish *publisher, uint64_t pts);
int bf01_rtsp_publish_frame(struct bf01_rtsp_publish *publisher,
    const unsigned char *frame, size_t length, uint64_t pts);
void bf01_rtsp_publish_close(struct bf01_rtsp_publish *publisher);
void bf01_rtsp_publish_abort(struct bf01_rtsp_publish *publisher);

#endif
