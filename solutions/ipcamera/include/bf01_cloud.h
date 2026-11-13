#ifndef BF01_CLOUD_H
#define BF01_CLOUD_H

#include "cJSON.h"

int bf_cloud_post(const cJSON *credentials, const char *endpoint, const cJSON *plain,
    cJSON **response, int *http_status);
int bf_cloud_put(const cJSON *prepared, const char *path, unsigned long long *uploaded,
    int *http_status, int (*cancelled)(void));
int bf_cloud_put_sha256(const cJSON *prepared, const char *path, unsigned long long *uploaded,
    char sha256[65], int *http_status, int (*cancelled)(void));

#endif
