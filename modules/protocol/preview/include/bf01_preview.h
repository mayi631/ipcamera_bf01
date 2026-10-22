#ifndef BF01_PREVIEW_H
#define BF01_PREVIEW_H

struct bf01_preview_config {
    const char *interface_name;
    unsigned short http_port;
    unsigned short rtsp_port;
    unsigned int session;
    int (*video_ready)(void *opaque);
    void *opaque;
};

int bf01_preview_start(const struct bf01_preview_config *config);
void bf01_preview_stop(void);

#endif
