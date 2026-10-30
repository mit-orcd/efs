#ifndef EFS_STAGE_LIMIT_H
#define EFS_STAGE_LIMIT_H
#include "efs/settings.h"
#include <stdint.h>
#include <stdlib.h>
/* Same target for staging eviction and optional deferred-rollup growth.
 * Preserve the existing environment parsing and default. */
static inline uint64_t efs_stage_cap_bytes(void)
{
    const char *env = getenv("EFS_CLIENT_META_MB");
    uint64_t cap = 256ULL << 20;
    static int noted;
    if (env && *env) {
        unsigned long v = strtoul(env, NULL, 10);
        if (v >= 16 && v <= (1UL << 20))
            cap = (uint64_t)v << 20;
    }
    if (!__atomic_exchange_n(&noted, 1, __ATOMIC_RELAXED))
        efs_setting_u64("EFS_CLIENT_META_MB", cap >> 20);
    return cap;
}
#endif
