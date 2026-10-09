#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/time.h>
#include <unistd.h>
#include "cvi_comm_sys.h"
#include "cvi_bin.h"
#include "cvi_ae.h"
#include "cvi_awb.h"
#include "cvi_sensor.h"
#include "app_ipcam_vi.h"
#include "cvi_ispd2.h"
#include "app_ipcam_paramparse.h"
#include "app_ipcam_ircut.h"
#ifdef SUPPORT_ISP_PQTOOL
#include <dlfcn.h>
#endif


/**************************************************************************
 *                              M A C R O S                               *
 **************************************************************************/
#ifdef SUPPORT_ISP_PQTOOL
#define ISPD_LIBNAME "libcvi_ispd2.so"
#define ISPD_CONNECT_PORT 5566
/* 183x not support continuous RAW dump */
#endif

/**************************************************************************
 *                           C O N S T A N T S                            *
 **************************************************************************/

/**************************************************************************
 *                          D A T A    T Y P E S                          *
 **************************************************************************/

/**************************************************************************
 *                         G L O B A L    D A T A                         *
 **************************************************************************/

APP_PARAM_VI_CTX_S g_stViCtx, *g_pstViCtx = &g_stViCtx;

static pthread_t AF_pthread;
static CVI_BOOL bAfFilterEnable;
static pthread_t g_IspPid[VI_MAX_DEV_NUM];

#ifdef SUPPORT_ISP_PQTOOL
static CVI_BOOL bISPDaemon = CVI_FALSE;
//static CVI_VOID *pISPDHandle = NULL;
#endif

APP_PARAM_VI_PM_DATA_S ViPmData[VI_MAX_DEV_NUM] = { 0 };

VI_DEV_ATTR_S vi_dev_attr_base = {
    .enIntfMode = VI_MODE_MIPI,
    .enWorkMode = VI_WORK_MODE_1Multiplex,
    .enScanMode = VI_SCAN_PROGRESSIVE,
    .as32AdChnId = {-1, -1, -1, -1},
    .enDataSeq = VI_DATA_SEQ_YUYV,
    .stSynCfg = {
    /*port_vsync    port_vsync_neg    port_hsync              port_hsync_neg*/
    VI_VSYNC_PULSE, VI_VSYNC_NEG_LOW, VI_HSYNC_VALID_SINGNAL, VI_HSYNC_NEG_HIGH,
    /*port_vsync_valid     port_vsync_valid_neg*/
    VI_VSYNC_VALID_SIGNAL, VI_VSYNC_VALID_NEG_HIGH,

    /*hsync_hfb  hsync_act  hsync_hhb*/
    {0,           1920,       0,
    /*vsync0_vhb vsync0_act vsync0_hhb*/
     0,           1080,       0,
    /*vsync1_vhb vsync1_act vsync1_hhb*/
     0,            0,         0}
    },
    .enInputDataType = VI_DATA_TYPE_RGB,
    .stSize = {1920, 1080},
    .stWDRAttr = {WDR_MODE_NONE, 1080},
    .enBayerFormat = BAYER_FORMAT_BG,
};

VI_PIPE_ATTR_S vi_pipe_attr_base = {
    .enPipeBypassMode = VI_PIPE_BYPASS_NONE,
    .bYuvSkip = CVI_FALSE,
    .bIspBypass = CVI_FALSE,
    .u32MaxW = 1920,
    .u32MaxH = 1080,
    .enPixFmt = PIXEL_FORMAT_RGB_BAYER_12BPP,
    .enCompressMode = COMPRESS_MODE_NONE,
    .enBitWidth = DATA_BITWIDTH_12,
    .bNrEn = CVI_TRUE,
    .bSharpenEn = CVI_FALSE,
    .stFrameRate = {-1, -1},
    .bDiscardProPic = CVI_FALSE,
    .bYuvBypassPath = CVI_FALSE,
};

VI_CHN_ATTR_S vi_chn_attr_base = {
    .stSize = {1920, 1080},
    .enPixelFormat = PIXEL_FORMAT_YUV_PLANAR_420,
    .enDynamicRange = DYNAMIC_RANGE_SDR8,
    .enVideoFormat = VIDEO_FORMAT_LINEAR,
    .enCompressMode = COMPRESS_MODE_NONE,
    .bMirror = CVI_FALSE,
    .bFlip = CVI_FALSE,
    .u32Depth = 0,
    .stFrameRate = {-1, -1},
};

ISP_PUB_ATTR_S isp_pub_attr_base = {
    .stWndRect = {0, 0, 1920, 1080},
    .stSnsSize = {1920, 1080},
    .f32FrameRate = 25.0f,
    .enBayer = BAYER_BGGR,
    .enWDRMode = WDR_MODE_NONE,
    .u8SnsMode = 0,
};

static pthread_t g_RgbIR_Thread;
static CVI_BOOL Auto_Rgb_Ir_Enable;

/**************************************************************************
 *                 E X T E R N A L    R E F E R E N C E S                 *
 **************************************************************************/

/**************************************************************************
 *               F U N C T I O N    D E C L A R A T I O N S               *
 **************************************************************************/

APP_PARAM_VI_CTX_S *app_ipcam_Vi_Param_Get(void)
{
    return g_pstViCtx;
}

static CVI_S32 app_ipcam_Isp_AfFilter_Init(CVI_VOID)
{
    CVI_S32 s32Ret = CVI_SUCCESS;
    VI_PIPE ViPipe = 0;
    //CVI_CHAR input[10];
    ISP_STATISTICS_CFG_S stsCfg;
    ISP_PUB_ATTR_S stPubAttr;

    ISP_CTRL_PARAM_S stIspCtrlParam;
    CVI_ISP_GetCtrlParam(ViPipe, &stIspCtrlParam);
    stIspCtrlParam.u32AFStatIntvl = 1;
    CVI_ISP_SetCtrlParam(ViPipe, &stIspCtrlParam);

    // Get current statistic and related size setting.
    s32Ret = CVI_ISP_GetStatisticsConfig(ViPipe, &stsCfg);
    s32Ret |= CVI_ISP_GetPubAttr(ViPipe, &stPubAttr);
    if (s32Ret != CVI_SUCCESS) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "Get Statistic info fail with %#x\n", s32Ret);
        return s32Ret;
    }
    // Config AF Enable.
    stsCfg.stFocusCfg.stConfig.bEnable = 1;
    // Config low pass filter.
    stsCfg.stFocusCfg.stConfig.u8HFltShift = 0;
    stsCfg.stFocusCfg.stConfig.s8HVFltLpCoeff[0] = 0;
    stsCfg.stFocusCfg.stConfig.s8HVFltLpCoeff[1] = 1;
    stsCfg.stFocusCfg.stConfig.s8HVFltLpCoeff[2] = 2;
    stsCfg.stFocusCfg.stConfig.s8HVFltLpCoeff[3] = 3;
    stsCfg.stFocusCfg.stConfig.s8HVFltLpCoeff[4] = 4;
    // Config gamma enable.
    stsCfg.stFocusCfg.stConfig.stRawCfg.PreGammaEn  = 0;
    // Config pre NR enable.
    stsCfg.stFocusCfg.stConfig.stPreFltCfg.PreFltEn = 1;
    // Config H & V window.
    stsCfg.stFocusCfg.stConfig.u16Hwnd = 17;
    stsCfg.stFocusCfg.stConfig.u16Vwnd = 15;
    // Config crop related setting. Has some limitation
    stsCfg.stFocusCfg.stConfig.stCrop.bEnable = 1;
    stsCfg.stFocusCfg.stConfig.stCrop.u16X = 8;
    stsCfg.stFocusCfg.stConfig.stCrop.u16Y = 2;
    stsCfg.stFocusCfg.stConfig.stCrop.u16W = stPubAttr.stWndRect.u32Width - 8 * 2;
    stsCfg.stFocusCfg.stConfig.stCrop.u16H = stPubAttr.stWndRect.u32Height - 2 * 2;
    // Config first horizontal high pass filter.
    stsCfg.stFocusCfg.stHParam_FIR0.s8HFltHpCoeff[0] = 0;
    stsCfg.stFocusCfg.stHParam_FIR0.s8HFltHpCoeff[1] = 0;
    stsCfg.stFocusCfg.stHParam_FIR0.s8HFltHpCoeff[2] = 13;
    stsCfg.stFocusCfg.stHParam_FIR0.s8HFltHpCoeff[3] = 24;
    stsCfg.stFocusCfg.stHParam_FIR0.s8HFltHpCoeff[4] = 0;
    // Config 2nd horizontal high pass filter.
    stsCfg.stFocusCfg.stHParam_FIR1.s8HFltHpCoeff[0] = 1;
    stsCfg.stFocusCfg.stHParam_FIR1.s8HFltHpCoeff[1] = 2;
    stsCfg.stFocusCfg.stHParam_FIR1.s8HFltHpCoeff[2] = 4;
    stsCfg.stFocusCfg.stHParam_FIR1.s8HFltHpCoeff[3] = 8;
    stsCfg.stFocusCfg.stHParam_FIR1.s8HFltHpCoeff[4] = 0;

    // Config vertical high pass filter.
    stsCfg.stFocusCfg.stVParam_FIR.s8VFltHpCoeff[0] = 8;
    stsCfg.stFocusCfg.stVParam_FIR.s8VFltHpCoeff[1] = -15;
    stsCfg.stFocusCfg.stVParam_FIR.s8VFltHpCoeff[2 ] = 0;
    stsCfg.unKey.bit1FEAfStat = 1;

    // Config high luma thr
    stsCfg.stFocusCfg.stConfig.u16HighLumaTh = 3800;

    //LDG
    // stsCfg.stFocusCfg.stConfig.u8ThLow = 0;
    // stsCfg.stFocusCfg.stConfig.u8ThHigh = 255;
    // stsCfg.stFocusCfg.stConfig.u8GainLow = 30;
    // stsCfg.stFocusCfg.stConfig.u8GainHigh = 20;
    // stsCfg.stFocusCfg.stConfig.u8SlopLow = 8;
    // stsCfg.stFocusCfg.stConfig.u8SlopHigh = 15;

    s32Ret = CVI_ISP_SetStatisticsConfig(ViPipe, &stsCfg);
    if (s32Ret != CVI_SUCCESS) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "Set Statistic info fail with %#x\n", s32Ret);
        return s32Ret;
    }

    return CVI_SUCCESS;
}

void app_ipcam_Isp_AfFilter_Get(ISP_AF_STATISTICS_S *pAfStat)
{
    CVI_U64 FVP = 0, FVPn = 0, FVQ = 0, FVQn = 0;
    CVI_U32 totalWeightSum = 0;
    CVI_U32 row, col;
    const CVI_U32 weight1 = 1, weight2 = 1, weight3 = 1;

    for (row = 0; row < AF_ZONE_ROW; row++) {
        for (col = 0; col < AF_ZONE_COLUMN; col++) {
            CVI_U64 h0 = pAfStat->stFEAFStat.stZoneMetrics[row][col].u64h0;
            CVI_U64 h1 = pAfStat->stFEAFStat.stZoneMetrics[row][col].u64h1;
            CVI_U32 v0 = pAfStat->stFEAFStat.stZoneMetrics[row][col].u32v0;
            FVPn = (weight1 * h1 + weight3 * v0) / (weight1 + weight3);
            FVQn = (weight1 * h0 + weight2 * v0) / (weight1 + weight2);
            FVP += FVPn;
            FVQ += FVQn;
            totalWeightSum += 1;
        }
    }

    FVP = FVP / totalWeightSum;
    FVQ = FVQ / totalWeightSum;

    APP_PROF_LOG_PRINT(LEVEL_TRACE, "FVP = %llu FVQ = %llu\n", FVP, FVQ);

    ISP_EXP_INFO_S stExpInfo;
    CVI_ISP_QueryExposureInfo(0, &stExpInfo);

    CVI_U8 u8P1 = FVP & 0xFF00;
    CVI_U8 u8P2 = FVP & 0x00FF;
    CVI_U8 u8Q1 = FVQ & 0xFF00;
    CVI_U8 u8Q2 = FVQ & 0x00FF;
    CVI_U8 u8R1 = log(stExpInfo.u32ISO)/log(2);

    APP_PROF_LOG_PRINT(LEVEL_DEBUG, "u32ISO=%d log(stExpInfo.u32ISO/100)/log(2)=%f\n", stExpInfo.u32ISO, log(stExpInfo.u32ISO/100)/log(2));

    APP_PROF_LOG_PRINT(LEVEL_TRACE, "R1=%d P1=%d P2=%d Q1=%d Q2=%d\n", u8R1, u8P1, u8P2, u8Q1, u8Q2);
}

static void *Thread_AF_Filter_Proc(void *pArgs)
{
    CVI_S32 s32Ret = CVI_SUCCESS;

    CVI_CHAR TaskName[64];
    sprintf(TaskName, "AF_Filter_Get");
    prctl(PR_SET_NAME, TaskName, 0, 0, 0);
    APP_PROF_LOG_PRINT(LEVEL_INFO, "Thread_AF_Filter_Proc task started \n");

    ISP_AF_STATISTICS_S afStat;
    CVI_U32 row, col;

    VI_PIPE ViPipe = 0;
    ISP_VD_TYPE_E enIspVDType = ISP_VD_FE_START;
    CVI_U64 FVn = 0, FV = 0;
    CVI_U32 totalWeightSum = 0;
    // weight for each statistic
    const CVI_U32 weight1 = 1, weight2 = 1, weight3 = 1;
    const CVI_U32 blockWeightSum = weight1 + weight2 + weight3;

    while (bAfFilterEnable) {
        s32Ret = CVI_ISP_GetVDTimeOut(ViPipe, enIspVDType, 5000);
        s32Ret |= CVI_ISP_GetFocusStatistics(ViPipe, &afStat);
        if (s32Ret != CVI_SUCCESS) {
            APP_PROF_LOG_PRINT(LEVEL_ERROR, "Get Statistic failed with %#x\n", s32Ret);
            continue;
        }
        FVn = 0, FV = 0;
        totalWeightSum = 0;
        // calculate AF statistics
        for (row = 0; row < AF_ZONE_ROW; row++) {
            for (col = 0; col < AF_ZONE_COLUMN; col++) {
                CVI_U64 h0 = afStat.stFEAFStat.stZoneMetrics[row][col].u64h0;
                CVI_U64 h1 = afStat.stFEAFStat.stZoneMetrics[row][col].u64h1;
                CVI_U32 v0 = afStat.stFEAFStat.stZoneMetrics[row][col].u32v0;
                FVn = (weight1 * h0 + weight2 * h1 + weight3 * v0) / blockWeightSum;
                FV += FVn;
                totalWeightSum += 1;
            }
        }

        FV = FV / totalWeightSum;

        CVI_U32 u32Fv = FV & 0xFFFFFFFF;

        APP_PROF_LOG_PRINT(LEVEL_TRACE, "FV = %llu, u32Fv = %u\n", FV, u32Fv);

        /* for customer used */
        app_ipcam_Isp_AfFilter_Get(&afStat);
    }

    return (void *) CVI_SUCCESS;
}


int app_ipcam_Isp_AfFilter_Start(void)
{
    CVI_S32 s32Ret = CVI_SUCCESS;

    s32Ret = app_ipcam_Isp_AfFilter_Init();
    if(s32Ret != CVI_SUCCESS) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "app_ipcam_Isp_AfFilter_Start failed with %#x\n", s32Ret);
        return CVI_FAILURE;
    }

    pthread_attr_t pthread_attr;
    pthread_attr_init(&pthread_attr);

    s32Ret = pthread_create(
                    &AF_pthread,
                    &pthread_attr,
                    Thread_AF_Filter_Proc,
                    NULL);
    if (s32Ret) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "AF filter pthread_create failed:0x%x\n", s32Ret);
        return CVI_FAILURE;
    }

    return CVI_SUCCESS;
}

#ifdef SUPPORT_ISP_PQTOOL
CVI_VOID app_ipcam_Ispd_Load(CVI_VOID)
{
    // char *dlerr = NULL;
    // UNUSED(dlerr);
    if (!bISPDaemon) {
        isp_daemon2_init(ISPD_CONNECT_PORT);
        APP_PROF_LOG_PRINT(LEVEL_INFO, "Isp_daemon2_init %d success\n", ISPD_CONNECT_PORT);
        bISPDaemon = CVI_TRUE;
        //pISPDHandle = dlopen(ISPD_LIBNAME, RTLD_NOW);

        // dlerr = dlerror();
        // if (pISPDHandle) {
        //     APP_PROF_LOG_PRINT(LEVEL_INFO, "Load dynamic library %s success\n", ISPD_LIBNAME);

        //     void (*daemon_init)(unsigned int port);
        //     daemon_init = dlsym(pISPDHandle, "isp_daemon2_init");

        //     dlerr = dlerror();
        //     if (dlerr == NULL) {
        //         (*daemon_init)(ISPD_CONNECT_PORT);
        //         bISPDaemon = CVI_TRUE;
        //     } else {
        //         APP_PROF_LOG_PRINT(LEVEL_ERROR, "Run daemon initial failed with %s\n", dlerr);
        //         dlclose(pISPDHandle);
        //         pISPDHandle = NULL;
        //     }
        // } else {
        //     APP_PROF_LOG_PRINT(LEVEL_ERROR, "Load dynamic library %s failed with %s\n", ISPD_LIBNAME, dlerr);
        // }
    } else {
        APP_PROF_LOG_PRINT(LEVEL_WARN, "%s already loaded\n", ISPD_LIBNAME);
    }
}

static CVI_VOID app_ipcam_Ispd_Unload(CVI_VOID)
{
    if (bISPDaemon) {
        // void (*daemon_uninit)(void);

        // daemon_uninit = dlsym(pISPDHandle, "isp_daemon_uninit");
        // if (dlerror() == NULL) {
        //     (*daemon_uninit)();
        // }

        // dlclose(pISPDHandle);
        // pISPDHandle = NULL;

        bISPDaemon = CVI_FALSE;
    } else {
        APP_PROF_LOG_PRINT(LEVEL_WARN, "%s not load yet!\n", ISPD_LIBNAME);
    }
}
#endif

CVI_S32 app_ipcam_Vi_DevAttr_Get(SNS_TYPE_E enSnsType, VI_DEV_ATTR_S *pstViDevAttr)
{
    CVI_S32 s32Ret = CVI_SUCCESS;

    memcpy(pstViDevAttr, &vi_dev_attr_base, sizeof(VI_DEV_ATTR_S));
    switch (enSnsType) {
        case PIXELPLUS_PR2020_1M_25FPS_8BIT:
        case PIXELPLUS_PR2020_1M_30FPS_8BIT:
        case PIXELPLUS_PR2020_2M_25FPS_8BIT:
        case PIXELPLUS_PR2020_2M_30FPS_8BIT:
            pstViDevAttr->enDataSeq = VI_DATA_SEQ_YUYV;
            pstViDevAttr->enInputDataType = VI_DATA_TYPE_YUV;
            pstViDevAttr->enIntfMode = VI_MODE_MIPI_YUV422;
            break;
        default:
            break;
    };

    // TODO add BT601 sensor
    switch (enSnsType) {
        case GCORE_GC0308_MIPI_1M_30FPS_8BIT:
        case GCORE_GC2145_MIPI_2M_12FPS_8BIT:
            pstViDevAttr->enDataSeq = VI_DATA_SEQ_YUYV;
            pstViDevAttr->enInputDataType = VI_DATA_TYPE_YUV;
            pstViDevAttr->enIntfMode = VI_MODE_BT601;
            break;
        default:
            break;
    }
    // BT656
    switch (enSnsType) {
        case PIXELPLUS_PR2020_1M_25FPS_8BIT:
        case PIXELPLUS_PR2020_1M_30FPS_8BIT:
        case PIXELPLUS_PR2020_2M_25FPS_8BIT:
        case PIXELPLUS_PR2020_2M_30FPS_8BIT:
            pstViDevAttr->enIntfMode = VI_MODE_BT656;
            break;
        default:
            break;
    };
    switch (enSnsType) {
        // Sony
        case SONY_IMX307_MIPI_2M_30FPS_12BIT:
        case SONY_IMX307_MIPI_2M_30FPS_12BIT_WDR2TO1:
        case SONY_IMX307_SLAVE_MIPI_2M_30FPS_12BIT:
        case SONY_IMX307_SLAVE_MIPI_2M_30FPS_12BIT_WDR2TO1:
        case SONY_IMX307_2L_MIPI_2M_30FPS_12BIT:
        case SONY_IMX307_2L_MIPI_2M_30FPS_12BIT_WDR2TO1:
        case SONY_IMX327_MIPI_2M_30FPS_12BIT:
        case SONY_IMX327_MIPI_2M_30FPS_12BIT_WDR2TO1:
        case SONY_IMX675_MIPI_5M_30FPS_12BIT:
        // GalaxyCore
        case GCORE_GC02M1_MIPI_2M_30FPS_10BIT:
        case GCORE_GC1054_MIPI_1M_30FPS_10BIT:
        case GCORE_GC2053_MIPI_2M_30FPS_10BIT:
        case GCORE_GC2093_MIPI_2M_30FPS_10BIT:
        case GCORE_GC2093_MIPI_2M_30FPS_10BIT_WDR2TO1:
        case GCORE_GC4023_MIPI_4M_30FPS_10BIT:
            pstViDevAttr->enBayerFormat = BAYER_FORMAT_RG;
            break;
            // brigates
        case GCORE_GC4653_MIPI_4M_30FPS_10BIT:
            pstViDevAttr->enBayerFormat = BAYER_FORMAT_GR;
            break;
        default:
            pstViDevAttr->enBayerFormat = BAYER_FORMAT_BG;
            break;
    };

    return s32Ret;
}

CVI_S32 app_ipcam_Vi_PipeAttr_Get(SNS_TYPE_E enSnsType, VI_PIPE_ATTR_S *pstViPipeAttr)
{
    CVI_S32 s32Ret = CVI_SUCCESS;

    memcpy(pstViPipeAttr, &vi_pipe_attr_base, sizeof(VI_PIPE_ATTR_S));
    switch (enSnsType) {
        case PIXELPLUS_PR2020_1M_25FPS_8BIT:
        case PIXELPLUS_PR2020_1M_30FPS_8BIT:
        case PIXELPLUS_PR2020_2M_25FPS_8BIT:
        case PIXELPLUS_PR2020_2M_30FPS_8BIT:
            pstViPipeAttr->bYuvBypassPath = CVI_TRUE;
            break;
        default:
        break;
    }

    return s32Ret;
}

CVI_S32 app_ipcam_Vi_ChnAttr_Get(SNS_TYPE_E enSnsType, VI_CHN_ATTR_S *pstViChnAttr)
{
    CVI_S32 s32Ret = CVI_SUCCESS;

    memcpy(pstViChnAttr, &vi_chn_attr_base, sizeof(VI_CHN_ATTR_S));

    switch (enSnsType) {
    case PIXELPLUS_PR2020_1M_25FPS_8BIT:
	case PIXELPLUS_PR2020_1M_30FPS_8BIT:
	case PIXELPLUS_PR2020_2M_25FPS_8BIT:
	case PIXELPLUS_PR2020_2M_30FPS_8BIT:
        pstViChnAttr->enPixelFormat = PIXEL_FORMAT_YUV_PLANAR_422;
        break;
    default:
        break;
    }

    return s32Ret;
}

CVI_S32 app_ipcam_Isp_InitAttr_Get(SNS_TYPE_E enSnsType, WDR_MODE_E enWDRMode, ISP_INIT_ATTR_S *pstIspInitAttr)
{
    CVI_S32 s32Ret = CVI_SUCCESS;

    memset(pstIspInitAttr, 0, sizeof(ISP_INIT_ATTR_S));

    return s32Ret;
}

CVI_S32 app_ipcam_Isp_PubAttr_Get(SNS_TYPE_E enSnsType, ISP_PUB_ATTR_S *pstIspPubAttr)
{
    CVI_S32 s32Ret = CVI_SUCCESS;

    memcpy(pstIspPubAttr, &isp_pub_attr_base, sizeof(ISP_PUB_ATTR_S));
    //FPS
    switch(enSnsType) {
        case SMS_SC1346_1L_MIPI_1M_60FPS_10BIT:
            pstIspPubAttr->f32FrameRate = 60;
        break;
        default:
            pstIspPubAttr->f32FrameRate = 25;
        break;
    }
    switch (enSnsType) {
    case GCORE_GC1054_MIPI_1M_30FPS_10BIT:
    case GCORE_GC2053_MIPI_2M_30FPS_10BIT:
    case GCORE_GC2093_MIPI_2M_30FPS_10BIT:
    case GCORE_GC4023_MIPI_4M_30FPS_10BIT:
        pstIspPubAttr->enBayer = BAYER_RGGB;
        break;
    case GCORE_GC4653_MIPI_4M_30FPS_10BIT:
        pstIspPubAttr->enBayer = BAYER_GRBG;
        break;
    case PIXELPLUS_PR2020_1M_25FPS_8BIT:
    case PIXELPLUS_PR2020_1M_30FPS_8BIT:
    case PIXELPLUS_PR2020_2M_25FPS_8BIT:
	case PIXELPLUS_PR2020_2M_30FPS_8BIT:
    case SMS_SC1346_1L_MIPI_1M_30FPS_10BIT:
    case SMS_SC1346_1L_MIPI_1M_60FPS_10BIT:
    case SMS_SC230AI_2L_MIPI_2M_30FPS_10BIT:
    case SMS_SC230AI_2L_SLAVE_MIPI_2M_30FPS_10BIT:
        pstIspPubAttr->enBayer = BAYER_BGGR;
        break;
    case SONY_IMX327_MIPI_2M_30FPS_12BIT:
    case SONY_IMX307_MIPI_2M_30FPS_12BIT:
    case SONY_IMX307_2L_MIPI_2M_30FPS_12BIT:
    case SONY_IMX307_SLAVE_MIPI_2M_30FPS_12BIT:
    case SONY_IMX675_MIPI_5M_30FPS_12BIT:
        pstIspPubAttr->enBayer = BAYER_RGGB;
        break;
    default:
        break;
    }

    return s32Ret;
}

int app_ipcam_Vi_framerate_Set(VI_PIPE ViPipe, CVI_S32 framerate)
{
    ISP_PUB_ATTR_S pubAttr = {0};

    CVI_ISP_GetPubAttr(ViPipe, &pubAttr);

    pubAttr.f32FrameRate = (CVI_FLOAT)framerate;

    APP_CHK_RET(CVI_ISP_SetPubAttr(ViPipe, &pubAttr), "set vi framerate");

    return CVI_SUCCESS;
}

int app_ipcam_Vi_Sensor_Start(void)
{
    for (CVI_U32 i = 0; i < g_pstViCtx->u32WorkSnsCnt; i++) {
        RX_INIT_ATTR_S stRxAttr = {0};
        ISP_CMOS_SENSOR_IMAGE_MODE_S stSnsrMode = {0};
        APP_CHK_RET(CVI_SENSOR_SetSnsType(i, g_pstViCtx->astSensorCfg[i].enSnsType), "CVI_SENSOR_SetSnsType failed");
        stRxAttr.MipiDev = g_pstViCtx->astSensorCfg[i].MipiDev;
        if (g_pstViCtx->astSensorCfg[i].bMclkEn) {
            stRxAttr.stMclkAttr.bMclkEn = CVI_TRUE;
            stRxAttr.stMclkAttr.u8Mclk = g_pstViCtx->astSensorCfg[i].u8Mclk;
        }
        for (CVI_U32 j = 0; j < sizeof(stRxAttr.as16LaneId)/sizeof(CVI_S16); j++) {
            stRxAttr.as16LaneId[j] = g_pstViCtx->astSensorCfg[i].as16LaneId[j];
        }
        for (CVI_U32 j = 0; j < sizeof(stRxAttr.as8PNSwap)/sizeof(CVI_S8); j++) {
            stRxAttr.as8PNSwap[j] = g_pstViCtx->astSensorCfg[i].as8PNSwap[j];
        }
        APP_CHK_RET(CVI_SENSOR_SetSnsRxAttr(i, &stRxAttr), "CVI_SENSOR_SetSnsRxAttr failed");
        APP_CHK_RET(CVI_SENSOR_SetSnsI2c(i, g_pstViCtx->astSensorCfg[i].s32BusId,
        g_pstViCtx->astSensorCfg[i].s32I2cAddr), "CVI_SENSOR_SetSnsI2c failed");
        APP_CHK_RET(CVI_SENSOR_RegCallback(i, i), "CVI_SENSOR_RegCallback failed");
        stSnsrMode.u16Width = g_pstViCtx->astChnInfo[i].u32Width;
        stSnsrMode.u16Height = g_pstViCtx->astChnInfo[i].u32Height;
        stSnsrMode.f32Fps = g_pstViCtx->astChnInfo[i].f32Fps;
        APP_CHK_RET(CVI_SENSOR_SetSnsImgMode(i, &stSnsrMode), "CVI_SENSOR_SetSnsImgMode failed");
        APP_CHK_RET(CVI_SENSOR_SetSnsWdrMode(i, g_pstViCtx->astSensorCfg[i].enWDRMode), "CVI_SENSOR_SetSnsWdrMode failed");
    }
    return CVI_SUCCESS;
}

int app_ipcam_Vi_Mipi_Start(void)
{
    for (CVI_U32 i = 0; i < g_pstViCtx->u32WorkSnsCnt; i++) {
        APP_CHK_RET(CVI_SENSOR_SetSnsGpioInit(i, g_pstViCtx->astSensorCfg[i].s32Rst_port_idx,
        g_pstViCtx->astSensorCfg[i].s32Rst_pin,
        g_pstViCtx->astSensorCfg[i].s32Rst_pol), "CVI_SENSOR_SetSnsGpioInit failed");
        printf("enter i = %d, g_pstViCtx->astSensorCfg[i].s32Rst_port_idx = %d, g_pstViCtx->astSensorCfg[i].s32Rst_pin = %d, g_pstViCtx->astSensorCfg[i].s32Rst_pol = %d\n", i, g_pstViCtx->astSensorCfg[i].s32Rst_port_idx, g_pstViCtx->astSensorCfg[i].s32Rst_pin, g_pstViCtx->astSensorCfg[i].s32Rst_pol);
        APP_CHK_RET(CVI_SENSOR_RstSnsGpio(i, 1), "CVI_SENSOR_RstSnsGpio failed");
        APP_CHK_RET(CVI_SENSOR_RstMipi(i, 1), "CVI_SENSOR_RstMipi failed");
        APP_CHK_RET(CVI_SENSOR_SetMipiAttr(i, g_pstViCtx->astSensorCfg[i].enSnsType), "CVI_SENSOR_SetMipiAttr failed");
        APP_CHK_RET(CVI_SENSOR_EnableSnsClk(i, 1), "CVI_SENSOR_EnableSnsClk failed");
        APP_CHK_RET(CVI_SENSOR_RstSnsGpio(i, 0), "CVI_SENSOR_RstSnsGpio failed");
    }
    return CVI_SUCCESS;
}

int app_ipcam_Vi_Start_SensorProbe()
{
    for (CVI_U32 i = 0; i < g_pstViCtx->u32WorkSnsCnt; i++) {
        APP_CHK_RET(CVI_SENSOR_SetSnsProbe(i), "CVI_SENSOR_SetSnsProbe failed");
    }
    return CVI_SUCCESS;
}

int app_ipcam_Vi_Dev_Start(void)
{
    CVI_S32 s32Ret;

    VI_DEV         ViDev;
    VI_DEV_ATTR_S  stViDevAttr;

    for (CVI_U32 i = 0; i < g_pstViCtx->u32WorkSnsCnt; i++) {
        APP_PARAM_SNS_CFG_T *pstSnsCfg = &g_pstViCtx->astSensorCfg[i];
        APP_PARAM_CHN_CFG_T *pstChnCfg = &g_pstViCtx->astChnInfo[i];
        ViDev = pstChnCfg->s32ChnId;

        app_ipcam_Vi_DevAttr_Get(pstSnsCfg->enSnsType, &stViDevAttr);
        stViDevAttr.stSize.u32Width     = pstChnCfg->u32Width;
        stViDevAttr.stSize.u32Height    = pstChnCfg->u32Height;
        stViDevAttr.stWDRAttr.enWDRMode = pstChnCfg->enWDRMode;

        s32Ret = CVI_VI_SetDevAttr(ViDev, &stViDevAttr);
        APP_IPCAM_CHECK_RET(s32Ret, "CVI_VI_SetDevAttr(%d) failed!\n", ViDev);

        s32Ret = CVI_VI_EnableDev(ViDev);
        APP_IPCAM_CHECK_RET(s32Ret, "CVI_VI_EnableDev(%d) failed!\n", ViDev);
    }

    return CVI_SUCCESS;
}

int app_ipcam_Vi_Dev_Stop(void)
{
    CVI_S32 s32Ret;
    VI_DEV ViDev;

    for (CVI_U32 i = 0; i < g_pstViCtx->u32WorkSnsCnt; i++) {
        APP_PARAM_CHN_CFG_T *pstChnCfg = &g_pstViCtx->astChnInfo[i];
        ViDev = pstChnCfg->s32ChnId;
        s32Ret  = CVI_VI_DisableDev(ViDev);

        // CVI_VI_UnRegChnFlipMirrorCallBack(0, ViDev);
        // CVI_VI_UnRegPmCallBack(ViDev);
        // memset(&ViPmData[ViDev], 0, sizeof(struct VI_PM_DATA_S));

        if (s32Ret != CVI_SUCCESS) {
            APP_PROF_LOG_PRINT(LEVEL_ERROR, "CVI_VI_DisableDev failed with %#x!\n", s32Ret);
            return s32Ret;
        }
    }

    return CVI_SUCCESS;
}

int app_ipcam_Vi_Pipe_Start(void)
{
    CVI_S32 s32Ret;

    VI_PIPE        ViPipe;
    VI_PIPE_ATTR_S stViPipeAttr;

    for (CVI_U32 i = 0; i < g_pstViCtx->u32WorkSnsCnt; i++) {
        APP_PARAM_SNS_CFG_T *pstSnsCfg = &g_pstViCtx->astSensorCfg[i];
        APP_PARAM_CHN_CFG_T *pstChnCfg = &g_pstViCtx->astChnInfo[i];
        APP_PARAM_PIPE_CFG_T *psPipeCfg = &g_pstViCtx->astPipeInfo[i];

        s32Ret = app_ipcam_Vi_PipeAttr_Get(pstSnsCfg->enSnsType, &stViPipeAttr);
        APP_IPCAM_CHECK_RET(s32Ret, "app_ipcam_Vi_PipeAttr_Get failed!\n");

        stViPipeAttr.u32MaxW = pstChnCfg->u32Width;
        stViPipeAttr.u32MaxH = pstChnCfg->u32Height;
        stViPipeAttr.enCompressMode = pstChnCfg->enCompressMode;

        for (int j = 0; j < WDR_MAX_PIPE_NUM; j++) {
            if ((psPipeCfg->aPipe[j] >= 0) && (psPipeCfg->aPipe[j] < WDR_MAX_PIPE_NUM)) {
                ViPipe = psPipeCfg->aPipe[j];

                s32Ret = CVI_VI_CreatePipe(ViPipe, &stViPipeAttr);
                APP_IPCAM_CHECK_RET(s32Ret, "CVI_VI_CreatePipe(%d) failed!\n", ViPipe);
                s32Ret = CVI_VI_StartPipe(ViPipe);
                APP_IPCAM_CHECK_RET(s32Ret, "CVI_VI_StartPipe(%d) failed!\n", ViPipe);
            }
        }
    }

    return CVI_SUCCESS;
}

int app_ipcam_PQBin_Load(const CVI_CHAR *pBinPath)
{
    CVI_S32 ret = CVI_SUCCESS;
    FILE *fp = NULL;
    CVI_U8 *buf = NULL;
    CVI_U64 file_size;

    fp = fopen((const CVI_CHAR *)pBinPath, "rb");
    if (fp == NULL) {
        APP_PROF_LOG_PRINT(LEVEL_INFO, "Can't find bin(%s), use default parameters\n", pBinPath);
        return CVI_FAILURE;
    }

    fseek(fp, 0L, SEEK_END);
    file_size = ftell(fp);
    rewind(fp);

    buf = (CVI_U8 *)malloc(file_size);
    if (buf == NULL) {
        APP_PROF_LOG_PRINT(LEVEL_INFO, "%s\n", "Allocae memory fail");
        fclose(fp);
        return CVI_FAILURE;
    }

    fread(buf, file_size, 1, fp);

    if (fp != NULL) {
        fclose(fp);
    }
    ret = CVI_BIN_ImportBinData(buf, (CVI_U32)file_size);
    if (ret != CVI_SUCCESS) {
        APP_PROF_LOG_PRINT(LEVEL_INFO, "CVI_BIN_ImportBinData error! value:(0x%x)\n", ret);
        free(buf);
        return CVI_FAILURE;
    }

    free(buf);

    return CVI_SUCCESS;
}

int app_ipcam_Vi_Pipe_Stop(void)
{
    CVI_S32 s32Ret;
    VI_PIPE ViPipe;

    for (CVI_U32 i = 0; i < g_pstViCtx->u32WorkSnsCnt; i++) {
        APP_PARAM_PIPE_CFG_T *psPipeCfg = &g_pstViCtx->astPipeInfo[i];
        ViPipe = psPipeCfg->aPipe[0];
        s32Ret = CVI_VI_StopPipe(ViPipe);
        if (s32Ret != CVI_SUCCESS) {
            APP_PROF_LOG_PRINT(LEVEL_ERROR, "CVI_VI_StopPipe failed with %#x!\n", s32Ret);
            return s32Ret;
        }
        s32Ret = CVI_VI_DestroyPipe(ViPipe);
        if (s32Ret != CVI_SUCCESS) {
            APP_PROF_LOG_PRINT(LEVEL_ERROR, "CVI_VI_DestroyPipe failed with %#x!\n", s32Ret);
            return s32Ret;
        }
    }

    return CVI_SUCCESS;
}

void app_ipcam_Framerate_Set(CVI_U8 viPipe, CVI_U8 fps)
{
    ISP_PUB_ATTR_S stPubAttr;

    memset(&stPubAttr, 0, sizeof(stPubAttr));

    CVI_ISP_GetPubAttr(viPipe, &stPubAttr);

    stPubAttr.f32FrameRate = fps;

    APP_PROF_LOG_PRINT(LEVEL_DEBUG,"set pipe: %d, fps: %d\n", viPipe, fps);

    CVI_ISP_SetPubAttr(viPipe, &stPubAttr);
}

CVI_U8 app_ipcam_Framerate_Get(CVI_U8 viPipe)
{
    ISP_PUB_ATTR_S stPubAttr;

    memset(&stPubAttr, 0, sizeof(stPubAttr));

    CVI_ISP_GetPubAttr(viPipe, &stPubAttr);

    return stPubAttr.f32FrameRate;
}

int app_ipcam_Vi_Isp_Init(void)
{
    CVI_S32 s32Ret;

    for (CVI_U32 i = 0; i < g_pstViCtx->u32WorkSnsCnt; i++) {
        APP_PARAM_SNS_CFG_T *pstSnsCfg = &g_pstViCtx->astSensorCfg[i];
        s32Ret = CVI_ISP_Init(i);
        APP_IPCAM_CHECK_RET(s32Ret, "ISP Init fail, ViPipe[%d]\n", i);
        app_ipcam_Framerate_Set(i, pstSnsCfg->s32Framerate);
    }

    if (access(PQ_BIN_SDR, F_OK) == 0) {
        s32Ret = app_ipcam_PQBin_Load(PQ_BIN_SDR);
        if (s32Ret != CVI_SUCCESS) {
            APP_PROF_LOG_PRINT(LEVEL_WARN, "load %s failed with %#x!\n", PQ_BIN_SDR, s32Ret);
        }
    }

    #ifdef SUPPORT_ISP_PQTOOL
        app_ipcam_Ispd_Load();
    #endif

    return CVI_SUCCESS;
}

int app_ipcam_Vi_Isp_DeInit(void)
{
    for (CVI_U32 i = 0; i < g_pstViCtx->u32WorkSnsCnt; i++) {
        ISP_SNS_CFG_S stSnsCfg = {
            .bHwSync = CVI_FALSE,
        };
        CVI_ISP_SnsExit(i, &stSnsCfg);  // mars3 没有这个函数了
    }

    return CVI_SUCCESS;

}

static void callback_FPS(int fps)
{
    static CVI_FLOAT uMaxFPS[VI_MAX_DEV_NUM] = {0};
    CVI_U32 i;

    for (i = 0; i < VI_MAX_DEV_NUM && g_IspPid[i]; i++) {
        ISP_PUB_ATTR_S pubAttr = {0};

        CVI_ISP_GetPubAttr(i, &pubAttr);
        if (uMaxFPS[i] == 0) {
            uMaxFPS[i] = pubAttr.f32FrameRate;
        }
        if (fps == 0) {
            pubAttr.f32FrameRate = uMaxFPS[i];
        } else {
            pubAttr.f32FrameRate = (CVI_FLOAT) fps;
        }
        CVI_ISP_SetPubAttr(i, &pubAttr);
    }
}

void *ISP_Thread(void *arg)
{
    CVI_S32 s32Ret;
    VI_PIPE ViPipe = *(VI_PIPE *)arg;
    char szThreadName[20];

    snprintf(szThreadName, sizeof(szThreadName), "ISP%d_RUN", ViPipe);
    prctl(PR_SET_NAME, szThreadName, 0, 0, 0);

    // if (ViPipe > 0) {
    //     APP_PROF_LOG_PRINT(LEVEL_ERROR,"ISP Dev %d return\n", ViPipe);
    //     return CVI_NULL;
    // }

    CVI_SYS_RegisterThermalCallback(callback_FPS);

    APP_PROF_LOG_PRINT(LEVEL_INFO, "ISP Dev %d running!\n", ViPipe);
    s32Ret = CVI_ISP_Run(ViPipe);
    if (s32Ret != 0) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR,"CVI_ISP_Run(%d) failed with %#x!\n", ViPipe, s32Ret);
    }

    return CVI_NULL;
}

int app_ipcam_Vi_Isp_Start(void)
{
    return CVI_SUCCESS;
}


int app_ipcam_Vi_Isp_Stop(void)
{
    CVI_S32 s32Ret;
    VI_PIPE ViPipe;

    for (CVI_U32 i = 0; i < g_pstViCtx->u32WorkSnsCnt; i++) {
        APP_PARAM_CHN_CFG_T *pstChnCfg = &g_pstViCtx->astChnInfo[i];
        ViPipe = pstChnCfg->s32ChnId;

        #ifdef SUPPORT_ISP_PQTOOL
        app_ipcam_Ispd_Unload();
        #endif

        s32Ret = CVI_ISP_Exit(ViPipe);
        if (s32Ret != CVI_SUCCESS) {
                APP_PROF_LOG_PRINT(LEVEL_ERROR, "CVI_ISP_Exit fail with %#x!\n", s32Ret);
                return CVI_FAILURE;
        }
    }

    return CVI_SUCCESS;
}

int app_ipcam_Vi_Chn_Start(void)
{
    CVI_S32 s32Ret;

    VI_PIPE        ViPipe = 0;
    VI_CHN         ViChn  = 0;
    VI_DEV_ATTR_S  stViDevAttr;
    VI_CHN_ATTR_S  stViChnAttr;
    VI_DEV         ViDev  = 0;

    for (CVI_U32 i = 0; i < g_pstViCtx->u32WorkSnsCnt; i++) {
        APP_PARAM_SNS_CFG_T *pstSnsCfg = &g_pstViCtx->astSensorCfg[i];
        APP_PARAM_CHN_CFG_T *pstChnCfg = &g_pstViCtx->astChnInfo[i];
        ViPipe = pstChnCfg->s32ChnId;
        ViChn = pstChnCfg->s32ChnId;

        s32Ret = app_ipcam_Vi_DevAttr_Get(pstSnsCfg->enSnsType, &stViDevAttr);
        APP_IPCAM_CHECK_RET(s32Ret, "app_ipcam_Vi_DevAttr_Get(%d) failed!\n", ViPipe);

        s32Ret = app_ipcam_Vi_ChnAttr_Get(pstSnsCfg->enSnsType, &stViChnAttr);
        APP_IPCAM_CHECK_RET(s32Ret, "app_ipcam_Vi_ChnAttr_Get(%d) failed!\n", ViPipe);

        stViChnAttr.stSize.u32Width  = pstChnCfg->u32Width;
        stViChnAttr.stSize.u32Height = pstChnCfg->u32Height;
        stViChnAttr.enCompressMode = pstChnCfg->enCompressMode;
        stViChnAttr.enPixelFormat = pstChnCfg->enPixFormat;

        stViChnAttr.u32Depth         = 0; // depth
        // stViChnAttr.bLVDSflow        = (stViDevAttr.enIntfMode == VI_MODE_LVDS) ? 1 : 0;
        // stViChnAttr.u8TotalChnNum    = vt->ViConfig.s32WorkingViNum;

        /* fill the sensor orientation */
        if (pstSnsCfg->u8Orien <= 3) {
            stViChnAttr.bMirror = pstSnsCfg->u8Orien & 0x1;
            stViChnAttr.bFlip = pstSnsCfg->u8Orien & 0x2;
        }

        s32Ret = CVI_VI_SetChnAttr(ViPipe, ViChn, &stViChnAttr);
        APP_IPCAM_CHECK_RET(s32Ret, "CVI_VI_SetChnAttr(%d) failed!\n", ViPipe);

        ViDev = g_pstViCtx->astDevInfo[i].ViDev;
        s32Ret = CVI_SENSOR_SetVIFlipMirrorCB(ViPipe, ViDev);
        if (s32Ret != CVI_SUCCESS) {
            CVI_TRACE_LOG(CVI_DBG_ERR, "set vi mirror and filp callback failed!\n");
            return s32Ret;
        }

        s32Ret = CVI_VI_EnableChn(ViPipe, ViChn);
        APP_IPCAM_CHECK_RET(s32Ret, "CVI_VI_EnableChn(%d) failed!\n", ViPipe);
    }
    return CVI_SUCCESS;
}

int app_ipcam_Vi_Chn_Stop(void)
{
    CVI_S32 s32Ret;
    VI_CHN ViChn;
    VI_PIPE ViPipe;

    for (CVI_U32 i = 0; i < g_pstViCtx->u32WorkSnsCnt; i++) {
        APP_PARAM_CHN_CFG_T *pstChnCfg = &g_pstViCtx->astChnInfo[i];
        ViPipe = pstChnCfg->s32ChnId;
        ViChn = pstChnCfg->s32ChnId;

        if (ViChn < VI_MAX_CHN_NUM) {
            CVI_VI_UnRegChnFlipMirrorCallBack(ViPipe, ViChn);
            s32Ret = CVI_VI_DisableChn(ViPipe, ViChn);
            if (s32Ret != CVI_SUCCESS) {
                APP_PROF_LOG_PRINT(LEVEL_ERROR, "CVI_VI_DisableChn failed with %#x!\n", s32Ret);
                return s32Ret;
            }
        }
    }

    return CVI_SUCCESS;
}

int app_ipcam_Vi_DeInit(void)
{
    APP_PARAM_MODULE_CFG_S * pModuleCfg = app_ipcam_Module_Param_Get();

    if(!pModuleCfg->alios_vi_mode){
        if (Auto_Rgb_Ir_Enable)
        {
            Auto_Rgb_Ir_Enable = CVI_FALSE;
            pthread_join(g_RgbIR_Thread, CVI_NULL);
            g_RgbIR_Thread = 0;
        }

        APP_CHK_RET(app_ipcam_Vi_Isp_Stop(),  "app_ipcam_Vi_Isp_Stop");
        APP_CHK_RET(app_ipcam_Vi_Isp_DeInit(),  "app_ipcam_Vi_Isp_DeInit");
        APP_CHK_RET(app_ipcam_Vi_Chn_Stop(),  "app_ipcam_Vi_Chn_Stop");
        APP_CHK_RET(app_ipcam_Vi_Pipe_Stop(), "app_ipcam_Vi_Pipe_Stop");
        APP_CHK_RET(app_ipcam_Vi_Dev_Stop(),  "app_ipcam_Vi_Dev_Stop");
    }

    return CVI_SUCCESS;
}

int app_ipcam_Vi_Init(void)
{
    CVI_S32 s32Ret = CVI_SUCCESS;

    APP_PARAM_MODULE_CFG_S * pModuleCfg = app_ipcam_Module_Param_Get();
    if(pModuleCfg->alios_vi_mode){
        s32Ret = CVI_VI_GetDevNum(&g_pstViCtx->u32WorkSnsCnt);
        if(s32Ret != CVI_SUCCESS){
            APP_PROF_LOG_PRINT(LEVEL_ERROR, "CVI_VI_GetDevNum failed with %#x\n", s32Ret);
            return s32Ret;
        }
        for (CVI_U32 i = 0; i < g_pstViCtx->u32WorkSnsCnt; i++) {
            s32Ret = CVI_ISP_Init(g_pstViCtx->astPipeInfo[i].aPipe[0]);
            if(s32Ret != CVI_SUCCESS){
                APP_PROF_LOG_PRINT(LEVEL_ERROR, "CVI_ISP_Init(%d) failed with %#x\n", i, s32Ret);
                return s32Ret;
            }
        }
#ifdef SUPPORT_ISP_PQTOOL
        app_ipcam_Ispd_Load();
#endif
        return s32Ret;
    }else{
        s32Ret = app_ipcam_Vi_Sensor_Start();
        if(s32Ret != CVI_SUCCESS) {
            APP_PROF_LOG_PRINT(LEVEL_ERROR, "app_ipcam_Vi_Sensor_Start failed with %#x\n", s32Ret);
            goto VI_EXIT0;
        }

        s32Ret = app_ipcam_Vi_Mipi_Start();
        if(s32Ret != CVI_SUCCESS) {
            APP_PROF_LOG_PRINT(LEVEL_ERROR, "app_ipcam_Mipi_Start failed with %#x\n", s32Ret);
            goto VI_EXIT0;
        }

        s32Ret = app_ipcam_Vi_Start_SensorProbe();
        if(s32Ret != CVI_SUCCESS) {
            APP_PROF_LOG_PRINT(LEVEL_ERROR, "app_ipcam_Vi_Start_SensorProbe failed with %#x\n", s32Ret);
            goto VI_EXIT0;
        }

        s32Ret = app_ipcam_Vi_Dev_Start();
        if(s32Ret != CVI_SUCCESS) {
            APP_PROF_LOG_PRINT(LEVEL_ERROR, "app_ipcam_Dev_Start failed with %#x\n", s32Ret);
            goto VI_EXIT0;
        }

        s32Ret = app_ipcam_Vi_Pipe_Start();
        if(s32Ret != CVI_SUCCESS) {
            APP_PROF_LOG_PRINT(LEVEL_ERROR, "app_ipcam_Pipe_Start failed with %#x\n", s32Ret);
            goto VI_EXIT1;
        }

        s32Ret = app_ipcam_Vi_Isp_Init();
        if(s32Ret != CVI_SUCCESS) {
            APP_PROF_LOG_PRINT(LEVEL_ERROR, "app_ipcam_Isp_Init failed with %#x\n", s32Ret);
            goto VI_EXIT2;
        }

        s32Ret = app_ipcam_Vi_Isp_Start();
        if(s32Ret != CVI_SUCCESS) {
            APP_PROF_LOG_PRINT(LEVEL_ERROR, "app_ipcam_Isp_Start failed with %#x\n", s32Ret);
            goto VI_EXIT3;
        }

        s32Ret = app_ipcam_Vi_Chn_Start();
        if(s32Ret != CVI_SUCCESS) {
            APP_PROF_LOG_PRINT(LEVEL_ERROR, "app_ipcam_Chn_Start failed with %#x\n", s32Ret);
            goto VI_EXIT4;
        }
    }

    return CVI_SUCCESS;

VI_EXIT4:
    app_ipcam_Vi_Isp_Stop();

VI_EXIT3:
    app_ipcam_Vi_Isp_DeInit();

VI_EXIT2:
    app_ipcam_Vi_Pipe_Stop();

VI_EXIT1:
    app_ipcam_Vi_Dev_Stop();

VI_EXIT0:
    app_ipcam_Sys_DeInit();

    return s32Ret;
}
