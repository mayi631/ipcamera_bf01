#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cvi_log.h"
#include "cvi_osal.h"

#define VAR_UNUSED(var) (void)var

static int test_time(void)
{
    int rc = 0;
    uint64_t time_us_1 = 0;
    uint64_t time_us_2 = 0;
    rc |= cvi_osal_get_boot_time_us(&time_us_1);
    usleep(1500000);
    rc |= cvi_osal_get_boot_time_us(&time_us_2);
    CVI_LOGI("Test Time: %" PRIu64 " -> %" PRIu64 ", %" PRIu64 " us\n",
           time_us_1, time_us_2, time_us_2 - time_us_1);
    return rc;
}

static cvi_osal_mutex_t static_mutex = CVI_OSAL_MUTEX_INITIALIZER;
static cvi_osal_mutex_t static_mutex_r = CVI_OSAL_MUTEX_INITIALIZER_R;
static int test_mutex(void)
{
    cvi_osal_mutex_lock(&static_mutex);
    cvi_osal_mutex_lock(&static_mutex_r);
    cvi_osal_mutex_lock(&static_mutex_r);
    int rc = 0;
    cvi_osal_mutex_attr_t ma;
    memset(&ma, 0, sizeof(ma));
    cvi_osal_mutex_handle_t mutex;
    ma.name = "Test Mutex 1";
    ma.type = PTHREAD_MUTEX_NORMAL;
    // ma.type = PTHREAD_MUTEX_RECURSIVE;
    rc = cvi_osal_mutex_create(&ma, &mutex);
    if (rc != CVI_OSAL_SUCCESS) {
        CVI_LOGI("cvi_osal_mutex_create %s failed\n", ma.name);
        exit(-1);
    }
    rc = cvi_osal_mutex_lock(mutex);
    if (rc != CVI_OSAL_SUCCESS) {
        CVI_LOGI("cvi_osal_mutex_lock %s failed\n", mutex->attr.name);
        exit(-1);
    }
    rc = cvi_osal_mutex_unlock(mutex);
    if (rc != CVI_OSAL_SUCCESS) {
        CVI_LOGI("cvi_osal_mutex_unlock %s failed\n", mutex->attr.name);
        exit(-1);
    }
    rc = cvi_osal_mutex_destroy(mutex);
    if (rc != CVI_OSAL_SUCCESS) {
        CVI_LOGI("cvi_osal_mutex_destroy %s failed\n", ma.name);
        exit(-1);
    }
    cvi_osal_mutex_unlock(&static_mutex_r);
    cvi_osal_mutex_unlock(&static_mutex_r);
    cvi_osal_mutex_unlock(&static_mutex);
    CVI_LOGI("Test mutex %s succeeded\n", ma.name);
    return rc;
}

static void test_task_entry(void *p)
{
    int i;
    CVI_LOGI("test_task_entry: >>> %s\n", (char *)p);
    for (i = 0; i < 3; i++) {
        CVI_LOGI("  [%s] count %d\n", (char *)p, i);
        cvi_osal_task_sleep(1000000);
        cvi_osal_task_resched();
    }
    CVI_LOGI("test_task_entry: <<< %s\n", (char *)p);
}

static int test_task(void)
{
    int rc = 0;
    cvi_osal_task_attr_t ta;
    cvi_osal_task_handle_t task;

    // joinable task
    ta.name = "Test Task Joinable";
    ta.entry = test_task_entry;
    ta.param = (void *)ta.name; // carry the name for testing
    ta.priority = CVI_OSAL_PRI_NORMAL;
    ta.detached = false;
    ta.stack_size = 128 * 1024;
    rc = cvi_osal_task_create(&ta, &task);
    if (rc != CVI_OSAL_SUCCESS) {
        CVI_LOGI("cvi_osal_task_create %s failed\n", ta.name);
        exit(-1);
    }
    rc = cvi_osal_task_join(task);
    if (rc != CVI_OSAL_SUCCESS) {
        CVI_LOGI("cvi_osal_task_join %s failed\n", ta.name);
        exit(-1);
    }

    // detached task
    ta.name = "Test Task Detached";
    ta.entry = test_task_entry;
    ta.param = (void *)ta.name; // carry the name for testing
    ta.priority = CVI_OSAL_PRI_NORMAL;
    ta.detached = true;
    ta.stack_size = 128 * 1024;
    rc = cvi_osal_task_create(&ta, &task);
    if (rc != CVI_OSAL_SUCCESS) {
        CVI_LOGI("cvi_osal_task_create %s failed\n", ta.name);
        exit(-1);
    }
    usleep(4000000);

    CVI_LOGI("Test task %s succeeded\n", ta.name);
    return rc;
}

int main(int argc, char **argv)
{
    VAR_UNUSED(argc);
    VAR_UNUSED(argv);

    int rc = 0;
    rc |= test_time();
    rc |= test_mutex();
    rc |= test_task();
    return rc;
}
