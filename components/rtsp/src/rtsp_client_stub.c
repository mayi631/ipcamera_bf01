#include <stddef.h>
#include <stdint.h>

#include "rtsp.h"

int32_t RTSP_ClientCreate(void **hdl, RTSP_CLIENT_ATTR_S *attr)
{
	(void)hdl;
	(void)attr;
	return -1;
}

int32_t RTSP_ClientDestroy(void *hdl)
{
	(void)hdl;
	return -1;
}

int32_t RTSP_RecvFrame(void *hdl, RTSP_FRAME_S *frame, int32_t timeout_ms)
{
	(void)hdl;
	(void)frame;
	(void)timeout_ms;
	return -1;
}

void RTSP_ReleaseFrame(void *hdl, RTSP_FRAME_S *frame)
{
	(void)hdl;
	(void)frame;
}
