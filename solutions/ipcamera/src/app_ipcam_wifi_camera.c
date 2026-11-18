#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "app_ipcam_wifi_camera.h"
#include "app_ipcam_stream.h"
#include "app_ipcam_vi.h"
#include "app_ipcam_boot_timestamp.h"
#include "app_ipcam_event.h"
#include "app_ipcam_settings.h"
#include "cvi_ae.h"

#define CAMERA_RUN "/run/ipcamera-wifi"
#define CAMERA_SOCKET CAMERA_RUN "/camera.sock"

static int camera_listener = -1;
static int camera_lock = -1;

static int camera_root(int descriptor)
{
    struct ucred credentials;
    socklen_t size = sizeof(credentials);
    return !getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &size) && credentials.uid == 0;
}

static int camera_exposure(const char *setting, cJSON *reply)
{
    APP_PARAM_VI_CTX_S *parameters = app_ipcam_Vi_Param_Get();
    int pipe = parameters->astPipeInfo[0].aPipe[0];
    ISP_EXPOSURE_ATTR_S exposure;
    int error = CVI_ISP_GetExposureAttr(pipe, &exposure);
    if (error)
        return error;
    if (setting) {
        if (strcmp(setting, "off") && strcmp(setting, "50Hz") && strcmp(setting, "60Hz"))
            return EINVAL;
        CVI_BOOL enabled = strcmp(setting, "off") ? CVI_TRUE : CVI_FALSE;
        ISP_AE_ANTIFLICKER_FREQUENCE_E frequency = !strcmp(setting, "50Hz") ?
            AE_FREQUENCE_50HZ : AE_FREQUENCE_60HZ;
        if (exposure.stAuto.stAntiflicker.bEnable != enabled ||
            (enabled && exposure.stAuto.stAntiflicker.enFrequency != frequency)) {
            exposure.stAuto.stAntiflicker.bEnable = enabled;
            if (enabled)
                exposure.stAuto.stAntiflicker.enFrequency = frequency;
            error = CVI_ISP_SetExposureAttr(pipe, &exposure);
            if (error)
                return error;
            error = CVI_ISP_GetExposureAttr(pipe, &exposure);
            if (error)
                return error;
        }
        if (exposure.stAuto.stAntiflicker.bEnable != enabled ||
            (enabled && exposure.stAuto.stAntiflicker.enFrequency != frequency))
            return EIO;
    }
    const char *observed = !exposure.stAuto.stAntiflicker.bEnable ? "off" :
        exposure.stAuto.stAntiflicker.enFrequency == AE_FREQUENCE_50HZ ? "50Hz" :
        exposure.stAuto.stAntiflicker.enFrequency == AE_FREQUENCE_60HZ ? "60Hz" : NULL;
    if (!observed)
        return EPROTO;
    return cJSON_AddStringToObject(reply, "image_anti_flicker", observed) ? 0 : ENOMEM;
}

static void camera_record(cJSON *reply)
{
    FILE *status = fopen("/run/ipcamera-capture/status.ini", "r");
    if (!status)
        return;
    char line[256];
    while (fgets(line, sizeof(line), status)) {
        if (strncmp(line, "state=", 6))
            continue;
        char *state = line + 6;
        state[strcspn(state, "\r\n")] = 0;
        if (!strcmp(state, "recording"))
            cJSON_AddStringToObject(reply, "record", "recording");
        else if (!strcmp(state, "completed") || !strcmp(state, "failed") ||
                 !strcmp(state, "awaiting_ai") || !strcmp(state, "waiting_for_storage") ||
                 !strcmp(state, "capturing_photo") || !strcmp(state, "interrupted"))
            cJSON_AddStringToObject(reply, "record", "idle");
        break;
    }
    fclose(status);
}

void app_ipcam_WifiCamera_Stop(void)
{
    app_ipcam_Stream_Stop();
    if (camera_listener >= 0) {
        unlink("/run/bf01-preview/camera-loop.ready");
        close(camera_listener);
        camera_listener = -1;
        unlink(CAMERA_SOCKET);
    }
    if (camera_lock >= 0) {
        close(camera_lock);
        camera_lock = -1;
    }
}

int app_ipcam_WifiCamera_Start(void)
{
    if (camera_listener >= 0)
        return 0;
    if (geteuid() != 0 || (mkdir(CAMERA_RUN, 0700) && errno != EEXIST))
        return EACCES;
    camera_lock = open(CAMERA_RUN "/camera.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (camera_lock < 0)
        return errno;
    if (flock(camera_lock, LOCK_EX | LOCK_NB)) {
        int error = errno;
        app_ipcam_WifiCamera_Stop();
        return error;
    }
    unlink("/run/bf01-preview/camera-loop.ready");
    int descriptor = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = CAMERA_SOCKET };
    int error = descriptor < 0 ? errno : 0;
    if (!error) {
        unlink(CAMERA_SOCKET);
        if (bind(descriptor, (struct sockaddr *)&address, sizeof(address)) ||
            chmod(CAMERA_SOCKET, 0600) || listen(descriptor, 4))
            error = errno;
    }
    if (error) {
        if (descriptor >= 0) {
            close(descriptor);
            unlink(CAMERA_SOCKET);
        }
        app_ipcam_WifiCamera_Stop();
        return error;
    }
    camera_listener = descriptor;
    cJSON *settings = NULL;
    int settings_error = app_settings_load(&settings);
    const cJSON *flicker = cJSON_GetObjectItemCaseSensitive(settings, "image_anti_flicker");
    if (!settings_error && flicker) {
        cJSON *observed = cJSON_CreateObject();
        settings_error = observed ? camera_exposure(flicker->valuestring, observed) : ENOMEM;
        cJSON_Delete(observed);
        fprintf(stderr, "[wifi-camera] restore image_anti_flicker=%s errno=%d\n",
            flicker->valuestring, settings_error);
    } else if (settings_error)
        fprintf(stderr, "[wifi-camera] load saved settings errno=%d\n", settings_error);
    cJSON_Delete(settings);
    fprintf(stderr, "[wifi-camera] production control ready pid=%ld\n", (long)getpid());
    return 0;
}

int app_ipcam_WifiCamera_Descriptor(void)
{
    return camera_listener;
}

void app_ipcam_WifiCamera_Poll(void)
{
    if (camera_listener < 0)
        return;
    int descriptor = accept4(camera_listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (descriptor < 0)
        return;
    struct pollfd pending = { .fd = descriptor, .events = POLLIN };
    char buffer[4096];
    ssize_t length = camera_root(descriptor) && poll(&pending, 1, 100) > 0 ?
        recv(descriptor, buffer, sizeof(buffer) - 1, MSG_TRUNC) : -1;
    cJSON *request = NULL;
    if (length > 0 && (size_t)length < sizeof(buffer)) {
        buffer[length] = 0;
        request = cJSON_ParseWithOpts(buffer, NULL, 1);
    }
    cJSON *reply = cJSON_CreateObject();
    const cJSON *operation = cJSON_GetObjectItemCaseSensitive(request, "operation");
    const cJSON *setting = cJSON_GetObjectItemCaseSensitive(request, "image_anti_flicker");
    int error = EINVAL;
    if (reply && cJSON_IsString(operation)) {
        if (!strcmp(operation->valuestring, "status")) {
            int isp_error = camera_exposure(NULL, reply);
            cJSON_AddNumberToObject(reply, "isp_errno", isp_error);
            camera_record(reply);
            app_ipcam_Stream_Status(reply);
#ifdef BF01_BOOT_CAPTURE
            cJSON_AddNumberToObject(reply, "datetime_policy_errno", app_ipcam_BootTimestampApplyPolicy());
#endif
            error = 0;
        } else if (!strcmp(operation->valuestring, "apply") && cJSON_IsString(setting))
            error = camera_exposure(setting->valuestring, reply);
        else if (!strcmp(operation->valuestring, "stream"))
            error = app_ipcam_Stream_Apply(request, reply);
        else if (!strcmp(operation->valuestring, "stream-status")) {
            app_ipcam_Stream_Status(reply);
            error = 0;
        }
    }
    if (reply) {
        cJSON_AddNumberToObject(reply, "camera_pid", getpid());
        cJSON_AddNumberToObject(reply, "camera_errno", error);
        char *encoded = cJSON_PrintUnformatted(reply);
        if (encoded) {
            send(descriptor, encoded, strlen(encoded), MSG_NOSIGNAL);
            cJSON_free(encoded);
        }
    }
    cJSON_Delete(request);
    cJSON_Delete(reply);
    close(descriptor);
}

int app_ipcam_WifiCamera_Request(const cJSON *request, cJSON **reply)
{
    return app_ipcam_WifiCamera_RequestTimed(request, reply, 1500);
}

int app_ipcam_WifiCamera_RequestTimed(const cJSON *request, cJSON **reply, int timeout_ms)
{
    *reply = NULL;
    char *encoded = cJSON_PrintUnformatted(request);
    if (!encoded)
        return ENOMEM;
    int descriptor = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = CAMERA_SOCKET };
    int error = descriptor < 0 ? errno : 0;
    if (!error && connect(descriptor, (struct sockaddr *)&address, sizeof(address)))
        error = errno;
    if (!error && !camera_root(descriptor))
        error = EACCES;
    if (!error && send(descriptor, encoded, strlen(encoded), MSG_NOSIGNAL) != (ssize_t)strlen(encoded))
        error = errno ? errno : EIO;
    cJSON_free(encoded);
    struct pollfd pending = { .fd = descriptor, .events = POLLIN };
    char buffer[4096];
    if (!error) {
        int ready = poll(&pending, 1, timeout_ms);
        if (ready <= 0)
            error = ready ? errno : ETIMEDOUT;
        else {
            ssize_t length = recv(descriptor, buffer, sizeof(buffer) - 1, MSG_TRUNC);
            if (length <= 0 || (size_t)length >= sizeof(buffer))
                error = length < 0 ? errno : EPROTO;
            else {
                buffer[length] = 0;
                *reply = cJSON_ParseWithOpts(buffer, NULL, 1);
                const cJSON *result = cJSON_GetObjectItemCaseSensitive(*reply, "camera_errno");
                error = cJSON_IsNumber(result) ? result->valueint : EPROTO;
            }
        }
    }
    if (descriptor >= 0)
        close(descriptor);
    return error;
}
