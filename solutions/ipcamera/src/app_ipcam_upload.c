#define _GNU_SOURCE
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include "app_ipcam_event.h"
#include "app_ipcam_upload.h"
#include "bf01_cloud.h"
#include "minIni.h"

static pthread_mutex_t upload_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t upload_condition = PTHREAD_COND_INITIALIZER;
static pthread_t upload_thread;
static int upload_running;
static int upload_stopping;
static cJSON *upload_http;
static cJSON *upload_notice;
static unsigned int upload_notice_id;
static int upload_notice_result;
static int upload_refresh;
static int upload_error;
static char upload_state[64] = "idle";
static char upload_event[17];
static char upload_purpose[32];
static char rejected_token[1025];
static char upload_boot_id[64];
static int upload_boot_valid;

static int load_upload_boot_id(void)
{
    FILE *input = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (!input)
        return errno;
    int read_ok = fgets(upload_boot_id, sizeof(upload_boot_id), input) != NULL;
    int error = read_ok ? 0 : (ferror(input) ? errno : EIO);
    fclose(input);
    upload_boot_id[strcspn(upload_boot_id, "\r\n")] = 0;
    if (!error && !*upload_boot_id)
        error = EPROTO;
    upload_boot_valid = !error;
    if (error)
        upload_boot_id[0] = 0;
    return error;
}

static int current_boot_job(const cJSON *job)
{
    return upload_boot_valid && !strcmp(app_event_text(job, "bootId"), upload_boot_id);
}

static void set_string(cJSON *object, const char *name, const char *value)
{
    cJSON_DeleteItemFromObjectCaseSensitive(object, name);
    cJSON_AddStringToObject(object, name, value);
}

static void set_number(cJSON *object, const char *name, double value)
{
    cJSON_DeleteItemFromObjectCaseSensitive(object, name);
    cJSON_AddNumberToObject(object, name, value);
}

static int stopping(void)
{
    pthread_mutex_lock(&upload_mutex);
    int stopped = upload_stopping;
    pthread_mutex_unlock(&upload_mutex);
    return stopped;
}

static int cancelled(void)
{
    return stopping() || app_event_live();
}

static void status(const cJSON *job, const char *state, int error)
{
    pthread_mutex_lock(&upload_mutex);
    snprintf(upload_state, sizeof(upload_state), "%s", state);
    snprintf(upload_event, sizeof(upload_event), "%s", app_event_text(job, "eventId"));
    snprintf(upload_purpose, sizeof(upload_purpose), "%s", app_event_text(job, "purpose"));
    upload_error = error;
    pthread_mutex_unlock(&upload_mutex);
    fprintf(stderr, "[upload] event=%s purpose=%s state=%s errno=%d\n",
        app_event_text(job, "eventId"), app_event_text(job, "purpose"), state, error);
}

static cJSON *credentials(void)
{
    pthread_mutex_lock(&upload_mutex);
    const cJSON *expiry = cJSON_GetObjectItemCaseSensitive(upload_http, "expireTime");
    cJSON *copy = cJSON_IsNumber(expiry) && expiry->valuedouble > (double)app_event_realtime() + 30000 ?
        cJSON_Duplicate(upload_http, 1) : NULL;
    pthread_mutex_unlock(&upload_mutex);
    return copy;
}

static void refresh_credentials(const cJSON *failed)
{
    pthread_mutex_lock(&upload_mutex);
    upload_refresh = 1;
    snprintf(rejected_token, sizeof(rejected_token), "%s", app_event_text(failed, "accessToken"));
    if (!strcmp(rejected_token, app_event_text(upload_http, "accessToken"))) {
        cJSON_Delete(upload_http);
        upload_http = NULL;
    }
    pthread_mutex_unlock(&upload_mutex);
}

static int notify(const cJSON *job, const char *phase, int result)
{
    cJSON *notice = cJSON_CreateObject();
    if (!notice)
        return ENOMEM;
    cJSON_AddStringToObject(notice, "cmd", "file");
    cJSON_AddStringToObject(notice, "phase", phase);
    cJSON_AddStringToObject(notice, "purpose", app_event_text(job, "purpose"));
    cJSON_AddStringToObject(notice, "eventId", app_event_text(job, "eventId"));
    cJSON_AddStringToObject(notice, "detectType", app_event_text(job, "detectType"));
    const cJSON *prepared = cJSON_GetObjectItemCaseSensitive(job, "prepared");
    cJSON_AddStringToObject(notice, "fileId", app_event_text(prepared, "fileId"));
    cJSON_AddNumberToObject(notice, "result", result);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 20;
    pthread_mutex_lock(&upload_mutex);
    cJSON_Delete(upload_notice);
    upload_notice = notice;
    upload_notice_id = 0;
    upload_notice_result = EINPROGRESS;
    while (!upload_stopping && upload_notice_result == EINPROGRESS) {
        int error = pthread_cond_timedwait(&upload_condition, &upload_mutex, &deadline);
        if (error == ETIMEDOUT)
            break;
    }
    int error = upload_stopping ? ECANCELED : upload_notice_result == EINPROGRESS ? ETIMEDOUT : upload_notice_result;
    cJSON_Delete(upload_notice);
    upload_notice = NULL;
    upload_notice_id = 0;
    pthread_mutex_unlock(&upload_mutex);
    return error;
}

static int resolve_detection(cJSON *job)
{
    if (*app_event_text(job, "detectType"))
        return 0;
    const cJSON *ai = cJSON_GetObjectItemCaseSensitive(job, "ai");
    int bypassed = !strcmp(app_event_text(ai, "state"), "bypassed");
    char label[32] = "other";
    if (!bypassed) {
        if (strcmp(app_event_text(ai, "state"), "passed"))
            return EPERM;
        const cJSON *identity = cJSON_GetObjectItemCaseSensitive(ai, "classId");
        if (!cJSON_IsNumber(identity) || (identity->valueint != 0 && identity->valueint != 1))
            return EPROTO;
        char key[32], config[PATH_MAX];
        snprintf(key, sizeof(key), "class_%d", identity->valueint);
        int config_error = app_event_config_path("cloud.ini", config, sizeof(config));
        if (config_error && config_error != ENODEV)
            return config_error;
        if (config_error || access(config, F_OK)) {
            if (!config_error && errno != ENOENT)
                return errno;
            ssize_t length = readlink("/proc/self/exe", config, sizeof(config) - 1);
            if (length <= 0)
                return EIO;
            config[length] = 0;
            char *name = strrchr(config, '/');
            if (!name || (size_t)(name - config) + sizeof("/cloud.ini") > sizeof(config))
                return EOVERFLOW;
            strcpy(name, "/cloud.ini");
        }
        ini_gets("cloud", key, "", label, sizeof(label), config);
        if (strcmp(label, "bird") && strcmp(label, "squirrel") && strcmp(label, "other"))
            return ENOKEY;
    }
    const char *alarm_key = !strcmp(label, "bird") ? "birdAlarm" : !strcmp(label, "squirrel") ? "squirrelAlarm" : "otherAlarm";
    int alarm = !bypassed && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job, alarm_key));
    set_string(job, "detectType", label);
    cJSON_AddBoolToObject(job, "alarm", alarm);
    return 0;
}

static int resolve_time(cJSON *job)
{
    const cJSON *event_time = cJSON_GetObjectItemCaseSensitive(job, "eventTime");
    if (cJSON_IsNumber(event_time) && event_time->valuedouble >= 1577836800000.0)
        return 0;
    if (app_event_realtime() < 1577836800000ULL)
        return ENODATA;
    char boot_id[64] = {0};
    FILE *input = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (!input)
        return errno;
    int read_ok = fgets(boot_id, sizeof(boot_id), input) != NULL;
    fclose(input);
    boot_id[strcspn(boot_id, "\r\n")] = 0;
    if (!read_ok || strcmp(boot_id, app_event_text(job, "bootId")))
        return ESTALE;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t uptime = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    const cJSON *captured = cJSON_GetObjectItemCaseSensitive(job, "captureUptimeMs");
    if (!cJSON_IsNumber(captured) || captured->valuedouble < 0 || captured->valuedouble > (double)uptime)
        return EPROTO;
    set_number(job, "eventTime", (double)(app_event_realtime() - uptime) + captured->valuedouble);
    return 0;
}

static int post(cJSON *job, const char *endpoint, const cJSON *request, cJSON **reply)
{
    cJSON *http = credentials();
    if (!http)
        return ENOKEY;
    int http_status = 0;
    int error = bf_cloud_post(http, endpoint, request, reply, &http_status);
    set_number(job, "lastHttpStatus", http_status);
    if (http_status == 401)
        refresh_credentials(http);
    cJSON_Delete(http);
    return error;
}

static int response_success(const cJSON *reply)
{
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(reply, "code");
    return (cJSON_IsNumber(code) && code->valuedouble == 200) ||
        (cJSON_IsString(code) && !strcmp(code->valuestring, "200"));
}

static int prepare(cJSON *job)
{
    cJSON *request = cJSON_CreateObject(), *files = cJSON_CreateArray(), *file = cJSON_CreateObject(), *reply = NULL;
    if (!request || !files || !file) {
        cJSON_Delete(request);
        cJSON_Delete(files);
        cJSON_Delete(file);
        return ENOMEM;
    }
    cJSON_AddStringToObject(request, "purpose", app_event_text(job, "purpose"));
    cJSON_AddStringToObject(file, "fileType", app_event_text(job, "fileType"));
    cJSON_AddItemToArray(files, file);
    cJSON_AddItemToObject(request, "files", files);
    int error = post(job, "/api/device-api/files/uploads/prepare", request, &reply);
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(reply, "code");
    const cJSON *payload = cJSON_HasObjectItem(reply, "files") ? reply : cJSON_GetObjectItemCaseSensitive(reply, "data");
    const cJSON *returned = cJSON_GetObjectItemCaseSensitive(payload, "files");
    const cJSON *prepared = cJSON_GetArrayItem(returned, 0);
    const cJSON *expiry = cJSON_GetObjectItemCaseSensitive(prepared, "expireTime");
    if (!error && ((code && !response_success(reply)) ||
        !cJSON_IsArray(returned) || cJSON_GetArraySize(returned) != 1 ||
        !*app_event_text(prepared, "fileId") || !*app_event_text(prepared, "uploadUrl") ||
        !cJSON_IsNumber(expiry) || expiry->valuedouble <= (double)app_event_realtime()))
        error = EPROTO;
    if (!error && (strcmp(app_event_text(prepared, "uploadMethod"), "PUT") ||
        !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(prepared, "enc")) ||
        !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(prepared, "headers")) ||
        (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(prepared, "enc")) &&
         (!*app_event_text(prepared, "keyId") || !*app_event_text(prepared, "fileEncryptKey") ||
          strcmp(app_event_text(prepared, "encType"), "AES_256_GCM")))))
        error = EPROTO;
    if (!error) {
        cJSON_DeleteItemFromObjectCaseSensitive(job, "prepared");
        if (!cJSON_AddItemToObject(job, "prepared", cJSON_Duplicate(prepared, 1)))
            error = ENOMEM;
    }
    cJSON_Delete(reply);
    cJSON_Delete(request);
    return error;
}

static const char *report_reconciliation_reason(const cJSON *job)
{
    const char *sha256 = app_event_text(job, "uploadedSha256");
    if (strlen(sha256) != 64 || strspn(sha256, "0123456789abcdef") != 64)
        return "uploaded_hash_requires_reconciliation";
    const cJSON *duration = cJSON_GetObjectItemCaseSensitive(job, "videoDuration");
    if (!strcmp(app_event_text(job, "purpose"), "cloud_video") &&
        (!cJSON_IsNumber(duration) || !(duration->valuedouble > 0) || duration->valuedouble > 300))
        return "uploaded_duration_requires_reconciliation";
    return NULL;
}

static int report(cJSON *job)
{
    const char *reason = report_reconciliation_reason(job);
    if (reason) {
        status(job, reason, ENODATA);
        return ENODATA;
    }
    const char *sha256 = app_event_text(job, "uploadedSha256");
    int video = !strcmp(app_event_text(job, "purpose"), "cloud_video");
    const cJSON *duration = cJSON_GetObjectItemCaseSensitive(job, "videoDuration");
    const cJSON *prepared = cJSON_GetObjectItemCaseSensitive(job, "prepared");
    cJSON *request = cJSON_CreateObject(), *files = cJSON_CreateArray(), *file = cJSON_CreateObject(),
        *params = cJSON_CreateObject(), *reply = NULL;
    if (!request || !files || !file || !params) {
        cJSON_Delete(request);
        cJSON_Delete(files);
        cJSON_Delete(file);
        cJSON_Delete(params);
        return ENOMEM;
    }
    cJSON_AddStringToObject(request, "purpose", app_event_text(job, "purpose"));
    cJSON_AddStringToObject(file, "fileId", app_event_text(prepared, "fileId"));
    cJSON_AddStringToObject(file, "keyId", app_event_text(prepared, "keyId"));
    cJSON_AddStringToObject(file, "fileType", app_event_text(job, "fileType"));
    cJSON_AddStringToObject(file, "detectType", app_event_text(job, "detectType"));
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(job, "uploadedBytes");
    cJSON_AddNumberToObject(file, "fileSize", cJSON_IsNumber(size) ? size->valuedouble : 0);
    cJSON_AddStringToObject(file, "sha256", sha256);
    if (video)
        cJSON_AddNumberToObject(file, "duration", duration->valuedouble);
    cJSON_AddItemToArray(files, file);
    cJSON_AddItemToObject(request, "files", files);
    cJSON_AddStringToObject(params, "eventId", app_event_text(job, "eventId"));
    const cJSON *time = cJSON_GetObjectItemCaseSensitive(job, "eventTime");
    cJSON_AddNumberToObject(params, "eventTime", cJSON_IsNumber(time) ? time->valuedouble : 0);
    cJSON_AddBoolToObject(params, "alarm", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job, "alarm")));
    cJSON_AddItemToObject(request, "params", params);
    int error = post(job, "/api/device-api/files/uploads/report", request, &reply);
    if (!error && !response_success(reply)) {
        char *request_text = cJSON_PrintUnformatted(request);
        char *reply_text = cJSON_PrintUnformatted(reply);
        fprintf(stderr, "[cloud-report] event=%s purpose=%s request=%s response=%s\n",
            app_event_text(job, "eventId"), app_event_text(job, "purpose"),
            request_text ? request_text : "null", reply_text ? reply_text : "null");
        cJSON_free(request_text);
        cJSON_free(reply_text);
        error = EREMOTEIO;
    }
    cJSON_Delete(reply);
    cJSON_Delete(request);
    return error;
}

static int save_state(const char *manifest, cJSON *job, const char *state)
{
    set_string(job, "state", state);
    return app_event_write_json(manifest, job);
}

static int pause_for_reconciliation(const char *manifest, cJSON *job, const char *reason, int error)
{
    set_string(job, "reconciliationRequired", reason);
    set_number(job, "lastError", error);
    int save_error = app_event_write_json(manifest, job);
    if (save_error) {
        fprintf(stderr, "[upload] manifest=%s state_save_errno=%d\n", manifest, save_error);
        status(job, "state_save_failed", save_error);
        return save_error;
    }
    status(job, reason, error);
    return error;
}

static int process_job(const char *manifest, cJSON *job)
{
    if (cancelled())
        return ECANCELED;
    if (*app_event_text(job, "reconciliationRequired")) {
        if (!current_boot_job(job))
            return EINPROGRESS;
        cJSON_DeleteItemFromObjectCaseSensitive(job, "reconciliationRequired");
        int clear_error = app_event_write_json(manifest, job);
        if (clear_error)
            return clear_error;
    }
    struct statvfs storage;
    if (statvfs("/mnt/sd", &storage))
        return errno;
    if (storage.f_flag & ST_RDONLY) {
        status(job, "storage_read_only", EROFS);
        return EROFS;
    }
    char state[32];
    int error;
    snprintf(state, sizeof(state), "%s", app_event_text(job, "state"));
    if (!strcmp(state, "uploading") || !strcmp(state, "put_uncertain")) {
        error = save_state(manifest, job, "pending");
        if (error)
            return error;
        strcpy(state, "pending");
    }
    if (!strcmp(state, "uploaded")) {
        const char *reason = report_reconciliation_reason(job);
        if (reason)
            return pause_for_reconciliation(manifest, job, reason, ENODATA);
    }
    error = resolve_detection(job);
    if (error) {
        status(job, error == ENOKEY ? "waiting_class_mapping" : "invalid_detection", error);
        return error;
    }
    error = resolve_time(job);
    if (error) {
        if (error == ESTALE)
            return pause_for_reconciliation(manifest, job, "event_time_requires_reconciliation", error);
        status(job, "waiting_event_time", error);
        return error;
    }
    cJSON *http = credentials();
    if (!http) {
        status(job, "waiting_http_handshake", ENOKEY);
        return ENOKEY;
    }
    cJSON_Delete(http);
    const char *path = app_event_text(job, "path");
    if (strncmp(path, "/mnt/sd/DCIM/", 13) || strstr(path, "/../") || strstr(path, ".part"))
        return EINVAL;
    struct stat info;
    const cJSON *source_size = cJSON_GetObjectItemCaseSensitive(job, "sourceSize");
    if (stat(path, &info) || !S_ISREG(info.st_mode) || !cJSON_IsNumber(source_size) ||
        (double)info.st_size != source_size->valuedouble)
        return ESTALE;
    if (!strcmp(state, "pending")) {
        status(job, "prepare", 0);
        error = prepare(job);
        if (error) {
            if (error != ENOKEY) {
                notify(job, "prepare", 1);
                notify(job, "done", 1);
            }
            return error;
        }
        error = save_state(manifest, job, "prepared");
        if (error)
            return error;
        strcpy(state, "prepared");
    }
    if (!strcmp(state, "prepared")) {
        error = notify(job, "prepare", 0);
        if (error || cancelled())
            return error ? error : ECANCELED;
        const cJSON *prepared = cJSON_GetObjectItemCaseSensitive(job, "prepared");
        const cJSON *expiry = cJSON_GetObjectItemCaseSensitive(prepared, "expireTime");
        if (!cJSON_IsNumber(expiry) || expiry->valuedouble <= (double)app_event_realtime() + 10000) {
            save_state(manifest, job, "pending");
            return EKEYEXPIRED;
        }
        error = save_state(manifest, job, "uploading");
        if (error)
            return error;
        status(job, "upload", 0);
        unsigned long long bytes = 0;
        char sha256[65];
        int http_status = 0;
        error = bf_cloud_put_sha256(prepared, path, &bytes, sha256, &http_status, cancelled);
        set_number(job, "lastHttpStatus", http_status);
        set_number(job, "lastUploadHttpStatus", http_status);
        if (error) {
            save_state(manifest, job, "pending");
            notify(job, "upload", 1);
            notify(job, "done", 1);
            return error;
        }
        set_number(job, "uploadedBytes", (double)bytes);
        set_string(job, "uploadedSha256", sha256);
        error = save_state(manifest, job, "uploaded");
        if (error)
            return error;
        strcpy(state, "uploaded");
    }
    if (!strcmp(state, "uploaded")) {
        const char *reason = report_reconciliation_reason(job);
        if (reason)
            return pause_for_reconciliation(manifest, job, reason, ENODATA);
        error = notify(job, "upload", 0);
        if (error || cancelled())
            return error ? error : ECANCELED;
        status(job, "report", 0);
        error = report(job);
        if (error) {
            notify(job, "done", 1);
            return error;
        }
        error = save_state(manifest, job, "reported");
        if (error)
            return error;
        strcpy(state, "reported");
    }
    if (!strcmp(state, "reported")) {
        error = notify(job, "done", 0);
        if (!error) {
            set_number(job, "lastError", 0);
            error = save_state(manifest, job, "done");
        }
        if (!error)
            status(job, "done", 0);
    }
    return error;
}

static void *upload_worker(void *context)
{
    (void)context;
    while (!stopping()) {
        if (!app_event_live()) {
            glob_t matches = {0};
            if (!glob("/mnt/sd/.capture/task_*/*.upload.json", 0, NULL, &matches)) {
                for (size_t index = 0; index < matches.gl_pathc && !cancelled(); index++) {
                    cJSON *job = NULL;
                    int error = app_event_read_json(matches.gl_pathv[index], &job);
                    if (!error && current_boot_job(job) && strcmp(app_event_text(job, "state"), "done")) {
                        error = process_job(matches.gl_pathv[index], job);
                        if (error && error != EROFS && !*app_event_text(job, "reconciliationRequired")) {
                            set_number(job, "lastError", error);
                            int save_error = app_event_write_json(matches.gl_pathv[index], job);
                            if (save_error) {
                                fprintf(stderr, "[upload] manifest=%s state_save_errno=%d\n",
                                    matches.gl_pathv[index], save_error);
                                status(job, "state_save_failed", save_error);
                            } else if (error != ENOKEY && error != ENODATA && error != EINPROGRESS)
                                status(job, "retry_wait", error);
                        }
                    }
                    cJSON_Delete(job);
                }
            }
            globfree(&matches);
        }
        for (unsigned int interval = 0; interval < 100 && !stopping(); interval++)
            usleep(100000);
    }
    return NULL;
}

int app_upload_start(void)
{
    upload_stopping = 0;
    int boot_error = load_upload_boot_id();
    if (boot_error)
        fprintf(stderr, "[upload] current boot id unavailable errno=%d; old manifests will not upload\n", boot_error);
    int error = pthread_create(&upload_thread, NULL, upload_worker, NULL);
    upload_running = !error;
    return error;
}

void app_upload_stop(void)
{
    pthread_mutex_lock(&upload_mutex);
    upload_stopping = 1;
    pthread_cond_broadcast(&upload_condition);
    pthread_mutex_unlock(&upload_mutex);
    if (upload_running)
        pthread_join(upload_thread, NULL);
    upload_running = 0;
    cJSON_Delete(upload_http);
    upload_http = NULL;
    cJSON_Delete(upload_notice);
    upload_notice = NULL;
}

void app_upload_poll(struct bf_wifi_link *link)
{
    const cJSON *http = bf_wifi_http(link, app_event_realtime());
    pthread_mutex_lock(&upload_mutex);
    cJSON_Delete(upload_http);
    upload_http = http && (!*rejected_token || strcmp(rejected_token, app_event_text(http, "accessToken"))) ?
        cJSON_Duplicate(http, 1) : NULL;
    if (http && upload_notice && !upload_notice_id) {
        unsigned int identifier = bf_wifi_next_id(link);
        if (!bf_wifi_enqueue(link, upload_notice))
            upload_notice_id = identifier;
    }
    pthread_mutex_unlock(&upload_mutex);
}

int app_upload_refresh_needed(void)
{
    pthread_mutex_lock(&upload_mutex);
    int needed = upload_refresh;
    pthread_mutex_unlock(&upload_mutex);
    return needed;
}

void app_upload_refresh_started(void)
{
    pthread_mutex_lock(&upload_mutex);
    upload_refresh = 0;
    pthread_mutex_unlock(&upload_mutex);
}

void app_upload_completed(const cJSON *request, const cJSON *reply)
{
    if (strcmp(app_event_text(request, "cmd"), "file"))
        return;
    const cJSON *identity = cJSON_GetObjectItemCaseSensitive(request, "id");
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(reply, "result");
    pthread_mutex_lock(&upload_mutex);
    if (cJSON_IsNumber(identity) && (unsigned int)identity->valueint == upload_notice_id) {
        upload_notice_result = !reply ? ETIMEDOUT : !cJSON_IsNumber(result) ? EPROTO : result->valueint ? EREMOTEIO : 0;
        pthread_cond_broadcast(&upload_condition);
    }
    pthread_mutex_unlock(&upload_mutex);
}

void app_upload_status(cJSON *reply)
{
    pthread_mutex_lock(&upload_mutex);
    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "state", upload_state);
    cJSON_AddStringToObject(value, "eventId", upload_event);
    cJSON_AddStringToObject(value, "purpose", upload_purpose);
    cJSON_AddNumberToObject(value, "errno", upload_error);
    cJSON_AddBoolToObject(value, "liveMode", app_event_live());
    cJSON_AddItemToObject(reply, "upload", value);
    pthread_mutex_unlock(&upload_mutex);
}
