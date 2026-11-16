#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/signalfd.h>
#include <fcntl.h>
#include "app_ipcam_paramparse.h"
#include "cvi_mbuf.h"
#include "app_ipcam_preview.h"
#include "app_ipcam_capture.h"
#include "app_ipcam_storage.h"
#include "app_ipcam_boot_capture.h"
#include "app_ipcam_boot_ai.h"
#include "app_ipcam_boot_timestamp.h"
#include "app_ipcam_playback.h"
#include "app_ipcam_wifi_link.h"
#include "app_ipcam_wifi_camera.h"
#include "app_ipcam_event.h"
#include "app_ipcam_power_guard.h"

/**************************************************************************
 *                              M A C R O S                               *
 **************************************************************************/

/**************************************************************************
 *                           C O N S T A N T S                            *
 **************************************************************************/

/**************************************************************************
 *                          D A T A    T Y P E S                          *
 **************************************************************************/

/**************************************************************************
 *                         G L O B A L    D A T A                         *
 **************************************************************************/
static pthread_t g_pthMisc;
static CVI_BOOL g_bMisc;
static int live_only;
static int boot_capture_allowed = 1;

/**************************************************************************
 *                 E X T E R N A L    R E F E R E N C E S                 *
 **************************************************************************/

/**************************************************************************
 *               F U N C T I O N    D E C L A R A T I O N S               *
 **************************************************************************/
static int app_ipcam_Exit(void);

static int app_ipcam_Peripheral_Init(void)
{
    /* do peripheral Init here */
    app_ipcam_IRCut_Init();

    return CVI_SUCCESS;
}

static int app_ipcam_Peripheral_UnInit(void)
{
    /* do peripheral Init here */
    app_ipcam_IrCut_DeInit();

    return CVI_SUCCESS;
}

/* 
* this thread handle a series of small tasks include
* a. send AI framerate to Web-client
* b. 
*/
static void *ThreadMisc(void *arg)
{
    while (g_bMisc) {
        #ifdef BF01_BOOT_CAPTURE
        app_ipcam_BootTimestampPoll();
        #endif
        #ifdef WEB_SOCKET
        if (!app_ipcam_Preview_Enabled()) {
            app_ipcam_WebSocket_AiFps_Send();
        }
        #endif
        sleep(1);
    }

    return NULL;
}

static int app_ipcam_MiscThread_Init(void)
{
    int s32Ret = CVI_SUCCESS;

    g_bMisc = CVI_TRUE;
    s32Ret = pthread_create(&g_pthMisc, NULL, ThreadMisc, NULL);
    if (s32Ret != 0) {
        APP_PROF_LOG_PRINT(LEVEL_DEBUG,"pthread_create failed!\n");
        return s32Ret;
    }

    return CVI_SUCCESS;
}

static int app_ipcam_MiscThread_DeInit(void)
{
    g_bMisc = CVI_FALSE;

    if (g_pthMisc) {
        pthread_cancel(g_pthMisc);
        pthread_join(g_pthMisc, NULL);
        g_pthMisc = 0;
    }

    return CVI_SUCCESS;
}

static int app_ipcam_Exit(void)
{
    app_ipcam_PowerGuard_Stop();
    app_ipcam_WifiCamera_Stop();
    app_ipcam_Playback_Stop();
    if (!live_only)
        app_ipcam_Capture_Stop();
#ifdef BF01_BOOT_CAPTURE
    app_ipcam_BootVideo_UnInit();
    app_ipcam_BootCapture_UnInit();
#endif
    app_ipcam_Storage_Stop();
    app_ipcam_Preview_Stop();
    APP_CHK_RET(app_ipcam_MiscThread_DeInit(), "DeInit Misc Process");

    #ifdef RTSP_SUPPORT
    APP_CHK_RET(app_ipcam_rtsp_Server_Destroy(), "RTSP Server Destroy");
    #endif

    #ifdef CVI_UVC_SUPPORT
    app_uvc_exit();
    #endif

    #ifdef CVI_UAC_SUPPORT
    app_uac_exit();
    #endif

    #ifdef RECORD_SUPPORT
    APP_CHK_RET(app_ipcam_Record_UnInit(), "running SD Record");
    #endif

    #ifdef WEB_SOCKET
    if (!app_ipcam_Preview_Enabled()) {
        APP_CHK_RET(app_ipcam_WebSocket_DeInit(), "WebSocket DeInit");
        APP_CHK_RET(app_ipcam_NetCtrl_DeInit(), "NetCtrl DeInit");
    }
    #endif

    APP_CHK_RET(app_ipcam_Osdc_DeInit(), "OsdC DeInit");

    #ifdef AI_SUPPORT
    #ifdef PD_SUPPORT
    APP_CHK_RET(app_ipcam_Ai_PD_Stop(), "PD Stop");
    #endif
    #ifdef MD_SUPPORT
    APP_CHK_RET(app_ipcam_Ai_MD_Stop(), "MD Stop");
    #endif
    #ifdef FACE_SUPPORT
    APP_CHK_RET(app_ipcam_Ai_FD_Stop(), "FD Stop");
    #endif
    #endif

    #if defined(AUDIO_SUPPORT) && !defined(BF01_BOOT_CAPTURE)
    APP_CHK_RET(app_ipcam_Audio_UnInit(), "Audio Stop");
    #endif

    #ifdef VDEC_SUPPORT
    APP_CHK_RET(app_ipcam_Vdec_Stop(), "VDEC Stop");
    #endif

    #ifdef DISPLAY
    APP_CHK_RET(app_ipcam_Display_Exit(), "Display Exit");
    #endif

    APP_CHK_RET(app_ipcam_Venc_Stop(APP_VENC_ALL), "Venc Stop");

    APP_CHK_RET(app_ipcam_Vpss_DeInit(), "Vpss DeInit");

    APP_CHK_RET(app_ipcam_Vi_DeInit(), "Vi DeInit");

    APP_CHK_RET(app_ipcam_Sys_DeInit(), "System DeInit");

    APP_CHK_RET(app_ipcam_Peripheral_UnInit(), "UnInit Peripheral");

    #ifdef ANONMSG_ENABLE
    APP_CHK_RET(app_ipcam_MsgAnonDeInit(), "MsgAnon DeInit");
    #endif

    APP_CHK_RET(app_ipcam_Msg_Deinit(), "Msg Stop");

    APP_CHK_RET(app_ipcam_Mbuf_UnInit(), "UnInit Mbuf");

    return CVI_SUCCESS;

}

static int app_ipcam_Init(void)
{
#ifndef BF01_BOOT_CAPTURE
    APP_CHK_RET(app_ipcam_Msg_Init(), "init Msg failed");

    #ifdef ANONMSG_ENABLE
    APP_CHK_RET(app_ipcam_MsgAnonInit(), "init MsgAnon");
    #endif
#endif

    APP_CHK_RET(app_ipcam_Peripheral_Init(), "Init Peripheral");

    APP_CHK_RET(app_ipcam_Sys_Init(), "init systerm");

    APP_CHK_RET(app_ipcam_Vi_Init(), "init vi module");

    APP_CHK_RET(app_ipcam_Vpss_Init(), "init vpss module");

    APP_CHK_RET(app_ipcam_Osdc_Init(), "init Draw Osdc");

    #ifdef WEB_SOCKET
    if (!app_ipcam_Preview_Enabled()) {
        APP_CHK_RET(app_ipcam_NetCtrl_Init(), "Net Ctrl init");
        APP_CHK_RET(app_ipcam_WebSocket_Init(), "websocket init");
    }
    #endif

    APP_CHK_RET(app_ipcam_Venc_Init(APP_VENC_ALL), "init video encode");

    #ifdef VDEC_SUPPORT
    APP_CHK_RET(app_ipcam_Vdec_Init(), "Init VDEC");
    #endif

    #ifdef AUDIO_SUPPORT
    #ifdef BF01_BOOT_CAPTURE
    APP_PROF_LOG_PRINT(LEVEL_INFO, "RTOS owns MIC for the complete early video; Linux AI/AO disabled\n");
    #else
    int audioResult = app_ipcam_Audio_Init();
    if (audioResult != CVI_SUCCESS) {
        if (!app_ipcam_Audio_Param_Get()->bCaptureOnly)
            return audioResult;
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "MIC unavailable: %#x; preview and photo remain available, audio recording will report failure\n", audioResult);
    }
    #endif
    #endif

    #ifdef CVI_UVC_SUPPORT
    app_uvc_init();
    #endif

    #ifdef CVI_UAC_SUPPORT
    app_uac_init();
    #endif

    #ifdef DISPLAY
    APP_CHK_RET(app_ipcam_Display_Init(), "Init Display");
    #endif

    APP_CHK_RET(app_ipcam_MiscThread_Init(), "Init Misc Process");

    return CVI_SUCCESS;
}

int main(int argc, char *argv[])
{
    if (argc == 2 && !strcmp(argv[1], "--wifi-sync-time"))
        return app_ipcam_Wifi_SyncClock();
    if (argc == 2 && !strcmp(argv[1], "--wifi-service"))
        return app_ipcam_Wifi_Service();
    if (argc == 2 && !strcmp(argv[1], "--wifi-status"))
        return app_ipcam_Wifi_Control("status");
    if (argc == 3 && !strcmp(argv[1], "--wifi-get")) {
        if (!argv[2][0] || strlen(argv[2]) > 16)
            return EXIT_FAILURE;
        if (!strcmp(argv[2], "http"))
            return app_ipcam_Wifi_Control("refresh-http");
        char operation[32];
        snprintf(operation, sizeof(operation), "get:%s", argv[2]);
        return app_ipcam_Wifi_Control(operation);
    }
    if (argc == 2 && !strcmp(argv[1], "--wifi-refresh-http"))
        return app_ipcam_Wifi_Control("refresh-http");
    if (argc == 2 && !strcmp(argv[1], "--wifi-filter"))
        return app_ipcam_Wifi_Control("filter");
    if (argc == 2 && !strcmp(argv[1], "--wifi-camera-status"))
        return app_ipcam_Wifi_Control("camera-status");
    if (argc == 2 && !strcmp(argv[1], "--wifi-module-time"))
        return app_ipcam_Wifi_Control("module-time");
    if (argc == 2 && !strcmp(argv[1], "--wifi-speaker-status"))
        return app_ipcam_Wifi_Control("speaker-status");
    if (argc == 2 && !strcmp(argv[1], "--wifi-microphone-status"))
        return app_ipcam_Wifi_Control("microphone-status");
#ifdef BF01_BOOT_CAPTURE
    if (argc == 2 && !strcmp(argv[1], "--boot-ai-worker"))
        return app_ipcam_BootAi_Worker();
    if (argc == 2 && !strcmp(argv[1], "--sync-rtc"))
        return app_ipcam_BootTimestampSyncRtc() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
    if (argc > 1 && !strcmp(argv[1], "--live-camera")) {
        live_only = 1;
        argc--;
        argv++;
    }
    if (argc > 1 && !strcmp(argv[1], "--capture-config")) {
        if (argc != 4) {
            fprintf(stderr, "Usage: ipcamera --capture-config SECONDS(1-300) CHANNEL(0-2)\n");
            return EXIT_FAILURE;
        }
        return app_ipcam_Capture_Configure(argv[2], argv[3]) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
#ifdef AUDIO_SUPPORT
    APP_CHK_RET(app_ipcam_Audio_SpeakerDisable(), "disable speaker before startup");
#endif
    if (access("/proc/sys/kernel/core_pattern", W_OK) == 0)
        system("echo /mnt/nfs/core-%e-%p-%t > /proc/sys/kernel/core_pattern");
    APP_CHK_RET(app_ipcam_Opts_Parse(argc, argv), "parse optinos");

    sigset_t photoSignals;
    sigemptyset(&photoSignals);
    sigaddset(&photoSignals, SIGUSR1);
#ifdef BF01_AUDIO_DUPLEX
    sigaddset(&photoSignals, SIGUSR2);
#endif
    sigaddset(&photoSignals, SIGINT);
    sigaddset(&photoSignals, SIGTERM);
    APP_CHK_RET(pthread_sigmask(SIG_BLOCK, &photoSignals, NULL), "block photo signal");

    APP_CHK_RET(app_ipcam_PowerGuard_Start(), "start GPIO power guard");

#ifdef BF01_BOOT_CAPTURE
    int boot_ai_enabled = 1;
    APP_CHK_RET(app_ipcam_Capture_LoadAiEnabled(&boot_ai_enabled), "load boot AI switch");
    fprintf(stderr, "[boot-ai] recognition=%s\n", boot_ai_enabled ? "enabled" : "disabled");
    APP_CHK_RET(app_ipcam_BootTimestampLockIpc(), "lock camera IPC");
    APP_CHK_RET(app_ipcam_Msg_Init(), "init early Msg");
    APP_CHK_RET(app_ipcam_MsgAnonInit(), "init early MsgAnon");
    APP_CHK_RET(app_ipcam_BootCapture_Init(), "init early RTOS capture receiver");
    APP_CHK_RET(app_ipcam_BootVideo_Init(), "init early RTOS video receiver");
    if (!live_only) {
        app_ipcam_BootTimestampPoll();
        int gate_result = 0;
        if (!app_event_boot_cancelled())
            gate_result = boot_ai_enabled ? app_ipcam_BootAi_Run() : app_ipcam_BootAi_Bypass();
        if (app_event_boot_cancelled()) {
            app_ipcam_BootAi_Discard();
            app_ipcam_BootCapture_Discard();
            app_ipcam_BootVideo_Stop();
            boot_capture_allowed = 0;
        } else if (gate_result) {
            fprintf(stderr, "[boot-ai] gate closed: error=%d; stop recording, no SD media writes\n", gate_result);
            app_ipcam_BootVideo_UnInit();
            app_ipcam_BootCapture_UnInit();
            boot_capture_allowed = 0;
            fprintf(stderr, "[boot-ai] media gate closed; camera control remains available for APP live\n");
        }
    } else {
        APP_CHK_RET(app_ipcam_BootAi_Discard(), "discard AI input on camera recovery");
        APP_CHK_RET(app_ipcam_BootCapture_Discard(), "discard boot photos on camera recovery");
        APP_CHK_RET(app_ipcam_BootVideo_Stop(), "discard boot video on camera recovery");
        fprintf(stderr, "[live] dedicated camera: no boot capture, recording or AI policy changes\n");
    }
#endif

    APP_CHK_RET(app_ipcam_Preview_Prepare(), "prepare integrated camera network");

    /* load each moudles parameter from param_config.ini */
    APP_CHK_RET(app_ipcam_Param_Load(), "load global parameter");

    APP_CHK_RET(app_ipcam_Mbuf_Init(), "Init Mbuf");

    /* init modules include <Peripheral; Sys; VI; VB; OSD; Venc; AI; Audio; etc.> */
    APP_CHK_RET(app_ipcam_Init(), "app_ipcam_Init");

    #ifndef BF01_BOOT_CAPTURE
    if (app_ipcam_Playback_Start() != 0)
        fprintf(stderr, "[audio] playback unavailable; capture and preview remain enabled\n");
    #endif

    /* create rtsp server */
    APP_CHK_RET(app_ipcam_Rtsp_Server_Create(), "create rtsp server");

    /* start video encode */
    APP_CHK_RET(app_ipcam_Venc_Start(APP_VENC_ALL), "start video processing");

    APP_CHK_RET(app_ipcam_Storage_Start(), "start SD storage monitor");
    if (!live_only && boot_capture_allowed && !app_event_boot_cancelled()) {
        APP_CHK_RET(app_ipcam_Capture_Start(), "start boot capture workflow");
    }

    int wifiCameraError = app_ipcam_WifiCamera_Start();
    if (wifiCameraError)
        fprintf(stderr, "[wifi-camera] control unavailable error=%d; camera continues\n", wifiCameraError);

    int previewStarted = app_ipcam_Preview_Start() == CVI_SUCCESS;
    if (!previewStarted)
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "Preview control unavailable; local capture remains enabled\n");
    unsigned int previewRetry = 0;

    #ifdef VDEC_SUPPORT
    APP_CHK_RET(app_ipcam_Vdec_Start(), "Start VDEC");
    #endif

    #ifdef AI_SUPPORT
    #ifdef PD_SUPPORT
    /* start AI PD (Pedestrian Detection) */
    APP_CHK_RET(app_ipcam_Ai_PD_Start(), "running AI PD");
    #endif

    #ifdef MD_SUPPORT
    /* start AI MD (Motion Detection)*/
    APP_CHK_RET(app_ipcam_Ai_MD_Start(), "running AI MD");
    #endif

    /* start AI FD (Face Detection)*/
    #ifdef FACE_SUPPORT
    #ifdef IR_FACE_SUPPORT
    APP_CHK_RET(app_ipcam_Ai_IR_FD_Start(), "running AI IR FD");
    #else
    APP_CHK_RET(app_ipcam_Ai_FD_Start(), "running AI FD");
    #endif
    #endif

    #ifdef AUDIO_SUPPORT
    #ifdef CRY_SUPPORT
    APP_CHK_RET(app_ipcam_Ai_Cry_Start(), "running AI CRY");
    #endif
    #endif

    #endif

    #ifdef RECORD_SUPPORT
    /* start SD Record */
    APP_CHK_RET(app_ipcam_Record_Recover_Init(), "Init SD Record Recover");
    APP_CHK_RET(app_ipcam_Record_Init(), "running SD Record");
    #endif

    /* enable receive a command form another progress for test ipcam */
    //APP_CHK_RET(app_ipcam_CmdTask_Create(), "running cmd test");

    int signal_descriptor = signalfd(-1, &photoSignals, SFD_CLOEXEC | SFD_NONBLOCK);
    if (signal_descriptor < 0) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "Camera signal descriptor failed: %d\n", errno);
        return app_ipcam_Exit();
    }
    int readiness = wifiCameraError ? -1 :
        open("/run/bf01-preview/camera-loop.ready", O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (readiness >= 0)
        close(readiness);
    while (1) {
        struct pollfd descriptors[] = {
            { .fd = app_ipcam_WifiCamera_Descriptor(), .events = POLLIN },
            { .fd = signal_descriptor, .events = POLLIN },
        };
        int ready = poll(descriptors, sizeof(descriptors) / sizeof(descriptors[0]), 1000);
        if (ready < 0 && errno != EINTR) {
            close(signal_descriptor);
            return app_ipcam_Exit();
        }
        struct signalfd_siginfo notification;
        int receivedSignal = read(signal_descriptor, &notification, sizeof(notification)) == sizeof(notification) ?
            (int)notification.ssi_signo : -1;
        if (receivedSignal == SIGINT || receivedSignal == SIGTERM) {
            close(signal_descriptor);
            return app_ipcam_Exit();
        }
        if (descriptors[0].revents & POLLIN)
            app_ipcam_WifiCamera_Poll();
        if (receivedSignal == SIGUSR1) {
            if (live_only || app_event_live()) {
                fprintf(stderr, "[live] photo signal ignored by live-only camera\n");
                continue;
            }
#ifdef BF01_BOOT_CAPTURE
            if (!app_ipcam_BootCapture_Ready()) {
                fprintf(stderr, "[boot-capture] manual photo rejected: RTOS still owns JPEG channel\n");
                continue;
            }
#endif
            uint64_t generation;
            int storageError = app_ipcam_Storage_Acquire(&generation);
            if (storageError) {
                fprintf(stderr, "[storage] manual photo rejected: SD not ready, error=%d\n", storageError);
            } else {
                app_ipcam_JpgCapFlag_Set(CVI_TRUE);
                app_ipcam_Storage_Release();
            }
#ifdef BF01_AUDIO_DUPLEX
        } else if (receivedSignal == SIGUSR2) {
            const char *prompt = getenv("BF01_AUDIO_PROMPT");
            if (!prompt || !*prompt)
                prompt = "/mnt/data/Button.wav";
            int playbackResult = app_ipcam_Playback_Play(prompt);
            fprintf(stderr, "[audio] local prompt request: file=%s result=%d\n", prompt, playbackResult);
#endif
        }
        app_ipcam_Preview_NetworkAlive();
        if (!previewStarted && previewRetry++ % 5 == 0) {
            const char *mode = getenv("BF01_WIFI_MODE");
            if (access("/run/bf01-preview/network.ready", F_OK) == 0 ||
                (mode && !strcmp(mode, "off") && access("/sys/class/net/wlan0", F_OK) == 0))
                previewStarted = app_ipcam_Preview_Start() == CVI_SUCCESS;
        }
    };
    
    return CVI_SUCCESS;
}
