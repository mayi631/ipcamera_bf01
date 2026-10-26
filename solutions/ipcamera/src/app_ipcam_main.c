#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include "app_ipcam_paramparse.h"
#include "cvi_mbuf.h"
#include "app_ipcam_preview.h"

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

    #ifdef AUDIO_SUPPORT
    APP_CHK_RET(app_ipcam_Audio_UnInit(), "Audio Stop");
    #endif

    #ifdef VDEC_SUPPORT
    APP_CHK_RET(app_ipcam_Vdec_Stop(), "VDEC Stop");
    #endif

    #ifdef DISPLAY
    APP_CHK_RET(app_ipcam_Display_Exit(), "Display Exit");
    #endif

    APP_CHK_RET(app_ipcam_Vpss_DeInit(), "Vpss DeInit");

    APP_CHK_RET(app_ipcam_Venc_Stop(APP_VENC_ALL), "Venc Stop");

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
    APP_CHK_RET(app_ipcam_Msg_Init(), "init Msg failed");

    #ifdef ANONMSG_ENABLE
    APP_CHK_RET(app_ipcam_MsgAnonInit(), "init MsgAnon");
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
    APP_CHK_RET(app_ipcam_Audio_Init(), "start audio processing");
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
    system("echo /mnt/nfs/core-%e-%p-%t > /proc/sys/kernel/core_pattern");
    APP_CHK_RET(app_ipcam_Opts_Parse(argc, argv), "parse optinos");

    sigset_t photoSignals;
    sigemptyset(&photoSignals);
    sigaddset(&photoSignals, SIGUSR1);
    sigaddset(&photoSignals, SIGINT);
    sigaddset(&photoSignals, SIGTERM);
    APP_CHK_RET(pthread_sigmask(SIG_BLOCK, &photoSignals, NULL), "block photo signal");

    APP_CHK_RET(app_ipcam_Preview_Prepare(), "prepare integrated camera network");

    /* load each moudles parameter from param_config.ini */
    APP_CHK_RET(app_ipcam_Param_Load(), "load global parameter");

    APP_CHK_RET(app_ipcam_Mbuf_Init(), "Init Mbuf");

    /* init modules include <Peripheral; Sys; VI; VB; OSD; Venc; AI; Audio; etc.> */
    APP_CHK_RET(app_ipcam_Init(), "app_ipcam_Init");

    /* create rtsp server */
    APP_CHK_RET(app_ipcam_Rtsp_Server_Create(), "create rtsp server");

    /* start video encode */
    APP_CHK_RET(app_ipcam_Venc_Start(APP_VENC_ALL), "start video processing");

    APP_CHK_RET(app_ipcam_Preview_Start(), "start BF01 preview control");

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

    while (1) {
        struct timespec waitTime = { .tv_sec = 1 };
        int receivedSignal = sigtimedwait(&photoSignals, NULL, &waitTime);
        int signalError = errno;
        if (receivedSignal == SIGUSR1) {
            app_ipcam_JpgCapFlag_Set(CVI_TRUE);
        } else if (receivedSignal == SIGINT || receivedSignal == SIGTERM) {
            return app_ipcam_Exit();
        } else if ((receivedSignal < 0 && signalError != EAGAIN && signalError != EINTR) ||
            !app_ipcam_Preview_NetworkAlive()) {
            APP_PROF_LOG_PRINT(LEVEL_ERROR, "Camera control or network helper stopped; shutting down ipcamera\n");
            app_ipcam_Exit();
            return CVI_FAILURE;
        }
    };
    
    return CVI_SUCCESS;
}
