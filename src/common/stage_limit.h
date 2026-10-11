#ifndef EFS_STAGE_LIMIT_H
#define EFS_STAGE_LIMIT_H
#include <stdint.h>
#include <stdlib.h>
/* Same target for staging eviction and optional deferred-rollup growth.
 * Preserve the existing environment parsing and default. */
static inline uint64_t efs_stage_cap_bytes(void)
{
    const char *env = getenv("EFS_CLIENT_META_MB");
    if (env && *env) {
        unsigned long v = strtoul(env, NULL, 10);
        if (v >= 16 && v <= (1UL << 20))
            return (uint64_t)v << 20;
    }
    return 256ULL << 20;
}
#endif
