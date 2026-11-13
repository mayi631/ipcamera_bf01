#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "cJSON.h"

#define RECEIVE_MAX 1600U
#define SEND_MAX 1595U

static unsigned int sequence;

static uint64_t clock_ms(clockid_t clock_type)
{
    struct timespec timestamp;
    clock_gettime(clock_type, &timestamp);
    return (uint64_t)timestamp.tv_sec * 1000 + timestamp.tv_nsec / 1000000;
}

static void record(const char *label, const char *event, const char *payload,
                   int error, unsigned int status, uint64_t elapsed)
{
    cJSON *entry = cJSON_CreateObject();
    if (!entry)
        exit(5);
    cJSON_AddNumberToObject(entry, "sequence", ++sequence);
    cJSON_AddNumberToObject(entry, "realtime_ms", (double)clock_ms(CLOCK_REALTIME));
    cJSON_AddNumberToObject(entry, "monotonic_ms", (double)clock_ms(CLOCK_MONOTONIC));
    cJSON_AddStringToObject(entry, "case", label);
    cJSON_AddStringToObject(entry, "event", event);
    if (payload) {
        cJSON_AddStringToObject(entry, "payload", payload);
        cJSON_AddNumberToObject(entry, "length", strlen(payload));
    }
    cJSON_AddNumberToObject(entry, "errno", error);
    cJSON_AddNumberToObject(entry, "status", status);
    cJSON_AddNumberToObject(entry, "elapsed_ms", (double)elapsed);
    char *serialized = cJSON_PrintUnformatted(entry);
    if (!serialized)
        exit(5);
    puts(serialized);
    fflush(stdout);
    cJSON_free(serialized);
    cJSON_Delete(entry);
}

static int exchange(int descriptor, const char *label, const char *request)
{
    struct {
        uint32_t status;
        char trans[RECEIVE_MAX];
    } message = {0};
    size_t length = strlen(request);
    if (length > SEND_MAX) {
        record(label, "local_reject", request, EMSGSIZE, 0, 0);
        return -1;
    }
    memcpy(message.trans, request, length + 1);
    record(label, "host_to_wifi_0x003B", request, 0, 0, 0);
    uint64_t started = clock_ms(CLOCK_MONOTONIC);
    int result = ioctl(descriptor, _IOWR(121, 53, unsigned int), &message);
    int error = result < 0 ? errno : (result ? EIO : 0);
    if (!memchr(message.trans, 0, sizeof(message.trans)))
        error = EOVERFLOW;
    message.trans[RECEIVE_MAX - 1] = 0;
    record(label, error ? "transport_error_0x003B" : "wifi_to_host_0x003B",
           error ? NULL : message.trans, error, message.status,
           clock_ms(CLOCK_MONOTONIC) - started);
    return error || message.status ? -1 : 0;
}

static int receive_one(int descriptor, const char *label, char *response)
{
    memset(response, 0, RECEIVE_MAX + 1);
    int result = ioctl(descriptor, _IOR(121, 90, unsigned int), response);
    int error = result < 0 ? errno : (result ? EPROTO : 0);
    if (error == EAGAIN)
        return 0;
    if (!error && (!memchr(response, 0, RECEIVE_MAX + 1) || !response[0]))
        error = EPROTO;
    response[RECEIVE_MAX] = 0;
    record(label, error ? "transport_error_0x083B" : "wifi_to_host_0x083B",
           error ? NULL : response, error, 0, 0);
    return error ? -1 : 1;
}

static int listen_for(int descriptor, const char *label, unsigned int duration,
                      int set_result)
{
    uint64_t deadline = clock_ms(CLOCK_MONOTONIC) + duration;
    char response[RECEIVE_MAX + 1];
    do {
        int received = receive_one(descriptor, label, response);
        if (received < 0)
            return -1;
        if (!received) {
            usleep(20000);
            continue;
        }
        if (set_result < 0)
            continue;
        cJSON *request = cJSON_Parse(response);
        const cJSON *command = cJSON_GetObjectItemCaseSensitive(request, "cmd");
        const cJSON *identifier = cJSON_GetObjectItemCaseSensitive(request, "id");
        int matched = cJSON_IsString(command) && !strcmp(command->valuestring, "set") &&
                      cJSON_IsNumber(identifier) && identifier->valuedouble >= 0;
        if (matched) {
            cJSON *acknowledgement = cJSON_CreateObject();
            if (!acknowledgement) {
                cJSON_Delete(request);
                return -1;
            }
            cJSON_AddStringToObject(acknowledgement, "cmd", "ack");
            cJSON_AddNumberToObject(acknowledgement, "id", identifier->valuedouble);
            cJSON_AddNumberToObject(acknowledgement, "result", set_result);
            char *serialized = cJSON_PrintUnformatted(acknowledgement);
            int result = serialized ? exchange(descriptor, label, serialized) : -1;
            cJSON_free(serialized);
            cJSON_Delete(acknowledgement);
            cJSON_Delete(request);
            return result;
        }
        cJSON_Delete(request);
    } while (clock_ms(CLOCK_MONOTONIC) < deadline);
    if (set_result >= 0)
        record(label, "no_set_before_deadline", NULL, 0, 0, duration);
    return 0;
}

static int milliseconds(const char *text, unsigned int *duration)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !text[0] || *end || value > 180000)
        return -1;
    *duration = (unsigned int)value;
    return 0;
}

int main(int count, char **arguments)
{
    if (count != 2) {
        fprintf(stderr, "Usage: wire_probe CASES.tsv\n");
        return 2;
    }
    int lock_descriptor = open("/tmp/bf01-wire-probe.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_descriptor < 0 || flock(lock_descriptor, LOCK_EX | LOCK_NB)) {
        perror("diagnostic lock");
        return 3;
    }
    int descriptor = open("/dev/atbm_ioctl", O_RDWR | O_CLOEXEC);
    if (descriptor < 0) {
        perror("open control");
        close(lock_descriptor);
        return 3;
    }
    FILE *script = fopen(arguments[1], "r");
    if (!script) {
        perror("open cases");
        close(descriptor);
        close(lock_descriptor);
        return 4;
    }
    char *line = NULL;
    size_t capacity = 0;
    int error = 0;
    while (getline(&line, &capacity, script) > 0) {
        line[strcspn(line, "\r\n")] = 0;
        char *operation = strchr(line, '\t');
        char *argument = operation ? strchr(operation + 1, '\t') : NULL;
        if (!operation || !argument) {
            error = 1;
            break;
        }
        *operation++ = 0;
        *argument++ = 0;
        if (!strcmp(operation, "exchange")) {
            error = exchange(descriptor, line, argument);
        } else {
            unsigned int duration = 0;
            if (milliseconds(argument, &duration)) {
                error = 1;
                break;
            }
            int set_result = -1;
            if (!strcmp(operation, "busy-set"))
                set_result = 2;
            else if (!strcmp(operation, "reject-set"))
                set_result = 1;
            else if (strcmp(operation, "listen")) {
                error = 1;
                break;
            }
            error = listen_for(descriptor, line, duration, set_result);
        }
        if (error)
            break;
    }
    if (ferror(script))
        error = 1;
    free(line);
    fclose(script);
    close(descriptor);
    close(lock_descriptor);
    record("suite", "finished", NULL, error ? EIO : 0, 0, 0);
    return error ? 1 : 0;
}
