#ifndef __CVI_LOG_H__
#define __CVI_LOG_H__

#include <stdio.h>
#include "cvi_datatype.h"
/* debug log level define */
#define CVI_LOG_DEBUG       (5) // 5
#define CVI_LOG_INFO        (4) // 4
#define CVI_LOG_WARNING     (3) // 3
#define CVI_LOG_ERROR       (2) // 2
#define CVI_LOG_FATAL       (1) // 1
#define CVI_LOG_NONE        (0) // 0
/* default builtin log*/
#ifndef BUILT_LOG_LEVEL
#define BUILT_LOG_LEVEL CVI_LOG_ERROR
#endif
/**/
#if BUILT_LOG_LEVEL == CVI_LOG_DEBUG
#define CVI_LOGD(fmt,...) printf(fmt,##__VA_ARGS__)
#define CVI_LOGI(fmt,...) printf(fmt,##__VA_ARGS__)
#define CVI_LOGW(fmt,...) printf(fmt,##__VA_ARGS__)
#define CVI_LOGE(fmt,...) printf(fmt,##__VA_ARGS__)
#define CVI_LOGF(fmt,...) printf(fmt,##__VA_ARGS__)
#endif
/**/
#if BUILT_LOG_LEVEL == CVI_LOG_INFO
#define CVI_LOGD(fmt,...) (void)(0)
#define CVI_LOGI(fmt,...) printf(fmt,##__VA_ARGS__)
#define CVI_LOGW(fmt,...) printf(fmt,##__VA_ARGS__)
#define CVI_LOGE(fmt,...) printf(fmt,##__VA_ARGS__)
#define CVI_LOGF(fmt,...) printf(fmt,##__VA_ARGS__)
#endif
/**/
#if BUILT_LOG_LEVEL == CVI_LOG_WARNING
#define CVI_LOGD(fmt,...) (void)(0)
#define CVI_LOGI(fmt,...) (void)(0)
#define CVI_LOGW(fmt,...) printf(fmt,##__VA_ARGS__)
#define CVI_LOGE(fmt,...) printf(fmt,##__VA_ARGS__)
#define CVI_LOGF(fmt,...) printf(fmt,##__VA_ARGS__)
#endif
/**/
#if BUILT_LOG_LEVEL == CVI_LOG_ERROR
#define CVI_LOGD(fmt,...) (void)(0)
#define CVI_LOGI(fmt,...) (void)(0)
#define CVI_LOGW(fmt,...) (void)(0)
#define CVI_LOGE(fmt,...) printf(fmt,##__VA_ARGS__)
#define CVI_LOGF(fmt,...) printf(fmt,##__VA_ARGS__)
#endif
/**/
#if BUILT_LOG_LEVEL == CVI_LOG_FATAL
#define CVI_LOGD(fmt,...) (void)(0)
#define CVI_LOGI(fmt,...) (void)(0)
#define CVI_LOGW(fmt,...) (void)(0)
#define CVI_LOGE(fmt,...) (void)(0)
#define CVI_LOGF(fmt,...) printf(fmt,##__VA_ARGS__)
#endif
/**/
#if BUILT_LOG_LEVEL == CVI_LOG_NONE
#define CVI_LOGD(fmt,...) (void)(0)
#define CVI_LOGI(fmt,...) (void)(0)
#define CVI_LOGW(fmt,...) (void)(0)
#define CVI_LOGE(fmt,...) (void)(0)
#define CVI_LOGF(fmt,...) (void)(0)
#endif
/**/
#ifndef CVI_LOG_ASSERT
#define CVI_LOG_ASSERT(x, ...)     \
    do {                           \
        if (!(x)) {                \
            CVI_LOGE(__VA_ARGS__); \
            abort();               \
        }                          \
    } while (0)
#endif

#endif //__CVI_LOG_H__
