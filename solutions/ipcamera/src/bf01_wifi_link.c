#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bf01_wifi_link.h"

#define WIFI_QUEUE 8U
#define WIFI_CACHE 8U
#define WIFI_RETRY_MS 3000U

struct wifi_cache {
    unsigned int id;
    char body[BF_WIFI_RECEIVE_MAX + 1];
    char ack[BF_WIFI_SEND_MAX + 1];
};

struct wifi_parts {
    char *data[BF_WIFI_PARTS_MAX];
    unsigned int id;
    unsigned int count;
    unsigned int received;
    size_t length;
    uint64_t started;
};

struct bf_wifi_link {
    struct bf_wifi_ops ops;
    void *context;
    cJSON *settings;
    cJSON *http;
    cJSON *next_http;
    cJSON *busy_http;
    cJSON *queue[WIFI_QUEUE];
    struct wifi_cache cache[WIFI_CACHE];
    struct wifi_parts parts;
    unsigned int cache_next;
    unsigned int head;
    unsigned int count;
    unsigned int next_id;
    unsigned int peer_id;
    unsigned int highest_peer_id;
    unsigned int full_report_id;
    unsigned int attempts;
    uint64_t sent_at;
    char peer[BF_WIFI_RECEIVE_MAX + 1];
    char ack[BF_WIFI_SEND_MAX + 1];
    char pending[BF_WIFI_SEND_MAX + 1];
    int ack_result;
    int ack_full;
    int full_applied;
    int report_needed;
    int report_ready;
    char version[65];
};

static const char *const setting_names[] = {
    "pir_sens_level", "pir_detect_intv", "pir_record_dur", "ai_detect_switch",
    "bird_detect_switch", "squirrel_detect_switch", "other_detect_switch",
    "image_filter_level", "image_anti_flicker", "brand_wm_switch",
    "datetime_wm_switch", "datetime_wm_format", "mic_switch", "spk_vol", "status_led_switch"
};

static const cJSON *item(const cJSON *object, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static int text(const cJSON *value, size_t maximum)
{
    return cJSON_IsString(value) && *value->valuestring &&
        strlen(value->valuestring) <= maximum && !strpbrk(value->valuestring, "\r\n");
}

static int equals(const cJSON *value, const char *expected)
{
    return cJSON_IsString(value) && !strcmp(value->valuestring, expected);
}

static int integer(const cJSON *value, double minimum, double maximum)
{
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
        value->valuedouble >= minimum && value->valuedouble <= maximum &&
        floor(value->valuedouble) == value->valuedouble;
}

static int unique(const cJSON *object)
{
    const cJSON *field;
    cJSON_ArrayForEach(field, object) {
        if (!field->string)
            return 0;
        for (const cJSON *other = field->next; other; other = other->next)
            if (other->string && !strcmp(field->string, other->string))
                return 0;
    }
    return 1;
}

static int serialize(const cJSON *object, char *buffer, size_t capacity)
{
    char *encoded = cJSON_PrintUnformatted(object);
    if (!encoded)
        return ENOMEM;
    size_t length = strlen(encoded);
    int error = length < capacity ? 0 : EMSGSIZE;
    if (!error)
        memcpy(buffer, encoded, length + 1);
    cJSON_free(encoded);
    return error;
}

static int choice(const cJSON *value, const char *first, const char *second, const char *third)
{
    return equals(value, first) || equals(value, second) || equals(value, third);
}

static int setting_valid(size_t index, const cJSON *value)
{
    if (index == 0 || index == 7)
        return choice(value, "low", "medium", "high");
    if (index == 1)
        return integer(value, 0, 30) && value->valueint % 5 == 0;
    if (index == 2)
        return integer(value, -1, 30) && (value->valueint == -1 || value->valueint % 10 == 0);
    if (index == 8)
        return choice(value, "off", "50Hz", "60Hz");
    if (index == 11)
        return choice(value, "yyyy-MM-dd HH:mm:ss", "MM/dd/yyyy HH:mm:ss", "dd-MM-yyyy HH:mm:ss");
    if (index == 13)
        return integer(value, 0, 100) && value->valueint % 5 == 0;
    return cJSON_IsBool(value);
}

static int key_valid(const cJSON *value)
{
    if (!text(value, 44) || strlen(value->valuestring) != 44 || value->valuestring[43] != '=')
        return 0;
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t index = 0; index < 43; index++) {
        const char *position = strchr(alphabet, value->valuestring[index]);
        if (!position || (index == 42 && ((position - alphabet) & 3)))
            return 0;
    }
    return 1;
}

static int http_valid(const cJSON *http, uint64_t now)
{
    const cJSON *url = item(http, "baseUrl");
    const cJSON *expiry = item(http, "expireTime");
    if (now < 1000000000000ULL || !cJSON_IsObject(http) || !unique(http) || !text(url, 512) ||
        (strncmp(url->valuestring, "http://", 7) && strncmp(url->valuestring, "https://", 8)) ||
        url->valuestring[strlen(url->valuestring) - 1] == '/' ||
        !text(item(http, "productCode"), 128) || !text(item(http, "deviceSn"), 128) ||
        !text(item(http, "version"), 64) || !text(item(http, "accessToken"), 1024) ||
        !key_valid(item(http, "httpHmacSignKey")) || !key_valid(item(http, "httpAesKey")) ||
        item(http, "refreshToken") || !integer(expiry, 1000000000000.0, 9999999999999.0))
        return 0;
    return expiry->valuedouble > (double)now;
}

static int result(int value)
{
    return value >= 0 && value <= 2 ? value : 2;
}

static void clear_parts(struct bf_wifi_link *link)
{
    for (unsigned int index = 0; index < BF_WIFI_PARTS_MAX; index++)
        free(link->parts.data[index]);
    memset(&link->parts, 0, sizeof(link->parts));
}

static void clear_outgoing(struct bf_wifi_link *link)
{
    for (unsigned int index = 0; index < WIFI_QUEUE; index++) {
        cJSON_Delete(link->queue[index]);
        link->queue[index] = NULL;
    }
    link->head = 0;
    link->count = 0;
    link->attempts = 0;
    link->pending[0] = '\0';
    link->full_report_id = 0;
    cJSON_Delete(link->busy_http);
    link->busy_http = NULL;
}

void bf_wifi_reset(struct bf_wifi_link *link)
{
    if (!link)
        return;
    clear_parts(link);
    clear_outgoing(link);
    cJSON_Delete(link->settings);
    cJSON_Delete(link->http);
    cJSON_Delete(link->next_http);
    link->settings = NULL;
    link->http = NULL;
    link->next_http = NULL;
    memset(link->cache, 0, sizeof(link->cache));
    link->cache_next = 0;
    link->highest_peer_id = 0;
    link->peer[0] = '\0';
    link->ack[0] = '\0';
    link->full_applied = 0;
    link->report_needed = 0;
    link->report_ready = 0;
    link->version[0] = '\0';
}

struct bf_wifi_link *bf_wifi_create(const struct bf_wifi_ops *ops, void *context)
{
    if (!ops || !ops->receive || !ops->exchange)
        return NULL;
    struct bf_wifi_link *link = calloc(1, sizeof(*link));
    if (link) {
        link->ops = *ops;
        link->context = context;
        link->next_id = 1;
    }
    return link;
}

void bf_wifi_destroy(struct bf_wifi_link *link)
{
    bf_wifi_reset(link);
    free(link);
}

int bf_wifi_resume_id(struct bf_wifi_link *link, unsigned int next_id)
{
    if (!link || !next_id || next_id > INT_MAX || link->count || link->next_id != 1)
        return EINVAL;
    link->next_id = next_id;
    return 0;
}

unsigned int bf_wifi_next_id(const struct bf_wifi_link *link)
{
    return link ? link->next_id : 0;
}

static int apply_set(struct bf_wifi_link *link, const cJSON *request, int *full)
{
    const cJSON *changes = item(request, "dps");
    if (!cJSON_IsObject(changes) || !unique(changes))
        return 1;
    cJSON *filtered = cJSON_CreateObject();
    if (!filtered)
        return 2;
    unsigned int present = 0;
    int accepted = 0;
    for (size_t index = 0; index < sizeof(setting_names) / sizeof(setting_names[0]); index++) {
        const cJSON *value = item(changes, setting_names[index]);
        if (!value)
            continue;
        if (!setting_valid(index, value)) {
            cJSON_Delete(filtered);
            return 1;
        }
        if (!cJSON_AddItemToObject(filtered, setting_names[index], cJSON_Duplicate(value, 1))) {
            cJSON_Delete(filtered);
            return 2;
        }
        present++;
    }
    *full = present == sizeof(setting_names) / sizeof(setting_names[0]);
    const cJSON *format = item(changes, "memory_format");
    if (format && !cJSON_IsTrue(format))
        accepted = 1;
    else if (format && !cJSON_AddTrueToObject(filtered, "memory_format"))
        accepted = 2;
    else if (!present && !format)
        accepted = 0;
    else if ((!*full && !link->full_applied) || !link->ops.apply)
        accepted = 2;
    else {
        cJSON *snapshot = link->settings ? cJSON_Duplicate(link->settings, 1) : cJSON_CreateObject();
        if (!snapshot)
            accepted = 2;
        const cJSON *field;
        cJSON_ArrayForEach(field, filtered) {
            if (accepted || !strcmp(field->string, "memory_format"))
                continue;
            cJSON_DeleteItemFromObjectCaseSensitive(snapshot, field->string);
            if (!cJSON_AddItemToObject(snapshot, field->string, cJSON_Duplicate(field, 1)))
                accepted = 2;
        }
        if (!accepted)
            accepted = result(link->ops.apply(link->context, filtered, *full));
        if (!accepted) {
            cJSON_Delete(link->settings);
            link->settings = snapshot;
            snapshot = NULL;
            if (*full) {
                clear_outgoing(link);
                cJSON_Delete(link->http);
                link->http = NULL;
                link->full_applied = 1;
                link->report_ready = 0;
            }
        }
        cJSON_Delete(snapshot);
    }
    cJSON_Delete(filtered);
    return accepted;
}

static int stream_valid(const cJSON *request, uint64_t now)
{
    if (!text(item(request, "streamId"), 128))
        return 0;
    if (equals(item(request, "action"), "stop"))
        return 1;
    const cJSON *url = item(request, "pushUrl");
    const cJSON *expiry = item(request, "expireTime");
    if (!equals(item(request, "action"), "start") || !text(url, 1400) ||
        !integer(expiry, 1000000000000.0, 9999999999999.0) || expiry->valuedouble <= (double)now ||
        !equals(item(request, "channel"), "ch1") ||
        (!equals(item(request, "profile"), "main") && !equals(item(request, "profile"), "sub")))
        return 0;
    return (equals(item(request, "pushProtocol"), "RTSP") && !strncmp(url->valuestring, "rtsp://", 7)) ||
        (equals(item(request, "pushProtocol"), "RTMP") && !strncmp(url->valuestring, "rtmp://", 7));
}

static int receive_part(struct bf_wifi_link *link, const cJSON *request, uint64_t monotonic)
{
    const cJSON *identifier = item(request, "id");
    const cJSON *sequence = item(request, "seq");
    const cJSON *count = item(request, "cnt");
    const cJSON *data = item(request, "data");
    if (!unique(request) || !integer(identifier, 1, INT_MAX) ||
        !integer(count, 1, BF_WIFI_PARTS_MAX) || !integer(sequence, 0, count->valueint - 1) ||
        !text(data, BF_WIFI_RECEIVE_MAX)) {
        clear_parts(link);
        return EPROTO;
    }
    unsigned int id = (unsigned int)identifier->valueint;
    unsigned int index = (unsigned int)sequence->valueint;
    if (link->parts.count && link->parts.id != id)
        clear_parts(link);
    if (!link->parts.count) {
        link->parts.id = id;
        link->parts.count = (unsigned int)count->valueint;
        link->parts.started = monotonic;
    }
    if (link->parts.count != (unsigned int)count->valueint) {
        clear_parts(link);
        return EPROTO;
    }
    if (link->parts.data[index]) {
        if (!strcmp(link->parts.data[index], data->valuestring))
            return EINPROGRESS;
        clear_parts(link);
        return EPROTO;
    }
    size_t length = strlen(data->valuestring);
    if (length > BF_WIFI_RECEIVE_MAX - link->parts.length) {
        clear_parts(link);
        return EMSGSIZE;
    }
    link->parts.data[index] = malloc(length + 1);
    if (!link->parts.data[index])
        return ENOMEM;
    memcpy(link->parts.data[index], data->valuestring, length + 1);
    link->parts.length += length;
    link->parts.received++;
    if (link->parts.received != link->parts.count)
        return EINPROGRESS;
    size_t offset = 0;
    for (unsigned int fragment = 0; fragment < link->parts.count; fragment++) {
        length = strlen(link->parts.data[fragment]);
        memcpy(link->peer + offset, link->parts.data[fragment], length);
        offset += length;
    }
    link->peer[offset] = '\0';
    fprintf(stderr, "[wifi-link] reassembled id=%u parts=%u bytes=%zu\n",
        link->parts.id, link->parts.count, offset);
    clear_parts(link);
    return 0;
}

static int receive_message(struct bf_wifi_link *link, uint64_t monotonic, uint64_t now)
{
    memset(link->peer, 0, sizeof(link->peer));
    int error = link->ops.receive(link->context, link->peer, sizeof(link->peer));
    if (error)
        return error;
    size_t length = strnlen(link->peer, sizeof(link->peer));
    if (!length || length > BF_WIFI_RECEIVE_MAX)
        return EMSGSIZE;
    cJSON *request = strpbrk(link->peer, "\r\n") ? NULL : cJSON_ParseWithOpts(link->peer, NULL, 1);
    unsigned int fragmented_id = 0;
    if (equals(item(request, "cmd"), "part")) {
        if (integer(item(request, "id"), 1, INT_MAX))
            fragmented_id = (unsigned int)item(request, "id")->valueint;
        error = receive_part(link, request, monotonic);
        cJSON_Delete(request);
        if (error)
            return error;
        request = strpbrk(link->peer, "\r\n") ? NULL : cJSON_ParseWithOpts(link->peer, NULL, 1);
        if (!cJSON_IsObject(request) || !unique(request) || equals(item(request, "cmd"), "part") ||
            !integer(item(request, "id"), 1, INT_MAX) ||
            (unsigned int)item(request, "id")->valueint != fragmented_id) {
            cJSON_Delete(request);
            request = NULL;
        }
    }
    unsigned int id = integer(item(request, "id"), 1, INT_MAX) ? (unsigned int)item(request, "id")->valueint : 0;
    if (fragmented_id)
        id = fragmented_id;
    if (id && link->parts.id == id)
        clear_parts(link);
    link->peer_id = id;
    for (unsigned int index = 0; id && index < WIFI_CACHE; index++) {
        if (link->cache[index].id == id && !strcmp(link->cache[index].body, link->peer)) {
            strcpy(link->ack, link->cache[index].ack);
            link->ack_result = 1;
            link->ack_full = 0;
            cJSON_Delete(request);
            return 0;
        }
    }
    cJSON *ack = cJSON_CreateObject();
    if (!ack) {
        cJSON_Delete(request);
        return ENOMEM;
    }
    cJSON_AddStringToObject(ack, "cmd", "ack");
    cJSON_AddNumberToObject(ack, "id", id);
    int accepted = 1;
    link->ack_full = 0;
    if (id && cJSON_IsObject(request) && unique(request)) {
        int reused = id <= link->highest_peer_id;
        const cJSON *command = item(request, "cmd");
        if (reused)
            accepted = 1;
        else if (equals(command, "set")) {
            accepted = apply_set(link, request, &link->ack_full);
            if (accepted)
                link->ack_full = 0;
        } else if (equals(command, "http")) {
            if (now < 1000000000000ULL)
                accepted = 2;
            else if (!http_valid(request, now) || (*link->version && !equals(item(request, "version"), link->version)))
                accepted = 1;
            else if (!link->report_ready)
                accepted = 2;
            else {
                cJSON_Delete(link->next_http);
                link->next_http = cJSON_Duplicate(request, 1);
                accepted = link->next_http ? 0 : 2;
            }
        } else if (equals(command, "stream"))
            accepted = !stream_valid(request, now) ? 1 :
                link->ops.stream ? result(link->ops.stream(link->context, request)) : 2;
        else if (equals(command, "get")) {
            const cJSON *name = item(request, "name");
            cJSON *value = NULL;
            if (equals(name, "dps")) {
                value = cJSON_Duplicate(link->settings, 1);
                accepted = value ? 0 : 2;
            } else if (equals(name, "status") || equals(name, "stream"))
                accepted = link->ops.snapshot ? result(link->ops.snapshot(link->context, name->valuestring, &value)) : 2;
            if (!accepted && !cJSON_IsObject(value))
                accepted = 2;
            if (!accepted && !cJSON_AddItemToObject(ack, name->valuestring, value))
                accepted = 2;
            else if (!accepted)
                value = NULL;
            cJSON_Delete(value);
        }
    }
    if (cJSON_HasObjectItem(ack, "status") || cJSON_HasObjectItem(ack, "dps") || cJSON_HasObjectItem(ack, "stream")) {
        cJSON_AddNumberToObject(ack, "result", accepted);
        error = serialize(ack, link->ack, sizeof(link->ack));
    } else {
        snprintf(link->ack, sizeof(link->ack), "{\"cmd\":\"ack\",\"id\":%u,\"result\":%d}", id, accepted);
        error = 0;
    }
    link->ack_result = accepted;
    cJSON_Delete(ack);
    cJSON_Delete(request);
    return error;
}

int bf_wifi_enqueue(struct bf_wifi_link *link, const cJSON *request)
{
    if (!link || !cJSON_IsObject(request) || !unique(request))
        return EINVAL;
    const cJSON *command = item(request, "cmd");
    if (!equals(command, "get") && !equals(command, "report") && !equals(command, "file") &&
        !(equals(command, "stream") && equals(item(request, "action"), "state")))
        return EINVAL;
    if (link->count == WIFI_QUEUE || link->next_id > INT_MAX)
        return ENOBUFS;
    cJSON *copy = cJSON_Duplicate(request, 1);
    if (!copy)
        return ENOMEM;
    cJSON_DeleteItemFromObjectCaseSensitive(copy, "id");
    cJSON_AddNumberToObject(copy, "id", link->next_id);
    char buffer[BF_WIFI_SEND_MAX + 1];
    int error = serialize(copy, buffer, sizeof(buffer));
    if (error) {
        cJSON_Delete(copy);
        return error;
    }
    link->next_id++;
    link->queue[(link->head + link->count++) % WIFI_QUEUE] = copy;
    if (equals(command, "get") && equals(item(request, "name"), "http")) {
        cJSON_Delete(link->busy_http);
        link->busy_http = NULL;
    }
    return 0;
}

int bf_wifi_retry_http(struct bf_wifi_link *link)
{
    if (!link || !link->busy_http)
        return ENOENT;
    if (link->count == WIFI_QUEUE)
        return ENOBUFS;
    unsigned int identifier = (unsigned int)item(link->busy_http, "id")->valueint;
    for (unsigned int index = 0; index < link->count; index++) {
        const cJSON *queued = link->queue[(link->head + index) % WIFI_QUEUE];
        if ((unsigned int)item(queued, "id")->valueint == identifier)
            return EALREADY;
    }
    cJSON *copy = cJSON_Duplicate(link->busy_http, 1);
    if (!copy)
        return ENOMEM;
    link->queue[(link->head + link->count++) % WIFI_QUEUE] = copy;
    return 0;
}

static int send_ack(struct bf_wifi_link *link)
{
    char response[BF_WIFI_RECEIVE_MAX + 1] = {0};
    int error = link->ops.exchange(link->context, link->ack, response, sizeof(response));
    if (error || *response)
        return error ? error : EPROTO;
    if (link->ack_result != 2) {
        struct wifi_cache *cached = &link->cache[link->cache_next++ % WIFI_CACHE];
        cached->id = link->peer_id;
        strcpy(cached->body, link->peer);
        strcpy(cached->ack, link->ack);
        if (link->peer_id > link->highest_peer_id)
            link->highest_peer_id = link->peer_id;
    }
    if (link->ack_full)
        link->report_needed = 1;
    if (link->next_http) {
        cJSON_Delete(link->http);
        link->http = link->next_http;
        link->next_http = NULL;
    }
    link->ack[0] = '\0';
    link->ack_full = 0;
    return 0;
}

static int report_valid(const cJSON *properties)
{
    const cJSON *capacity = item(properties, "memory_capacity");
    const cJSON *total = item(capacity, "total");
    const cJSON *used = item(capacity, "used");
    if (!cJSON_IsObject(properties) || !unique(properties) ||
        !cJSON_IsObject(capacity) || !unique(capacity) || !integer(total, 0, INT_MAX) ||
        !cJSON_IsNumber(used) || !isfinite(used->valuedouble) || used->valuedouble < 0 ||
        used->valuedouble > total->valuedouble ||
        fabs(used->valuedouble * 10 - round(used->valuedouble * 10)) > 0.000001)
        return 0;
    return choice(item(properties, "memory_status"), "normal", "unmount", "error") &&
        choice(item(properties, "memory_format_status"), "formatting", "success", "fail") &&
        text(item(properties, "firmware_version"), 64);
}

static int queue_report(struct bf_wifi_link *link)
{
    cJSON *properties = NULL;
    if (!link->ops.snapshot || link->ops.snapshot(link->context, "report", &properties) ||
        !report_valid(properties)) {
        cJSON_Delete(properties);
        return EAGAIN;
    }
    cJSON *request = cJSON_CreateObject();
    if (!request) {
        cJSON_Delete(properties);
        return ENOMEM;
    }
    cJSON_AddStringToObject(request, "cmd", "report");
    cJSON_AddItemToObject(request, "properties", properties);
    int error = bf_wifi_enqueue(link, request);
    if (!error) {
        strcpy(link->version, item(properties, "firmware_version")->valuestring);
        link->report_needed = 0;
        unsigned int tail = (link->head + link->count - 1) % WIFI_QUEUE;
        link->full_report_id = (unsigned int)item(link->queue[tail], "id")->valueint;
    }
    cJSON_Delete(request);
    return error;
}

static int send_request(struct bf_wifi_link *link, uint64_t monotonic, uint64_t realtime)
{
    if (!link->count)
        return 0;
    if (link->attempts && monotonic - link->sent_at < WIFI_RETRY_MS)
        return 0;
    if (link->attempts == 3) {
        cJSON *expired = link->queue[link->head];
        if (link->full_report_id == (unsigned int)item(expired, "id")->valueint)
            link->full_report_id = 0;
        if (link->ops.completed)
            link->ops.completed(link->context, expired, NULL);
        cJSON_Delete(expired);
        link->queue[link->head] = NULL;
        link->head = (link->head + 1) % WIFI_QUEUE;
        link->count--;
        link->attempts = 0;
        link->pending[0] = '\0';
        return ETIMEDOUT;
    }
    cJSON *request = link->queue[link->head];
    int error = serialize(request, link->pending, sizeof(link->pending));
    if (error)
        return error;
    link->attempts++;
    link->sent_at = monotonic;
    char response[BF_WIFI_RECEIVE_MAX + 1] = {0};
    error = link->ops.exchange(link->context, link->pending, response, sizeof(response));
    if (error)
        return error;
    cJSON *ack = strnlen(response, sizeof(response)) > BF_WIFI_RECEIVE_MAX ? NULL :
        cJSON_ParseWithOpts(response, NULL, 1);
    int valid = cJSON_IsObject(ack) && unique(ack) && equals(item(ack, "cmd"), "ack") &&
        integer(item(ack, "id"), 1, INT_MAX) && item(ack, "id")->valueint == item(request, "id")->valueint &&
        integer(item(ack, "result"), 0, 2);
    int accepted = valid ? item(ack, "result")->valueint : -1;
    if (!accepted && equals(item(request, "cmd"), "get")) {
        const cJSON *name = item(request, "name");
        valid = text(name, 16) && cJSON_IsObject(item(ack, name->valuestring));
        if (valid && equals(name, "http")) {
            const cJSON *http = item(ack, "http");
            valid = http_valid(http, realtime) && (!*link->version || equals(item(http, "version"), link->version));
            if (valid) {
                cJSON *copy = cJSON_Duplicate(http, 1);
                if (!copy) {
                    cJSON_Delete(ack);
                    return ENOMEM;
                }
                cJSON_Delete(link->http);
                link->http = copy;
            }
        }
    }
    int http_refresh = equals(item(request, "cmd"), "get") && equals(item(request, "name"), "http");
    if (!valid || (accepted == 2 && !http_refresh)) {
        cJSON_Delete(ack);
        return valid ? EAGAIN : EPROTO;
    }
    if (http_refresh) {
        cJSON *retry = accepted == 2 ? cJSON_Duplicate(request, 1) : NULL;
        if (accepted == 2 && !retry) {
            cJSON_Delete(ack);
            return ENOMEM;
        }
        cJSON_Delete(link->busy_http);
        link->busy_http = retry;
    }
    if (link->full_report_id == (unsigned int)item(request, "id")->valueint) {
        link->report_ready = accepted == 0;
        link->full_report_id = 0;
    }
    if (link->ops.completed)
        link->ops.completed(link->context, request, ack);
    cJSON_Delete(ack);
    cJSON_Delete(request);
    link->queue[link->head] = NULL;
    link->head = (link->head + 1) % WIFI_QUEUE;
    link->count--;
    link->attempts = 0;
    link->pending[0] = '\0';
    return 0;
}

int bf_wifi_step(struct bf_wifi_link *link, uint64_t monotonic, uint64_t realtime)
{
    if (!link)
        return EINVAL;
    if (*link->ack)
        return send_ack(link);
    if (link->parts.count && monotonic - link->parts.started >= BF_WIFI_PART_TIMEOUT_MS) {
        fprintf(stderr, "[wifi-link] part timeout id=%u received=%u/%u bytes=%zu\n",
            link->parts.id, link->parts.received, link->parts.count, link->parts.length);
        clear_parts(link);
        return ETIMEDOUT;
    }
    int error = receive_message(link, monotonic, realtime);
    if (!error)
        return send_ack(link);
    if (error == EINPROGRESS)
        return 0;
    if (error != EAGAIN)
        return error;
    if (link->parts.count)
        return 0;
    if (link->report_needed) {
        error = queue_report(link);
        if (error)
            return error;
    }
    return send_request(link, monotonic, realtime);
}

const cJSON *bf_wifi_http(const struct bf_wifi_link *link, uint64_t realtime)
{
    return link && link->report_ready && http_valid(link->http, realtime) ? link->http : NULL;
}

int bf_wifi_http_refresh_due(const struct bf_wifi_link *link, uint64_t realtime)
{
    if (!link || !link->report_ready || realtime < 1000000000000ULL)
        return 0;
    const cJSON *expiry = item(link->http, "expireTime");
    return integer(expiry, 1000000000000.0, 9999999999999.0) &&
        expiry->valuedouble - (double)realtime < 600000.0;
}
