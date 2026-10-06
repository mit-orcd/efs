#ifndef EFS_WRITE_EXTENT_H
#define EFS_WRITE_EXTENT_H
#include "efs/common.h"

/* Wire chunk indices and cache pull exclusive ends are uint32_t. Keep
 * ci+1 representable. Validate before division/casts or copying;
 * subtraction avoids wrapping the request end at UINT64_MAX. */
static inline int efs_write_extent_valid(uint64_t off, uint64_t len, uint32_t cs)
{
    if (!efs_chunk_size_valid(cs))
        return EFS_ERR_INVAL;
    uint64_t limit = (uint64_t)UINT32_MAX * cs;
    return off <= limit && len <= limit - off ? EFS_OK : EFS_ERR_INVAL;
}
#endif
