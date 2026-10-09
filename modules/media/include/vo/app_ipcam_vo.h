#ifndef __APP_IPCAM_VO_H__
#define __APP_IPCAM_VO_H__

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif

#include <stdio.h>
#include "cvi_defines.h"
#include "cvi_comm_vo.h"

typedef struct APP_PARAM_VO_CFG_S {
    VO_DEV s32VoDev;
    VO_PUB_ATTR_S stVoPubAttr;
    VO_VIDEO_LAYER_ATTR_S stLayerAttr;
    VO_MODE_E enVoMode;
    ROTATION_E enRotation;
    CVI_U32 u32DisBufLen;
    MMF_CHN_S stSrcChn;
    MMF_CHN_S stDstChn;
    CVI_BOOL bBindMode;
} APP_PARAM_VO_CFG_T;

CVI_S32 app_ipcam_Vo_Start(const APP_PARAM_VO_CFG_T* const pstVoCfg);
CVI_S32 app_ipcam_Vo_Stop(const APP_PARAM_VO_CFG_T* const pstVoCfg);

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* End of #ifdef __cplusplus */

#endif
