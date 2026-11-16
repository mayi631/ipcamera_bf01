#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app_ipcam_gpio.h"
#include "app_ipcam_power_guard.h"

#define POWER_GUARD_GPIO CVI_GPIOA_29
#define POWER_GUARD_POLL_INTERVAL_US 10000U

static pthread_t g_power_guard_thread;
static volatile int g_power_guard_running;
static volatile int g_power_guard_triggered;
static int g_power_guard_gpio_exported;

extern char **environ;

static void app_ipcam_PowerGuard_RequestPoweroff(void)
{
    posix_spawnattr_t attributes;
    sigset_t signals;
    pid_t child;
    int status;
    int result;
    const char *command = "/mnt/data/bf01-poweroff";
    char *arguments[] = { "bf01-poweroff", NULL };

    if (access(command, X_OK) != 0)
        command = "/mnt/system/bf01-preview/bf01-poweroff";
    if (access(command, X_OK) != 0) {
        fprintf(stderr, "[power-guard] shutdown helper missing; Wi-Fi unload skipped\n");
        command = "/sbin/poweroff";
        arguments[0] = "poweroff";
    }
    fprintf(stderr, "[power-guard] GPIOA29 is low; executing %s\n", command);
    result = posix_spawnattr_init(&attributes);
    if (result != 0) {
        fprintf(stderr, "[power-guard] poweroff spawn setup failed: %s\n", strerror(result));
        return;
    }

    sigemptyset(&signals);
    result = posix_spawnattr_setsigmask(&attributes, &signals);
    if (result == 0)
        result = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK);
    if (result == 0)
        result = posix_spawn(&child, command, NULL, &attributes, arguments, environ);
    posix_spawnattr_destroy(&attributes);
    if (result != 0) {
        fprintf(stderr, "[power-guard] %s launch failed: %s\n", command, strerror(result));
        return;
    }

    fprintf(stderr, "[power-guard] %s started: pid=%ld\n", command, (long)child);
    do {
        result = waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        fprintf(stderr, "[power-guard] %s wait failed: %s\n", command, strerror(errno));
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[power-guard] shutdown command exited successfully; awaiting init shutdown\n");
    } else {
        fprintf(stderr, "[power-guard] %s failed: wait_status=%d\n", command, status);
    }
}

static void *app_ipcam_PowerGuard_Thread(void *arg)
{
    CVI_GPIO_VALUE_E value;

    (void)arg;
    while (g_power_guard_running) {
        if (app_ipcam_Gpio_Value_Get(POWER_GUARD_GPIO, &value) != 0) {
            fprintf(stderr, "[power-guard] GPIOA29 read failed; poweroff not triggered\n");
        } else if (value == CVI_GPIO_VALUE_L && !g_power_guard_triggered) {
            g_power_guard_triggered = 1;
            app_ipcam_PowerGuard_RequestPoweroff();
            break;
        }
        usleep(POWER_GUARD_POLL_INTERVAL_US);
    }

    return NULL;
}

int app_ipcam_PowerGuard_Start(void)
{
    int result;

    if (g_power_guard_running)
        return 0;

    result = app_ipcam_Gpio_Export(POWER_GUARD_GPIO);
    if (result != 0) {
        fprintf(stderr, "[power-guard] GPIOA29 export failed: %d\n", result);
        return result;
    }
    g_power_guard_gpio_exported = 1;

    result = app_ipcam_Gpio_Dir_Set(POWER_GUARD_GPIO, CVI_GPIO_DIR_IN);
    if (result != 0) {
        fprintf(stderr, "[power-guard] GPIOA29 input setup failed: %d\n", result);
        app_ipcam_Gpio_Unexport(POWER_GUARD_GPIO);
        g_power_guard_gpio_exported = 0;
        return result;
    }

    g_power_guard_triggered = 0;
    g_power_guard_running = 1;
    result = pthread_create(&g_power_guard_thread, NULL,
                            app_ipcam_PowerGuard_Thread, NULL);
    if (result != 0) {
        g_power_guard_running = 0;
        fprintf(stderr, "[power-guard] thread create failed: %s\n", strerror(result));
        app_ipcam_Gpio_Unexport(POWER_GUARD_GPIO);
        g_power_guard_gpio_exported = 0;
        return result;
    }

    fprintf(stderr, "[power-guard] GPIOA29 input monitor started\n");
    return 0;
}

void app_ipcam_PowerGuard_Stop(void)
{
    g_power_guard_running = 0;
    if (g_power_guard_thread) {
        pthread_join(g_power_guard_thread, NULL);
        g_power_guard_thread = 0;
    }
    if (g_power_guard_gpio_exported) {
        app_ipcam_Gpio_Unexport(POWER_GUARD_GPIO);
        g_power_guard_gpio_exported = 0;
    }
}
