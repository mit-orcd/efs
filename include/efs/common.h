#ifndef EFS_COMMON_H
#define EFS_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limits.h>
#include <sys/stat.h>

#define EFS_VERSION_MAJOR 0
#define EFS_VERSION_MINOR 1
#define EFS_VERSION_PATCH 0
#define EFS_VERSION_PACK \
    ((EFS_VERSION_MAJOR << 16) | (EFS_VERSION_MINOR << 8) | EFS_VERSION_PATCH)

/* Build identifier baked in by the Makefile (git commit + dirty flag). Nodes
 * refuse to cluster with a different build so mixed-version deployments fail
 * loudly at join instead of corrupting each other. "unknown" (no git at build
 * time) only matches "unknown". */
#ifndef EFS_BUILD_ID
#define EFS_BUILD_ID "unknown"
#endif
#define EFS_BUILD_ID_LEN 48

/* Data chunk size bounds (per-export; power of two). */
#define EFS_MIN_CHUNK_SIZE     (128 * 1024)
#define EFS_DEFAULT_CHUNK_SIZE (128 * 1024)
#define EFS_MAX_CHUNK_SIZE     (128u * 1024 * 1024)
/* Compile-time defaults (also used when export chunk_size is unset). */
#define EFS_CHUNK_SIZE         EFS_DEFAULT_CHUNK_SIZE
#define EFS_FRAGMENT_SIZE      (EFS_CHUNK_SIZE / 2)
#define EFS_MAX_FRAGMENT_SIZE  (EFS_MAX_CHUNK_SIZE / 2)

/* Metadata 2+1 pages stay fixed (independent of data chunk_size). */
#define EFS_META_PAGE_SIZE     (128 * 1024)
#define EFS_META_FRAGMENT_SIZE (EFS_META_PAGE_SIZE / 2)

#define EFS_NUM_FRAGMENTS    3
#define EFS_MAX_NODES        4
/* Max TCP connections the client keeps open to each server (pool size).
 * Sized for many FUSE writers × fragment fanout × chunk pipeline. */
#define EFS_CLIENT_CONNS_PER_NODE 64
/* How many data chunks one FUSE write may PUT concurrently. */
#define EFS_WRITE_PIPELINE 32
#define EFS_MAX_EXPORTS      16
#define EFS_MAX_PATH         4096
#define EFS_MAX_NAME         256
/* Local data roots per efsd (1..24). New writes pick the path with least
 * writer-queue wait (fairness-weighted by bytes assigned) — not local EC. */
#define EFS_MAX_STORAGE_PATHS 24
#define EFS_HASH_SIZE        32
#define EFS_LISTEN_BACKLOG   512
#define EFS_DEFAULT_PORT     7432
#define EFS_HEARTBEAT_MS     2000
#define EFS_IO_TIMEOUT_MS    30000

#define EFS_ROOT_INO         1
/* Reserved inode for 2+1 metadata table pages (not a user-visible file).
 * High bit set so it cannot collide with client inode namespaces. */
#define EFS_META_TABLE_INO   ((efs_ino_t)0x8000000000000002ULL)
/* Max pages for a fragmented metadata blob (each page = EFS_META_PAGE_SIZE).
 * 8192 × 128 KiB = 1 GiB — enough for ~millions of inodes (ImageNet-scale). */
#define EFS_META_MAX_PAGES   8192
/* Dual-slot meta pages: generation parity selects which on-disk chunk_index
 * range is written. Flush writes the new gen's slot only, then flips EFSR so
 * a torn flush cannot clobber the live generation. */
#define EFS_META_SLOT_STRIDE EFS_META_MAX_PAGES
/* On-disk inode directory sharding: five base-10000 groups (0000-9999)
 * encode any uint64 ino uniquely (10^20 > 2^64). seg[0] is least-significant. */
#define EFS_INO_PATH_SEGS    5

#define EFS_OK               0
#define EFS_ERR_IO          -1
#define EFS_ERR_NOMEM       -2
#define EFS_ERR_NOT_FOUND   -3
#define EFS_ERR_EXIST       -4
#define EFS_ERR_INVAL       -5
#define EFS_ERR_NET         -6
#define EFS_ERR_PROTO       -7
#define EFS_ERR_NO_QUORUM   -8
#define EFS_ERR_DECODE      -9
#define EFS_ERR_CHECKSUM   -10
#define EFS_ERR_QUOTA      -11
#define EFS_ERR_NOT_EMPTY  -12
#define EFS_ERR_BUSY       -13

typedef uint64_t efs_ino_t;
typedef uint32_t efs_export_id_t;
typedef uint32_t efs_node_id_t;

void efs_ino_path_segments(efs_ino_t ino, char seg[EFS_INO_PATH_SEGS][5]);

struct efs_node {
    efs_node_id_t id;
    char addr[64];
    uint16_t port;
    char storage_path[EFS_MAX_PATH];
    uint64_t quota; /* max bytes this node may store; 0 means unlimited */
    uint64_t used;  /* bytes currently stored on this node */
    /* Failure detection (in-memory; reset on heartbeat reply / HELLO). */
    uint64_t down_until_ms; /* heartbeat-marked-down cooldown deadline */
    uint32_t hb_fail_streak;
};

static inline bool efs_mode_is_dir(uint32_t mode) { return S_ISDIR(mode); }
static inline bool efs_mode_is_reg(uint32_t mode) { return S_ISREG(mode); }
static inline bool efs_mode_is_lnk(uint32_t mode) { return S_ISLNK(mode); }

static inline uint32_t efs_frag_size(uint32_t chunk_size)
{
    return chunk_size / 2;
}

/* Map (generation, page) → fragment chunk_index. page must be < EFS_META_MAX_PAGES.
 * Returns UINT32_MAX if page_index is out of range. */
static inline uint32_t efs_meta_page_chunk_index(uint64_t generation,
                                                uint32_t page_index)
{
    if (page_index >= EFS_META_MAX_PAGES)
        return UINT32_MAX;
    return page_index +
           (uint32_t)((generation & 1ULL) * (uint64_t)EFS_META_SLOT_STRIDE);
}

/* True if size is a power of two in [EFS_MIN_CHUNK_SIZE, EFS_MAX_CHUNK_SIZE]. */
int efs_chunk_size_valid(uint32_t chunk_size);

const char *efs_strerror(int rc);

/* Parse a size/quota string. Accepts plain bytes or suffixes: T/TiB, G/GiB,
 * M/MiB, K/KiB. Returns 0 on error. */
uint64_t efs_parse_quota(const char *str);

#endif
