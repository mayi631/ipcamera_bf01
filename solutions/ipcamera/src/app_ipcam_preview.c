#include "app_ipcam_preview.h"

#ifdef BF01_PREVIEW_SUPPORT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "app_ipcam_paramparse.h"
#include "bf01_preview.h"
#include "cvi_mbuf.h"

static CVI_MBUF_HANDLE *preview_reader;
static CVI_MEDIA_FRAME_INFO_T preview_frame;
static unsigned int parameter_sets;
static time_t last_frame;
static time_t last_keyframe;
static int ready_written;
static pid_t network_pid = -1;
static int instance_fd = -1;

static void preview_network_stop(void)
{
    if (network_pid > 0) {
        kill(network_pid, SIGTERM);
        while (waitpid(network_pid, NULL, 0) < 0 && errno == EINTR) { }
        network_pid = -1;
    }
    if (instance_fd >= 0) {
        unlink("/run/bf01-preview/ipcamera.pid");
        unlink("/run/bf01-preview/service.ready");
        unlink("/run/bf01-preview/media.ready");
        unlink("/run/bf01-preview/mode");
        close(instance_fd);
        instance_fd = -1;
    }
}

int app_ipcam_Preview_NetworkAlive(void)
{
    if (network_pid <= 0)
        return network_pid == -1;
    pid_t result = waitpid(network_pid, NULL, WNOHANG);
    if (result == 0 || (result < 0 && errno == EINTR))
        return 1;
    network_pid = -1;
    fprintf(stderr, "ipcamera: network helper stopped; local capture continues\n");
    return 1;
}

int app_ipcam_Preview_Prepare(void)
{
    char helper[PATH_MAX];
    char owner[32];
    const char *mode = getenv("BF01_WIFI_MODE");
    if (!mode)
        mode = "ap";
    if (strcmp(mode, "ap") && strcmp(mode, "sta") && strcmp(mode, "off")) {
        fprintf(stderr, "ipcamera: BF01_WIFI_MODE must be ap, sta or off\n");
        return -1;
    }
    if (mkdir("/run/bf01-preview", 0700) != 0 && errno != EEXIST)
        return -1;
    int lock_fd = open("/run/bf01-preview/ipcamera.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "ipcamera: another ipcamera is running; do not start a second camera\n");
        if (lock_fd >= 0)
            close(lock_fd);
        return -1;
    }
    instance_fd = lock_fd;
    if (atexit(preview_network_stop) != 0) {
        preview_network_stop();
        return -1;
    }
    FILE *pid_file = fopen("/run/bf01-preview/ipcamera.pid", "w");
    if (!pid_file)
        return -1;
    int written = fprintf(pid_file, "%ld\n", (long)getpid());
    if (fclose(pid_file) != 0 || written < 0)
        return -1;
    unlink("/run/bf01-preview/service.ready");
    unlink("/run/bf01-preview/media.ready");
    if (setenv("BF01_PREVIEW", "1", 1) != 0 ||
        setenv("BF01_PREVIEW_SSID_FILE", "/run/bf01-preview/ap.ssid", 1) != 0)
        return -1;
    if (!strcmp(mode, "off")) {
        FILE *mode_file = fopen("/run/bf01-preview/mode", "w");
        if (!mode_file)
            return -1;
        int result = fprintf(mode_file, "off\n");
        if (fclose(mode_file) != 0 || result < 0)
            return -1;
        return 0;
    }
    ssize_t length = readlink("/proc/self/exe", helper, sizeof(helper) - 1);
    if (length <= 0 || (size_t)length >= sizeof(helper) - 1)
        return -1;
    helper[length] = '\0';
    char *name = strrchr(helper, '/');
    if (!name || (size_t)(name - helper) + sizeof("/bf01-preview") > sizeof(helper))
        return -1;
    strcpy(name, "/bf01-preview");
    if (access(helper, X_OK) != 0) {
        fprintf(stderr, "ipcamera: missing internal network helper: %s\n", helper);
        return 0;
    }
    snprintf(owner, sizeof(owner), "%ld", (long)getpid());
    unlink("/run/bf01-preview/network.ready");
    network_pid = fork();
    if (network_pid < 0) {
        fprintf(stderr, "ipcamera: network helper could not start; local capture continues\n");
        return 0;
    }
    if (network_pid == 0) {
        sigset_t signals;
        sigemptyset(&signals);
        sigprocmask(SIG_SETMASK, &signals, NULL);
        execl(helper, helper, "_network", mode, owner, (char *)NULL);
        _exit(127);
    }
    return 0;
}

int app_ipcam_Preview_Enabled(void)
{
    const char *enabled = getenv("BF01_PREVIEW");
    return enabled && !strcmp(enabled, "1");
}

static int preview_video_ready(void *opaque)
{
    unsigned int offset;
    int result;
    int ready;
    struct timespec now;
    (void)opaque;
    clock_gettime(CLOCK_MONOTONIC, &now);
    preview_frame.frameBufLen = CVI_MBUF_STREAM_MAX_SIZE;
    result = app_ipcam_Mbuf_ReadFrame(preview_reader, 0, &preview_frame, 1);
    if (result > 0 && preview_frame.frameParam.frameLen > 0 &&
        preview_frame.frameParam.frameLen <= CVI_MBUF_STREAM_MAX_SIZE &&
        preview_frame.frameParam.frameType != CVI_MEDIA_AFRAME_A) {
        last_frame = now.tv_sec;
        for (offset = 0; offset + 4 < preview_frame.frameParam.frameLen; offset++) {
            unsigned char *data = preview_frame.frameBuf + offset;
            unsigned int prefix = 0;
            unsigned int nal_type;
            if (!data[0] && !data[1] && data[2] == 1)
                prefix = 3;
            else if (!data[0] && !data[1] && !data[2] && data[3] == 1)
                prefix = 4;
            if (!prefix)
                continue;
            nal_type = data[prefix] & 31;
            if (nal_type == 7) {
                parameter_sets &= ~4U;
                parameter_sets |= 1;
            } else if (nal_type == 8) {
                parameter_sets &= ~4U;
                parameter_sets |= 2;
            } else if (nal_type == 5 && (parameter_sets & 3) == 3) {
                parameter_sets |= 4;
                last_keyframe = now.tv_sec;
            }
            offset += prefix;
        }
    }
    ready = parameter_sets == 7 && last_frame && last_keyframe &&
        now.tv_sec - last_frame < 3 && now.tv_sec - last_keyframe < 3;
    if (ready && !ready_written) {
        FILE *marker = fopen("/run/bf01-preview/media.ready", "w");
        if (marker) {
            fprintf(marker, "%ld\n", (long)getpid());
            fclose(marker);
            ready_written = 1;
            marker = fopen("/run/bf01-preview/service.ready", "w");
            if (marker)
                fclose(marker);
        }
    } else if (!ready && ready_written) {
        unlink("/run/bf01-preview/media.ready");
        unlink("/run/bf01-preview/service.ready");
        ready_written = 0;
    }
    return ready;
}

int app_ipcam_Preview_Start(void)
{
    APP_PARAM_RTSP_T *rtsp;
    APP_VENC_CHN_CFG_S *encoder;
    struct bf01_preview_config config;
    const char *session_text;
    char *end;
    long session = 1;
    if (!app_ipcam_Preview_Enabled())
        return 0;
    rtsp = app_ipcam_Rtsp_Param_Get();
    session_text = getenv("BF01_PREVIEW_SESSION");
    if (session_text) {
        session = strtol(session_text, &end, 10);
        if (!*session_text || *end)
            return -1;
    }
    if (session < 0 || session >= rtsp->session_cnt || session >= RTSP_INSTANCE_NUM)
        return -1;
    encoder = app_ipcam_VencChnCfg_Get(rtsp->VencChn[session]);
    if (!encoder || encoder->enType != PT_H264)
        return -1;
    preview_reader = app_ipcam_Mbuf_CreateReader(rtsp->VencChn[session], 1);
    preview_frame.frameBuf = malloc(CVI_MBUF_STREAM_MAX_SIZE);
    if (!preview_reader || !preview_frame.frameBuf) {
        app_ipcam_Preview_Stop();
        return -1;
    }
    memset(&config, 0, sizeof(config));
    config.interface_name = "wlan0";
    config.http_port = 80;
    config.rtsp_port = rtsp->port;
    config.session = (unsigned int)session;
    config.video_ready = preview_video_ready;
    if (bf01_preview_start(&config) != 0) {
        app_ipcam_Preview_Stop();
        return -1;
    }
    CVI_VENC_RequestIDR(rtsp->VencChn[session], CVI_TRUE);
    return 0;
}

void app_ipcam_Preview_Stop(void)
{
    bf01_preview_stop();
    if (preview_reader)
        app_ipcam_Mbuf_DestoryReader(preview_reader);
    preview_reader = NULL;
    free(preview_frame.frameBuf);
    preview_frame.frameBuf = NULL;
    parameter_sets = 0;
    last_frame = 0;
    last_keyframe = 0;
    ready_written = 0;
    if (app_ipcam_Preview_Enabled())
        unlink("/run/bf01-preview/media.ready");
}
#endif
