#ifndef EFS_META_APPLY_INTERNAL_H
#define EFS_META_APPLY_INTERNAL_H

/* Shared by every C file of src/meta/meta_apply/: the includes, the KV value
 * sizes and five byte helpers. Each area declares what it exports inside the
 * module in <area>.h next to its owner, with the ownership, lock, lifetime
 * and failure contract at the top; a file includes only the areas it calls.
 * Public callers include efs/meta_apply.h and nothing here.
 *
 * The byte helpers are static inline so every file keeps them inlined, as
 * they were in the single file. */

#include "efs/meta_apply.h"
#include "efs/telemetry.h"
#include "efs/expiry.h"
#include "efs/metadata.h"
#include "efs/raft.h"
#include "efs/meta_cmd.h"
#include "efs/kv_key.h"
#include "efs/membership.h"
#include "efs/session.h"
#include "efs/put_ticket.h"
#include "efs/pack_mapping.h"
#include "efs/kv_lsm.h"
#include "efs/checksum.h"
#include "efs/publication.h"
#include "efs/dir_layout.h"
#include "efs/dir_spread.h"
#include "efs/txn.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define INO_VAL  EFS_META_INO_BYTES
#define DENT_VAL 20
#define ALLOC_VAL 8
#define LANE_VAL EFS_META_LANE_BYTES
#define CHUNK_HDR 24 /* gen:8 profile:4 epoch:8 base_clip:4 */
#define CHUNK_VAL (CHUNK_HDR + 4 * EFS_NUM_FRAGMENTS + \
                   EFS_HASH_SIZE * EFS_NUM_FRAGMENTS)
/* Trailer after CHUNK_VAL: u16 count, u16 pad, then count records.
 * Absent (vlen == CHUNK_VAL) means no deltas. A full-chunk CAS writes
 * the short value again and the list is gone. */
#define DELTA_REC 140
#define DELTA_HDR 4
#define CHUNK_VAL_MAX (CHUNK_VAL + EFS_PACK_REF_BYTES + DELTA_HDR + \
                       EFS_CHUNK_DELTA_MAX * DELTA_REC)

static inline void be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline void be64(uint8_t *p, uint64_t v)
{
    efs_store_be32(p, (uint32_t)(v >> 32));
    efs_store_be32(p + 4, (uint32_t)v);
}

static inline uint64_t rd64(const uint8_t *p)
{
    return ((uint64_t)efs_load_be32(p) << 32) | efs_load_be32(p + 4);
}

static inline uint64_t max_u64(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
}

#endif
