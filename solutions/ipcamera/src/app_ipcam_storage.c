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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
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
        "ready=%d\ngeneration=%llu\nactive_users=%u\ntotal_bytes=%llu\nfree_bytes=%llu\n",
        storage_state, storage_error, storage_card.device, APP_STORAGE_MOUNT,
        storage_ready, (unsigned long long)storage_generation, storage_users,
        (unsigned long long)storage_total, (unsigned long long)storage_free);
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
        if (mount(detected.device, APP_STORAGE_MOUNT, "vfat", MS_NODEV | MS_NOSUID | MS_NOATIME, NULL) != 0) {
            set_state("mount_failed", errno);
            return;
        }
        fprintf(stderr, "[storage] mounted %s at %s\n", detected.device, APP_STORAGE_MOUNT);
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
        refresh_storage(removed_event());
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
    if (storage_users)
        storage_users--;
    pthread_mutex_unlock(&storage_mutex);
}
