#ifndef EFS_STORAGE_CAPACITY_H
#define EFS_STORAGE_CAPACITY_H
#include "efs/common.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
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
