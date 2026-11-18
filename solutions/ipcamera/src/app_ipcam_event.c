#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mntent.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "app_ipcam_event.h"
#include "app_ipcam_settings.h"

#define LIVE_FILE "/run/ipcamera-wifi/live-mode"
#define BOOT_CANCEL_FILE "/run/ipcamera-wifi/boot-media-cancelled"

static int config_directory(char *directory, size_t capacity, int create)
{
    FILE *mounts = setmntent("/proc/mounts", "r");
    if (!mounts)
        return errno;
    struct mntent entry;
    char buffer[4096];
    int cfg_found = 0, data_found = 0, cfg_readonly = 0, data_readonly = 0;
    while (getmntent_r(mounts, &entry, buffer, sizeof(buffer))) {
        if (!strcmp(entry.mnt_dir, "/mnt/cfg")) {
            cfg_found = 1;
            cfg_readonly = hasmntopt(&entry, "rw") == NULL;
        } else if (!strcmp(entry.mnt_dir, "/mnt/data")) {
            data_found = 1;
            data_readonly = hasmntopt(&entry, "rw") == NULL;
        }
    }
    int error = ferror(mounts) ? EIO : 0;
    endmntent(mounts);
    if (error)
        return error;
    if (!cfg_found && !data_found)
        return ENODEV;
    const char *selected = cfg_found ? "/mnt/cfg/ipcamera" : "/mnt/data/ipcamera-config";
    int length = snprintf(directory, capacity, "%s", selected);
    if (length < 0 || (size_t)length >= capacity)
        return ENAMETOOLONG;
    if (!create)
        return 0;
    if (cfg_found ? cfg_readonly : data_readonly)
        return EROFS;
    if (mkdir(directory, 0700) && errno != EEXIST)
        return errno;
    struct stat info;
    if (lstat(directory, &info))
        return errno;
    if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid())
        return EACCES;
    if ((info.st_mode & 07777) == 0700)
        return 0;
    return chmod(directory, 0700) ? errno : 0;
}

int app_event_config_path(const char *name, char *path, size_t capacity)
{
    if (!name || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..") || !path || !capacity)
        return EINVAL;
    char directory[PATH_MAX];
    int error = config_directory(directory, sizeof(directory), 0);
    if (error)
        return error;
    int length = snprintf(path, capacity, "%s/%s", directory, name);
    return length < 0 || (size_t)length >= capacity ? ENAMETOOLONG : 0;
}

int app_event_config_prepare(void)
{
    char directory[PATH_MAX];
    return config_directory(directory, sizeof(directory), 1);
}

const char *app_event_text(const cJSON *object, const char *name)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(value) ? value->valuestring : "";
}

uint64_t app_event_realtime(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int app_event_read_json(const char *path, cJSON **value)
{
    *value = NULL;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return errno;
    struct stat info;
    int error = fstat(descriptor, &info) ? errno : 0;
    if (!error && (!S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > 65536))
        error = EINVAL;
    char *text = !error ? malloc((size_t)info.st_size + 1) : NULL;
    if (!error && !text)
        error = ENOMEM;
    size_t offset = 0;
    while (!error && offset < (size_t)info.st_size) {
        ssize_t count = read(descriptor, text + offset, (size_t)info.st_size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            error = count < 0 ? errno : EIO;
        else
            offset += (size_t)count;
    }
    close(descriptor);
    if (!error) {
        text[offset] = 0;
        *value = cJSON_ParseWithOpts(text, NULL, 1);
        if (!cJSON_IsObject(*value)) {
            cJSON_Delete(*value);
            *value = NULL;
            error = EPROTO;
        }
    }
    free(text);
    return error;
}

int app_event_write_json(const char *path, const cJSON *value)
{
    char temporary[PATH_MAX], parent[PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path) >= (int)sizeof(temporary) ||
        snprintf(parent, sizeof(parent), "%s", path) >= (int)sizeof(parent))
        return ENAMETOOLONG;
    char *slash = strrchr(parent, '/');
    if (!slash)
        return EINVAL;
    *slash = 0;
    cJSON *stored = NULL;
    int unchanged = !app_event_read_json(path, &stored) && cJSON_Compare(stored, value, 1);
    cJSON_Delete(stored);
    if (unchanged)
        return 0;
    char *text = cJSON_PrintUnformatted(value);
    if (!text)
        return ENOMEM;
    int descriptor = mkostemp(temporary, O_CLOEXEC);
    int error = descriptor < 0 ? errno : 0;
    size_t offset = 0, length = strlen(text);
    while (!error && offset < length) {
        ssize_t count = write(descriptor, text + offset, length - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            error = count < 0 ? errno : EIO;
        else
            offset += (size_t)count;
    }
    if (!error && fsync(descriptor))
        error = errno;
    if (descriptor >= 0 && close(descriptor) && !error)
        error = errno;
    if (!error && rename(temporary, path))
        error = errno;
    if (!error) {
        descriptor = open(*parent ? parent : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (descriptor < 0)
            error = errno;
        else {
            if (fsync(descriptor))
                error = errno;
            close(descriptor);
        }
    }
    if (error)
        unlink(temporary);
    cJSON_free(text);
    return error;
}

static int filter_percent(const cJSON *value)
{
    if (!cJSON_IsString(value))
        return 0;
    return !strcmp(value->valuestring, "low") ? 30 : !strcmp(value->valuestring, "medium") ? 60 :
        !strcmp(value->valuestring, "high") ? 80 : 0;
}

static int datetime_format(const cJSON *value)
{
    if (!cJSON_IsString(value))
        return -1;
    return !strcmp(value->valuestring, "yyyy-MM-dd HH:mm:ss") ? 0 :
        !strcmp(value->valuestring, "MM/dd/yyyy HH:mm:ss") ? 1 :
        !strcmp(value->valuestring, "dd-MM-yyyy HH:mm:ss") ? 2 : -1;
}

int app_event_policy_field(const char *name)
{
    return name && (!strcmp(name, "pir_record_dur") || !strcmp(name, "ai_detect_switch") ||
        !strcmp(name, "bird_detect_switch") || !strcmp(name, "squirrel_detect_switch") ||
        !strcmp(name, "other_detect_switch") || !strcmp(name, "image_filter_level") ||
        !strcmp(name, "datetime_wm_switch") || !strcmp(name, "datetime_wm_format"));
}

int app_event_policy_load(struct app_event_policy *policy)
{
    *policy = (struct app_event_policy){
        .ai_enabled = 1, .bird_alarm = 1, .squirrel_alarm = 1, .record_seconds = 10,
        .datetime_enabled = -1, .datetime_format = -1
    };
    cJSON *saved = NULL;
    int error = app_settings_load(&saved);
    if (error)
        return error;
    const char *names[] = {"ai_detect_switch", "bird_detect_switch", "squirrel_detect_switch", "other_detect_switch"};
    policy->ai_configured = cJSON_GetObjectItemCaseSensitive(saved, "ai_detect_switch") != NULL;
    int *destinations[] = {&policy->ai_enabled, &policy->bird_alarm, &policy->squirrel_alarm, &policy->other_alarm};
    for (size_t index = 0; index < 4; index++) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(saved, names[index]);
        if (value && !cJSON_IsBool(value))
            error = EPROTO;
        else if (value)
            *destinations[index] = cJSON_IsTrue(value);
    }
    const cJSON *duration = cJSON_GetObjectItemCaseSensitive(saved, "pir_record_dur");
    if (duration) {
        policy->record_configured = 1;
        policy->record_seconds = duration->valueint;
    }
    const cJSON *filter = cJSON_GetObjectItemCaseSensitive(saved, "image_filter_level");
    if (filter) {
        policy->confidence_percent = filter_percent(filter);
        if (!policy->confidence_percent)
            error = EPROTO;
    }
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(saved, "datetime_wm_switch");
    if (enabled) {
        if (!cJSON_IsBool(enabled))
            error = EPROTO;
        else
            policy->datetime_enabled = cJSON_IsTrue(enabled);
    }
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(saved, "datetime_wm_format");
    if (format) {
        policy->datetime_format = datetime_format(format);
        if (policy->datetime_format < 0)
            error = EPROTO;
    }
    cJSON_Delete(saved);
    return error;
}

int app_event_live(void)
{
    return access(LIVE_FILE, F_OK) == 0;
}

int app_event_boot_cancelled(void)
{
    return app_event_live() || access(BOOT_CANCEL_FILE, F_OK) == 0;
}

int app_event_set_live(int enabled)
{
    if (!enabled)
        return unlink(LIVE_FILE) && errno != ENOENT ? errno : 0;
    if (mkdir("/run/ipcamera-wifi", 0700) && errno != EEXIST)
        return errno;
    int cancelled = open(BOOT_CANCEL_FILE, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (cancelled < 0)
        return errno;
    close(cancelled);
    int descriptor = open(LIVE_FILE, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0)
        return errno;
    close(descriptor);
    return 0;
}

int app_event_begin(cJSON **event)
{
    *event = NULL;
    struct app_event_policy policy;
    int error = app_event_policy_load(&policy);
    if (error)
        return error;
    cJSON *ai = NULL;
    error = app_event_read_json("/run/ipcamera-ai/result.json", &ai);
    if (error)
        return error;
    unsigned char random[8];
    char identity[17];
    if (RAND_bytes(random, sizeof(random)) != 1) {
        cJSON_Delete(ai);
        return EIO;
    }
    for (size_t index = 0; index < sizeof(random); index++)
        snprintf(identity + index * 2, 3, "%02x", random[index]);
    cJSON *value = cJSON_CreateObject();
    if (!value) {
        cJSON_Delete(ai);
        return ENOMEM;
    }
    cJSON_AddStringToObject(value, "eventId", identity);
    cJSON_AddItemToObject(value, "ai", ai);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    cJSON_AddNumberToObject(value, "captureUptimeMs", (double)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000));
    char boot_id[64] = {0};
    FILE *input = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (input) {
        if (!fgets(boot_id, sizeof(boot_id), input))
            boot_id[0] = 0;
        fclose(input);
    }
    boot_id[strcspn(boot_id, "\r\n")] = 0;
    cJSON_AddStringToObject(value, "bootId", boot_id);
    cJSON_AddNumberToObject(value, "eventTime", app_event_realtime() >= 1577836800000ULL ? (double)app_event_realtime() : 0);
    cJSON_AddBoolToObject(value, "birdAlarm", policy.bird_alarm);
    cJSON_AddBoolToObject(value, "squirrelAlarm", policy.squirrel_alarm);
    cJSON_AddBoolToObject(value, "otherAlarm", policy.other_alarm);
    *event = value;
    return 0;
}

int app_event_media(const char *directory, cJSON *event, const char *purpose, const char *path)
{
    if (app_event_live())
        return ECANCELED;
    struct stat info;
    if (stat(path, &info))
        return errno;
    if (!S_ISREG(info.st_mode) || !info.st_size)
        return EINVAL;
    char manifest[PATH_MAX];
    if (snprintf(manifest, sizeof(manifest), "%s/%s.upload.json", directory, purpose) >= (int)sizeof(manifest))
        return ENAMETOOLONG;
    cJSON *job = cJSON_Duplicate(event, 1);
    if (!job)
        return ENOMEM;
    cJSON_AddStringToObject(job, "purpose", purpose);
    cJSON_AddStringToObject(job, "path", path);
    cJSON_AddStringToObject(job, "fileType", !strcmp(purpose, "image") ? "jpg" : "mp4");
    cJSON_AddStringToObject(job, "state", "pending");
    cJSON_AddNumberToObject(job, "sourceSize", (double)info.st_size);
    int error = app_event_write_json(manifest, job);
    cJSON_Delete(job);
    return error;
}
