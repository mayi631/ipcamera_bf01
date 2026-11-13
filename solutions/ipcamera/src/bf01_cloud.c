#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "app_ipcam_event.h"
#include "bf01_cloud.h"

#define CLOUD_RESPONSE_MAX 65536U

struct cloud_connection {
    int descriptor;
    SSL_CTX *context;
    SSL *tls;
    uint64_t deadline;
    int (*cancelled)(void);
};

struct cloud_url {
    int secure;
    char host[256];
    char port[6];
    char authority[264];
    const char *path;
};

static uint64_t cloud_clock(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int clean_header(const char *text)
{
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor; cursor++)
        if (*cursor < 32 || *cursor == 127)
            return 0;
    return 1;
}

static int parse_url(const char *text, struct cloud_url *url)
{
    memset(url, 0, sizeof(*url));
    url->secure = !strncmp(text, "https://", 8);
    if ((!url->secure && strncmp(text, "http://", 7)) || !clean_header(text) || strchr(text, '#'))
        return EINVAL;
    const char *host = text + (url->secure ? 8 : 7);
    const char *end = strchr(host, '/');
    if (!end)
        end = host + strlen(host);
    size_t length = (size_t)(end - host);
    if (!length || length >= sizeof(url->authority) || memchr(host, '@', length) || memchr(host, '?', length))
        return EINVAL;
    memcpy(url->authority, host, length);
    memcpy(url->host, host, length < sizeof(url->host) ? length : 0);
    const char *colon = memchr(host, ':', length);
    size_t host_length = colon ? (size_t)(colon - host) : length;
    if (!host_length || host_length >= sizeof(url->host))
        return EINVAL;
    memcpy(url->host, host, host_length);
    url->host[host_length] = 0;
    strcpy(url->port, url->secure ? "443" : "80");
    if (colon) {
        size_t port_length = (size_t)(end - colon - 1);
        if (!port_length || port_length >= sizeof(url->port))
            return EINVAL;
        memcpy(url->port, colon + 1, port_length);
        url->port[port_length] = 0;
        for (size_t index = 0; index < port_length; index++)
            if (!isdigit((unsigned char)url->port[index]))
                return EINVAL;
        long port = strtol(url->port, NULL, 10);
        if (port < 1 || port > 65535)
            return EINVAL;
    }
    url->path = *end ? end : "/";
    return 0;
}

static int connection_wait(struct cloud_connection *connection, short events)
{
    for (;;) {
        if (connection->cancelled && connection->cancelled())
            return ECANCELED;
        uint64_t now = cloud_clock();
        if (now >= connection->deadline)
            return ETIMEDOUT;
        struct pollfd pending = {connection->descriptor, events, 0};
        int ready = poll(&pending, 1, (int)(connection->deadline - now > 200 ? 200 : connection->deadline - now));
        if (ready < 0 && errno != EINTR)
            return errno;
        if (ready > 0) {
            if (pending.revents & events)
                return 0;
            if (pending.revents & (POLLERR | POLLHUP | POLLNVAL))
                return ECONNRESET;
        }
    }
}

static void connection_close(struct cloud_connection *connection)
{
    SSL_free(connection->tls);
    SSL_CTX_free(connection->context);
    if (connection->descriptor >= 0)
        close(connection->descriptor);
}

static int connection_open(struct cloud_connection *connection, const struct cloud_url *url)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(url->host, url->port, &hints, &addresses))
        return EHOSTUNREACH;
    int error = EHOSTUNREACH;
    for (const struct addrinfo *address = addresses; address; address = address->ai_next) {
        connection->descriptor = socket(address->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (connection->descriptor < 0) {
            error = errno;
            continue;
        }
        error = 0;
        if (connect(connection->descriptor, address->ai_addr, address->ai_addrlen)) {
            error = errno;
            if (error == EINPROGRESS) {
                error = connection_wait(connection, POLLOUT);
                socklen_t size = sizeof(error);
                if (!error && getsockopt(connection->descriptor, SOL_SOCKET, SO_ERROR, &error, &size))
                    error = errno;
            }
        }
        if (!error)
            break;
        close(connection->descriptor);
        connection->descriptor = -1;
    }
    freeaddrinfo(addresses);
    if (error || !url->secure)
        return error;
    connection->context = SSL_CTX_new(TLS_client_method());
    if (!connection->context)
        return ENOMEM;
    SSL_CTX_set_verify(connection->context, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_default_verify_paths(connection->context);
    char bundle[PATH_MAX];
    ssize_t bundle_length = readlink("/proc/self/exe", bundle, sizeof(bundle) - 1);
    if (bundle_length > 0) {
        bundle[bundle_length] = 0;
        char *name = strrchr(bundle, '/');
        if (name && (size_t)(name - bundle) + sizeof("/cloud-ca.pem") < sizeof(bundle)) {
            strcpy(name, "/cloud-ca.pem");
            if (access(bundle, R_OK) == 0 && !SSL_CTX_load_verify_locations(connection->context, bundle, NULL))
                return EACCES;
        }
    }
    SSL_CTX_set_min_proto_version(connection->context, TLS1_2_VERSION);
    connection->tls = SSL_new(connection->context);
    if (!connection->tls)
        return ENOMEM;
    unsigned char address_bytes[16];
    int numeric = inet_pton(AF_INET, url->host, address_bytes) == 1 || inet_pton(AF_INET6, url->host, address_bytes) == 1;
    if (numeric ? !X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(connection->tls), url->host) :
        (!SSL_set_tlsext_host_name(connection->tls, url->host) || !SSL_set1_host(connection->tls, url->host)))
        return EINVAL;
    SSL_set_fd(connection->tls, connection->descriptor);
    for (;;) {
        int connected = SSL_connect(connection->tls);
        if (connected == 1)
            return 0;
        int tls_error = SSL_get_error(connection->tls, connected);
        if (tls_error != SSL_ERROR_WANT_READ && tls_error != SSL_ERROR_WANT_WRITE)
            return EACCES;
        error = connection_wait(connection, tls_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT);
        if (error)
            return error;
    }
}

static int connection_write(struct cloud_connection *connection, const void *buffer, size_t length)
{
    const unsigned char *bytes = buffer;
    while (length) {
        if (connection->cancelled && connection->cancelled())
            return ECANCELED;
        if (cloud_clock() >= connection->deadline)
            return ETIMEDOUT;
        int count = connection->tls ? SSL_write(connection->tls, bytes, (int)length) :
            (int)send(connection->descriptor, bytes, length, MSG_NOSIGNAL);
        if (count > 0) {
            bytes += count;
            length -= (size_t)count;
            continue;
        }
        short events = POLLOUT;
        if (connection->tls) {
            int tls_error = SSL_get_error(connection->tls, count);
            if (tls_error != SSL_ERROR_WANT_READ && tls_error != SSL_ERROR_WANT_WRITE)
                return EIO;
            events = tls_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
        } else if (errno == EINTR)
            continue;
        else if (errno != EAGAIN && errno != EWOULDBLOCK)
            return errno ? errno : EIO;
        int error = connection_wait(connection, events);
        if (error)
            return error;
    }
    return 0;
}

static int connection_read(struct cloud_connection *connection, char *buffer, size_t capacity, int *length)
{
    for (;;) {
        int count = connection->tls ? SSL_read(connection->tls, buffer, (int)capacity) :
            (int)recv(connection->descriptor, buffer, capacity, 0);
        if (count > 0) {
            *length = count;
            return 0;
        }
        short events = POLLIN;
        if (connection->tls) {
            int tls_error = SSL_get_error(connection->tls, count);
            if (tls_error == SSL_ERROR_ZERO_RETURN) {
                *length = 0;
                return 0;
            }
            if (tls_error != SSL_ERROR_WANT_READ && tls_error != SSL_ERROR_WANT_WRITE)
                return EIO;
            events = tls_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
        } else if (!count) {
            *length = 0;
            return 0;
        } else if (errno == EINTR)
            continue;
        else if (errno != EAGAIN && errno != EWOULDBLOCK)
            return errno;
        int error = connection_wait(connection, events);
        if (error)
            return error;
    }
}

static int response_read(struct cloud_connection *connection, int *status, char **body)
{
    *body = NULL;
    char *response = calloc(1, CLOUD_RESPONSE_MAX + 1);
    if (!response)
        return ENOMEM;
    size_t used = 0, header_length = 0, content_length = 0;
    int has_length = 0, chunked = 0, error = 0;
    for (;;) {
        int count = 0;
        if (used == CLOUD_RESPONSE_MAX) {
            error = EOVERFLOW;
            break;
        }
        error = connection_read(connection, response + used, CLOUD_RESPONSE_MAX - used, &count);
        if (error)
            break;
        used += (size_t)count;
        response[used] = 0;
        if (!header_length) {
            char *end = strstr(response, "\r\n\r\n");
            if (end) {
                header_length = (size_t)(end + 4 - response);
                if (sscanf(response, "HTTP/%*u.%*u %d", status) != 1 || *status < 200 || *status > 599) {
                    error = EPROTO;
                    break;
                }
                char *line = strstr(response, "\r\n");
                while (line && line + 2 < end) {
                    line += 2;
                    if (!strncasecmp(line, "Content-Length:", 15)) {
                        char *tail;
                        errno = 0;
                        unsigned long length = strtoul(line + 15, &tail, 10);
                        while (*tail == ' ' || *tail == '\t')
                            tail++;
                        const char *digits = line + 15;
                        while (*digits == ' ' || *digits == '\t')
                            digits++;
                        if (has_length || !isdigit((unsigned char)*digits) || errno ||
                            length > CLOUD_RESPONSE_MAX || strncmp(tail, "\r\n", 2))
                            error = EPROTO;
                        content_length = length;
                        has_length = 1;
                    } else if (!strncasecmp(line, "Transfer-Encoding:", 18)) {
                        const char *encoding = line + 18;
                        while (*encoding == ' ')
                            encoding++;
                        if (strncasecmp(encoding, "chunked\r\n", 9))
                            error = EPROTO;
                        chunked = 1;
                    }
                    line = strstr(line, "\r\n");
                }
                if (has_length && chunked)
                    error = EPROTO;
                if (error)
                    break;
            } else if (used > 16384) {
                error = EOVERFLOW;
                break;
            }
        }
        if (header_length && has_length && used >= header_length + content_length)
            break;
        if (header_length && chunked) {
            size_t offset = header_length;
            int complete = 0;
            while (offset < used) {
                char *end;
                errno = 0;
                unsigned long size = strtoul(response + offset, &end, 16);
                if (end == response + offset || errno || size > CLOUD_RESPONSE_MAX) {
                    error = EPROTO;
                    break;
                }
                if (end + 2 > response + used || strncmp(end, "\r\n", 2))
                    break;
                offset = (size_t)(end + 2 - response);
                if (size > used - offset || used - offset - size < 2)
                    break;
                if (memcmp(response + offset + size, "\r\n", 2)) {
                    error = EPROTO;
                    break;
                }
                offset += size + 2;
                if (!size) {
                    complete = 1;
                    break;
                }
            }
            if (error || complete)
                break;
        }
        if (!count) {
            if (!header_length || chunked || (has_length && used != header_length + content_length))
                error = EPROTO;
            break;
        }
    }
    if (!error) {
        if (chunked) {
            size_t offset = header_length, written = 0;
            for (;;) {
                char *end;
                unsigned long size = strtoul(response + offset, &end, 16);
                offset = (size_t)(end + 2 - response);
                if (!size)
                    break;
                memmove(response + written, response + offset, size);
                written += size;
                offset += size + 2;
            }
            response[written] = 0;
        } else {
            size_t length = has_length ? content_length : used - header_length;
            memmove(response, response + header_length, length);
            response[length] = 0;
        }
        *body = response;
    } else
        free(response);
    return error;
}

static int decode_key(const char *text, unsigned char key[32])
{
    unsigned char decoded[36];
    if (strlen(text) != 44 || text[43] != '=' || EVP_DecodeBlock(decoded, (const unsigned char *)text, 44) != 33)
        return EINVAL;
    memcpy(key, decoded, 32);
    OPENSSL_cleanse(decoded, sizeof(decoded));
    return 0;
}

static int encrypt_json(const char *plain, const unsigned char key[32], char **encoded)
{
    size_t length = strlen(plain);
    if (length > 32768)
        return EOVERFLOW;
    unsigned char *bytes = malloc(length + 28);
    *encoded = malloc(4 * ((length + 30) / 3) + 1);
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    int error = !bytes || !*encoded || !context ? ENOMEM : 0;
    int count = 0, final = 0;
    if (!error && (RAND_bytes(bytes, 12) != 1 ||
        EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), NULL, key, bytes) != 1 ||
        EVP_EncryptUpdate(context, bytes + 12, &count, (const unsigned char *)plain, (int)length) != 1 ||
        EVP_EncryptFinal_ex(context, bytes + 12 + count, &final) != 1 ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, 16, bytes + 12 + count + final) != 1))
        error = EIO;
    if (!error)
        EVP_EncodeBlock((unsigned char *)*encoded, bytes, 12 + count + final + 16);
    EVP_CIPHER_CTX_free(context);
    free(bytes);
    if (error) {
        free(*encoded);
        *encoded = NULL;
    }
    return error;
}

static int decrypt_response(cJSON **response, const unsigned char key[32])
{
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(*response, "data");
    int wrapped = cJSON_IsString(data);
    const cJSON *encrypted = wrapped ? data : cJSON_GetObjectItemCaseSensitive(*response, "encryptedData");
    if (!encrypted)
        return 0;
    if (!cJSON_IsString(encrypted))
        return EPROTO;
    size_t length = strlen(encrypted->valuestring);
    if (!length || length % 4 || length > CLOUD_RESPONSE_MAX)
        return EPROTO;
    unsigned char *bytes = malloc(length), *plain = malloc(length + 1);
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    int error = !bytes || !plain || !context ? ENOMEM : 0;
    int decoded = !error ? EVP_DecodeBlock(bytes, (const unsigned char *)encrypted->valuestring, (int)length) : -1;
    if (decoded > 0) {
        if (encrypted->valuestring[length - 1] == '=')
            decoded--;
        if (encrypted->valuestring[length - 2] == '=')
            decoded--;
    }
    int count = 0, final = 0;
    if (!error && (decoded < 28 || EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), NULL, key, bytes) != 1 ||
        EVP_DecryptUpdate(context, plain, &count, bytes + 12, decoded - 28) != 1 ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, 16, bytes + decoded - 16) != 1 ||
        EVP_DecryptFinal_ex(context, plain + count, &final) != 1))
        error = EBADMSG;
    if (!error) {
        plain[count + final] = 0;
        cJSON *value = cJSON_ParseWithOpts((char *)plain, NULL, 1);
        if (!cJSON_IsObject(value)) {
            cJSON_Delete(value);
            error = EPROTO;
        } else if (wrapped) {
            if (!cJSON_ReplaceItemInObjectCaseSensitive(*response, "data", value)) {
                cJSON_Delete(value);
                error = ENOMEM;
            }
        } else {
            cJSON_Delete(*response);
            *response = value;
        }
    }
    EVP_CIPHER_CTX_free(context);
    free(bytes);
    free(plain);
    return error;
}

static void log_http_failure(const char *method, int status, const char *reply)
{
    cJSON *details = cJSON_CreateObject();
    if (!details)
        return;
    cJSON_AddStringToObject(details, "method", method);
    cJSON_AddNumberToObject(details, "status", status);
    if (reply)
        cJSON_AddStringToObject(details, "body", reply);
    else
        cJSON_AddNullToObject(details, "body");
    char *text = cJSON_PrintUnformatted(details);
    if (text)
        fprintf(stderr, "[cloud-http] error_response=%s\n", text);
    cJSON_free(text);
    cJSON_Delete(details);
}

int bf_cloud_post(const cJSON *credentials, const char *endpoint, const cJSON *plain,
    cJSON **response, int *http_status)
{
    *response = NULL;
    *http_status = 0;
    const char *base = app_event_text(credentials, "baseUrl");
    char url_text[1024];
    if (snprintf(url_text, sizeof(url_text), "%s%s", base, endpoint) >= (int)sizeof(url_text))
        return EOVERFLOW;
    struct cloud_url url;
    int error = parse_url(url_text, &url);
    unsigned char sign_key[32] = {0}, aes_key[32] = {0}, random[16], signature[32];
    char nonce[33], timestamp[32], sign_hex[65];
    char *json = cJSON_PrintUnformatted(plain), *encrypted = NULL, *body = NULL, *headers = NULL, *reply = NULL;
    cJSON *envelope = cJSON_CreateObject();
    if (!error && (!json || !envelope))
        error = ENOMEM;
    if (!error)
        error = decode_key(app_event_text(credentials, "httpHmacSignKey"), sign_key);
    if (!error)
        error = decode_key(app_event_text(credentials, "httpAesKey"), aes_key);
    if (!error && RAND_bytes(random, sizeof(random)) != 1)
        error = EIO;
    if (!error) {
        for (size_t index = 0; index < sizeof(random); index++)
            snprintf(nonce + index * 2, 3, "%02x", random[index]);
        snprintf(timestamp, sizeof(timestamp), "%llu", (unsigned long long)app_event_realtime());
        char *signing = NULL;
        if (asprintf(&signing, "%s%s%s", json, nonce, timestamp) < 0)
            error = ENOMEM;
        else {
            unsigned int size = 0;
            if (!HMAC(EVP_sha256(), sign_key, 32, (unsigned char *)signing, strlen(signing), signature, &size) || size != 32)
                error = EIO;
            free(signing);
            for (size_t index = 0; !error && index < 32; index++)
                snprintf(sign_hex + index * 2, 3, "%02x", signature[index]);
        }
    }
    if (!error)
        error = encrypt_json(json, aes_key, &encrypted);
    if (!error) {
        cJSON_AddStringToObject(envelope, "encryptedData", encrypted);
        body = cJSON_PrintUnformatted(envelope);
        if (!body)
            error = ENOMEM;
    }
    const char *product = app_event_text(credentials, "productCode"), *serial = app_event_text(credentials, "deviceSn");
    const char *version = app_event_text(credentials, "version"), *token = app_event_text(credentials, "accessToken");
    if (!error && (!*product || !*serial || !*version || !*token || !clean_header(product) ||
        !clean_header(serial) || !clean_header(version) || !clean_header(token)))
        error = EINVAL;
    if (!error && asprintf(&headers,
        "POST %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nContent-Type: application/json\r\n"
        "X-Device-ProductCode: %s\r\nX-Device-SN: %s\r\nX-Device-Version: %s\r\n"
        "X-Device-Timestamp: %s\r\nX-Device-Nonce: %s\r\nX-Device-Sign: %s\r\n"
        "Authorization: Bearer %s\r\nContent-Length: %zu\r\n\r\n",
        url.path, url.authority, product, serial, version, timestamp, nonce, sign_hex, token, strlen(body)) < 0)
        error = ENOMEM;
    struct cloud_connection connection = {.descriptor = -1, .deadline = cloud_clock() + 30000};
    if (!error)
        error = connection_open(&connection, &url);
    if (!error)
        error = connection_write(&connection, headers, strlen(headers));
    if (!error)
        error = connection_write(&connection, body, strlen(body));
    if (!error)
        error = response_read(&connection, http_status, &reply);
    if (!error && (*http_status < 200 || *http_status >= 300))
        error = *http_status == 401 ? EKEYEXPIRED : EREMOTEIO;
    if (*http_status >= 300)
        log_http_failure("POST", *http_status, reply);
    if (!error) {
        *response = cJSON_ParseWithOpts(reply, NULL, 1);
        error = cJSON_IsObject(*response) ? decrypt_response(response, aes_key) : EPROTO;
    }
    fprintf(stderr, "[cloud-http] endpoint=%s http=%d errno=%d\n", endpoint, *http_status, error);
    connection_close(&connection);
    OPENSSL_cleanse(sign_key, sizeof(sign_key));
    OPENSSL_cleanse(aes_key, sizeof(aes_key));
    cJSON_free(json);
    free(encrypted);
    cJSON_free(body);
    free(headers);
    free(reply);
    cJSON_Delete(envelope);
    return error;
}

static int put_body(struct cloud_connection *connection, EVP_MD_CTX *digest,
    const unsigned char *bytes, size_t length)
{
    int error = connection_write(connection, bytes, length);
    if (!error && EVP_DigestUpdate(digest, bytes, length) != 1)
        error = EIO;
    return error;
}

int bf_cloud_put_sha256(const cJSON *prepared, const char *path, unsigned long long *uploaded,
    char sha256[65], int *http_status, int (*cancelled)(void))
{
    *uploaded = 0;
    *http_status = 0;
    sha256[0] = 0;
    if (strcmp(app_event_text(prepared, "uploadMethod"), "PUT"))
        return ENOTSUP;
    const cJSON *enc = cJSON_GetObjectItemCaseSensitive(prepared, "enc");
    if (!cJSON_IsBool(enc))
        return EPROTO;
    int encrypted = cJSON_IsTrue(enc);
    static const unsigned char file_header[] = {'K', 'L', 'F', '1', 1, 1, 12};
    unsigned char key[32] = {0}, iv[12], input[32768], output[32784];
    int error = encrypted && strcmp(app_event_text(prepared, "encType"), "AES_256_GCM") ? ENOTSUP : 0;
    if (!error && encrypted)
        error = decode_key(app_event_text(prepared, "fileEncryptKey"), key);
    struct cloud_url url;
    if (!error)
        error = parse_url(app_event_text(prepared, "uploadUrl"), &url);
    int descriptor = !error ? open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK) : -1;
    struct stat before = {0}, after;
    if (!error && (descriptor < 0 || fstat(descriptor, &before)))
        error = errno;
    if (!error && (!S_ISREG(before.st_mode) || before.st_size <= 0 || before.st_size > INT_MAX))
        error = EINVAL;
    unsigned long long size = (unsigned long long)before.st_size + (encrypted ? sizeof(file_header) + 28 : 0);
    char headers[16384];
    int length = !error ? snprintf(headers, sizeof(headers),
        "PUT %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nContent-Length: %llu\r\n", url.path, url.authority, size) : 0;
    if (!error && (length < 0 || (size_t)length >= sizeof(headers)))
        error = EOVERFLOW;
    const cJSON *extra = cJSON_GetObjectItemCaseSensitive(prepared, "headers"), *field;
    if (!error && !cJSON_IsObject(extra))
        error = EPROTO;
    cJSON_ArrayForEach(field, extra) {
        if (error)
            break;
        if (!field->string || !*field->string || !clean_header(field->string) || strchr(field->string, ':') ||
            !cJSON_IsString(field) || !clean_header(field->valuestring) ||
            !strcasecmp(field->string, "Authorization") || !strcasecmp(field->string, "Host") ||
            !strcasecmp(field->string, "Content-Length") || !strcasecmp(field->string, "Transfer-Encoding") ||
            !strcasecmp(field->string, "Connection")) {
            error = EPROTO;
            break;
        }
        int appended = snprintf(headers + length, sizeof(headers) - (size_t)length, "%s: %s\r\n", field->string, field->valuestring);
        if (appended < 0 || (size_t)appended >= sizeof(headers) - (size_t)length)
            error = EOVERFLOW;
        else
            length += appended;
    }
    if (!error && (size_t)length + 3 >= sizeof(headers))
        error = EOVERFLOW;
    if (!error) {
        memcpy(headers + length, "\r\n", 3);
        length += 2;
    }
    EVP_CIPHER_CTX *cipher = encrypted ? EVP_CIPHER_CTX_new() : NULL;
    if (!error && encrypted && (!cipher || RAND_bytes(iv, sizeof(iv)) != 1 ||
        EVP_EncryptInit_ex(cipher, EVP_aes_256_gcm(), NULL, key, iv) != 1))
        error = EIO;
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    if (!error && (!digest || EVP_DigestInit_ex(digest, EVP_sha256(), NULL) != 1))
        error = EIO;
    struct cloud_connection connection = {.descriptor = -1, .deadline = cloud_clock() + 120000, .cancelled = cancelled};
    if (!error)
        error = connection_open(&connection, &url);
    if (!error)
        error = connection_write(&connection, headers, (size_t)length);
    if (!error && encrypted)
        error = put_body(&connection, digest, file_header, sizeof(file_header));
    if (!error && encrypted)
        error = put_body(&connection, digest, iv, sizeof(iv));
    off_t consumed = 0;
    while (!error && consumed < before.st_size) {
        size_t remaining = (size_t)(before.st_size - consumed);
        ssize_t count = read(descriptor, input, remaining < sizeof(input) ? remaining : sizeof(input));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            error = count < 0 ? errno : EIO;
            break;
        }
        consumed += count;
        int encoded = (int)count;
        if (encrypted && EVP_EncryptUpdate(cipher, output, &encoded, input, (int)count) != 1)
            error = EIO;
        if (!error)
            error = put_body(&connection, digest, encrypted ? output : input, (size_t)encoded);
    }
    if (!error && (fstat(descriptor, &after) || before.st_size != after.st_size || before.st_mtime != after.st_mtime))
        error = ESTALE;
    if (!error && encrypted) {
        int final = 0;
        if (EVP_EncryptFinal_ex(cipher, output, &final) != 1 ||
            EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_GET_TAG, 16, output + final) != 1)
            error = EIO;
        else
            error = put_body(&connection, digest, output, (size_t)final + 16);
    }
    unsigned char hash[32];
    unsigned int hash_length = 0;
    if (!error && (EVP_DigestFinal_ex(digest, hash, &hash_length) != 1 || hash_length != sizeof(hash)))
        error = EIO;
    char *reply = NULL;
    if (!error)
        error = response_read(&connection, http_status, &reply);
    if (!error && (*http_status < 200 || *http_status >= 300))
        error = EREMOTEIO;
    if (*http_status >= 300)
        log_http_failure("PUT", *http_status, reply);
    if (!error) {
        *uploaded = size;
        for (size_t index = 0; index < sizeof(hash); index++)
            snprintf(sha256 + index * 2, 3, "%02x", hash[index]);
    }
    fprintf(stderr, "[cloud-http] PUT http=%d errno=%d bytes=%llu\n", *http_status, error, *uploaded);
    free(reply);
    connection_close(&connection);
    EVP_CIPHER_CTX_free(cipher);
    EVP_MD_CTX_free(digest);
    OPENSSL_cleanse(key, sizeof(key));
    if (descriptor >= 0)
        close(descriptor);
    return error;
}

int bf_cloud_put(const cJSON *prepared, const char *path, unsigned long long *uploaded,
    int *http_status, int (*cancelled)(void))
{
    char sha256[65];
    return bf_cloud_put_sha256(prepared, path, uploaded, sha256, http_status, cancelled);
}
