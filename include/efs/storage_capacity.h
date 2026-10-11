#ifndef EFS_STORAGE_CAPACITY_H
#define EFS_STORAGE_CAPACITY_H
#include "efs/common.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
/* Startup policy: reserve 20% (at least 1 GiB) on every filesystem,
 * then budget only 7/8 of remaining space for fragment payloads. The extra
 * allowance covers allocation padding and container/checksum overhead. This
 * is a startup estimate, not a reservation against unrelated disk writers. */
static inline int efs_storage_quota_budget(uint64_t total, uint64_t available,
                                          uint64_t used, uint64_t *budget)
{
    const uint64_t minimum = UINT64_C(1) << 30;
    uint64_t reserve = total / 5;
    if (reserve < minimum) reserve = minimum;
    if (available > total || reserve >= total || available <= reserve)
        return -1;
    uint64_t room = available - reserve;
    uint64_t additional = room - room / 8 - (room % 8 != 0);
    uint64_t ceiling = total - reserve;
    ceiling -= ceiling / 8 + (ceiling % 8 != 0);
    if (used > ceiling) return -1;
    *budget = additional > ceiling - used ? ceiling : used + additional;
    return *budget ? 0 : -1;
}

/* Aggregate budgets only after grouping roots by backing filesystem. */
static inline int efs_storage_safe_quota(
    const char paths[][EFS_MAX_PATH], const uint64_t *usage, uint32_t count,
    uint64_t *maximum)
{
    dev_t devices[EFS_MAX_STORAGE_PATHS];
    uint64_t totals[EFS_MAX_STORAGE_PATHS], available[EFS_MAX_STORAGE_PATHS];
    uint64_t used[EFS_MAX_STORAGE_PATHS] = {0};
    uint32_t seen = 0;
    if (!count || count > EFS_MAX_STORAGE_PATHS) return -1;
    for (uint32_t i = 0; i < count; i++) {
        struct stat st;
        struct statvfs fs;
        if (usage[i] == UINT64_MAX || stat(paths[i], &st) ||
            statvfs(paths[i], &fs)) return -1;
        uint32_t j;
        for (j = 0; j < seen && devices[j] != st.st_dev; j++) {}
        if (j == seen) {
            uint64_t unit = fs.f_frsize ? fs.f_frsize : fs.f_bsize;
            if (!unit || fs.f_blocks > UINT64_MAX / unit ||
                fs.f_bavail > UINT64_MAX / unit) return -1;
            devices[j] = st.st_dev;
            totals[j] = (uint64_t)fs.f_blocks * unit;
            available[j] = (uint64_t)fs.f_bavail * unit;
            seen++;
        }
        if (used[j] > UINT64_MAX - usage[i]) return -1;
        used[j] += usage[i];
    }
    uint64_t sum = 0;
    for (uint32_t j = 0; j < seen; j++) {
        uint64_t budget;
        if (efs_storage_quota_budget(totals[j], available[j], used[j], &budget)
            || sum > UINT64_MAX - budget) return -1;
        sum += budget;
    }
    *maximum = sum;
    return 0;
}

/* Count a filesystem once even when several roots share it. Failure leaves
 * capacity unknown; never advertise partially inventoried storage. */
static inline int efs_storage_capacity(
    const char paths[][EFS_MAX_PATH], uint32_t count, uint64_t quota,
    uint64_t used, uint64_t *capacity, uint64_t *available)
{
    dev_t devices[EFS_MAX_STORAGE_PATHS];
    uint32_t seen = 0;
    uint64_t total = 0, free_bytes = 0;
    if (!count || count > EFS_MAX_STORAGE_PATHS) return -1;
    for (uint32_t i = 0; i < count; i++) {
        struct stat st;
        struct statvfs fs;
        if (stat(paths[i], &st) || statvfs(paths[i], &fs)) return -1;
        uint32_t j;
        for (j = 0; j < seen && devices[j] != st.st_dev; j++) {}
        if (j < seen) continue;
        devices[seen++] = st.st_dev;
        uint64_t unit = fs.f_frsize ? fs.f_frsize : fs.f_bsize;
        if (!unit || fs.f_blocks > UINT64_MAX / unit ||
            fs.f_bavail > UINT64_MAX / unit) return -1;
        uint64_t c = (uint64_t)fs.f_blocks * unit;
        uint64_t a = (uint64_t)fs.f_bavail * unit;
        if (total > UINT64_MAX - c || free_bytes > UINT64_MAX - a) return -1;
        total += c; free_bytes += a;
    }
    if (quota) {
        if (total > quota) total = quota;
        uint64_t room = quota > used ? quota - used : 0;
        if (free_bytes > room) free_bytes = room;
    }
    if (free_bytes > total) free_bytes = total;
    *capacity = total; *available = free_bytes;
    return 0;
}
#endif
