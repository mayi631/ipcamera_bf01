#ifndef APP_IPCAM_MP4_H
#define APP_IPCAM_MP4_H

#include <stddef.h>
#include <stdint.h>

struct app_mp4;
struct app_mp4 *app_mp4_open(const char *path, unsigned int width, unsigned int height);
struct app_mp4 *app_mp4_open_h265(const char *path, unsigned int width, unsigned int height);
int app_mp4_enable_audio(struct app_mp4 *muxer, unsigned int sample_rate, unsigned int seconds);
int app_mp4_write_pcm(struct app_mp4 *muxer, const int16_t *samples, unsigned int count, uint64_t pts);
int app_mp4_audio_complete(const struct app_mp4 *muxer);
int app_mp4_write(struct app_mp4 *muxer, const unsigned char *data, size_t size, uint64_t pts);
int app_mp4_finish(struct app_mp4 *muxer, uint64_t end_pts);
void app_mp4_abort(struct app_mp4 *muxer);

#endif
