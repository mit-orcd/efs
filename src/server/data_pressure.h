#ifndef EFS_DATA_PRESSURE_H
#define EFS_DATA_PRESSURE_H
#include <stdint.h>
/* Experimental logical-quota backpressure, not filesystem read-only policy.
 * Only actual recent deletion distinguishes temporary GC pressure from a
 * full live dataset. Arithmetic must remain safe at UINT64_MAX. */
static inline int efs_data_gc_pressure(int enabled, uint64_t quota, uint64_t used,
                                       uint64_t charge, uint64_t deleted_us, uint64_t now_us)
{
    if (!enabled || !quota || used > quota || charge > quota - used ||
        !deleted_us || deleted_us > now_us || now_us - deleted_us > 5000000)
        return 0;
    return used + charge > quota - quota / 10;
}
#endif
