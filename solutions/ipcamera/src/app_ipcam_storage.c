#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <linux/netlink.h>
#include <mntent.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "app_ipcam_storage.h"

#define STORAGE_RUN "/run/ipcamera-storage"
#define STORAGE_MIN_FREE (16ULL * 1024 * 1024)

struct storage_card {
    char name[32];
    char device[64];
    ino_t identity;
    dev_t number;
};

struct storage_mount {
    int found;
    int readonly;
    int dirsync;
    int elsewhere;
    char device[128];
};

static pthread_mutex_t storage_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t storage_thread;
static int storage_started;
static int storage_stopping;
static int storage_socket = -1;
static struct storage_card storage_card;
static uint64_t storage_generation;
static unsigned int storage_users;
static int storage_ready;
static int storage_stale;
static char storage_managed[128];
static const char *storage_state = "starting";
static int storage_error;
static uint64_t storage_free;
static uint64_t storage_total;
static int storage_access = -1;
static ino_t storage_format_identity;
static struct timespec storage_format_time;
static pthread_mutex_t format_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t format_thread;
static int format_started;
static int format_running;
static int format_stopping;
static int format_initial_pending;
static int format_final_pending;
static struct app_storage_status format_initial;

static uint64_t storage_clock(void)
{
    struct timespec timestamp;
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp))
        return 0;
    return (uint64_t)timestamp.tv_sec * 1000 + timestamp.tv_nsec / 1000000;
}

static int storage_lock(int exclusive, int *descriptor)
{
    *descriptor = -1;
    if (mkdir(STORAGE_RUN, 0700) && errno != EEXIST)
        return errno;
    int access = open(STORAGE_RUN "/access.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (access < 0)
        return errno;
    if (flock(access, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB)) {
        int error = errno;
        close(access);
        return error == EWOULDBLOCK ? EBUSY : error;
    }
    *descriptor = access;
    return 0;
}

int app_ipcam_Storage_Lock(int *descriptor)
{
    return descriptor ? storage_lock(0, descriptor) : EINVAL;
}

static int find_card(struct storage_card *card)
{
    glob_t cards = {0};
    int result = glob("/sys/class/block/mmcblk*/device/type", 0, NULL, &cards);
    if (result != 0) {
        globfree(&cards);
        return result == GLOB_NOMATCH ? 0 : -1;
    }
    int found = 0;
    for (size_t index = 0; index < cards.gl_pathc; index++) {
        FILE *type = fopen(cards.gl_pathv[index], "r");
        char kind[16];
        int sd = type && fscanf(type, "%15s", kind) == 1 && !strcmp(kind, "SD");
        if (type)
            fclose(type);
        struct stat info;
        if (!sd || stat(cards.gl_pathv[index], &info) != 0 ||
            sscanf(cards.gl_pathv[index], "/sys/class/block/%31[^/]/device/type", card->name) != 1)
            continue;
        card->identity = info.st_ino;
        snprintf(card->device, sizeof(card->device), "/dev/%sp1", card->name);
        if (stat(card->device, &info) != 0) {
            char partition[128];
            snprintf(partition, sizeof(partition), "/sys/class/block/%sp1", card->name);
            if (access(partition, F_OK) == 0)
                continue;
            snprintf(card->device, sizeof(card->device), "/dev/%s", card->name);
        }
        if (stat(card->device, &info) != 0 || !S_ISBLK(info.st_mode))
            continue;
        card->number = info.st_rdev;
        found = 1;
        break;
    }
    globfree(&cards);
    return found;
}

static int read_mounts(const struct storage_card *card, struct storage_mount *mounted)
{
    FILE *mounts = setmntent("/proc/mounts", "r");
    if (!mounts)
        return -1;
    struct mntent entry;
    char buffer[4096];
    while (getmntent_r(mounts, &entry, buffer, sizeof(buffer))) {
        if (!strcmp(entry.mnt_dir, APP_STORAGE_MOUNT)) {
            mounted->found = 1;
            mounted->readonly = hasmntopt(&entry, "ro") != NULL;
            mounted->dirsync = hasmntopt(&entry, "dirsync") != NULL || hasmntopt(&entry, "sync") != NULL;
            if (strlen(entry.mnt_fsname) >= sizeof(mounted->device)) {
                endmntent(mounts);
                errno = ENAMETOOLONG;
                return -1;
            }
            strcpy(mounted->device, entry.mnt_fsname);
        } else if (*card->device) {
            struct stat info;
            if (stat(entry.mnt_fsname, &info) == 0 && S_ISBLK(info.st_mode) &&
                info.st_rdev == card->number)
                mounted->elsewhere = 1;
        }
    }
    int failed = ferror(mounts);
    endmntent(mounts);
    return failed ? -1 : 0;
}

static void set_state(const char *state, int error)
{
    if (strcmp(state, storage_state) || error != storage_error)
        fprintf(stderr, "[storage] state=%s error=%d device=%s generation=%llu\n",
            state, error, storage_card.device, (unsigned long long)storage_generation);
    storage_state = state;
    storage_error = error;
}

static void publish_status(void)
{
    FILE *file = fopen(STORAGE_RUN "/status.tmp", "w");
    if (!file)
        return;
    int written = fprintf(file, "[storage]\nstate=%s\nerror=%d\ndevice=%s\nmount_point=%s\n"
        "ready=%d\ngeneration=%llu\nactive_users=%u\ntotal_bytes=%llu\nfree_bytes=%llu\nuptime_ms=%llu\n",
        storage_state, storage_error, storage_card.device, APP_STORAGE_MOUNT,
        storage_ready, (unsigned long long)storage_generation, storage_users,
        (unsigned long long)storage_total, (unsigned long long)storage_free,
        (unsigned long long)storage_clock());
    int closed = fclose(file);
    if (written > 0 && closed == 0)
        rename(STORAGE_RUN "/status.tmp", STORAGE_RUN "/status.ini");
}

static int probe_writable(void)
{
    DIR *directory = opendir(APP_STORAGE_MOUNT);
    if (!directory)
        return -1;
    int readError;
    for (;;) {
        errno = 0;
        if (!readdir(directory)) {
            readError = errno;
            break;
        }
    }
    if (closedir(directory) != 0 && !readError)
        readError = errno;
    if (readError) {
        errno = readError;
        return -1;
    }
    char path[] = APP_STORAGE_MOUNT "/.bf01-probe-XXXXXX";
    int descriptor = mkstemp(path);
    if (descriptor < 0)
        return -1;
    int result = write(descriptor, "SD", 2) == 2 && fsync(descriptor) == 0 ? 0 : -1;
    int error = errno;
    if (close(descriptor) != 0 && result == 0) {
        result = -1;
        error = errno;
    }
    if (unlink(path) != 0 && result == 0) {
        result = -1;
        error = errno;
    }
    errno = error;
    return result;
}

static void refresh_storage(int removed)
{
    struct storage_card detected = {0};
    struct storage_mount mounted = {0};
    int found = find_card(&detected);
    if (found < 0 || read_mounts(&detected, &mounted) != 0) {
        storage_ready = 0;
        set_state("io_error", EIO);
        return;
    }
    int changed = strcmp(storage_card.device, detected.device) ||
        storage_card.identity != detected.identity || storage_card.number != detected.number;
    if (removed || changed) {
        storage_generation++;
        storage_ready = 0;
        storage_free = 0;
        storage_total = 0;
        if (*storage_managed)
            storage_stale = 1;
        storage_card = detected;
    }
    if (storage_stale) {
        if (mounted.found && strcmp(mounted.device, storage_managed)) {
            set_state("mount_conflict", EBUSY);
            return;
        }
        if (storage_users) {
            set_state("removing", EBUSY);
            return;
        }
        if (mounted.found && umount(APP_STORAGE_MOUNT) != 0) {
            set_state("unmount_failed", errno);
            return;
        }
        storage_stale = 0;
        storage_managed[0] = '\0';
        mounted.found = 0;
    }
    if (!found) {
        storage_ready = 0;
        set_state("no_card", ENODEV);
        return;
    }
    if (mounted.found && strcmp(mounted.device, detected.device)) {
        storage_ready = 0;
        set_state("mount_conflict", EBUSY);
        return;
    }
    if (!mounted.found) {
        storage_ready = 0;
        if (storage_users) {
            set_state("removing", EBUSY);
            return;
        }
        if (mounted.elsewhere) {
            set_state("mounted_elsewhere", EBUSY);
            return;
        }
        struct stat info;
        if ((mkdir(APP_STORAGE_MOUNT, 0755) != 0 && errno != EEXIST) ||
            lstat(APP_STORAGE_MOUNT, &info) != 0 || !S_ISDIR(info.st_mode)) {
            set_state("mount_failed", EIO);
            return;
        }
        if (mount(detected.device, APP_STORAGE_MOUNT, "vfat", MS_NODEV | MS_NOSUID | MS_NOATIME | MS_DIRSYNC, NULL) != 0) {
            set_state("mount_failed", errno);
            return;
        }
        mounted.dirsync = 1;
        fprintf(stderr, "[storage] mounted %s at %s with synchronous directory updates\n", detected.device, APP_STORAGE_MOUNT);
        storage_generation++;
    }
    strcpy(storage_managed, detected.device);
    struct statvfs info;
    if (statvfs(APP_STORAGE_MOUNT, &info) != 0) {
        storage_ready = 0;
        storage_stale = 1;
        storage_generation++;
        set_state("io_error", errno);
        return;
    }
    storage_total = (uint64_t)info.f_blocks * info.f_frsize;
    storage_free = (uint64_t)info.f_bavail * info.f_frsize;
    if (mounted.readonly || (info.f_flag & ST_RDONLY)) {
        storage_ready = 0;
        set_state("read_only", EROFS);
        return;
    }
    if (!mounted.dirsync) {
        storage_ready = 0;
        if (storage_users) {
            set_state("mount_options_busy", EBUSY);
            return;
        }
        if (mount(detected.device, APP_STORAGE_MOUNT, "vfat",
                MS_REMOUNT | MS_NODEV | MS_NOSUID | MS_NOATIME | MS_DIRSYNC, NULL) != 0) {
            set_state("mount_options_failed", errno);
            return;
        }
        fprintf(stderr, "[storage] enabled synchronous directory updates on %s\n", detected.device);
    }
    if (storage_free < STORAGE_MIN_FREE) {
        storage_ready = 0;
        set_state("full", ENOSPC);
        return;
    }
    if (!storage_ready && probe_writable() != 0) {
        storage_ready = 0;
        storage_stale = 1;
        storage_generation++;
        set_state("io_error", errno ? errno : EIO);
        return;
    }
    storage_ready = 1;
    set_state("ready", 0);
}

static int removed_event(void)
{
    if (storage_socket < 0)
        return 0;
    int removed = 0;
    for (;;) {
        char buffer[4096];
        struct sockaddr_nl sender = {0};
        socklen_t size = sizeof(sender);
        ssize_t length = recvfrom(storage_socket, buffer, sizeof(buffer) - 1, MSG_DONTWAIT,
            (struct sockaddr *)&sender, &size);
        if (length < 0)
            return removed || errno == ENOBUFS;
        if (length == 0)
            return removed;
        if (sender.nl_pid != 0)
            continue;
        buffer[length] = '\0';
        int action = 0;
        int disk = 0;
        int match = 0;
        for (size_t offset = 0; offset < (size_t)length; offset += strlen(buffer + offset) + 1) {
            const char *field = buffer + offset;
            action |= !strcmp(field, "ACTION=remove");
            disk |= !strcmp(field, "DEVTYPE=disk");
            match |= !strncmp(field, "DEVNAME=", 8) && !strcmp(field + 8, storage_card.name);
        }
        removed |= action && disk && match;
    }
}

static void *storage_monitor(void *opaque)
{
    (void)opaque;
    for (;;) {
        pthread_mutex_lock(&storage_mutex);
        if (storage_stopping) {
            storage_ready = 0;
            set_state("stopped", 0);
            publish_status();
            pthread_mutex_unlock(&storage_mutex);
            break;
        }
        int access = -1;
        int error = app_ipcam_Storage_Lock(&access);
        if (!error) {
            struct stat format = {0};
            ino_t identity = !stat(STORAGE_RUN "/format.status", &format) ? format.st_ino : 0;
            if (identity != storage_format_identity || format.st_mtim.tv_sec != storage_format_time.tv_sec ||
                format.st_mtim.tv_nsec != storage_format_time.tv_nsec) {
                storage_format_identity = identity;
                storage_format_time = format.st_mtim;
                storage_generation++;
                storage_ready = 0;
            }
            refresh_storage(removed_event());
            close(access);
        } else {
            if (strcmp(storage_state, "formatting"))
                storage_generation++;
            storage_ready = 0;
            set_state(error == EBUSY ? "formatting" : "io_error", error);
        }
        publish_status();
        pthread_mutex_unlock(&storage_mutex);
        struct pollfd event = { .fd = storage_socket, .events = POLLIN };
        poll(&event, 1, 500);
    }
    return NULL;
}

int app_ipcam_Storage_Start(void)
{
    pthread_mutex_lock(&storage_mutex);
    if (storage_started) {
        pthread_mutex_unlock(&storage_mutex);
        return 0;
    }
    if (mkdir(STORAGE_RUN, 0700) != 0 && errno != EEXIST) {
        pthread_mutex_unlock(&storage_mutex);
        return -1;
    }
    storage_stopping = 0;
    storage_ready = 0;
    storage_state = "starting";
    storage_error = 0;
    storage_socket = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (storage_socket >= 0) {
        struct sockaddr_nl address = { .nl_family = AF_NETLINK, .nl_groups = 1 };
        if (bind(storage_socket, (struct sockaddr *)&address, sizeof(address)) != 0) {
            close(storage_socket);
            storage_socket = -1;
        }
    }
    if (storage_socket < 0)
        fprintf(stderr, "[storage] uevent unavailable; using 500ms SD polling\n");
    int result = pthread_create(&storage_thread, NULL, storage_monitor, NULL);
    storage_started = result == 0;
    if (result != 0 && storage_socket >= 0) {
        close(storage_socket);
        storage_socket = -1;
    }
    pthread_mutex_unlock(&storage_mutex);
    return result;
}

void app_ipcam_Storage_Stop(void)
{
    pthread_mutex_lock(&storage_mutex);
    int started = storage_started;
    storage_stopping = 1;
    pthread_mutex_unlock(&storage_mutex);
    if (!started)
        return;
    pthread_join(storage_thread, NULL);
    pthread_mutex_lock(&storage_mutex);
    if (storage_socket >= 0)
        close(storage_socket);
    storage_socket = -1;
    storage_started = 0;
    pthread_mutex_unlock(&storage_mutex);
}

int app_ipcam_Storage_Acquire(uint64_t *generation)
{
    if (!generation)
        return EINVAL;
    pthread_mutex_lock(&storage_mutex);
    int error = storage_started && !storage_stopping && storage_ready ? 0 :
        (storage_error ? storage_error : ENODEV);
    if (!error) {
        if (!storage_users)
            error = app_ipcam_Storage_Lock(&storage_access);
    }
    if (!error) {
        *generation = storage_generation;
        storage_users++;
    }
    pthread_mutex_unlock(&storage_mutex);
    return error;
}

int app_ipcam_Storage_Check(uint64_t generation)
{
    pthread_mutex_lock(&storage_mutex);
    int error = !storage_started || storage_stopping || generation != storage_generation ? ENODEV :
        (storage_ready ? 0 : (storage_error ? storage_error : ENODEV));
    pthread_mutex_unlock(&storage_mutex);
    return error;
}

void app_ipcam_Storage_ReportError(uint64_t generation, int error)
{
    if (error != EIO && error != ENODEV && error != EROFS && error != ENOSPC)
        return;
    pthread_mutex_lock(&storage_mutex);
    if (generation == storage_generation) {
        storage_ready = 0;
        if (error == EIO || error == ENODEV) {
            storage_stale = 1;
            storage_generation++;
        }
        set_state(error == EROFS ? "read_only" : error == ENOSPC ? "full" : "io_error", error);
    }
    pthread_mutex_unlock(&storage_mutex);
}

void app_ipcam_Storage_Release(void)
{
    pthread_mutex_lock(&storage_mutex);
    if (storage_users && !--storage_users) {
        close(storage_access);
        storage_access = -1;
    }
    pthread_mutex_unlock(&storage_mutex);
}

static void cached_status(struct app_storage_status *status)
{
    FILE *file = fopen(STORAGE_RUN "/status.ini", "r");
    if (!file)
        return;
    struct app_storage_status cached = {0};
    unsigned long long updated = 0;
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        unsigned long long value;
        if (sscanf(line, "state=%31s", cached.state) == 1 ||
            sscanf(line, "device=%63s", cached.device) == 1 ||
            sscanf(line, "error=%d", &cached.error) == 1 ||
            sscanf(line, "ready=%d", &cached.ready) == 1)
            continue;
        if (sscanf(line, "generation=%llu", &value) == 1)
            cached.generation = value;
        else if (sscanf(line, "total_bytes=%llu", &value) == 1)
            cached.total_bytes = value;
        else if (sscanf(line, "free_bytes=%llu", &value) == 1)
            cached.free_bytes = value;
        else
            sscanf(line, "uptime_ms=%llu", &updated);
    }
    fclose(file);
    uint64_t now = storage_clock();
    if (updated && now >= updated && now - updated <= 2000 && *cached.state)
        *status = cached;
}

static void read_format(char *state, size_t capacity, int busy)
{
    snprintf(state, capacity, "success");
    FILE *file = fopen(STORAGE_RUN "/format.status", "r");
    if (file) {
        char saved[16];
        if (fscanf(file, "%15s", saved) == 1 &&
            (!strcmp(saved, "formatting") || !strcmp(saved, "success") || !strcmp(saved, "fail")))
            snprintf(state, capacity, "%s", !strcmp(saved, "formatting") && !busy ? "fail" : saved);
        else
            snprintf(state, capacity, "fail");
        fclose(file);
    }
    if (busy)
        snprintf(state, capacity, "formatting");
}

int app_ipcam_Storage_Status(struct app_storage_status *status)
{
    if (!status)
        return EINVAL;
    memset(status, 0, sizeof(*status));
    cached_status(status);
    int access = -1;
    int error = app_ipcam_Storage_Lock(&access);
    read_format(status->format_status, sizeof(status->format_status), error == EBUSY);
    if (error) {
        snprintf(status->state, sizeof(status->state), "%s", error == EBUSY ? "formatting" : "io_error");
        status->ready = 0;
        status->error = error;
        return 0;
    }
    struct storage_card card = {0};
    struct storage_mount mounted = {0};
    int found = find_card(&card);
    error = found < 0 || read_mounts(&card, &mounted) ? EIO : 0;
    if (!error && !found) {
        memset(status, 0, sizeof(*status));
        snprintf(status->state, sizeof(status->state), "no_card");
        status->error = ENODEV;
    } else if (!error) {
        if (strcmp(status->device, card.device))
            memset(status, 0, sizeof(*status));
        snprintf(status->device, sizeof(status->device), "%s", card.device);
        if (!mounted.found || strcmp(mounted.device, card.device)) {
            snprintf(status->state, sizeof(status->state), "%s", mounted.found ? "mount_conflict" : "mount_failed");
            status->ready = 0;
            status->error = mounted.found ? EBUSY : ENXIO;
            status->total_bytes = status->free_bytes = 0;
        } else {
            struct statvfs info;
            if (statvfs(APP_STORAGE_MOUNT, &info))
                error = errno;
            else {
                status->total_bytes = (uint64_t)info.f_blocks * info.f_frsize;
                status->free_bytes = (uint64_t)info.f_bfree * info.f_frsize;
                if (mounted.readonly || (info.f_flag & ST_RDONLY)) {
                    snprintf(status->state, sizeof(status->state), "read_only");
                    status->ready = 0;
                    status->error = EROFS;
                } else if ((uint64_t)info.f_bavail * info.f_frsize < STORAGE_MIN_FREE) {
                    snprintf(status->state, sizeof(status->state), "full");
                    status->ready = 0;
                    status->error = ENOSPC;
                } else if (!*status->state || !strcmp(status->state, "formatting")) {
                    snprintf(status->state, sizeof(status->state), "ready");
                    status->ready = 1;
                    status->error = 0;
                }
            }
        }
    }
    if (error) {
        snprintf(status->state, sizeof(status->state), "io_error");
        status->ready = 0;
        status->error = error;
        status->total_bytes = status->free_bytes = 0;
    }
    read_format(status->format_status, sizeof(status->format_status), 0);
    close(access);
    return 0;
}

int app_ipcam_Storage_ReportStatus(struct app_storage_status *status)
{
    if (!status)
        return EINVAL;
    pthread_mutex_lock(&format_mutex);
    int pending = format_initial_pending;
    if (pending)
        *status = format_initial;
    pthread_mutex_unlock(&format_mutex);
    return pending ? 0 : app_ipcam_Storage_Status(status);
}

void app_ipcam_Storage_Reported(const char *state)
{
    pthread_mutex_lock(&format_mutex);
    if (state && !strcmp(state, "formatting"))
        format_initial_pending = 0;
    else if (state && !format_initial_pending && !format_running &&
             (!strcmp(state, "success") || !strcmp(state, "fail")))
        format_final_pending = 0;
    pthread_mutex_unlock(&format_mutex);
}

void app_ipcam_Storage_ReportReset(void)
{
    pthread_mutex_lock(&format_mutex);
    format_initial_pending = 0;
    pthread_mutex_unlock(&format_mutex);
}

static int save_format(const char *state)
{
    int descriptor = open(STORAGE_RUN "/format.status.tmp", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0)
        return errno;
    size_t length = strlen(state);
    int error = write(descriptor, state, length) == (ssize_t)length && !fsync(descriptor) ? 0 : EIO;
    if (close(descriptor) && !error)
        error = errno;
    if (!error && rename(STORAGE_RUN "/format.status.tmp", STORAGE_RUN "/format.status"))
        error = errno;
    if (error)
        unlink(STORAGE_RUN "/format.status.tmp");
    return error;
}

struct format_task {
    struct storage_card card;
    char tool[64];
    int access;
};

static int same_card(const struct storage_card *expected)
{
    struct storage_card detected = {0};
    return find_card(&detected) == 1 && !strcmp(expected->device, detected.device) &&
        expected->identity == detected.identity && expected->number == detected.number;
}

static int format_filesystem(const struct format_task *task)
{
    int descriptor = open(task->card.device, O_RDWR | O_EXCL | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return errno;
    struct stat info;
    int error = fstat(descriptor, &info) ? errno :
        !S_ISBLK(info.st_mode) || info.st_rdev != task->card.number || !same_card(&task->card) ? ENODEV : 0;
    posix_spawn_file_actions_t actions;
    int initialized = 0;
    if (!error) {
        error = posix_spawn_file_actions_init(&actions);
        initialized = !error;
    }
    if (!error)
        error = posix_spawn_file_actions_adddup2(&actions, descriptor, STDIN_FILENO);
    if (!error)
        error = posix_spawn_file_actions_adddup2(&actions, task->access, 3);
    if (!error)
        error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, STORAGE_RUN "/format.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (!error)
        error = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t child = -1;
    char *arguments[] = { (char *)task->tool, "-n", "BF01", "/proc/self/fd/0", NULL };
    extern char **environ;
    if (!error) {
        pthread_mutex_lock(&format_mutex);
        error = format_stopping ? ECANCELED : posix_spawn(&child, task->tool, &actions, NULL, arguments, environ);
        pthread_mutex_unlock(&format_mutex);
    }
    if (initialized)
        posix_spawn_file_actions_destroy(&actions);
    uint64_t deadline = storage_clock() + 120000;
    while (!error && child > 0) {
        int result = 0;
        pid_t finished = waitpid(child, &result, WNOHANG);
        if (finished == child) {
            error = WIFEXITED(result) && !WEXITSTATUS(result) ? 0 : EIO;
            break;
        }
        if (finished < 0 && errno != EINTR) {
            error = errno;
            break;
        }
        pthread_mutex_lock(&format_mutex);
        int stopping = format_stopping;
        pthread_mutex_unlock(&format_mutex);
        int removed = !same_card(&task->card);
        if (stopping || removed || storage_clock() >= deadline) {
            kill(child, SIGKILL);
            while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
            error = stopping ? ECANCELED : removed ? ENODEV : ETIMEDOUT;
            break;
        }
        usleep(100000);
    }
    if (!error && fsync(descriptor))
        error = errno;
    close(descriptor);
    return error;
}

static void *format_worker(void *opaque)
{
    struct format_task *task = opaque;
    struct storage_mount mounted = {0};
    pthread_mutex_lock(&format_mutex);
    int stopping = format_stopping;
    pthread_mutex_unlock(&format_mutex);
    int error = stopping ? ECANCELED : !same_card(&task->card) ? ENODEV :
        read_mounts(&task->card, &mounted) ? EIO : 0;
    int unmounted = 0;
    if (!error && (mounted.elsewhere || (mounted.found && strcmp(mounted.device, task->card.device))))
        error = EBUSY;
    if (!error && mounted.found && umount(APP_STORAGE_MOUNT))
        error = errno;
    else if (!error)
        unmounted = 1;
    if (!error)
        error = format_filesystem(task);
    if (unmounted && same_card(&task->card)) {
        if (mount(task->card.device, APP_STORAGE_MOUNT, "vfat", MS_NODEV | MS_NOSUID | MS_NOATIME | MS_DIRSYNC, NULL)) {
            if (!error)
                error = errno;
        } else if (!error && probe_writable())
            error = errno ? errno : EIO;
    } else if (!error)
        error = ENODEV;
    int save_error = save_format(error ? "fail" : "success");
    if (!error)
        error = save_error;
    fprintf(stderr, "[storage] format finished device=%s errno=%d status=%s\n",
        task->card.device, error, error ? "fail" : "success");
    pthread_mutex_lock(&format_mutex);
    close(task->access);
    free(task);
    format_running = 0;
    pthread_mutex_unlock(&format_mutex);
    return NULL;
}

int app_ipcam_Storage_Format(void)
{
    pthread_mutex_lock(&format_mutex);
    if (format_running || format_initial_pending || format_final_pending || format_stopping) {
        pthread_mutex_unlock(&format_mutex);
        return EBUSY;
    }
    if (format_started) {
        pthread_join(format_thread, NULL);
        format_started = 0;
    }
    struct format_task *task = calloc(1, sizeof(*task));
    if (task)
        task->access = -1;
    int error = task ? app_ipcam_Storage_Status(&format_initial) : ENOMEM;
    if (!error)
        error = storage_lock(1, &task->access);
    if (!error) {
        int found = find_card(&task->card);
        error = found == 1 ? 0 : found ? EIO : ENODEV;
    }
    if (!error) {
        const char *tools[] = { "/sbin/mkfs.vfat", "/usr/sbin/mkfs.vfat", "/sbin/mkdosfs", "/usr/sbin/mkdosfs" };
        for (size_t index = 0; index < sizeof(tools) / sizeof(tools[0]); index++) {
            if (!access(tools[index], X_OK)) {
                snprintf(task->tool, sizeof(task->tool), "%s", tools[index]);
                break;
            }
        }
        if (!*task->tool)
            error = ENOSYS;
    }
    if (!error) {
        struct storage_mount mounted = {0};
        error = read_mounts(&task->card, &mounted) ? EIO : mounted.elsewhere ||
            (mounted.found && strcmp(mounted.device, task->card.device)) ? EBUSY : 0;
    }
    if (!error)
        error = save_format("formatting");
    if (!error) {
        snprintf(format_initial.state, sizeof(format_initial.state), "formatting");
        snprintf(format_initial.format_status, sizeof(format_initial.format_status), "formatting");
        format_initial.ready = 0;
        format_initial.error = 0;
        format_initial_pending = 1;
        format_final_pending = 1;
        format_running = 1;
        error = pthread_create(&format_thread, NULL, format_worker, task);
        if (!error)
            format_started = 1;
        else {
            format_final_pending = format_initial_pending = format_running = 0;
            save_format("fail");
        }
    }
    if (error && task) {
        if (task->access >= 0)
            close(task->access);
        free(task);
    }
    pthread_mutex_unlock(&format_mutex);
    return error;
}

void app_ipcam_Storage_FormatStop(void)
{
    pthread_mutex_lock(&format_mutex);
    format_stopping = 1;
    int started = format_started;
    pthread_mutex_unlock(&format_mutex);
    if (started)
        pthread_join(format_thread, NULL);
    pthread_mutex_lock(&format_mutex);
    format_started = 0;
    pthread_mutex_unlock(&format_mutex);
}
