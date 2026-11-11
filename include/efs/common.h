#ifndef EFS_COMMON_H
#define EFS_COMMON_H

#include <stdint.h>
#include "efs/byteorder.h"
#include <stddef.h>
#include <stdbool.h>
#include <limits.h>
#include <sys/stat.h>
#include <stdlib.h>

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
/* K28/W151: cluster size cap. Data placement, membership and clients use
 * every member up to this count; metadata groups keep three voters (D38,
 * efs_md_group_voters in raft.h). 16, not the spec's 64: node-id masks are
 * uint32 with (1u << EFS_MAX_NODES) - 1 (pack ledger, publication, put
 * ticket, Raft voters), and each struct efs_node is ~4.2 KiB, so a node
 * table is ~68 KiB at 16 but ~270 KiB at 64, both on the wire (HELLO_ACK,
 * LIST_NODES_REPLY) and as stack copies on 1 MiB server threads. */
#define EFS_MAX_NODES        16
_Static_assert(EFS_MAX_NODES >= EFS_NUM_FRAGMENTS && EFS_MAX_NODES < 32,
               "node-id masks are uint32 and need (1u << EFS_MAX_NODES)");
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

/* Durable single-export query policy mask. Advertisement and query RPCs
 * intersect it with the local server startup capability ceiling. */
#define EFS_FEATURE_STATS    (1u << 0) /* serve directory stats queries */
#define EFS_FEATURE_FIND     (1u << 1) /* serve directory name queries  */
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
#define EFS_DEFAULT_SHARD_BITS  3
/* Metadata pages are 128 KiB (EFS_META_PAGE_SIZE); the inode staging table
 * still slabs rows at that granularity. */
/* On-disk inode directory sharding: five base-10000 groups (0000-9999)
 * encode any uint64 ino uniquely (10^20 > 2^64). seg[0] is least-significant. */
#define EFS_INO_PATH_SEGS    5

#define EFS_OK               0
#define EFS_ERR_IO          -1 /* local file or storage I/O failed, or stored data unreadable */
#define EFS_ERR_NOMEM       -2 /* allocation or table growth failed; whether a retry helps is not determined */
#define EFS_ERR_NOT_FOUND   -3 /* the looked-up key, inode or entry is absent (or only a tombstone) */
#define EFS_ERR_EXIST       -4 /* target already exists (name taken or identity already abandoned) */
#define EFS_ERR_INVAL       -5 /* argument or request rejected as invalid; the operation did not proceed */
#define EFS_ERR_NET         -6 /* no connection, or send/receive to a peer failed; peer state unknown */
#define EFS_ERR_PROTO       -7 /* received or supplied bytes violate the expected format or protocol rules */
#define EFS_ERR_NO_QUORUM   -8 /* peers reachable but no majority answered; callers wait or retry as for BUSY */
#define EFS_ERR_DECODE      -9 /* encoded bytes or an erasure-decoded segment could not be decoded */
#define EFS_ERR_CHECKSUM   -10 /* computed hash differs from the recorded one: content mismatch */
#define EFS_ERR_QUOTA      -11 /* a configured quota or aggregate size limit would be exceeded */
#define EFS_ERR_NOT_EMPTY  -12 /* directory still has children; removal or replace refused */
#define EFS_ERR_BUSY       -13 /* not determined: byte or size limit reached vs queue/resource still in use */
#define EFS_ERR_STALE      -14 /* request's expected state, epoch or revision no longer matches current state */
/* Phase 2b: a metadata mutation was sent to a non-primary node; the client
 * should re-resolve the primary and retry. */
#define EFS_ERR_NOT_PRIMARY -15 /* this node does not lead the group; the caller must re-resolve the leader */
#define EFS_ERR_ACCES       -16 /* EACCES (search/execute denied) */
#define EFS_ERR_NAMETOOLONG -17 /* ENAMETOOLONG (component > 255) */
#define EFS_ERR_AGAIN      -18 /* retry: TCP side-channel has a frame */
#define EFS_ERR_DEADLK     -19 /* EDEADLK: same-inode lock cycle */
#define EFS_ERR_NOLCK      -20 /* ENOLCK: per-inode lock record cap */
#define EFS_ERR_OVERFLOW   -22 /* EOVERFLOW: explicit metadata/output limit */
#define EFS_ERR_ILSEQ      -23 /* EILSEQ: unrepresentable legacy name */
#define EFS_ERR_NODATA     -21 /* ENODATA: xattr name is not set */
/* A log entry this build cannot apply: unknown tag or sub-tag, malformed or
 * truncated encoding. Fatal for its Raft group; never a business verdict and
 * never normalized to success (W127-W129). */
#define EFS_ERR_UNSUPPORTED -24 /* log entry or command this build cannot apply (unknown tag or bad length) */
/* I25 fragment record refusals (fragment format 2, efs/frag_record.h):
 * an unknown/older on-disk record format, and a well-formed record naming a
 * different immutable object than the reader asked for. Never served, never
 * reconstruction input; distinct from payload corruption (EFS_ERR_CHECKSUM). */
#define EFS_ERR_FRAG_FORMAT   -25 /* fragment record format unknown or older; the record is not served */
#define EFS_ERR_FRAG_IDENTITY -26 /* fragment record names a different object than requested; not served */
/* Session refusals (I23, K5). FENCED: the request epoch is below the shard's
 * durable rejection floor or below the epoch established there; the mount
 * must stop mutating and never resend under that epoch. UNREGISTERED: the
 * shard has no established row for this (uuid, epoch); the client admits
 * the shard (REGISTER + ESTABLISH) and resends the same identity. */
#define EFS_ERR_FENCED      -27 /* not determined: epoch below the session floor vs data-commit reply not OK */
#define EFS_ERR_UNREGISTERED -28 /* request has no valid or established session identity (uuid, epoch) */
/* W153: the cluster has not committed its formation membership yet
 * (efs-mgmt raft-mkfs); no client metadata is served before it. */
#define EFS_ERR_UNFORMED    -29 /* cluster formation membership not committed yet; no client metadata served */

typedef uint64_t efs_ino_t;

/* Inode low bits encode its shard, not per-file identity. Lock tables must
 * mix the allocation counter too or an entire local directory serializes.
 * This mapping is process-local and never changes a stored/wire inode. */
static inline uint32_t efs_ino_lock_slot(efs_ino_t ino, uint32_t slots)
{
    ino ^= ino >> 33;
    ino *= UINT64_C(0xff51afd7ed558ccd);
    ino ^= ino >> 33;
    ino *= UINT64_C(0xc4ceb9fe1a85ec53);
    ino ^= ino >> 33;
    return (uint32_t)(ino % slots); /* slots is a positive lock-table size */
}

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
    uint64_t token; /* K33: the node's store token from its HELLO; 0 unknown */
    /* Failure detection (in-memory; reset on heartbeat reply / HELLO). */
    uint64_t down_until_ms; /* heartbeat-marked-down cooldown deadline */
    uint32_t hb_fail_streak;
};

/* K28: a node table is ~68 KiB at EFS_MAX_NODES 16 (struct efs_node carries
 * a storage path), too much for a 1 MiB server thread stack. EFS_NODE_TABLE
 * declares a heap table freed on every scope exit; the caller must check it
 * for NULL and refuse (an allocation failure proves nothing). */
static inline void efs_node_table_free(struct efs_node **table) { free(*table); }
#define EFS_NODE_TABLE(name)                                               \
    struct efs_node *name __attribute__((cleanup(efs_node_table_free))) = \
        calloc(EFS_MAX_NODES, sizeof(struct efs_node))

static inline bool efs_mode_is_dir(uint32_t mode) { return S_ISDIR(mode); }
static inline bool efs_mode_is_lnk(uint32_t mode) { return S_ISLNK(mode); }

static inline uint32_t efs_frag_size(uint32_t chunk_size)
{
    return chunk_size / 2;
}

/* True if size is a power of two in [EFS_MIN_CHUNK_SIZE, EFS_MAX_CHUNK_SIZE]. */
int efs_chunk_size_valid(uint32_t chunk_size);

const char *efs_strerror(int rc);
/* W149: in a build without EFS_FAULTS every EFS_FAULT_* variable has no
 * effect. Print one stderr line per such variable in the environment,
 * naming prog; returns how many. Called from the daemons' main under
 * #if !EFS_FAULTS (the gate is per binary). */
unsigned efs_faults_env_ignored(const char *prog);
/* A6: print one stderr line per removed setting present in the
 * environment (set, even empty), naming prog; returns how many. efsd and
 * efs-fuse stop when it is not 0. */
unsigned efs_env_removed_refuse(const char *prog);
/* D42: EFS_RDMA_TRACE=1 (off by default); diagnostics only. */
int efs_rdma_trace_on(void);

/* Parse a size/quota string. Accepts plain bytes or suffixes: T/TiB, G/GiB,
 * M/MiB, K/KiB. Returns 0 on error. */
uint64_t efs_parse_quota(const char *str);

#endif
