#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include "bf01_wifi_link.h"

int bf_wifi_sdio_time(struct bf_wifi_sdio *sdio, struct bf_wifi_time *value)
{
    _Static_assert(sizeof(*value) == 40, "ATBM time ABI mismatch");
    if (!sdio || !value)
        return EINVAL;
    memset(value, 0, sizeof(*value));
    int status = ioctl(sdio->descriptor, _IOR(121, 35, unsigned int), value);
    if (status < 0)
        return errno;
    return status || value->status ? EPROTO : 0;
}

int bf_wifi_sdio_filter(struct bf_wifi_sdio *sdio, struct bf_wifi_filter *filter)
{
    _Static_assert(sizeof(*filter) == 24, "ATBM filter ABI mismatch");
    if (!sdio || !filter)
        return EINVAL;
    memset(filter, 0, sizeof(*filter));
    int status = ioctl(sdio->descriptor, _IOR(121, 4, unsigned int), filter);
    if (status < 0)
        return errno;
    if (status || filter->status || filter->source_count > 4 || filter->destination_count > 4)
        return EPROTO;
    return 0;
}

int bf_wifi_sdio_receive(void *context, char *message, size_t capacity)
{
    struct bf_wifi_sdio *sdio = context;
    if (!sdio || !message || capacity < BF_WIFI_RECEIVE_MAX + 1)
        return EINVAL;
    sdio->status = 0;
    char response[BF_WIFI_RECEIVE_MAX + 1] = {0};
    int status = ioctl(sdio->descriptor, _IOR(121, 90, unsigned int), response);
    if (status < 0)
        return errno;
    if (status) {
        sdio->status = (unsigned int)status;
        return EPROTO;
    }
    size_t length = strnlen(response, sizeof(response));
    if (!length || length > BF_WIFI_RECEIVE_MAX)
        return EMSGSIZE;
    memcpy(message, response, length + 1);
    return 0;
}

int bf_wifi_sdio_exchange(void *context, const char *request, char *response, size_t capacity)
{
    struct bf_wifi_sdio *sdio = context;
    if (!sdio || !request || !response || capacity < BF_WIFI_RECEIVE_MAX + 1)
        return EINVAL;
    sdio->status = 0;
    size_t length = strnlen(request, BF_WIFI_SEND_MAX + 1);
    if (!length || length > BF_WIFI_SEND_MAX)
        return length ? EMSGSIZE : EINVAL;
    struct {
        uint32_t status;
        char trans[BF_WIFI_RECEIVE_MAX];
    } message = {0};
    memcpy(message.trans, request, length + 1);
    int status = ioctl(sdio->descriptor, _IOWR(121, 53, unsigned int), &message);
    sdio->status = message.status;
    if (status < 0)
        return errno;
    if (status || message.status)
        return EIO;
    length = strnlen(message.trans, sizeof(message.trans));
    if (length == sizeof(message.trans))
        return EMSGSIZE;
    memcpy(response, message.trans, length + 1);
    return 0;
}
