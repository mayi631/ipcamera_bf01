#ifndef _CVI_RINGBUF_H_
#define _CVI_RINGBUF_H_

#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CVI_ALIGN_UP(x, a) ((x + a - 1) & (~(a - 1)))
#define RINGBUF_ALIGN_SIZE 4096
#define FRAMEDATA_ALIGN_SIZE 64

typedef enum _CVI_RECORD_TYPE_E {
    CVI_RBUF_RECORD_TYPE_NORMAL,
    //CVI_RECORD_TYPE_TIMELAPSE,
    //CVI_RECORD_TYPE_EMR,
    //CVI_RECORD_TYPE_MEM,
    CVI_RBUF_RECORD_TYPE_BUTT
} CVI_RBUF_RECORD_TYPE_E;// be care of CVI_RECORDER_TYPE_E

typedef void * (*CVI_RBUF_MALLOC_CB)(uint32_t size , const char * name);
typedef void(*CVI_RBUF_FREE_CB)(void * vir_addr);

int32_t CVI_RBUF_Create(void **rbuf, uint32_t size, const char *name, void * mallocmemcb, void * freememcb);
void CVI_RBUF_Destroy(void *rbuf);
void *CVI_RBUF_Req_Mem(void *rbuf, uint32_t size);
int32_t CVI_RBUF_Refresh_WritePos(void *rbuf, uint32_t offs);
void *CVI_RBUF_ReadData(void *rbuf, CVI_RBUF_RECORD_TYPE_E type);
int32_t CVI_RBUF_Refresh_ReadPos(void *rbuf, uint32_t offs, CVI_RBUF_RECORD_TYPE_E type);
void CVI_RBUF_WriteRollback(void *rbuf, void *pos, uint32_t offs);
uint64_t CVI_RBUF_Get_DataCnt(void *rbuf);
uint32_t CVI_RBUF_Get_RemainSize(void *rbuf);
void CVI_RBUF_ShowMeminfo(void *rbuf);


int32_t CVI_RBUF_Init(void **rbuf, int32_t size, const char *name, int32_t outcnt, void * mallocmemcb, void * freememcb);
void CVI_RBUF_DeInit(void *rbuf);
uint32_t CVI_RBUF_DataCnt(void *rbuf, int32_t inx);
int32_t CVI_RBUF_Copy_In(void *rbuf, void *src, int32_t len, int32_t off);
int32_t CVI_RBUF_Copy_Out(void *rbuf, void *dst, int32_t len, int32_t off, int32_t inx);
uint32_t CVI_RBUF_Unused(void *rbuf);
void CVI_RBUF_Refresh_In(void *rbuf, int32_t off);
void CVI_RBUF_Refresh_Out(void *rbuf, int32_t off, int32_t inx);
void CVI_RBUF_ShowLog(void *rbuf);
int32_t CVI_RBUF_Get_Totalsize(void *rbuf);
int32_t CVI_RBUF_Reset(void *rbuf);
uint64_t CVI_RBUF_Get_InSize(void *rbuf);
int32_t CVI_RBUF_Copy_OutTmp(void *rbuf, void *dst, int32_t len, int32_t off, int32_t inx);
void CVI_RBUF_Refresh_OutTmp(void *rbuf, int32_t off, int32_t inx);

#ifdef __cplusplus
}
#endif


#endif
