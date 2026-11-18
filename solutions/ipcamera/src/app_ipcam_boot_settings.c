#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mtd/mtd-user.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "app_ipcam_boot_settings.h"
#ifdef BF01_BOOT_CAPTURE
#include "bf01_timestamp_settings.h"

struct boot_slot {
    int descriptor;
    char device[64];
    off_t block;
    struct mtd_info_user info;
    struct bf01_ts_record record;
};

static int slot_open(struct boot_slot *slot, const char *label, uint32_t expected_offset, int writable)
{
    memset(slot, 0, sizeof(*slot));
    slot->descriptor = -1;
    FILE *partitions = fopen("/proc/mtd", "r");
    if (!partitions)
        return errno;
    char line[256], name[128];
    unsigned int index = 0;
    int found = 0;
    while (fgets(line, sizeof(line), partitions)) {
        if (sscanf(line, "mtd%u: %*x %*x \"%127[^\"]\"", &index, name) == 2 && !strcmp(name, label)) {
            found = 1;
            break;
        }
    }
    fclose(partitions);
    if (!found)
        return ENODEV;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/class/mtd/mtd%u/offset", index);
    FILE *offset_file = fopen(path, "r");
    if (!offset_file)
        return errno;
    unsigned long long offset = ULLONG_MAX;
    int parsed = fscanf(offset_file, "%llu", &offset);
    fclose(offset_file);
    if (parsed != 1 || offset != expected_offset)
        return EXDEV;
    snprintf(path, sizeof(path), "/dev/mtd%u", index);
    snprintf(slot->device, sizeof(slot->device), "/dev/mtd%u", index);
    slot->descriptor = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW);
    if (slot->descriptor < 0)
        return errno;
    if (ioctl(slot->descriptor, MEMGETINFO, &slot->info))
        return errno;
    if (slot->info.type != MTD_NANDFLASH || slot->info.size != BF01_TS_SLOT_BYTES ||
        slot->info.erasesize != 0x20000U || slot->info.writesize != BF01_TS_RECORD_OFFSET)
        return EXDEV;
    for (slot->block = 0; slot->block < (off_t)slot->info.size; slot->block += slot->info.erasesize) {
        loff_t block = slot->block;
        int bad = ioctl(slot->descriptor, MEMGETBADBLOCK, &block);
        if (bad < 0)
            return errno;
        if (!bad) {
            ssize_t length = pread(slot->descriptor, &slot->record, sizeof(slot->record),
                slot->block + BF01_TS_RECORD_OFFSET);
            return length == sizeof(slot->record) ? 0 : length < 0 ? errno : EIO;
        }
    }
    return ENOSPC;
}

static void slots_close(struct boot_slot slots[2])
{
    for (int index = 0; index < 2; index++)
        if (slots[index].descriptor >= 0)
            close(slots[index].descriptor);
}

static int slots_open(struct boot_slot slots[2], int writable)
{
    memset(slots, 0, sizeof(struct boot_slot) * 2);
    slots[0].descriptor = slots[1].descriptor = -1;
    int error = slot_open(&slots[0], "BF01_TS0", BF01_TS_SLOT0_OFFSET, writable);
    if (!error)
        error = slot_open(&slots[1], "BF01_TS1", BF01_TS_SLOT1_OFFSET, writable);
    return error;
}

static int policy_from_settings(const cJSON *settings, uint32_t *policy)
{
    if (!cJSON_IsObject(settings))
        return EINVAL;
    *policy = BF01_TS_POLICY_MAGIC;
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(settings, "datetime_wm_switch");
    if (enabled) {
        if (!cJSON_IsBool(enabled))
            return EINVAL;
        *policy |= 0x100U | (uint32_t)cJSON_IsTrue(enabled);
    }
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(settings, "datetime_wm_format");
    if (format) {
        if (!cJSON_IsString(format))
            return EINVAL;
        int date_format = !strcmp(format->valuestring, "yyyy-MM-dd HH:mm:ss") ? 0 :
            !strcmp(format->valuestring, "MM/dd/yyyy HH:mm:ss") ? 1 :
            !strcmp(format->valuestring, "dd-MM-yyyy HH:mm:ss") ? 2 : -1;
        if (date_format < 0)
            return EINVAL;
        *policy |= 0x200U | ((uint32_t)date_format << 1);
    }
    return 0;
}

static int slot_write(struct boot_slot *slot, uint32_t generation, uint32_t policy)
{
    struct bf01_ts_record expected;
    bf01_ts_record_init(&expected, generation, policy);
    unsigned char page[BF01_TS_BOOT_BYTES];
    memset(page, 0xff, sizeof(page));
    memcpy(page, &expected, sizeof(expected));
    struct erase_info_user erase = {.start = slot->block, .length = slot->info.erasesize};
    if (ioctl(slot->descriptor, MEMERASE, &erase))
        return errno;
    ssize_t length = pwrite(slot->descriptor, page, slot->info.writesize,
        slot->block + BF01_TS_RECORD_OFFSET);
    if (length != (ssize_t)slot->info.writesize)
        return length < 0 ? errno : EIO;
    int descriptor = slot->descriptor;
    slot->descriptor = -1;
    if (close(descriptor))
        return errno;
    slot->descriptor = open(slot->device, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (slot->descriptor < 0)
        return errno;
    struct bf01_ts_record observed;
    length = pread(slot->descriptor, &observed, sizeof(observed), slot->block + BF01_TS_RECORD_OFFSET);
    if (length != sizeof(observed))
        return length < 0 ? errno : EIO;
    if (!bf01_ts_record_valid(&observed) || memcmp(&observed, &expected, sizeof(expected)))
        return EIO;
    slot->record = observed;
    return 0;
}

int app_boot_settings_sync(const cJSON *settings)
{
    uint32_t policy;
    int error = policy_from_settings(settings, &policy);
    if (error)
        return error;
    struct boot_slot slots[2];
    error = slots_open(slots, 1);
    struct bf01_ts_record records[2] = {slots[0].record, slots[1].record};
    int latest = error ? -1 : bf01_ts_record_choose(records);
    if (!error && latest < 0 && bf01_ts_record_valid(&records[0]) && bf01_ts_record_valid(&records[1]))
        error = EPROTO;
    uint32_t generation = latest < 0 ? 0 : records[latest].generation;
    int first = latest < 0 ? 0 : 1 - latest;
    for (int attempt = 0; !error && attempt < 2; attempt++) {
        int index = attempt ? 1 - first : first;
        if (bf01_ts_record_valid(&slots[index].record) && slots[index].record.policy == policy)
            continue;
        if (generation == UINT32_MAX) {
            error = EOVERFLOW;
            break;
        }
        error = slot_write(&slots[index], ++generation, policy);
    }
    slots_close(slots);
    if (error)
        fprintf(stderr, "[boot-settings] sync failed errno=%d; timestamp setting is not confirmed for next boot\n", error);
    return error;
}

int app_boot_settings_status(cJSON *status, const cJSON *settings)
{
    struct boot_slot slots[2];
    int error = slots_open(slots, 0);
    struct bf01_ts_record records[2] = {slots[0].record, slots[1].record};
    int selected = error ? -1 : bf01_ts_record_choose(records);
    if (!error && selected < 0)
        error = ENODATA;
    cJSON_AddNumberToObject(status, "errno", error);
    if (!error) {
        uint32_t policy = records[selected].policy;
        uint32_t saved_policy = 0;
        cJSON_AddBoolToObject(status, "matches_saved", !policy_from_settings(settings, &saved_policy) &&
            policy == saved_policy);
        cJSON_AddNumberToObject(status, "generation", records[selected].generation);
        cJSON_AddNumberToObject(status, "policy", policy);
        cJSON_AddBoolToObject(status, "redundant", bf01_ts_record_valid(&records[0]) &&
            bf01_ts_record_valid(&records[1]) && records[0].policy == records[1].policy);
        if (policy & 0x100U)
            cJSON_AddBoolToObject(status, "enabled", policy & 1U);
        if (policy & 0x200U)
            cJSON_AddNumberToObject(status, "format", (policy >> 1) & 3U);
    }
    slots_close(slots);
    return error;
}
#else
int app_boot_settings_sync(const cJSON *settings)
{
    (void)settings;
    return 0;
}

int app_boot_settings_status(cJSON *status, const cJSON *settings)
{
    (void)settings;
    cJSON_AddNumberToObject(status, "errno", ENOTSUP);
    return ENOTSUP;
}
#endif
