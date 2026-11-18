#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "app_ipcam_event.h"
#include "app_ipcam_settings.h"
#include "app_ipcam_boot_settings.h"

static int choice(const cJSON *value, const char *first, const char *second, const char *third)
{
    return cJSON_IsString(value) && (!strcmp(value->valuestring, first) ||
        !strcmp(value->valuestring, second) || !strcmp(value->valuestring, third));
}

static int integer(const cJSON *value, int minimum, int maximum)
{
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
        value->valuedouble >= minimum && value->valuedouble <= maximum &&
        value->valuedouble == value->valueint;
}

static int value_valid(const cJSON *field)
{
    if (!field->string)
        return 0;
    if (!strcmp(field->string, "pir_sens_level") || !strcmp(field->string, "image_filter_level"))
        return choice(field, "low", "medium", "high");
    if (!strcmp(field->string, "pir_detect_intv"))
        return integer(field, 0, 30) && field->valueint % 5 == 0;
    if (!strcmp(field->string, "pir_record_dur"))
        return integer(field, -1, 300);
    if (!strcmp(field->string, "capture_channel"))
        return integer(field, 0, 2);
    if (!strcmp(field->string, "spk_vol"))
        return integer(field, 0, 100) && field->valueint % 5 == 0;
    if (!strcmp(field->string, "image_anti_flicker"))
        return choice(field, "off", "50Hz", "60Hz");
    if (!strcmp(field->string, "datetime_wm_format"))
        return choice(field, "yyyy-MM-dd HH:mm:ss", "MM/dd/yyyy HH:mm:ss", "dd-MM-yyyy HH:mm:ss");
    if (!strcmp(field->string, "ai_detect_switch") || !strcmp(field->string, "bird_detect_switch") ||
        !strcmp(field->string, "squirrel_detect_switch") || !strcmp(field->string, "other_detect_switch") ||
        !strcmp(field->string, "brand_wm_switch") || !strcmp(field->string, "datetime_wm_switch") ||
        !strcmp(field->string, "mic_switch") || !strcmp(field->string, "status_led_switch") ||
        !strcmp(field->string, "capture_enabled"))
        return cJSON_IsBool(field);
    return 0;
}

static int settings_valid(const cJSON *settings)
{
    if (!cJSON_IsObject(settings))
        return 0;
    const cJSON *field;
    cJSON_ArrayForEach(field, settings) {
        if (!value_valid(field))
            return 0;
        for (const cJSON *other = field->next; other; other = other->next)
            if (other->string && !strcmp(field->string, other->string))
                return 0;
    }
    return 1;
}

int app_settings_path(char *path, size_t capacity)
{
    return app_event_config_path("app-settings.json", path, capacity);
}

int app_settings_load_locked(cJSON **settings)
{
    if (!settings)
        return EINVAL;
    *settings = NULL;
    char path[PATH_MAX];
    int error = app_settings_path(path, sizeof(path));
    if (!error)
        error = app_event_read_json(path, settings);
    if (!error && !settings_valid(*settings))
        error = EPROTO;
    if (error) {
        cJSON_Delete(*settings);
        *settings = NULL;
    }
    return error;
}

int app_settings_sync_locked(void)
{
    char path[PATH_MAX];
    int error = app_settings_path(path, sizeof(path));
    if (error)
        return error;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return errno;
    error = fsync(descriptor) ? errno : 0;
    close(descriptor);
    if (error)
        return error;
    char *slash = strrchr(path, '/');
    if (!slash)
        return EINVAL;
    *slash = 0;
    descriptor = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return errno;
    error = fsync(descriptor) ? errno : 0;
    close(descriptor);
    return error;
}

static int save_locked(const cJSON *settings)
{
    char path[PATH_MAX];
    int error = app_settings_path(path, sizeof(path));
    if (!error)
        error = app_event_write_json(path, settings);
    if (!error)
        error = app_settings_sync_locked();
    cJSON *verified = NULL;
    if (!error)
        error = app_settings_load_locked(&verified);
    if (!error && !cJSON_Compare(settings, verified, 1))
        error = EIO;
    cJSON_Delete(verified);
    return error;
}

static int import_audio(cJSON *settings, const char *path, const char *name, int maximum, int step)
{
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0)
        return errno == ENOENT ? 0 : errno;
    struct stat info;
    int error = fstat(descriptor, &info) ? errno : 0;
    if (!error && (!S_ISREG(info.st_mode) || info.st_size < 1 || info.st_size > 16))
        error = EPROTO;
    char text[17] = {0};
    ssize_t length = error ? -1 : read(descriptor, text, sizeof(text) - 1);
    if (!error && length != info.st_size)
        error = length < 0 ? errno : EIO;
    close(descriptor);
    if (error)
        return error;
    if (text[0] < '0' || text[0] > '9')
        return EPROTO;
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || value > (unsigned int)maximum || value % step ||
        (end != text + length && (end != text + length - 1 || *end != '\n')))
        return EPROTO;
    if (!strcmp(name, "mic_switch") && (length > 2 || (length == 2 && text[1] != '\n')))
        return EPROTO;
    cJSON *field = !strcmp(name, "mic_switch") ? cJSON_CreateBool(value) : cJSON_CreateNumber(value);
    cJSON_DeleteItemFromObjectCaseSensitive(settings, name);
    return cJSON_AddItemToObject(settings, name, field) ? 0 : ENOMEM;
}

static int migrate_locked(void)
{
    cJSON *settings = NULL;
    int error = app_settings_load_locked(&settings);
    if (error != ENOENT) {
        cJSON_Delete(settings);
        return error;
    }
    char path[PATH_MAX];
    error = app_event_config_path("event-policy.json", path, sizeof(path));
    if (!error)
        error = app_event_read_json(path, &settings);
    if (error == ENOENT) {
        settings = cJSON_CreateObject();
        error = settings ? 0 : ENOMEM;
    }
    if (!error && !settings_valid(settings))
        error = EPROTO;
    if (!error)
        error = import_audio(settings, "/mnt/data/ipcamera-wifi/speaker-volume", "spk_vol", 100, 5);
    if (!error)
        error = import_audio(settings, "/mnt/data/ipcamera-wifi/microphone-enabled", "mic_switch", 1, 1);
    if (!error)
        error = save_locked(settings);
    if (!error) {
        app_settings_path(path, sizeof(path));
        fprintf(stderr, "[settings] migrated legacy configuration to %s fields=%d; legacy files are no longer active\n",
            path, cJSON_GetArraySize(settings));
    }
    cJSON_Delete(settings);
    return error;
}

int app_settings_lock(int operation)
{
    char path[PATH_MAX];
    int error = app_event_config_prepare();
    if (!error)
        error = app_event_config_path("app-settings.lock", path, sizeof(path));
    int descriptor = !error ? open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600) : -1;
    if (!error && descriptor < 0)
        error = errno;
    struct stat info;
    if (!error && fstat(descriptor, &info))
        error = errno;
    if (!error && !S_ISREG(info.st_mode))
        error = EINVAL;
    if (!error && flock(descriptor, operation))
        error = errno;
    if (!error)
        error = app_settings_path(path, sizeof(path));
    if (!error && lstat(path, &info)) {
        error = errno;
        if (error == ENOENT) {
            error = 0;
            if (operation & LOCK_SH) {
                if (flock(descriptor, LOCK_UN) || flock(descriptor, LOCK_EX | (operation & LOCK_NB)))
                    error = errno;
            }
            if (!error)
                error = migrate_locked();
            if (!error && (operation & LOCK_SH) && flock(descriptor, operation))
                error = errno;
        }
    }
    if (error) {
        if (descriptor >= 0)
            close(descriptor);
        errno = error;
        return -1;
    }
    return descriptor;
}

int app_settings_load(cJSON **settings)
{
    if (!settings)
        return EINVAL;
    *settings = NULL;
    int descriptor = app_settings_lock(LOCK_SH);
    if (descriptor < 0)
        return errno;
    int error = app_settings_load_locked(settings);
    close(descriptor);
    return error;
}

int app_settings_apply_locked(const cJSON *changes)
{
    if (!settings_valid(changes))
        return EINVAL;
    cJSON *settings = NULL;
    int error = app_settings_load_locked(&settings);
    const cJSON *field;
    cJSON_ArrayForEach(field, changes) {
        if (error)
            break;
        cJSON_DeleteItemFromObjectCaseSensitive(settings, field->string);
        if (!cJSON_AddItemToObject(settings, field->string, cJSON_Duplicate(field, 1)))
            error = ENOMEM;
    }
    if (!error)
        error = save_locked(settings);
    if (!error && (cJSON_GetObjectItemCaseSensitive(changes, "datetime_wm_switch") ||
        cJSON_GetObjectItemCaseSensitive(changes, "datetime_wm_format")))
        error = app_boot_settings_sync(settings);
    cJSON_Delete(settings);
    return error;
}

int app_settings_apply(const cJSON *changes)
{
    if (!settings_valid(changes))
        return EINVAL;
    int descriptor = app_settings_lock(LOCK_EX | LOCK_NB);
    if (descriptor < 0)
        return errno;
    int error = app_settings_apply_locked(changes);
    close(descriptor);
    return error;
}

int app_settings_sync_boot(void)
{
    int descriptor = app_settings_lock(LOCK_EX | LOCK_NB);
    if (descriptor < 0)
        return errno;
    cJSON *settings = NULL;
    int error = app_settings_load_locked(&settings);
    if (!error)
        error = app_boot_settings_sync(settings);
    cJSON_Delete(settings);
    close(descriptor);
    return error;
}

int app_settings_init(void)
{
    cJSON *settings = NULL;
    int error = app_settings_load(&settings);
    char path[PATH_MAX] = "";
    app_settings_path(path, sizeof(path));
    fprintf(stderr, "[settings] active=%s fields=%d errno=%d\n", path, cJSON_GetArraySize(settings), error);
    cJSON_Delete(settings);
    if (!error) {
        int boot_error = app_settings_sync_boot();
        if (boot_error)
            fprintf(stderr, "[settings] boot timestamp cache pending errno=%d; capture continues\n", boot_error);
    }
    return error;
}

int app_settings_show(void)
{
    cJSON *settings = NULL;
    int descriptor = app_settings_lock(LOCK_SH);
    int error = descriptor < 0 ? errno : app_settings_load_locked(&settings);
    char path[PATH_MAX] = "";
    app_settings_path(path, sizeof(path));
    cJSON *status = cJSON_CreateObject();
    if (!status) {
        cJSON_Delete(settings);
        if (descriptor >= 0)
            close(descriptor);
        return ENOMEM;
    }
    cJSON_AddStringToObject(status, "path", path);
    cJSON_AddNumberToObject(status, "errno", error);
    cJSON *boot_status = cJSON_AddObjectToObject(status, "boot_timestamp");
    if (boot_status)
        app_boot_settings_status(boot_status, settings);
    if (descriptor >= 0)
        close(descriptor);
    if (settings)
        cJSON_AddItemToObject(status, "settings", settings);
    char *text = cJSON_Print(status);
    if (text)
        puts(text);
    else
        error = ENOMEM;
    cJSON_free(text);
    cJSON_Delete(status);
    return error;
}
