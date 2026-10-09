#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include "linux/cvi_type.h"
#include "app_ipcam_comm.h"
#include "app_ipcam_mipi_tx.h"

CVI_S32 app_ipcam_MipiTx_Enable(
    const int* const ps32DevNo,
    const CVI_CHAR* const pchPanelName,
    const struct combo_dev_cfg_s* const pstDevCfg,
    const struct hs_settle_s* const pstHsTimingCfg,
    const struct dsc_instr* const pstDsiInitCmds,
    const CVI_S32* const ps32DsiInitCmdsSize,
    CVI_S32* const ps32MipiTxFd)
{
    _NULL_POINTER_CHECK_(ps32DevNo, CVI_FAILURE);
    _NULL_POINTER_CHECK_(pchPanelName, CVI_FAILURE);
    _NULL_POINTER_CHECK_(pstDevCfg, CVI_FAILURE);
    _NULL_POINTER_CHECK_(pstHsTimingCfg, CVI_FAILURE);
    _NULL_POINTER_CHECK_(pstDsiInitCmds, CVI_FAILURE);
    _NULL_POINTER_CHECK_(ps32DsiInitCmdsSize, CVI_FAILURE);
    _NULL_POINTER_CHECK_(ps32MipiTxFd, CVI_FAILURE);

    *ps32MipiTxFd = 0;

    // APP_CHK_RET(CVI_MIPI_TX_Disable(*ps32MipiTxFd), "CVI_MIPI_TX_Disable");
    APP_CHK_RET(CVI_MIPI_TX_Cfg(*ps32MipiTxFd, (struct combo_dev_cfg_s*)pstDevCfg), "CVI_MIPI_TX_Cfg");

    for (CVI_S32 i = 0; i < *ps32DsiInitCmdsSize; i++) {
        struct cmd_info_s stCmdInfo = {
            .devno = *ps32DevNo,
            .cmd_size = pstDsiInitCmds[i].size,
            .data_type = pstDsiInitCmds[i].data_type,
            .cmd = (void *)pstDsiInitCmds[i].data
        };

        APP_FUNC_RET_CALLBACK(CVI_MIPI_TX_SendCmd(*ps32MipiTxFd, &stCmdInfo), {

            if (pstDsiInitCmds[i].delay) {
                usleep(pstDsiInitCmds[i].delay * 1000);
            }
        }, {
            if (pstDsiInitCmds[i].delay) {
                usleep(pstDsiInitCmds[i].delay * 1000);
            }

            APP_PROF_LOG_PRINT(LEVEL_ERROR, "[mipi_tx_send_cmd] didn't return success!\n");

            return CVI_FAILURE;
        });
    }


    APP_CHK_RET(CVI_MIPI_TX_SetHsSettle(*ps32MipiTxFd, pstHsTimingCfg), "CVI_MIPI_TX_SetHsSettle");
    APP_CHK_RET(CVI_MIPI_TX_Enable(*ps32MipiTxFd), "CVI_MIPI_TX_Enable");
    APP_PROF_LOG_PRINT(LEVEL_INFO, "Enable MIPI-TX driver for panel [%s].\n", pchPanelName);

    return CVI_SUCCESS;
}

CVI_S32 app_ipcam_MipiTx_Disable(const CVI_S32* const ps32MipiTxFd)
{
    _NULL_POINTER_CHECK_(ps32MipiTxFd, CVI_FAILURE);

    if (*ps32MipiTxFd == -1) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "Cannot close '%s': %d, %s\n", MIPI_TX_NAME, errno, strerror(errno));
        return CVI_FAILURE;
    }

    APP_CHK_RET(CVI_MIPI_TX_Disable(*ps32MipiTxFd), "CVI_MIPI_TX_Disable");
    APP_PROF_LOG_PRINT(LEVEL_INFO, "Disable MIPI-TX driver.\n");

    return CVI_SUCCESS;
}
