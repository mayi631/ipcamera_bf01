#define _GNU_SOURCE
#include "app_ipcam_boot_ai.h"

#ifdef BF01_BOOT_CAPTURE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "bf01_boot_ai.h"
#include "cvi_ipcm.h"
#include "cvi_sys.h"
#include "cviruntime.h"
#include "minIni.h"

#define AI_RUN_DIR "/run/ipcamera-ai"
#define AI_CONFIG "/mnt/cfg/ipcamera/boot_ai.ini"
#define AI_MIN_ION_BYTES 36745216U
#define AI_TOTAL_TIMEOUT_MS 28000U

#ifndef BF01_BOOT_AI_TEST_INPUT
#define BF01_BOOT_AI_TEST_INPUT 0
#endif
#if BF01_BOOT_AI_TEST_INPUT != 0 && BF01_BOOT_AI_TEST_INPUT != 1
#error BF01_BOOT_AI_TEST_INPUT must be 0 or 1
#endif
#if BF01_BOOT_AI_TEST_INPUT
#define AI_INPUT_SOURCE "file_test"
#define AI_CONTENT_TOP 0
#define AI_CONTENT_BOTTOM 640
#else
#define AI_INPUT_SOURCE "vpss_1_1"
#define AI_CONTENT_TOP 80
#define AI_CONTENT_BOTTOM 560
#endif

extern char **environ;

struct ai_job {
    struct bf01_ai_reply frame;
    char model[512];
    float confidence;
    int32_t class_id;
    uint32_t timeout_ms;
#if BF01_BOOT_AI_TEST_INPUT
    char test_image[512];
#endif
};

struct ai_result {
    uint32_t magic;
    int32_t error;
    int32_t passed;
    int32_t class_id;
    float score;
    uint32_t forward_us;
};

struct ai_runtime {
    CVI_RC (*register_model)(const char *, CVI_MODEL_HANDLE *);
    CVI_RC (*get_tensors)(CVI_MODEL_HANDLE, CVI_TENSOR **, int32_t *, CVI_TENSOR **, int32_t *);
    CVI_RC (*forward)(CVI_MODEL_HANDLE, CVI_TENSOR *, int32_t, CVI_TENSOR *, int32_t);
    CVI_RC (*cleanup)(CVI_MODEL_HANDLE);
    void *(*tensor_ptr)(CVI_TENSOR *);
    size_t (*tensor_size)(CVI_TENSOR *);
    CVI_SHAPE (*tensor_shape)(CVI_TENSOR *);
};

static pthread_mutex_t reply_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reply_condition = PTHREAD_COND_INITIALIZER;
static struct bf01_ai_reply response;
static uint32_t sequence;
static int received;
static int ai_allowed;

static uint64_t monotonic_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static int interrupted(void)
{
    sigset_t pending;
    if (sigpending(&pending) != 0)
        return 1;
    return sigismember(&pending, SIGINT) || sigismember(&pending, SIGTERM);
}

int app_ipcam_BootAi_Allowed(void)
{
    return ai_allowed;
}

static int receive_reply(void *private_data, IPCM_ANON_MSG_S *message)
{
    (void)private_data;
    if (!message || message->u8PortID != BF01_AI_PORT || message->u8MsgID != BF01_AI_VERSION ||
        message->u8DataType != IPCM_MSG_TYPE_SHM || message->stData.u32Size != sizeof(response) ||
        !message->stData.pData)
        return -EINVAL;
    struct bf01_ai_reply incoming;
    memcpy(&incoming, message->stData.pData, sizeof(incoming));
    if (incoming.magic != BF01_AI_MAGIC || incoming.version != BF01_AI_VERSION)
        return -EPROTO;
    pthread_mutex_lock(&reply_mutex);
    if (incoming.sequence == sequence) {
        response = incoming;
        received = 1;
        pthread_cond_signal(&reply_condition);
    }
    pthread_mutex_unlock(&reply_mutex);
    return 0;
}

static int exchange(uint32_t command, uint32_t image_paddr, struct bf01_ai_reply *reply)
{
    struct bf01_ai_request *request = CVI_IPCM_GetBuff(sizeof(*request));
    if (!request)
        return ENOMEM;
    pthread_mutex_lock(&reply_mutex);
    sequence++;
    received = 0;
    *request = (struct bf01_ai_request) {
        .magic = BF01_AI_MAGIC, .version = BF01_AI_VERSION, .sequence = sequence,
        .command = command, .image_paddr = image_paddr,
    };
    pthread_mutex_unlock(&reply_mutex);
    if (CVI_IPCM_AnonSendMsg(BF01_AI_PORT, BF01_AI_VERSION, request, sizeof(*request)) < 0) {
        CVI_IPCM_ReleaseBuff(request);
        return EIO;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 200000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&reply_mutex);
    int error = 0;
    while (!received && !error)
        error = pthread_cond_timedwait(&reply_condition, &reply_mutex, &deadline);
    if (received) {
        *reply = response;
        error = reply->error > 0 ? reply->error : (reply->error ? EIO : 0);
    }
    pthread_mutex_unlock(&reply_mutex);
    return error;
}

static int executable_directory(char *directory, size_t capacity)
{
    ssize_t length = readlink("/proc/self/exe", directory, capacity - 1);
    if (length <= 0 || (size_t)length >= capacity - 1)
        return ENAMETOOLONG;
    directory[length] = '\0';
    char *name = strrchr(directory, '/');
    if (!name)
        return EINVAL;
    *name = '\0';
    return 0;
}

static int setting_integer(const char *config, const char *name, const char *fallback, long *value)
{
    char text[64];
    char *end;
    ini_gets("boot_ai", name, fallback, text, sizeof(text), config);
    errno = 0;
    *value = strtol(text, &end, 10);
    return errno || end == text || *end ? EINVAL : 0;
}

static int load_job(struct ai_job *job)
{
    char directory[PATH_MAX];
    char config[PATH_MAX];
    int error = executable_directory(directory, sizeof(directory));
    if (error)
        return error;
    if (access(AI_CONFIG, F_OK) == 0) {
        strcpy(config, AI_CONFIG);
    } else {
        if (errno != ENOENT)
            return errno;
        int length = snprintf(config, sizeof(config), "%s/boot_ai.ini", directory);
        if (length < 0 || (size_t)length >= sizeof(config))
            return ENAMETOOLONG;
    }
    if (access(config, R_OK) != 0)
        return errno;
    char model[PATH_MAX];
    char confidence[64];
    ini_gets("boot_ai", "model", "yolov5s_cv181x_bf16.cvimodel", model, sizeof(model), config);
    ini_gets("boot_ai", "confidence", "0.25", confidence, sizeof(confidence), config);
    char *end;
    errno = 0;
    job->confidence = strtof(confidence, &end);
    if (errno || end == confidence || *end || !isfinite(job->confidence) ||
        job->confidence <= 0 || job->confidence > 1)
        return EINVAL;
    long class_id, timeout_ms;
    if (setting_integer(config, "class_id", "-1", &class_id) ||
        setting_integer(config, "timeout_ms", "12000", &timeout_ms) ||
        class_id < -1 || class_id > 1 || timeout_ms < 1000 || timeout_ms > 20000)
        return EINVAL;
    job->class_id = (int32_t)class_id;
    job->timeout_ms = (uint32_t)timeout_ms;
    int length = model[0] == '/' ? snprintf(job->model, sizeof(job->model), "%s", model) :
        snprintf(job->model, sizeof(job->model), "%s/%s", directory, model);
    if (length < 0 || (size_t)length >= sizeof(job->model))
        return ENAMETOOLONG;
    struct stat info;
    if (stat(job->model, &info) != 0)
        return errno;
    if (!S_ISREG(info.st_mode) || info.st_size <= 0)
        return EINVAL;
#if BF01_BOOT_AI_TEST_INPUT
    char image[PATH_MAX];
    ini_gets("boot_ai", "test_image", "boot_ai_test/input_1.rgbp", image, sizeof(image), config);
    if (!image[0])
        return EINVAL;
    length = image[0] == '/' ? snprintf(job->test_image, sizeof(job->test_image), "%s", image) :
        snprintf(job->test_image, sizeof(job->test_image), "%s/%s", directory, image);
    if (length < 0 || (size_t)length >= sizeof(job->test_image))
        return ENAMETOOLONG;
    fprintf(stderr, "[boot-ai] TEST INPUT ENABLED: %s; real TPU decision controls storage\n", job->test_image);
#endif
    return 0;
}

static int validate_frame(const struct bf01_ai_reply *frame)
{
    uint64_t rtos_end = (uint64_t)frame->rtos_base + frame->rtos_size;
    uint64_t image_end = (uint64_t)frame->image_paddr + frame->image_size;
    uint64_t ion_end = (uint64_t)frame->ion_base + frame->ion_size;
    if (frame->magic != BF01_AI_MAGIC || frame->version != BF01_AI_VERSION ||
        frame->state != BF01_AI_READY || frame->width != BF01_AI_WIDTH ||
        frame->height != BF01_AI_HEIGHT || frame->image_size != BF01_AI_IMAGE_SIZE ||
        !frame->rtos_base || !frame->rtos_size || rtos_end > (uint64_t)UINT32_MAX + 1 ||
        frame->image_paddr < frame->rtos_base || image_end > rtos_end ||
        !frame->ion_base || !frame->ion_size || frame->rtos_ion_size >= frame->ion_size ||
        ion_end != frame->rtos_base)
        return EPROTO;
    return 0;
}

static int read_counter(const char *name, uint64_t *value)
{
    char path[192];
    int length = snprintf(path, sizeof(path), "/sys/kernel/debug/ion/cvi_carveout_heap_dump/%s", name);
    if (length < 0 || (size_t)length >= sizeof(path))
        return ENAMETOOLONG;
    FILE *input = fopen(path, "r");
    if (!input)
        return errno;
    unsigned long long counter;
    int error = fscanf(input, "%llu", &counter) == 1 ? 0 : EIO;
    fclose(input);
    if (!error)
        *value = counter;
    return error;
}

static int check_pool(FILE *summary, const struct bf01_ai_reply *frame, uint64_t total, uint64_t allocated)
{
    uint64_t allowed_start = (uint64_t)frame->ion_base + frame->rtos_ion_size;
    uint64_t allowed_end = (uint64_t)frame->ion_base + frame->ion_size;
    if (total > frame->ion_size - frame->rtos_ion_size) {
        fprintf(stderr, "[boot-ai] unsafe ION pool: RTOS reservation is not excluded; inference denied\n");
        return EACCES;
    }
    if (!total || allocated > total || total - allocated < AI_MIN_ION_BYTES)
        return ENOMEM;
    char line[256];
    int bitmap = 0;
    uint64_t free_bytes = 0;
    while (fgets(line, sizeof(line), summary)) {
        if (strstr(line, "free memory regions:")) {
            bitmap = 1;
            continue;
        }
        unsigned int heap;
        unsigned long long start, end, length;
        if (bitmap && sscanf(line, "%u %llx %llx %llu", &heap, &start, &end, &length) == 4) {
            if (heap != 0 || start < allowed_start || end > allowed_end || end <= start ||
                end - start != length || length > total || free_bytes > total - length)
                return EACCES;
            free_bytes += length;
        }
    }
    if (ferror(summary))
        return EIO;
    if (bitmap && free_bytes != total - allocated)
        return EACCES;
    return bitmap && free_bytes >= AI_MIN_ION_BYTES ? 0 : ENOMEM;
}

static int preflight(const struct bf01_ai_reply *frame)
{
    int error = validate_frame(frame);
    if (error)
        return error;
    uint64_t total, allocated;
    error = read_counter("total_mem", &total);
    if (!error)
        error = read_counter("alloc_mem", &allocated);
    if (error)
        return error;
    FILE *summary = fopen("/sys/kernel/debug/ion/cvi_carveout_heap_dump/summary", "r");
    if (!summary)
        return errno;
    error = check_pool(summary, frame, total, allocated);
    fclose(summary);
    if (!error && (access("/dev/cvi-tpu0", R_OK | W_OK) != 0 || access("/dev/ion", R_OK | W_OK) != 0))
        error = errno;
    return error;
}

static void select_candidate(const float *output, const struct ai_job *job, struct ai_result *result)
{
    result->class_id = -1;
    for (unsigned int row = 0; row < 25200; row++) {
        const float *prediction = output + row * 7;
        int valid = 1;
        for (unsigned int attribute = 0; attribute < 7; attribute++)
            if (!isfinite(prediction[attribute]))
                valid = 0;
        if (!valid || prediction[2] <= 0 || prediction[3] <= 0 ||
            prediction[0] < 0 || prediction[0] >= 640 ||
            prediction[1] < AI_CONTENT_TOP || prediction[1] >= AI_CONTENT_BOTTOM ||
            prediction[4] < 0 || prediction[4] > 1 || prediction[5] < 0 || prediction[5] > 1 ||
            prediction[6] < 0 || prediction[6] > 1)
            continue;
        int class_id = prediction[6] > prediction[5] ? 1 : 0;
        float score = prediction[4] * prediction[5 + class_id];
        if ((job->class_id == -1 || job->class_id == class_id) && score >= job->confidence &&
            (!result->passed || score > result->score)) {
            result->passed = 1;
            result->class_id = class_id;
            result->score = score;
        }
    }
}

#if BF01_BOOT_AI_TEST_INPUT
static int read_test_input(const char *path, float *input)
{
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0)
        return errno;
    struct stat info;
    int error = fstat(descriptor, &info) == 0 ? 0 : errno;
    if (!error && (!S_ISREG(info.st_mode) || info.st_size != BF01_AI_IMAGE_SIZE))
        error = EINVAL;
    unsigned char bytes[4096];
    size_t offset = 0;
    while (!error && offset < BF01_AI_IMAGE_SIZE) {
        size_t remaining = BF01_AI_IMAGE_SIZE - offset;
        ssize_t count = read(descriptor, bytes, remaining < sizeof(bytes) ? remaining : sizeof(bytes));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            error = count < 0 ? errno : EIO;
            break;
        }
        for (size_t index = 0; index < (size_t)count; index++)
            input[offset + index] = bytes[index] / 255.0f;
        offset += (size_t)count;
    }
    if (close(descriptor) != 0 && !error)
        error = errno;
    return error;
}
#endif

static int infer_frame(const struct ai_job *job, struct ai_result *result)
{
    int error = preflight(&job->frame);
    if (error)
        return error;
    void *library = dlopen("libcviruntime.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "[boot-ai] runtime load failed: %s\n", dlerror());
        return ENOENT;
    }
    struct ai_runtime runtime = {
        .register_model = dlsym(library, "CVI_NN_RegisterModel"),
        .get_tensors = dlsym(library, "CVI_NN_GetInputOutputTensors"),
        .forward = dlsym(library, "CVI_NN_Forward"),
        .cleanup = dlsym(library, "CVI_NN_CleanupModel"),
        .tensor_ptr = dlsym(library, "CVI_NN_TensorPtr"),
        .tensor_size = dlsym(library, "CVI_NN_TensorSize"),
        .tensor_shape = dlsym(library, "CVI_NN_TensorShape"),
    };
    CVI_MODEL_HANDLE model = NULL;
#if !BF01_BOOT_AI_TEST_INPUT
    unsigned char *image = NULL;
#endif
    if (!runtime.register_model || !runtime.get_tensors || !runtime.forward || !runtime.cleanup ||
        !runtime.tensor_ptr || !runtime.tensor_size || !runtime.tensor_shape) {
        error = ENOSYS;
        goto done;
    }
    if (runtime.register_model(job->model, &model) != 0) {
        error = EIO;
        goto done;
    }
    CVI_TENSOR *inputs = NULL;
    CVI_TENSOR *outputs = NULL;
    int32_t input_count = 0, output_count = 0;
    if (runtime.get_tensors(model, &inputs, &input_count, &outputs, &output_count) != 0 ||
        input_count != 1 || output_count != 1 || !inputs || !outputs) {
        error = EPROTO;
        goto done;
    }
    CVI_SHAPE input_shape = runtime.tensor_shape(inputs);
    CVI_SHAPE output_shape = runtime.tensor_shape(outputs);
    if (inputs[0].fmt != CVI_FMT_FP32 || outputs[0].fmt != CVI_FMT_FP32 ||
        input_shape.dim_size != 4 || input_shape.dim[0] != 1 || input_shape.dim[1] != 3 ||
        input_shape.dim[2] != 640 || input_shape.dim[3] != 640 ||
        (output_shape.dim_size != 3 && output_shape.dim_size != 4) ||
        output_shape.dim[0] != 1 || output_shape.dim[1] != 25200 || output_shape.dim[2] != 7 ||
        (output_shape.dim_size == 4 && output_shape.dim[3] != 1) ||
        runtime.tensor_size(inputs) < BF01_AI_IMAGE_SIZE * sizeof(float) ||
        runtime.tensor_size(outputs) < 25200U * 7 * sizeof(float)) {
        error = EPROTO;
        goto done;
    }
    float *input = runtime.tensor_ptr(inputs);
    float *output = runtime.tensor_ptr(outputs);
    if (!input || !output) {
        error = ENXIO;
        goto done;
    }
#if BF01_BOOT_AI_TEST_INPUT
    error = read_test_input(job->test_image, input);
    if (error)
        goto done;
#else
    image = CVI_SYS_Mmap(job->frame.image_paddr, job->frame.image_size);
    if (!image) {
        error = ENXIO;
        goto done;
    }
    for (unsigned int index = 0; index < BF01_AI_IMAGE_SIZE; index++)
        input[index] = image[index] / 255.0f;
#endif
    uint64_t started = monotonic_us();
    if (runtime.forward(model, inputs, input_count, outputs, output_count) != 0) {
        error = EIO;
        goto done;
    }
    result->forward_us = (uint32_t)(monotonic_us() - started);
    select_candidate(output, job, result);
done:
#if !BF01_BOOT_AI_TEST_INPUT
    if (image && CVI_SYS_Munmap(image, job->frame.image_size) != CVI_SUCCESS)
        error = EIO;
#endif
    if (model && runtime.cleanup(model) != 0)
        error = EIO;
    dlclose(library);
    return error;
}

static int transfer(int descriptor, void *buffer, size_t length, int writing)
{
    sigset_t blocked, previous, pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    int error = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (error)
        return error;
    sigpending(&pending);
    int already_pending = sigismember(&pending, SIGPIPE);
    unsigned char *bytes = buffer;
    while (length) {
        ssize_t count = writing ? write(descriptor, bytes, length) : read(descriptor, bytes, length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            error = count < 0 ? errno : EPIPE;
            break;
        }
        bytes += count;
        length -= (size_t)count;
    }
    if (writing && error == EPIPE && !already_pending) {
        struct timespec immediate = {0};
        while (sigtimedwait(&blocked, NULL, &immediate) < 0 && errno == EINTR) {}
    }
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return error;
}

int app_ipcam_BootAi_Worker(void)
{
    struct rlimit core_limit = {0, 0};
    setrlimit(RLIMIT_CORE, &core_limit);
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() == 1)
        return EXIT_FAILURE;
    struct ai_job job;
    struct ai_result result = {.magic = BF01_AI_MAGIC, .class_id = -1};
    int error = transfer(3, &job, sizeof(job), 0);
    close(3);
    if (error)
        return EXIT_FAILURE;
    if (!memchr(job.model, '\0', sizeof(job.model)) || !isfinite(job.confidence) ||
        job.confidence <= 0 || job.confidence > 1 || job.class_id < -1 || job.class_id > 1
#if BF01_BOOT_AI_TEST_INPUT
        || !memchr(job.test_image, '\0', sizeof(job.test_image)) || !job.test_image[0]
#endif
        )
        result.error = EINVAL;
    else
        result.error = infer_frame(&job, &result);
    error = transfer(4, &result, sizeof(result), 1);
    close(4);
    return error ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int create_pipe(int descriptors[2])
{
    int temporary[2];
    if (pipe2(temporary, O_CLOEXEC) != 0)
        return errno;
    descriptors[0] = fcntl(temporary[0], F_DUPFD_CLOEXEC, 10);
    descriptors[1] = fcntl(temporary[1], F_DUPFD_CLOEXEC, 10);
    int error = descriptors[0] < 0 || descriptors[1] < 0 ? errno : 0;
    close(temporary[0]);
    close(temporary[1]);
    if (error) {
        if (descriptors[0] >= 0)
            close(descriptors[0]);
        if (descriptors[1] >= 0)
            close(descriptors[1]);
    }
    return error;
}

static int run_worker(struct ai_job *job, struct ai_result *result, uint64_t deadline_us)
{
    int job_pipe[2], result_pipe[2];
    int error = create_pipe(job_pipe);
    if (error)
        return error;
    error = create_pipe(result_pipe);
    if (error) {
        close(job_pipe[0]);
        close(job_pipe[1]);
        return error;
    }
    posix_spawn_file_actions_t actions;
    error = posix_spawn_file_actions_init(&actions);
    if (!error) {
        error = posix_spawn_file_actions_adddup2(&actions, job_pipe[0], 3);
        if (!error)
            error = posix_spawn_file_actions_adddup2(&actions, result_pipe[1], 4);
        pid_t child = -1;
        char *arguments[] = {"ipcamera", "--boot-ai-worker", NULL};
        if (!error)
            error = posix_spawn(&child, "/proc/self/exe", &actions, NULL, arguments, environ);
        posix_spawn_file_actions_destroy(&actions);
        close(job_pipe[0]);
        close(result_pipe[1]);
        if (!error) {
            error = transfer(job_pipe[1], job, sizeof(*job), 1);
            close(job_pipe[1]);
            uint64_t worker_deadline = monotonic_us() + (uint64_t)job->timeout_ms * 1000;
            if (worker_deadline > deadline_us)
                worker_deadline = deadline_us;
            int status = 0;
            while (!error) {
                pid_t waited = waitpid(child, &status, WNOHANG);
                if (waited == child)
                    break;
                if (waited < 0 && errno != EINTR) {
                    error = errno;
                    break;
                }
                if (interrupted())
                    error = ECANCELED;
                else if (monotonic_us() >= worker_deadline)
                    error = ETIMEDOUT;
                else
                    usleep(10000);
            }
            if (error) {
                kill(child, SIGKILL);
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                error = EIO;
            } else {
                error = transfer(result_pipe[0], result, sizeof(*result), 0);
                if (!error && (result->magic != BF01_AI_MAGIC || result->error < 0 ||
                    (result->passed != 0 && result->passed != 1)))
                    error = EPROTO;
                if (!error)
                    error = result->error;
                if (!error && result->passed && (!isfinite(result->score) ||
                    result->score < job->confidence || result->score > 1 ||
                    result->class_id < 0 || result->class_id > 1 ||
                    (job->class_id != -1 && result->class_id != job->class_id)))
                    error = EPROTO;
            }
        } else {
            close(job_pipe[1]);
        }
    } else {
        close(job_pipe[0]);
        close(job_pipe[1]);
        close(result_pipe[1]);
    }
    close(result_pipe[0]);
    return error;
}

static void report(const char *state, int error, const struct ai_result *result, const struct ai_job *job)
{
    (void)job;
    fprintf(stderr, "[boot-ai] source=%s state=%s error=%d class=%d score=%.4f forward_ms=%.3f\n",
        AI_INPUT_SOURCE, state, error, result->class_id, result->score, result->forward_us / 1000.0);
    if (mkdir(AI_RUN_DIR, 0700) != 0 && errno != EEXIST)
        return;
    int descriptor = open(AI_RUN_DIR "/status.part", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0)
        return;
    int written = dprintf(descriptor,
        "[boot_ai]\ninput_source=%s\nstate=%s\nerror=%d\nclass_id=%d\nscore=%.6f\nforward_us=%u\nstorage_allowed=%d\n",
        AI_INPUT_SOURCE, state, error, result->class_id, result->score, result->forward_us, ai_allowed);
#if BF01_BOOT_AI_TEST_INPUT
    if (written >= 0)
        written = dprintf(descriptor, "test_image=%s\n", job->test_image);
#endif
    int closed = close(descriptor);
    if (written >= 0 && closed == 0)
        rename(AI_RUN_DIR "/status.part", AI_RUN_DIR "/status.ini");
}

int app_ipcam_BootAi_Run(void)
{
    struct ai_job job = {0};
    struct ai_result result = {.class_id = -1};
    ai_allowed = 0;
    int error = CVI_IPCM_RegisterAnonHandle(BF01_AI_PORT, receive_reply, NULL);
    if (error) {
        report("failed", EIO, &result, &job);
        return EIO;
    }
    uint64_t deadline = monotonic_us() + (uint64_t)AI_TOTAL_TIMEOUT_MS * 1000;
    report("waiting_for_frame", 0, &result, &job);
    error = load_job(&job);
    while (!error) {
        if (interrupted()) {
            error = ECANCELED;
            break;
        }
        if (monotonic_us() >= deadline) {
            error = ETIMEDOUT;
            break;
        }
        error = exchange(BF01_AI_STATUS, 0, &job.frame);
        if (error && job.frame.magic == BF01_AI_MAGIC &&
            (job.frame.state == BF01_AI_FAILED || job.frame.state == BF01_AI_REJECTED))
            break;
        if (error == ETIMEDOUT || error == ENOMEM || error == EIO) {
            error = 0;
            usleep(50000);
            continue;
        }
        if (error)
            break;
        if (job.frame.state == BF01_AI_READY)
            break;
        if (job.frame.state != BF01_AI_WAITING) {
            error = EPERM;
            break;
        }
        usleep(50000);
    }
    if (!error)
        error = preflight(&job.frame);
    if (!error) {
        report("recognizing", 0, &result, &job);
        error = run_worker(&job, &result, deadline);
    }
    if (!error && interrupted())
        error = ECANCELED;
    uint32_t command = !error && result.passed ? BF01_AI_ACCEPT : BF01_AI_REJECT;
    struct bf01_ai_reply reply;
    int decision_error = ETIMEDOUT;
    for (unsigned int attempt = 0; attempt < 5; attempt++) {
        decision_error = exchange(command, job.frame.image_paddr, &reply);
        if (!decision_error)
            break;
    }
    if (decision_error && !error)
        error = decision_error;
    if (!error && result.passed && reply.state != BF01_AI_PASSED)
        error = EPROTO;
    if (error && command == BF01_AI_ACCEPT) {
        for (unsigned int attempt = 0; attempt < 5; attempt++)
            if (!exchange(BF01_AI_REJECT, job.frame.image_paddr, &reply))
                break;
    }
    if (!error && result.passed)
        ai_allowed = 1;
    if (!error && !result.passed)
        error = ENODATA;
    report(ai_allowed ? "passed" : (error == ENODATA ? "rejected" : "failed"), error, &result, &job);
    CVI_IPCM_DeregisterAnonHandle(BF01_AI_PORT);
    return error;
}

int app_ipcam_BootAi_Bypass(void)
{
    struct ai_job job = {0};
    struct ai_result result = {.class_id = -1};
    struct bf01_ai_reply reply = {0};
    ai_allowed = 0;
    int error = CVI_IPCM_RegisterAnonHandle(BF01_AI_PORT, receive_reply, NULL);
    if (error) {
        report("failed", EIO, &result, &job);
        return EIO;
    }
    uint64_t deadline = monotonic_us() + (uint64_t)AI_TOTAL_TIMEOUT_MS * 1000;
    report("waiting_for_frame", 0, &result, &job);
    while (!error) {
        if (interrupted()) {
            error = ECANCELED;
            break;
        }
        if (monotonic_us() >= deadline) {
            error = ETIMEDOUT;
            break;
        }
        error = exchange(BF01_AI_STATUS, 0, &job.frame);
        if (error && job.frame.magic == BF01_AI_MAGIC &&
            (job.frame.state == BF01_AI_FAILED || job.frame.state == BF01_AI_REJECTED))
            break;
        if (error == ETIMEDOUT || error == ENOMEM || error == EIO) {
            error = 0;
            usleep(50000);
            continue;
        }
        if (error)
            break;
        if (job.frame.state == BF01_AI_READY)
            break;
        if (job.frame.state != BF01_AI_WAITING) {
            error = EPERM;
            break;
        }
        usleep(50000);
    }
    if (!error) {
        report("bypass", 0, &result, &job);
        for (unsigned int attempt = 0; attempt < 5; attempt++) {
            error = exchange(BF01_AI_ACCEPT, job.frame.image_paddr, &reply);
            if (!error)
                break;
        }
        if (!error && reply.state != BF01_AI_PASSED)
            error = EPROTO;
    }
    if (error) {
        for (unsigned int attempt = 0; attempt < 5; attempt++)
            if (!exchange(BF01_AI_REJECT, job.frame.image_paddr, &reply))
                break;
    } else {
        ai_allowed = 1;
    }
    report(ai_allowed ? "bypassed" : "failed", error, &result, &job);
    CVI_IPCM_DeregisterAnonHandle(BF01_AI_PORT);
    return error;
}
#endif
