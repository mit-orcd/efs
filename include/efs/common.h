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
/* Phase 3b: chunk-metadata groups. 64 × 128 KiB = 8 MiB per group; a
 * file's mappings spread by hash(ino, group), not shard_of(ino). */
#define EFS_CHUNK_GROUP_SHIFT  6
#define EFS_CHUNK_GROUP_SIZE   (1u << EFS_CHUNK_GROUP_SHIFT)
/* Phase 3b: dentries stay parent-local until the directory has this many
 * immediate children, then they spread by hash(parent, name). */
#define EFS_DIR_SPREAD_MIN     (1u << 16)

/* Metadata 2+1 pages stay fixed (independent of data chunk_size). */
#define EFS_META_PAGE_SIZE     (128 * 1024)
#define EFS_META_FRAGMENT_SIZE (EFS_META_PAGE_SIZE / 2)

#define EFS_NUM_FRAGMENTS    3
#define EFS_MAX_NODES        4
/* Max TCP connections the client keeps open to each server (pool size).
 * Sized for many FUSE writers × fragment fanout × chunk pipeline. */
#define EFS_CLIENT_CONNS_PER_NODE 256
/* How many data chunks one FUSE write may PUT concurrently. 64 workers
 * measured WORSE than 32 (3780 vs 4686 MiB/s sw-1m): single-client is bound
 * by per-chunk latency + client CPU (blake3/memmove), not pipeline depth. */
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

/* Per-export feature switches (EFS_FEATURE_* bitmask). Persisted in the EFSR
 * root (server-owned), replicated, and toggled via efs-mgmt. Default: all on. */
#define EFS_FEATURE_STATS    (1u << 0) /* serve per-directory .stats */
#define EFS_FEATURE_FIND     (1u << 1) /* serve per-directory .find  */
#define EFS_FEATURES_DEFAULT (EFS_FEATURE_STATS | EFS_FEATURE_FIND)

#define EFS_ROOT_INO         1
/* Reserved inode for 2+1 metadata table pages (not a user-visible file).
 * High bit set so it cannot collide with client inode namespaces. */
#define EFS_META_TABLE_INO   ((efs_ino_t)0x8000000000000002ULL)
/* Per-shard metadata table inode (shard 0 == EFS_META_TABLE_INO).
 * Roadmap: bits=20 → 4096 shards. */
#define EFS_META_SHARD_INO_BASE EFS_META_TABLE_INO
#define EFS_META_MAX_SHARDS     4096
/* Shard tables flushed concurrently per metadata flush window. Each one is
 * ~1 ms of page round trips, so flushing them serially made the window linear
 * in the dirty-shard count while holding meta_flush_mu. */
#define EFS_META_FLUSH_PARALLEL 32
/* New exports are born sharded. 8 shards (2 per server on a 4-node
 * cluster) is the live default. bits=5 was measured 3-6x WORSE on the
 * full posixstress suite: the metadata flush pays a fixed per-shard-table
 * cost (snapshot under s->lock, CoW page setup, >=1 page write per table),
 * so 32 tables quadruple the flush's lock-hold and starve the handlers.
 * Raise this only after the flush is shard-lock-aware (snapshot under the
 * shard lock, pack small tables per page); the per-op CREATE/APPEND path
 * does benefit from more shards. bits=0 is not a product mode. */
#ifndef EFS_DEFAULT_SHARD_BITS
#define EFS_DEFAULT_SHARD_BITS  3
#endif
/* Metadata pages are 128 KiB (EFS_META_PAGE_SIZE); the inode staging table
 * still slabs rows at that granularity. */
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
#define EFS_ERR_STALE      -14
/* Phase 2b: a metadata mutation was sent to a non-primary node; the client
 * should re-resolve the primary and retry. */
#define EFS_ERR_NOT_PRIMARY -15
#define EFS_ERR_ACCES       -16 /* EACCES (search/execute denied) */
#define EFS_ERR_NAMETOOLONG -17 /* ENAMETOOLONG (component > 255) */
#define EFS_ERR_AGAIN      -18 /* retry: TCP side-channel has a frame */
#define EFS_ERR_DEADLK     -19 /* EDEADLK: same-inode lock cycle */
#define EFS_ERR_NOLCK      -20 /* ENOLCK: per-inode lock record cap */
#define EFS_ERR_NODATA     -21 /* ENODATA: xattr name is not set */

typedef uint64_t efs_ino_t;
typedef uint32_t efs_export_id_t;
typedef uint32_t efs_node_id_t;

static inline int efs_ino_is_meta_table(efs_ino_t ino)
{
    return ino >= EFS_META_SHARD_INO_BASE &&
           (uint64_t)(ino - EFS_META_SHARD_INO_BASE) < EFS_META_MAX_SHARDS;
}

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
static inline bool efs_mode_is_lnk(uint32_t mode) { return S_ISLNK(mode); }

static inline uint32_t efs_frag_size(uint32_t chunk_size)
{
    return chunk_size / 2;
}

/* True if size is a power of two in [EFS_MIN_CHUNK_SIZE, EFS_MAX_CHUNK_SIZE]. */
int efs_chunk_size_valid(uint32_t chunk_size);

const char *efs_strerror(int rc);

/* Parse a size/quota string. Accepts plain bytes or suffixes: T/TiB, G/GiB,
 * M/MiB, K/KiB. Returns 0 on error. */
uint64_t efs_parse_quota(const char *str);

#endif
