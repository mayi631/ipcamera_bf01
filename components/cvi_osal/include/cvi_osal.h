#ifndef __CVI_OSAL_H__
#define __CVI_OSAL_H__
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#define CVI_OSAL_SUCCESS ((int32_t)(0))
#define CVI_OSAL_ERR_FAILURE ((int32_t)(-1001))
#define CVI_OSAL_ERR_NOMEM ((int32_t)(-1002))
#define CVI_OSAL_ERR_TIMEOUT ((int32_t)(-1003))
#define CVI_OSAL_NO_WAIT ((int64_t)(0))
#define CVI_OSAL_WAIT_FOREVER ((int64_t)(-1))
#define CVI_OSAL_PRI_NORMAL ((int32_t)0)
#define CVI_OSAL_PRI_RT_LOWEST ((int32_t)1)
#define CVI_OSAL_PRI_RT_LOW ((int32_t)9)
#define CVI_OSAL_PRI_RT_MID ((int32_t)49)
#define CVI_OSAL_PRI_RT_HIGH ((int32_t)80)
#define CVI_OSAL_PRI_RT_HIGHEST ((int32_t)99)
#ifdef __cplusplus
extern "C" {
#endif
/*
 * time
 */
int32_t cvi_osal_get_boot_time_us(uint64_t *time_us);
int32_t cvi_osal_get_boot_time_ms(uint64_t *time_ms);
int32_t cvi_osal_get_boot_time_ns(uint64_t *time_ns);
/*
 * mutex
 */
typedef struct cvi_osal_mutex_attr {
    const char *name;
    int32_t type;
} cvi_osal_mutex_attr_t;
typedef struct cvi_osal_mutex {
    cvi_osal_mutex_attr_t attr;
    pthread_mutex_t mutex;
} cvi_osal_mutex_t, *cvi_osal_mutex_handle_t;
int32_t cvi_osal_mutex_create(cvi_osal_mutex_attr_t *attr, cvi_osal_mutex_handle_t *mutex);
int32_t cvi_osal_mutex_destroy(cvi_osal_mutex_handle_t mutex);
int32_t cvi_osal_mutex_lock(cvi_osal_mutex_handle_t mutex);
int32_t cvi_osal_mutex_unlock(cvi_osal_mutex_handle_t mutex);
#define CVI_OSAL_MUTEX_INITIALIZER                                  \
    {                                                               \
        {"static", PTHREAD_MUTEX_NORMAL}, PTHREAD_MUTEX_INITIALIZER \
    }
#ifdef __USE_GNU
#define CVI_OSAL_MUTEX_INITIALIZER_R                                                  \
    {                                                                                 \
        {"static_r", PTHREAD_MUTEX_RECURSIVE}, PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP \
    }
#else
#define CVI_OSAL_MUTEX_INITIALIZER_R                                   \
    {                                                                  \
        {"static_r", PTHREAD_MUTEX_RECURSIVE},                         \
        {                                                              \
            {                                                          \
                {                                                      \
                    PTHREAD_MUTEX_RECURSIVE, 0, 0, 0, 0, 0, 0, 0, 0, 0 \
                }                                                      \
            }                                                          \
        }                                                              \
    }
#endif
typedef struct cvi_osal_cond_attr {
    const char *name;
    int32_t clock;
} cvi_osal_cond_attr_t;
typedef struct cvi_osal_cond {
    cvi_osal_cond_attr_t attr;
    pthread_cond_t cond;
} cvi_osal_cond_t, *cvi_osal_cond_handle_t;
int32_t cvi_osal_cond_create(cvi_osal_cond_attr_t *attr, cvi_osal_cond_handle_t *cond);
int32_t cvi_osal_cond_destroy(cvi_osal_cond_handle_t cond);
int32_t cvi_osal_cond_signal(cvi_osal_cond_handle_t cond);
int32_t cvi_osal_cond_wait(cvi_osal_cond_handle_t cond, cvi_osal_mutex_handle_t mutex);
int32_t cvi_osal_cond_timedwait(cvi_osal_cond_handle_t cond, cvi_osal_mutex_handle_t mutex, int64_t timeout_us);
#define CVI_OSAL_COND_INITIALIZER                             \
    {                                                         \
        {"static", CLOCK_MONOTONIC}, PTHREAD_COND_INITIALIZER \
    }

typedef struct cvi_osal_sem_attr {
    const char *name;
} cvi_osal_sem_attr_t;
typedef struct cvi_osal_sem {
    cvi_osal_sem_attr_t attr;
    sem_t sem;
} cvi_osal_sem_t, *cvi_osal_sem_handle_t;
int32_t cvi_osal_sem_create(cvi_osal_sem_attr_t *attr, cvi_osal_sem_handle_t *sem);
int32_t cvi_osal_sem_destroy(cvi_osal_sem_handle_t sem);
int32_t cvi_osal_sem_wait(cvi_osal_sem_handle_t sem, int64_t timeout_us);
int32_t cvi_osal_sem_post(cvi_osal_sem_handle_t sem);

/*
 * task
 */
typedef void (*cvi_osal_task_entry_t)(void *param);
typedef struct cvi_osal_task_attr {
    const char *name;
    cvi_osal_task_entry_t entry;
    void *param;
    int32_t priority;
    bool detached;
    int32_t stack_size;
} cvi_osal_task_attr_t;
typedef struct cvi_osal_task {
    cvi_osal_task_attr_t attr;
    pthread_t task;
} cvi_osal_task_t, *cvi_osal_task_handle_t;
int32_t cvi_osal_task_create(cvi_osal_task_attr_t *attr, cvi_osal_task_handle_t *task);
int32_t cvi_osal_task_destroy(cvi_osal_task_handle_t *task);
int32_t cvi_osal_task_join(cvi_osal_task_handle_t task);
void cvi_osal_task_sleep(int64_t time_us);
void cvi_osal_task_resched(void);

/*
 * fs
 */
#define CVI_MAX_PATH_LEN (64)

int32_t cvi_insmod(const char *pszPath, const char *pszOptions);
int32_t cvi_rmmod(const char *pszPath);
int32_t cvi_PathIsDirectory(const char *pszPath);
int32_t cvi_rmdir(const char *pszPath);
int32_t cvi_mkdir(const char *pszPath, mode_t mode);
int32_t cvi_system(const char *pszCmd);
int32_t cvi_usleep(uint32_t usec);
int32_t cvi_du(const char *pszPath, uint64_t *pu64Size_KB);
int32_t cvi_async(void);

#ifdef __cplusplus
}
#endif
#endif
