#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <mntent.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "app_ipcam_wifi_link.h"
#include "app_ipcam_wifi_camera.h"
#include "app_ipcam_speaker.h"
#include "app_ipcam_microphone.h"
#include "app_ipcam_storage.h"
#include "bf01_wifi_link.h"
#include "app_ipcam_event.h"
#include "app_ipcam_upload.h"

#define WIFI_RUN "/run/ipcamera-wifi"
#define WIFI_STATE "/mnt/data/ipcamera-wifi"
#define WIFI_SOCKET WIFI_RUN "/control.sock"
#define WIFI_VERSION "1.0.0"

struct wifi_service {
    struct bf_wifi_sdio sdio;
    struct bf_wifi_link *link;
    unsigned int sequence;
    unsigned int reserved_id;
    unsigned int http_id;
    unsigned int http_attempts;
    unsigned int http_rounds;
    uint64_t http_retry_after;
    uint64_t http_refresh_after;
    int http_pending;
    unsigned int peer_messages;
    int last_error;
    int trace;
    FILE *wire;
    cJSON *status;
    cJSON *dps;
    cJSON *apply_failure;
    cJSON *applied_fields;
    cJSON *deferred_fields;
    char http_result[64];
    cJSON *live_pending;
    pid_t live_camera;
    uint64_t live_start_after;
    uint64_t live_poll_after;
    unsigned int live_generation;
    char live_identity[129];
    cJSON *live_status;
    int live_error;
};

static volatile sig_atomic_t wifi_stopping;

static uint64_t wifi_clock(clockid_t source)
{
    struct timespec timestamp;
    if (clock_gettime(source, &timestamp))
        return 0;
    return (uint64_t)timestamp.tv_sec * 1000 + timestamp.tv_nsec / 1000000;
}

static void wifi_stop(int received)
{
    (void)received;
    wifi_stopping = 1;
}

static int save_text(const char *name, const char *text)
{
    char temporary[PATH_MAX];
    int count = snprintf(temporary, sizeof(temporary), "%s.tmp", name);
    if (count < 0 || (size_t)count >= sizeof(temporary))
        return ENAMETOOLONG;
    int descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0)
        return errno;
    size_t length = strlen(text);
    size_t completed = 0;
    int error = 0;
    while (completed < length) {
        ssize_t written = write(descriptor, text + completed, length - completed);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            error = written ? errno : EIO;
            break;
        }
        completed += (size_t)written;
    }
    if (!error && fsync(descriptor))
        error = errno;
    if (close(descriptor) && !error)
        error = errno;
    if (!error && rename(temporary, name))
        error = errno;
    if (!error) {
        char directory[PATH_MAX];
        snprintf(directory, sizeof(directory), "%s", name);
        char *separator = strrchr(directory, '/');
        if (separator) {
            *separator = 0;
            descriptor = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (descriptor < 0)
                error = errno;
            else {
                if (fsync(descriptor))
                    error = errno;
                close(descriptor);
            }
        }
    }
    if (error)
        unlink(temporary);
    return error;
}

static int reserve_id(struct wifi_service *service, unsigned int identifier)
{
    if (identifier < service->reserved_id)
        return 0;
    if (identifier > INT_MAX - 64U)
        return EOVERFLOW;
    char text[32];
    unsigned int reserved = identifier + 64U;
    snprintf(text, sizeof(text), "%u\n", reserved);
    int error = save_text(WIFI_STATE "/next-id", text);
    if (!error)
        service->reserved_id = reserved;
    return error;
}

static void wire_record(struct wifi_service *service, const char *direction,
                        const char *payload, int error, unsigned int status, uint64_t elapsed)
{
    if (!service->wire) {
        wifi_stopping = 1;
        return;
    }
    cJSON *entry = cJSON_CreateObject();
    if (!entry)
        return;
    cJSON_AddNumberToObject(entry, "sequence", ++service->sequence);
    cJSON_AddNumberToObject(entry, "pid", getpid());
    cJSON_AddNumberToObject(entry, "realtime_ms", (double)wifi_clock(CLOCK_REALTIME));
    cJSON_AddNumberToObject(entry, "monotonic_ms", (double)wifi_clock(CLOCK_MONOTONIC));
    cJSON_AddStringToObject(entry, "case", "production");
    cJSON_AddStringToObject(entry, "event", direction);
    if (payload && service->trace) {
        cJSON_AddStringToObject(entry, "payload", payload);
        cJSON_AddNumberToObject(entry, "length", strlen(payload));
    } else if (payload) {
        cJSON *packet = cJSON_Parse(payload);
        const cJSON *command = cJSON_GetObjectItemCaseSensitive(packet, "cmd");
        const cJSON *identifier = cJSON_GetObjectItemCaseSensitive(packet, "id");
        const cJSON *result = cJSON_GetObjectItemCaseSensitive(packet, "result");
        if (cJSON_IsString(command))
            cJSON_AddStringToObject(entry, "cmd", command->valuestring);
        if (cJSON_IsNumber(identifier))
            cJSON_AddNumberToObject(entry, "id", identifier->valuedouble);
        if (cJSON_IsNumber(result))
            cJSON_AddNumberToObject(entry, "result", result->valuedouble);
        cJSON_Delete(packet);
    }
    cJSON_AddNumberToObject(entry, "errno", error);
    cJSON_AddNumberToObject(entry, "status", status);
    cJSON_AddNumberToObject(entry, "elapsed_ms", (double)elapsed);
    char *encoded = cJSON_PrintUnformatted(entry);
    if (encoded) {
        if (ftell(service->wire) >= 4 * 1024 * 1024) {
            fclose(service->wire);
            rename(WIFI_RUN "/wire.jsonl", WIFI_RUN "/wire.jsonl.1");
            service->wire = fopen(WIFI_RUN "/wire.jsonl", "a");
            if (!service->wire) {
                fprintf(stderr, "[wifi-link] cannot reopen wire log\n");
                cJSON_free(encoded);
                cJSON_Delete(entry);
                wifi_stopping = 1;
                return;
            }
        }
        if (fprintf(service->wire, "%s\n", encoded) < 0 || fflush(service->wire))
            fprintf(stderr, "[wifi-link] wire log write failed: %s\n", strerror(errno));
        cJSON_free(encoded);
    }
    cJSON_Delete(entry);
}

static int wifi_receive(void *context, char *message, size_t capacity)
{
    struct wifi_service *service = context;
    int error = bf_wifi_sdio_receive(&service->sdio, message, capacity);
    if (error != EAGAIN) {
        wire_record(service, error ? "transport_error_0x083B" : "wifi_to_host_0x083B",
                    error ? NULL : message, error, service->sdio.status, 0);
        if (!error)
            service->peer_messages++;
    }
    return error;
}

static int wifi_exchange(void *context, const char *request, char *response, size_t capacity)
{
    struct wifi_service *service = context;
    cJSON *packet = cJSON_Parse(request);
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(packet, "cmd");
    const cJSON *identifier = cJSON_GetObjectItemCaseSensitive(packet, "id");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(packet, "name");
    int error = 0;
    if (cJSON_IsNumber(identifier) && cJSON_IsString(command) && strcmp(command->valuestring, "ack"))
        error = reserve_id(service, (unsigned int)identifier->valueint);
    int refreshing = cJSON_IsString(command) && !strcmp(command->valuestring, "get") &&
        cJSON_IsString(name) && !strcmp(name->valuestring, "http");
    if (refreshing) {
        service->http_id = (unsigned int)identifier->valueint;
        service->http_attempts++;
        snprintf(service->http_result, sizeof(service->http_result), "refreshing");
    }
    cJSON_Delete(packet);
    if (error)
        return error;
    wire_record(service, "host_to_wifi_0x003B", request, 0, 0, 0);
    uint64_t started = wifi_clock(CLOCK_MONOTONIC);
    error = bf_wifi_sdio_exchange(&service->sdio, request, response, capacity);
    wire_record(service, error ? "transport_error_0x003B" : "wifi_to_host_0x003B",
                error ? NULL : response, error, service->sdio.status, wifi_clock(CLOCK_MONOTONIC) - started);
    if (refreshing) {
        cJSON *reply = error ? NULL : cJSON_Parse(response);
        const cJSON *result = cJSON_GetObjectItemCaseSensitive(reply, "result");
        snprintf(service->http_result, sizeof(service->http_result), "%s",
                 error ? "transport_error" : cJSON_IsNumber(result) && result->valueint == 2 ?
                 "wifi_busy_no_credentials" : "reply_received");
        cJSON_Delete(reply);
    }
    return error;
}

static int wifi_apply(void *context, const cJSON *changes, int full)
{
    struct wifi_service *service = context;
    cJSON *failure = cJSON_CreateObject();
    cJSON *fields = cJSON_CreateArray();
    cJSON *field_errors = cJSON_CreateObject();
    if (!failure || !fields || !field_errors) {
        cJSON_Delete(failure);
        cJSON_Delete(fields);
        cJSON_Delete(field_errors);
        return 2;
    }
    cJSON_AddBoolToObject(failure, "full", full);
    cJSON_AddStringToObject(failure, "reason", "settings_not_fully_applied");
    cJSON_AddItemToObject(failure, "fields", fields);
    cJSON_AddItemToObject(failure, "field_errno", field_errors);
    if (!service->applied_fields)
        service->applied_fields = cJSON_CreateObject();
    if (!service->deferred_fields)
        service->deferred_fields = cJSON_CreateObject();
    if (!service->applied_fields || !service->deferred_fields) {
        cJSON_Delete(failure);
        return 2;
    }
    int blocked = 0;
    const cJSON *field;
    cJSON_ArrayForEach(field, changes) {
        int error = 0;
        if (!strcmp(field->string, "image_anti_flicker")) {
            cJSON *request = cJSON_CreateObject();
            cJSON *reply = NULL;
            if (request) {
                cJSON_AddStringToObject(request, "operation", "apply");
                cJSON_AddStringToObject(request, "image_anti_flicker", field->valuestring);
                error = app_ipcam_WifiCamera_Request(request, &reply);
            } else
                error = ENOMEM;
            const cJSON *observed = cJSON_GetObjectItemCaseSensitive(reply, "image_anti_flicker");
            if (!error && (!cJSON_IsString(observed) || strcmp(observed->valuestring, field->valuestring)))
                error = EPROTO;
            cJSON_AddNumberToObject(failure, "image_anti_flicker_errno", error);
            cJSON_Delete(reply);
            cJSON_Delete(request);
        } else if (!strcmp(field->string, "spk_vol")) {
            struct app_speaker_state state;
            error = app_ipcam_Speaker_Apply((unsigned int)field->valueint, &state);
            cJSON_AddNumberToObject(failure, "spk_vol_errno", error);
            if (!error) {
                cJSON_AddNumberToObject(failure, "spk_vol_left_gain", state.left_gain);
                cJSON_AddNumberToObject(failure, "spk_vol_right_gain", state.right_gain);
            }
        } else if (!strcmp(field->string, "mic_switch")) {
            struct app_microphone_state state;
            error = app_ipcam_Microphone_Apply(cJSON_IsTrue(field), &state);
            cJSON_AddNumberToObject(failure, "mic_switch_errno", error);
            if (!error) {
                cJSON_AddBoolToObject(failure, "mic_left_muted", state.left_muted);
                cJSON_AddBoolToObject(failure, "mic_right_muted", state.right_muted);
                cJSON_AddNumberToObject(failure, "mic_left_gain", state.left_gain);
                cJSON_AddNumberToObject(failure, "mic_right_gain", state.right_gain);
            }
        } else if (!strcmp(field->string, "ai_detect_switch") || !strcmp(field->string, "bird_detect_switch") ||
            !strcmp(field->string, "squirrel_detect_switch") || !strcmp(field->string, "other_detect_switch") ||
            !strcmp(field->string, "pir_record_dur")) {
            cJSON *change = cJSON_CreateObject();
            if (!change || !cJSON_AddItemToObject(change, field->string, cJSON_Duplicate(field, 1)))
                error = ENOMEM;
            else
                error = app_event_policy_apply(change);
            cJSON_Delete(change);
        } else if (strcmp(field->string, "status_led_switch"))
            error = ENOSYS;
        if (error) {
            cJSON_AddItemToArray(fields, cJSON_CreateString(field->string));
            cJSON_AddNumberToObject(field_errors, field->string, error);
            cJSON_DeleteItemFromObjectCaseSensitive(service->applied_fields, field->string);
            int deferred = (error == ENOSYS && strcmp(field->string, "memory_format")) ||
                (!strcmp(field->string, "image_anti_flicker") &&
                 (error == ENOENT || error == ECONNREFUSED));
            if (deferred) {
                cJSON_DeleteItemFromObjectCaseSensitive(service->deferred_fields, field->string);
                if (!cJSON_AddItemToObject(service->deferred_fields, field->string, cJSON_Duplicate(field, 1)))
                    blocked = 1;
            } else
                blocked = 1;
        } else {
            cJSON_DeleteItemFromObjectCaseSensitive(service->deferred_fields, field->string);
            cJSON_DeleteItemFromObjectCaseSensitive(service->applied_fields, field->string);
            if (!cJSON_AddItemToObject(service->applied_fields, field->string, cJSON_Duplicate(field, 1))) {
                cJSON_AddItemToArray(fields, cJSON_CreateString(field->string));
                blocked = 1;
            }
        }
    }
    cJSON_AddItemToObject(failure, "applied_fields", cJSON_Duplicate(service->applied_fields, 1));
    cJSON_Delete(service->apply_failure);
    service->apply_failure = failure;
    if (cJSON_GetArraySize(fields)) {
        if (!blocked) {
            cJSON_DeleteItemFromObjectCaseSensitive(failure, "reason");
            cJSON_AddStringToObject(failure, "reason", "accepted_with_deferred_settings");
        }
        cJSON_AddNumberToObject(failure, "ack_result", blocked ? 2 : 0);
        char *encoded = cJSON_PrintUnformatted(failure);
        fprintf(stderr, "[wifi-link] set %s: %s\n", blocked ? "not applied" : "accepted with deferred settings",
                encoded ? encoded : "out of memory");
        cJSON_free(encoded);
        return blocked ? 2 : 0;
    }
    cJSON_Delete(service->apply_failure);
    service->apply_failure = NULL;
    return 0;
}

static int storage_report(cJSON **value)
{
    FILE *mounts = setmntent("/proc/mounts", "r");
    if (!mounts)
        return 2;
    struct mntent entry;
    char buffer[4096];
    int mounted = 0;
    int readonly = 0;
    while (getmntent_r(mounts, &entry, buffer, sizeof(buffer))) {
        if (!strcmp(entry.mnt_dir, APP_STORAGE_MOUNT)) {
            mounted = 1;
            readonly = hasmntopt(&entry, "ro") != NULL;
            break;
        }
    }
    int error = ferror(mounts);
    endmntent(mounts);
    if (error)
        return 2;
    struct statvfs storage = {0};
    int unreadable = mounted && statvfs(APP_STORAGE_MOUNT, &storage);
    double total = 0;
    double used = 0;
    if (mounted && !unreadable) {
        total = ceil((double)storage.f_blocks * storage.f_frsize / 1000000000.0);
        used = floor((double)(storage.f_blocks - storage.f_bfree) * storage.f_frsize / 100000000.0) / 10;
    }
    *value = cJSON_CreateObject();
    cJSON *capacity = cJSON_CreateObject();
    if (!*value || !capacity) {
        cJSON_Delete(*value);
        cJSON_Delete(capacity);
        *value = NULL;
        return 2;
    }
    cJSON_AddNumberToObject(capacity, "total", total);
    cJSON_AddNumberToObject(capacity, "used", used);
    cJSON_AddItemToObject(*value, "memory_capacity", capacity);
    cJSON_AddStringToObject(*value, "memory_status", !mounted ? "unmount" :
                            readonly || unreadable ? "error" : "normal");
    cJSON_AddStringToObject(*value, "memory_format_status", "success");
    cJSON_AddStringToObject(*value, "firmware_version", WIFI_VERSION);
    return 0;
}

static int wifi_snapshot(void *context, const char *name, cJSON **value)
{
    (void)context;
    if (!strcmp(name, "report"))
        return storage_report(value);
    if (!strcmp(name, "status")) {
        cJSON *request = cJSON_CreateObject();
        cJSON *reply = NULL;
        if (!request)
            return 2;
        cJSON_AddStringToObject(request, "operation", "status");
        int error = app_ipcam_WifiCamera_Request(request, &reply);
        cJSON_Delete(request);
        const cJSON *record = cJSON_GetObjectItemCaseSensitive(reply, "record");
        const cJSON *stream = cJSON_GetObjectItemCaseSensitive(reply, "stream");
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(stream, "state");
        if (error || !cJSON_IsString(record) || !cJSON_IsString(state) ||
            (strcmp(state->valuestring, "pushing") && strcmp(state->valuestring, "idle") && strcmp(state->valuestring, "fail")) ||
            storage_report(value)) {
            cJSON_Delete(reply);
            return 2;
        }
        int complete = cJSON_AddStringToObject(*value, "record", record->valuestring) != NULL &&
            cJSON_AddStringToObject(*value, "stream", state->valuestring) != NULL;
        cJSON_Delete(reply);
        return complete ? 0 : 2;
    }
    if (strcmp(name, "stream"))
        return 1;
    cJSON *request = cJSON_CreateObject();
    cJSON *reply = NULL;
    if (!request)
        return 2;
    cJSON_AddStringToObject(request, "operation", "stream-status");
    int error = app_ipcam_WifiCamera_Request(request, &reply);
    cJSON_Delete(request);
    const cJSON *stream = cJSON_GetObjectItemCaseSensitive(reply, "stream");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(stream, "state");
    const cJSON *identity = cJSON_GetObjectItemCaseSensitive(stream, "streamId");
    if (!error && cJSON_IsString(state) && cJSON_IsString(identity) &&
        (!strcmp(state->valuestring, "pushing") || !strcmp(state->valuestring, "idle") || !strcmp(state->valuestring, "fail"))) {
        *value = cJSON_CreateObject();
        if (*value) {
            cJSON_AddStringToObject(*value, "state", state->valuestring);
            cJSON_AddStringToObject(*value, "streamId", identity->valuestring);
        }
    } else
        error = EAGAIN;
    cJSON_Delete(reply);
    return !error && *value ? 0 : 2;
}

static int live_camera_start(struct wifi_service *service)
{
    if (service->live_camera > 0) {
        pid_t finished = waitpid(service->live_camera, NULL, WNOHANG);
        if (!finished)
            return EINPROGRESS;
        if (finished < 0 && errno != ECHILD)
            return errno;
        service->live_camera = 0;
    }
    uint64_t now = wifi_clock(CLOCK_MONOTONIC);
    if (now < service->live_start_after)
        return EINPROGRESS;
    service->live_start_after = now + 5000;
    char executable[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length <= 0 || (size_t)length >= sizeof(executable) - 1)
        return EIO;
    executable[length] = 0;
    char parameters[PATH_MAX];
    strcpy(parameters, executable);
    char *name = strrchr(parameters, '/');
    if (!name || (size_t)(name - parameters) + sizeof("/param_config.ini") > sizeof(parameters))
        return ENAMETOOLONG;
    strcpy(name, "/param_config.ini");
    pid_t child = fork();
    if (child < 0)
        return errno;
    if (!child) {
        sigset_t signals;
        sigemptyset(&signals);
        sigprocmask(SIG_SETMASK, &signals, NULL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        int log = open(WIFI_RUN "/live-camera.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (log >= 0) {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
            close(log);
        }
        for (int descriptor = 3; descriptor < 1024; descriptor++)
            close(descriptor);
        execl(executable, executable, "--live-camera", "-i", parameters, (char *)NULL);
        _exit(127);
    }
    service->live_camera = child;
    fprintf(stderr, "[wifi-link] requested dedicated live camera pid=%ld; boot capture policy unchanged\n", (long)child);
    return EINPROGRESS;
}

static int live_apply(struct wifi_service *service, const cJSON *request)
{
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(request, "action");
    const cJSON *expiry = cJSON_GetObjectItemCaseSensitive(request, "expireTime");
    if (cJSON_IsString(action) && !strcmp(action->valuestring, "start") &&
        (!cJSON_IsNumber(expiry) || expiry->valuedouble <= (double)wifi_clock(CLOCK_REALTIME)))
        return ESTALE;
    cJSON *command = cJSON_Duplicate(request, 1);
    cJSON *reply = NULL;
    if (!command)
        return ENOMEM;
    cJSON_AddStringToObject(command, "operation", "stream");
    int error = app_ipcam_WifiCamera_Request(command, &reply);
    cJSON_Delete(command);
    cJSON_Delete(reply);
    if ((error == ENOENT || error == ECONNREFUSED) && cJSON_IsString(action) && !strcmp(action->valuestring, "start"))
        error = live_camera_start(service);
    return error;
}

static int wifi_stream(void *context, const cJSON *request)
{
    struct wifi_service *service = context;
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(request, "action");
    const cJSON *protocol = cJSON_GetObjectItemCaseSensitive(request, "pushProtocol");
    if (cJSON_IsString(action) && !strcmp(action->valuestring, "start") &&
        (!cJSON_IsString(protocol) || strcmp(protocol->valuestring, "RTSP")))
        return 1;
    if (cJSON_IsString(action) && !strcmp(action->valuestring, "start") && app_event_set_live(1))
        return 2;
    int error = live_apply(service, request);
    int busy = error == EINPROGRESS || error == EBUSY || error == ENOENT || error == ECONNREFUSED || error == ETIMEDOUT;
    if (busy) {
        cJSON *copy = cJSON_Duplicate(request, 1);
        if (copy) {
            cJSON_Delete(service->live_pending);
            service->live_pending = copy;
        }
    } else {
        cJSON_Delete(service->live_pending);
        service->live_pending = NULL;
    }
    fprintf(stderr, "[wifi-link] live action=%s errno=%d ack=%d\n",
        cJSON_IsString(action) ? action->valuestring : "", error, !error ? 0 : busy ? 2 : 1);
    return !error ? 0 : busy ? 2 : 1;
}

static void live_poll(struct wifi_service *service, uint64_t now)
{
    if (now < service->live_poll_after)
        return;
    service->live_poll_after = now + 2000;
    if (service->live_pending) {
        int error = live_apply(service, service->live_pending);
        if (!error || (error != EINPROGRESS && error != EBUSY && error != ENOENT &&
                error != ECONNREFUSED && error != ETIMEDOUT)) {
            cJSON_Delete(service->live_pending);
            service->live_pending = NULL;
        }
    }
    cJSON *query = cJSON_CreateObject();
    cJSON *reply = NULL;
    if (!query)
        return;
    cJSON_AddStringToObject(query, "operation", "stream-status");
    int error = app_ipcam_WifiCamera_Request(query, &reply);
    cJSON_Delete(query);
    const cJSON *stream = cJSON_GetObjectItemCaseSensitive(reply, "stream");
    service->live_error = error;
    if (!error && cJSON_IsObject(stream)) {
        cJSON *copy = cJSON_Duplicate(stream, 1);
        if (copy) {
            cJSON_Delete(service->live_status);
            service->live_status = copy;
        }
    }
    const cJSON *identity = cJSON_GetObjectItemCaseSensitive(stream, "streamId");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(stream, "state");
    const cJSON *generation = cJSON_GetObjectItemCaseSensitive(stream, "generation");
    if (!error && cJSON_IsString(identity) && *identity->valuestring && strlen(identity->valuestring) < sizeof(service->live_identity) &&
        cJSON_IsString(state) && cJSON_IsNumber(generation) &&
        (!strcmp(state->valuestring, "pushing") || !strcmp(state->valuestring, "idle") || !strcmp(state->valuestring, "fail")) &&
        (strcmp(service->live_identity, identity->valuestring) || service->live_generation != (unsigned int)generation->valueint)) {
        cJSON *report = cJSON_CreateObject();
        cJSON_AddStringToObject(report, "cmd", "stream");
        cJSON_AddStringToObject(report, "action", "state");
        cJSON_AddStringToObject(report, "streamId", identity->valuestring);
        cJSON_AddStringToObject(report, "state", state->valuestring);
        error = bf_wifi_enqueue(service->link, report);
        cJSON_Delete(report);
        if (!error) {
            strcpy(service->live_identity, identity->valuestring);
            service->live_generation = (unsigned int)generation->valueint;
        }
    }
    cJSON_Delete(reply);
}

static void wifi_completed(void *context, const cJSON *request, const cJSON *reply)
{
    app_upload_completed(request, reply);
    struct wifi_service *service = context;
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(request, "cmd");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(request, "name");
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(reply, "result");
    if (!cJSON_IsString(command) || strcmp(command->valuestring, "get") || !cJSON_IsString(name))
        return;
    cJSON **destination = !strcmp(name->valuestring, "status") ? &service->status :
                          !strcmp(name->valuestring, "dps") ? &service->dps : NULL;
    if (destination && cJSON_IsNumber(result) && !result->valueint) {
        cJSON *copy = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(reply, name->valuestring), 1);
        if (copy) {
            cJSON_Delete(*destination);
            *destination = copy;
        }
    }
    if (!strcmp(name->valuestring, "http")) {
        service->http_pending = 0;
        service->http_retry_after = 0;
        service->http_refresh_after = wifi_clock(CLOCK_MONOTONIC) + 60000;
        if (cJSON_IsNumber(result) && result->valueint == 2 && service->http_rounds < 3) {
            service->http_pending = 1;
            service->http_retry_after = wifi_clock(CLOCK_MONOTONIC) + 30000;
            snprintf(service->http_result, sizeof(service->http_result), "waiting_busy_retry_same_id");
            return;
        }
        snprintf(service->http_result, sizeof(service->http_result), "%s",
                 !reply ? "request_timed_out" : cJSON_IsNumber(result) && !result->valueint ?
                 "received_valid_credentials" : cJSON_IsNumber(result) && result->valueint == 2 ?
                 "wifi_busy_after_three_requests" : "wifi_rejected");
    }
}

static int enqueue_get(struct wifi_service *service, const char *name)
{
    cJSON *request = cJSON_CreateObject();
    if (!request)
        return ENOMEM;
    cJSON_AddStringToObject(request, "cmd", "get");
    cJSON_AddStringToObject(request, "name", name);
    int error = bf_wifi_enqueue(service->link, request);
    cJSON_Delete(request);
    return error;
}

static cJSON *service_status(struct wifi_service *service)
{
    cJSON *status = cJSON_CreateObject();
    if (!status)
        return NULL;
    cJSON_AddNumberToObject(status, "pid", getpid());
    cJSON_AddStringToObject(status, "implementation", "ipcamera-production-sdio-v1");
    cJSON_AddStringToObject(status, "firmware_version", WIFI_VERSION);
    cJSON_AddBoolToObject(status, "sdio_open", service->sdio.descriptor >= 0);
    cJSON_AddNumberToObject(status, "last_error", service->last_error);
    cJSON_AddNumberToObject(status, "peer_messages", service->peer_messages);
    cJSON_AddNumberToObject(status, "http_request_id", service->http_id);
    cJSON_AddNumberToObject(status, "http_attempts", service->http_attempts);
    cJSON_AddNumberToObject(status, "http_rounds", service->http_rounds);
    cJSON_AddStringToObject(status, "http_refresh", service->http_result);
    cJSON_AddBoolToObject(status, "http_usable", bf_wifi_http(service->link, wifi_clock(CLOCK_REALTIME)) != NULL);
    cJSON_AddBoolToObject(status, "http_refresh_due", bf_wifi_http_refresh_due(service->link, wifi_clock(CLOCK_REALTIME)));
    cJSON_AddBoolToObject(status, "clock_valid", wifi_clock(CLOCK_REALTIME) >= 1000000000000ULL);
    if (service->status)
        cJSON_AddItemToObject(status, "wifi", cJSON_Duplicate(service->status, 1));
    if (service->dps)
        cJSON_AddItemToObject(status, "wifi_saved_dps", cJSON_Duplicate(service->dps, 1));
    cJSON_AddStringToObject(status, "settings_backend", "persistent_event_policy_and_status_led_speaker_microphone_camera_partial");
    cJSON_AddStringToObject(status, "settings_ack_policy", "accept_unimplemented_settings");
    if (service->applied_fields)
        cJSON_AddItemToObject(status, "host_applied_fields", cJSON_Duplicate(service->applied_fields, 1));
    if (service->deferred_fields)
        cJSON_AddItemToObject(status, "host_deferred_fields", cJSON_Duplicate(service->deferred_fields, 1));
    if (service->apply_failure)
        cJSON_AddItemToObject(status, "set_not_applied", cJSON_Duplicate(service->apply_failure, 1));
    if (service->live_status)
        cJSON_AddItemToObject(status, "live", cJSON_Duplicate(service->live_status, 1));
    cJSON_AddNumberToObject(status, "live_control_errno", service->live_error);
    cJSON_AddBoolToObject(status, "live_command_pending", service->live_pending != NULL);
    app_upload_status(status);
    return status;
}

static cJSON *filter_status(struct wifi_service *service, int *error)
{
    struct bf_wifi_filter filter;
    *error = bf_wifi_sdio_filter(&service->sdio, &filter);
    cJSON *value = cJSON_CreateObject();
    cJSON *sources = cJSON_CreateArray();
    cJSON *destinations = cJSON_CreateArray();
    if (!value || !sources || !destinations) {
        cJSON_Delete(value);
        cJSON_Delete(sources);
        cJSON_Delete(destinations);
        *error = ENOMEM;
        return NULL;
    }
    cJSON_AddNumberToObject(value, "status", filter.status);
    cJSON_AddNumberToObject(value, "flags", filter.flags);
    cJSON_AddNumberToObject(value, "source_count", filter.source_count);
    cJSON_AddNumberToObject(value, "destination_count", filter.destination_count);
    cJSON_AddItemToObject(value, "source_ports", sources);
    cJSON_AddItemToObject(value, "destination_ports", destinations);
    char raw[sizeof(filter) * 2 + 1];
    const unsigned char *bytes = (const unsigned char *)&filter;
    for (size_t index = 0; index < sizeof(filter); index++)
        snprintf(raw + index * 2, sizeof(raw) - index * 2, "%02x", bytes[index]);
    cJSON_AddStringToObject(value, "raw_hex", raw);
    if (!*error) {
        for (unsigned int index = 0; index < filter.source_count; index++)
            cJSON_AddItemToArray(sources, cJSON_CreateNumber(ntohs(filter.source_ports[index])));
        for (unsigned int index = 0; index < filter.destination_count; index++)
            cJSON_AddItemToArray(destinations, cJSON_CreateNumber(ntohs(filter.destination_ports[index])));
    }
    return value;
}

static void serve_control(struct wifi_service *service, int listener)
{
    int descriptor = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (descriptor < 0)
        return;
    struct ucred credentials;
    socklen_t size = sizeof(credentials);
    if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &size) || credentials.uid != 0) {
        close(descriptor);
        return;
    }
    struct pollfd pending = { .fd = descriptor, .events = POLLIN };
    char operation[64] = {0};
    ssize_t length = poll(&pending, 1, 500) > 0 ? recv(descriptor, operation, sizeof(operation) - 1, 0) : -1;
    if (length <= 0) {
        close(descriptor);
        return;
    }
    int error = 0;
    int queued = 0;
    if (!strncmp(operation, "get:", 4)) {
        const char *name = operation + 4;
        error = !name[0] || strlen(name) > 16 || !strcmp(name, "http") ? EINVAL :
                service->sdio.descriptor < 0 ? ENODEV : enqueue_get(service, name);
        queued = !error;
    } else if (!strcmp(operation, "refresh-http")) {
        error = service->sdio.descriptor < 0 ? ENODEV :
                service->http_pending ? EBUSY : enqueue_get(service, "http");
        queued = !error;
        if (queued) {
            service->http_pending = 1;
            service->http_attempts = 0;
            service->http_rounds = 1;
            service->http_retry_after = 0;
            snprintf(service->http_result, sizeof(service->http_result), "queued");
        }
    } else if (strcmp(operation, "status") && strcmp(operation, "filter") &&
               strcmp(operation, "camera-status") && strcmp(operation, "module-time") &&
               strcmp(operation, "speaker-status") && strcmp(operation, "microphone-status"))
        error = EINVAL;
    cJSON *reply = service_status(service);
    if (reply) {
        if (!strncmp(operation, "get:", 4))
            cJSON_AddStringToObject(reply, "queued_get_name", operation + 4);
        if (!strcmp(operation, "microphone-status")) {
            struct app_microphone_state state;
            error = app_ipcam_Microphone_Status(&state);
            cJSON *microphone = cJSON_CreateObject();
            if (microphone) {
                cJSON_AddNumberToObject(microphone, "errno", error);
                if (!error) {
                    cJSON_AddNumberToObject(microphone, "left_gain", state.left_gain);
                    cJSON_AddNumberToObject(microphone, "right_gain", state.right_gain);
                    cJSON_AddBoolToObject(microphone, "left_muted", state.left_muted);
                    cJSON_AddBoolToObject(microphone, "right_muted", state.right_muted);
                    cJSON_AddBoolToObject(microphone, "configured", state.configured);
                    if (state.configured)
                        cJSON_AddBoolToObject(microphone, "enabled", state.enabled);
                    cJSON_AddStringToObject(microphone, "recording_policy", "silence_before_aac_when_disabled");
                }
                cJSON_AddItemToObject(reply, "microphone", microphone);
            } else
                error = ENOMEM;
        }
        if (!strcmp(operation, "speaker-status")) {
            struct app_speaker_state state;
            error = app_ipcam_Speaker_Status(&state);
            cJSON *speaker = cJSON_CreateObject();
            if (speaker) {
                cJSON_AddNumberToObject(speaker, "errno", error);
                if (!error) {
                    cJSON_AddNumberToObject(speaker, "left_gain", state.left_gain);
                    cJSON_AddNumberToObject(speaker, "right_gain", state.right_gain);
                    cJSON_AddBoolToObject(speaker, "left_muted", state.left_muted);
                    cJSON_AddBoolToObject(speaker, "right_muted", state.right_muted);
                    cJSON_AddBoolToObject(speaker, "configured", state.configured);
                    if (state.configured)
                        cJSON_AddNumberToObject(speaker, "percent", state.percent);
                }
                cJSON_AddItemToObject(reply, "speaker", speaker);
            } else
                error = ENOMEM;
        }
        if (!strcmp(operation, "module-time")) {
            struct bf_wifi_time module_time;
            uint64_t started = wifi_clock(CLOCK_MONOTONIC);
            error = bf_wifi_sdio_time(&service->sdio, &module_time);
            cJSON *value = cJSON_CreateObject();
            if (value) {
                cJSON_AddNumberToObject(value, "elapsed_ms", wifi_clock(CLOCK_MONOTONIC) - started);
                cJSON_AddNumberToObject(value, "native_ioctl", 35);
                cJSON_AddNumberToObject(value, "wsm_request", 0x002B);
                cJSON_AddStringToObject(value, "timezone", "unspecified_by_native_interface");
                cJSON_AddBoolToObject(value, "response_copied", !error);
                if (!error) {
                    cJSON_AddNumberToObject(value, "status", module_time.status);
                    cJSON_AddNumberToObject(value, "tm_year", module_time.year);
                    cJSON_AddNumberToObject(value, "tm_mon", module_time.month);
                    cJSON_AddNumberToObject(value, "tm_mday", module_time.day);
                    cJSON_AddNumberToObject(value, "tm_hour", module_time.hour);
                    cJSON_AddNumberToObject(value, "tm_min", module_time.minute);
                    cJSON_AddNumberToObject(value, "tm_sec", module_time.second);
                    cJSON_AddNumberToObject(value, "tm_wday", module_time.weekday);
                    cJSON_AddNumberToObject(value, "tm_yday", module_time.yearday);
                    cJSON_AddNumberToObject(value, "tm_isdst", module_time.daylight);
                    char raw[sizeof(module_time) * 2 + 1];
                    const unsigned char *bytes = (const unsigned char *)&module_time;
                    for (size_t index = 0; index < sizeof(module_time); index++)
                        snprintf(raw + index * 2, sizeof(raw) - index * 2, "%02x", bytes[index]);
                    cJSON_AddStringToObject(value, "ioctl_result_hex", raw);
                }
                cJSON_AddBoolToObject(value, "host_clock_modified", 0);
                cJSON_AddItemToObject(reply, "module_time", value);
            } else
                error = ENOMEM;
        }
        if (!strcmp(operation, "camera-status")) {
            cJSON *request = cJSON_CreateObject();
            cJSON *camera = NULL;
            if (request) {
                cJSON_AddStringToObject(request, "operation", "status");
                error = app_ipcam_WifiCamera_Request(request, &camera);
            } else
                error = ENOMEM;
            if (camera)
                cJSON_AddItemToObject(reply, "camera", camera);
            cJSON_Delete(request);
        }
        if (!strcmp(operation, "filter")) {
            cJSON *filter = filter_status(service, &error);
            if (filter)
                cJSON_AddItemToObject(reply, "tcp_filter", filter);
        }
        cJSON_AddNumberToObject(reply, "control_errno", error);
        cJSON_AddBoolToObject(reply, "queued", queued);
        char *encoded = cJSON_PrintUnformatted(reply);
        if (encoded) {
            send(descriptor, encoded, strlen(encoded), MSG_NOSIGNAL);
            cJSON_free(encoded);
        }
        cJSON_Delete(reply);
    }
    close(descriptor);
}

int app_ipcam_Wifi_Control(const char *operation)
{
    int descriptor = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (descriptor < 0)
        return EXIT_FAILURE;
    struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = WIFI_SOCKET };
    int error = connect(descriptor, (struct sockaddr *)&address, sizeof(address));
    if (!error && send(descriptor, operation, strlen(operation), MSG_NOSIGNAL) < 0)
        error = -1;
    struct pollfd pending = { .fd = descriptor, .events = POLLIN };
    char response[8192];
    int timeout = !strcmp(operation, "module-time") ? 65000 : 20000;
    ssize_t length = !error && poll(&pending, 1, timeout) > 0 ? recv(descriptor, response, sizeof(response) - 1, 0) : -1;
    if (length > 0) {
        response[length] = 0;
        puts(response);
        cJSON *reply = cJSON_Parse(response);
        const cJSON *result = cJSON_GetObjectItemCaseSensitive(reply, "control_errno");
        error = !cJSON_IsNumber(result) || result->valueint;
        cJSON_Delete(reply);
    } else {
        fprintf(stderr, "[wifi-link] control unavailable: %s\n", error ? strerror(errno) : "no response");
        error = 1;
    }
    close(descriptor);
    return error ? EXIT_FAILURE : EXIT_SUCCESS;
}

int app_ipcam_Wifi_Service(void)
{
    umask(0077);
    if (geteuid() != 0 || (mkdir(WIFI_RUN, 0700) && errno != EEXIST) ||
        (mkdir(WIFI_STATE, 0700) && errno != EEXIST))
        return EXIT_FAILURE;
    int lock = open(WIFI_RUN "/service.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        fprintf(stderr, "[wifi-link] another production consumer owns the service\n");
        if (lock >= 0)
            close(lock);
        return EXIT_FAILURE;
    }
    struct wifi_service service = { .sdio = { .descriptor = -1 } };
    service.trace = getenv("BF01_WIFI_TRACE") && !strcmp(getenv("BF01_WIFI_TRACE"), "1");
    service.wire = fopen(WIFI_RUN "/wire.jsonl", "a");
    if (!service.wire) {
        close(lock);
        return EXIT_FAILURE;
    }
    struct bf_wifi_ops operations = { .receive = wifi_receive, .exchange = wifi_exchange,
        .apply = wifi_apply, .snapshot = wifi_snapshot, .stream = wifi_stream, .completed = wifi_completed };
    service.link = bf_wifi_create(&operations, &service);
    if (!service.link) {
        fclose(service.wire);
        close(lock);
        return EXIT_FAILURE;
    }
    FILE *counter = fopen(WIFI_STATE "/next-id", "r");
    unsigned int next_id = 1;
    if (counter) {
        if (fscanf(counter, "%u", &next_id) != 1 || !next_id || next_id > INT_MAX - 64U)
            next_id = 0;
        fclose(counter);
    }
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = WIFI_SOCKET };
    unlink(WIFI_SOCKET);
    int error = bf_wifi_resume_id(service.link, next_id);
    if (!error && (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 4)))
        error = errno;
    if (!error) {
        char pid[32];
        snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
        error = save_text(WIFI_RUN "/service.pid", pid);
    }
    if (error) {
        fprintf(stderr, "[wifi-link] startup failed: %s\n", strerror(error));
    } else {
        struct sigaction action = { .sa_handler = wifi_stop };
        sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGINT, &action, NULL);
        signal(SIGPIPE, SIG_IGN);
        wifi_stopping = 0;
        fprintf(stderr, "[wifi-link] production SDIO service pid=%ld trace=%d, camera gate unchanged\n",
                (long)getpid(), service.trace);
        uint64_t retry_after = 0;
        uint64_t status_after = 0;
        uint64_t transport_retry_after = 0;
        int upload_start_error = app_upload_start();
        if (upload_start_error)
            fprintf(stderr, "[upload] worker unavailable errno=%d\n", upload_start_error);
        while (!wifi_stopping) {
            uint64_t now = wifi_clock(CLOCK_MONOTONIC);
            if (service.sdio.descriptor < 0 && now >= retry_after) {
                service.sdio.descriptor = access("/sys/class/net/wlan0/device/driver", F_OK) ? -1 :
                    open("/dev/atbm_ioctl", O_RDWR | O_CLOEXEC);
                if (service.sdio.descriptor < 0) {
                    service.last_error = errno;
                    retry_after = now + 3000;
                } else {
                    service.last_error = 0;
                    enqueue_get(&service, "status");
                    enqueue_get(&service, "dps");
                    status_after = now + 60000;
                }
            }
            app_upload_poll(service.link);
            if (service.sdio.descriptor >= 0) {
                live_poll(&service, now);
                if (service.http_retry_after && now >= service.http_retry_after) {
                    int retry_error = bf_wifi_retry_http(service.link);
                    if (!retry_error) {
                        service.http_retry_after = 0;
                        service.http_rounds++;
                        snprintf(service.http_result, sizeof(service.http_result), "queued_busy_retry_same_id");
                    } else if (retry_error == ENOENT) {
                        service.http_retry_after = 0;
                        service.http_pending = 0;
                        service.http_refresh_after = now + 60000;
                        snprintf(service.http_result, sizeof(service.http_result), "busy_retry_canceled_by_resync");
                    }
                }
                if ((app_upload_refresh_needed() || bf_wifi_http_refresh_due(service.link, wifi_clock(CLOCK_REALTIME))) && !service.http_pending &&
                    now >= service.http_refresh_after &&
                    !enqueue_get(&service, "http")) {
                    service.http_pending = 1;
                    app_upload_refresh_started();
                    service.http_attempts = 0;
                    service.http_rounds = 1;
                    snprintf(service.http_result, sizeof(service.http_result), "queued_expiry_refresh");
                }
                if (now >= status_after) {
                    enqueue_get(&service, "status");
                    status_after = now + 60000;
                }
                int result = now >= transport_retry_after ?
                    bf_wifi_step(service.link, now, wifi_clock(CLOCK_REALTIME)) : EAGAIN;
                if (result && result != EAGAIN) {
                    if (result != service.last_error)
                        fprintf(stderr, "[wifi-link] step error=%d (%s)\n", result, strerror(result));
                    service.last_error = result;
                    if (result == EPERM || result == ESRCH || result == ETIMEDOUT)
                        transport_retry_after = wifi_clock(CLOCK_MONOTONIC) + 3000;
                    if (result == ENODEV || result == EBADF || result == ENXIO) {
                        close(service.sdio.descriptor);
                        service.sdio.descriptor = -1;
                        bf_wifi_reset(service.link);
                        service.http_pending = 0;
                        service.http_retry_after = 0;
                        retry_after = now + 3000;
                    }
                } else if (!result)
                    service.last_error = 0;
            }
            struct pollfd control = { .fd = listener, .events = POLLIN };
            if (poll(&control, 1, 50) > 0 && control.revents & POLLIN)
                serve_control(&service, listener);
        }
        app_upload_stop();
    }
    if (service.sdio.descriptor >= 0)
        close(service.sdio.descriptor);
    if (listener >= 0)
        close(listener);
    unlink(WIFI_SOCKET);
    unlink(WIFI_RUN "/service.pid");
    cJSON_Delete(service.status);
    cJSON_Delete(service.dps);
    cJSON_Delete(service.apply_failure);
    cJSON_Delete(service.applied_fields);
    cJSON_Delete(service.deferred_fields);
    cJSON_Delete(service.live_pending);
    cJSON_Delete(service.live_status);
    bf_wifi_destroy(service.link);
    if (service.wire)
        fclose(service.wire);
    close(lock);
    return error ? EXIT_FAILURE : EXIT_SUCCESS;
}
