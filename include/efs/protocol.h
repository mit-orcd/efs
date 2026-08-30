#ifndef EFS_PROTOCOL_H
#define EFS_PROTOCOL_H

#include "efs/common.h"
#include "efs/metadata.h"
#include <stdint.h>

/* Length-prefixed TCP frames: 4 bytes length (network order), 1 byte type,
 * length-1 bytes payload. */

/* Sanity bound on one frame's payload. GET_META_REPLY carries the full EFSM
 * blob: ~315 B per file (inode + chunk entry + name), so a 1.33M-file
 * ImageNet copy is already ~420 MB and the old 256 MB cap made mounts of
 * such exports fail with EFS_ERR_NET. 3 GiB covers ~10M files; still
 * bounds bogus mallocs. The wire length field is u32, so this must stay
 * under 4 GiB — before the blob approaches that, GET_META needs a
 * chunked/streamed variant. */
#define EFS_MSG_MAX_LEN (3072u * 1024 * 1024)

enum efs_msg_type {
    EFS_MSG_HEARTBEAT = 0,
    EFS_MSG_HEARTBEAT_ACK = 1,
    EFS_MSG_HELLO = 2,
    EFS_MSG_HELLO_ACK = 3,
    EFS_MSG_GET_CHUNK = 4,
    EFS_MSG_GET_CHUNK_REPLY = 5,
    EFS_MSG_PUT_CHUNK = 6,
    EFS_MSG_PUT_CHUNK_REPLY = 7,
    EFS_MSG_GET_META = 8,
    EFS_MSG_GET_META_REPLY = 9,
    EFS_MSG_PUT_META = 10,
    EFS_MSG_PUT_META_REPLY = 11,
    EFS_MSG_LIST_NODES = 12,
    EFS_MSG_LIST_NODES_REPLY = 13,
    EFS_MSG_CREATE_EXPORT = 14,
    EFS_MSG_CREATE_EXPORT_REPLY = 15,
    EFS_MSG_JOIN = 16,
    EFS_MSG_JOIN_REPLY = 17,
    EFS_MSG_STATUS = 18,
    EFS_MSG_STATUS_REPLY = 19,
    EFS_MSG_REMOVE_NODE = 20,
    EFS_MSG_REMOVE_NODE_REPLY = 21,
    EFS_MSG_SHRINK_QUOTA = 22,
    EFS_MSG_SHRINK_QUOTA_REPLY = 23,
    EFS_MSG_NODE_LEFT = 24,
    EFS_MSG_QUERY_STATS = 25,
    EFS_MSG_QUERY_STATS_REPLY = 26,
    EFS_MSG_LIST_EXPORTS = 27,
    EFS_MSG_LIST_EXPORTS_REPLY = 28,
    EFS_MSG_DRAIN_NODE = 29,
    EFS_MSG_DRAIN_NODE_REPLY = 30,
    EFS_MSG_UNDRAIN_NODE = 31,
    EFS_MSG_UNDRAIN_NODE_REPLY = 32,
    /* Network bench: same payload as PUT_CHUNK; server ACKs and discards. */
    EFS_MSG_BENCH_PUT = 33,
    EFS_MSG_BENCH_PUT_REPLY = 34,
    EFS_MSG_DESTROY_EXPORT = 35,
    EFS_MSG_DESTROY_EXPORT_REPLY = 36,
    /* Per-export feature switches (.stats/.find). Server-owned. */
    EFS_MSG_SET_FEATURES = 37,
    EFS_MSG_SET_FEATURES_REPLY = 38,
    EFS_MSG_GET_FEATURES = 39,
    EFS_MSG_GET_FEATURES_REPLY = 40,
    /* Append local --storage roots on a live node (no restart). */
    EFS_MSG_ADD_STORAGE = 41,
    EFS_MSG_ADD_STORAGE_REPLY = 42,
    /* Server inode/dentry RPCs (EFSR v6 sharded metadata). */
    EFS_MSG_INODE_LOOKUP = 43,
    EFS_MSG_INODE_LOOKUP_REPLY = 44,
    EFS_MSG_INODE_CREATE = 45,
    EFS_MSG_INODE_CREATE_REPLY = 46,
    EFS_MSG_INODE_GETATTR = 47,
    EFS_MSG_INODE_GETATTR_REPLY = 48,
    EFS_MSG_INODE_READDIR = 49,
    EFS_MSG_INODE_READDIR_REPLY = 50,
    EFS_MSG_INODE_UNLINK = 51,
    EFS_MSG_INODE_UNLINK_REPLY = 52,
    EFS_MSG_UPGRADE_META = 53,
    EFS_MSG_UPGRADE_META_REPLY = 54,
    /* RDMA QP bootstrap over the TCP conn; unknown-type/error -> stay TCP. */
    EFS_MSG_RDMA_SETUP = 55,
    EFS_MSG_RDMA_SETUP_REPLY = 56,
    /* Meta flush election: a client must win a majority of nodes before
     * PUTting meta pages for a new generation. Concurrent writers used to
     * PUT the same dual-slot page CIs with different content, tearing every
     * page they overlapped on; the election serializes flushes cluster-wide. */
    EFS_MSG_META_FLUSH_BEGIN = 57,
    EFS_MSG_META_FLUSH_BEGIN_REPLY = 58,
    /* Root-only meta poll for server catchup: same payload rules as GET_META
     * (empty = primary export, else export name) but the reply is just the
     * EFSR root (~KiB), never the GiB-scale EFSM blob. Catchup polls this
     * every 2 s per peer; full GET_META only fires when a peer is newer. */
    EFS_MSG_GET_META_ROOT = 59,
    EFS_MSG_GET_META_ROOT_REPLY = 60,
    /* Phase 3b op class (docs/scaling-roadmap.md Item 0):
     *   Commutative  — apply unordered; max/newer wins (REPORT size/mtime).
     *                  Size is grow-only, and a grow whose mtime is older
     *                  than the row is dropped (stale close-REPORT must
     *                  not undo SETATTR/O_TRUNC).
     *   Independent  — shard-local commit (CREATE/UNLINK inside one dir,
     *                  disjoint-chunk writes).
     *   Conflicting  — owner-serialized RPC + dual-apply (RENAME, HOLD,
     *                  APPEND reserve, FLOCK). If a new op is not obviously
     *                  the first two, it is Conflicting. */
    /* Phase 2b: additional server-owned metadata mutations. Replies reuse
     * struct efs_msg_inode_reply (status + resulting inode). */
    EFS_MSG_INODE_RENAME = 61,
    EFS_MSG_INODE_RENAME_REPLY = 62,
    EFS_MSG_INODE_SETATTR = 63,
    EFS_MSG_INODE_SETATTR_REPLY = 64,
    EFS_MSG_INODE_LINK = 65,
    EFS_MSG_INODE_LINK_REPLY = 66,
    /* Phase 2b: client reports written-chunk mappings (ino → nodes + checksums)
     * to the metadata primary, batched. This replaces the client blob flush for
     * the data path: the server applies each record via efs_export_set_chunk and
     * marks the export dirty so the meta-flush thread persists it. The reply is
     * struct efs_msg_inode_reply (status + primary_id; inode unused). */
    EFS_MSG_REPORT_CHUNKS = 67,
    EFS_MSG_REPORT_CHUNKS_REPLY = 68,
    /* Phase 2b reads: peer pulls chunk mappings for an inode it just looked up. */
    EFS_MSG_INODE_GETCHUNKS = 69,
    EFS_MSG_INODE_GETCHUNKS_REPLY = 70,
    /* Cross-client O_APPEND: atomically reserve the next `len` bytes at the
     * metadata owner (size advances under s->lock) and return the resulting
     * inode; the append offset is reply.size - len. The client then writes
     * the data there via the normal dcache/PUT/REPORT path. Without this,
     * two clients each appended at their own stale cached end and tore or
     * lost each other's lines (mc_stress appfile). */
    EFS_MSG_INODE_APPEND = 71,
    EFS_MSG_INODE_APPEND_REPLY = 72,
    /* Phase 3 data-path sharding: parent owner fans a child-row create to
     * the target shard's owner. Reply is struct efs_msg_inode_reply. */
    EFS_MSG_INODE_CREATE_SHARD = 73,
    EFS_MSG_INODE_CREATE_SHARD_REPLY = 74,
    /* Parent owner fans nlink++ / nlink-- to the child-row owner. Reply is
     * struct efs_msg_inode_reply. Without this, hardlink on a spread-created
     * file is NOT_FOUND on the parent (EIO) and last-link unlink orphans
     * the child row + chunks. */
    EFS_MSG_INODE_LINK_SHARD = 75,
    EFS_MSG_INODE_LINK_SHARD_REPLY = 76,
    EFS_MSG_INODE_UNLINK_SHARD = 77,
    EFS_MSG_INODE_UNLINK_SHARD_REPLY = 78,
    EFS_MSG_INODE_LOOKUP_PATH = 79,
    EFS_MSG_INODE_LOOKUP_PATH_REPLY = 80,
    EFS_MSG_HEAL_STATUS = 81,
    EFS_MSG_HEAL_STATUS_REPLY = 82,
    /* Open-fd refcount on the shard owner. Last-link unlink keeps inode+
     * chunks while refs>0 so a peer with an open fd can still read. */
    EFS_MSG_INODE_HOLD = 83,
    EFS_MSG_INODE_HOLD_REPLY = 84,
    /* Cluster-wide whole-file flock. Reply is struct efs_msg_inode_reply
     * (BUSY = would block). */
    EFS_MSG_INODE_FLOCK = 85,
    EFS_MSG_INODE_FLOCK_REPLY = 86,
    /* Rename a specific directory name (hard links share an ino). */
    EFS_MSG_INODE_RENAME_AT = 87,
    EFS_MSG_INODE_RENAME_AT_REPLY = 88,
    /* Phase 3b: drop chunk mappings with chunk_index >= first on the
     * receiving node's owned shard tables. Used by truncate / last-link
     * unlink so extent-sharded groups on peer shards are not leaked. */
    EFS_MSG_INODE_DROP_CHUNKS = 89,
    EFS_MSG_INODE_DROP_CHUNKS_REPLY = 90,
    /* Two-phase metadata root commit. PUT_META is phase 1 (prepare): the
     * receiver STASHES the root as pending and acks, but does not install,
     * persist, fence tables, or GC — the writer may still fail quorum and
     * discard it, and an uncommitted root must never become visible (a
     * same-gen retry rewrites the same CoW cis with different content, so
     * installing an uncommitted root corrupts the pages it references).
     * Phase 2: after a prepare quorum the writer commits locally and then
     * broadcasts META_COMMIT(export, gen); a receiver promotes its matching
     * pending root (install + persist + fence + GC old cis — safe now: the
     * gen's pages were 2-of-3 placed before the prepare was even sent).
     * Stragglers that miss COMMIT catch up via GET_META_ROOT, which only
     * ever serves committed roots. */
    EFS_MSG_META_COMMIT = 91,
    EFS_MSG_META_COMMIT_REPLY = 92,
};

/* META_COMMIT request: promote the pending (prepared) root for this export
 * whose generation matches gen AND whose stashed prepare bytes hash to
 * root_sum. The fingerprint matters because a failed flush retries the SAME
 * gen with fresh content at the SAME CoW cis: a peer that missed the retry's
 * prepare but matched on gen alone would promote a stale root whose pages
 * were already overwritten (checksum mismatch → unrecoverable wedge). On
 * mismatch the peer keeps its pending root and converges via catchup.
 * Reply is EFS_PUT_META_OK on promote, EFS_PUT_META_STALE when there is no
 * matching pending root (already committed, superseded, fingerprint
 * mismatch, or never prepared — the writer treats all as
 * success-equivalent since catchup converges stragglers). */
struct efs_msg_meta_commit {
    uint32_t export_id;
    uint32_t pad;
    uint64_t gen;
    uint8_t root_sum[EFS_HASH_SIZE];
};

/* Set per-export features. Only bits in set_mask are changed (to the
 * corresponding bits in features); other bits are preserved. */
struct efs_msg_set_features {
    char export_name[EFS_MAX_NAME];
    uint32_t features;
    uint32_t set_mask;
};

/* Reply for both SET_FEATURES and GET_FEATURES: the export's current mask. */
#define EFS_FEATURES_OK        0
#define EFS_FEATURES_NOT_FOUND 1
struct efs_msg_features_reply {
    uint32_t features;
    uint8_t status; /* EFS_FEATURES_* */
};

/* GET_FEATURES request: just the export name. */
struct efs_msg_get_features {
    char export_name[EFS_MAX_NAME];
};

struct efs_msg_hello {
    uint32_t version;
    efs_node_id_t node_id;
    char addr[64];
    uint16_t port;
    char storage_path[EFS_MAX_PATH];
    uint64_t quota; /* local storage quota in bytes; 0 = unlimited */
    uint64_t used;  /* bytes currently stored on this node */
    char build_id[EFS_BUILD_ID_LEN]; /* git build id; must match the cluster's */
};

struct efs_msg_hello_ack {
    uint32_t epoch;
    efs_node_id_t assigned_id;
    uint32_t node_count;
    struct efs_node nodes[EFS_MAX_NODES];
    uint32_t reject_reason; /* EFS_HELLO_REJECT_*; valid when assigned_id == 0 */
    char build_id[EFS_BUILD_ID_LEN]; /* receiver's build id (for diagnostics) */
};

#define EFS_HELLO_REJECT_NONE    0
#define EFS_HELLO_REJECT_FULL    1
#define EFS_HELLO_REJECT_VERSION 2

struct efs_msg_get_chunk {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint32_t chunk_index;
    uint32_t fragment_index; /* 0 = D1, 1 = D2, 2 = P */
};

/* Reply payload: [status:1][checksum:32][fragment_data:data_len]
 * data_len = frame_payload_len - 1 - EFS_HASH_SIZE when status == OK. */
#define EFS_GET_CHUNK_OK     0
#define EFS_GET_CHUNK_NOT_FOUND 1
#define EFS_GET_CHUNK_ERROR  2

/* PUT/BENCH frame: this header followed by data_len payload bytes. */
struct efs_msg_put_chunk {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint32_t chunk_index;
    uint32_t fragment_index;
    uint8_t checksum[EFS_HASH_SIZE];
    uint32_t data_len;
    /* uint8_t data[data_len]; */
};

#define EFS_PUT_CHUNK_OK     0
#define EFS_PUT_CHUNK_ERROR  1
#define EFS_PUT_CHUNK_QUOTA_EXCEEDED 2

#define EFS_BENCH_PUT_OK     0
#define EFS_BENCH_PUT_ERROR  1

struct efs_msg_put_meta {
    uint32_t epoch;
    efs_export_id_t export_id;
    /* serialized metadata follows as a length-prefixed blob */
};

#define EFS_PUT_META_OK     0
#define EFS_PUT_META_ERROR  1
#define EFS_PUT_META_STALE  2
#define EFS_PUT_META_BUSY   3 /* flush election held by another live writer */

/* Meta flush election request/response. writer_id is the client's
 * write_lease_id (also carried in the published root, so the server can
 * check the root publisher against the election holder). */
struct efs_msg_meta_flush_begin {
    uint32_t export_id;
    uint32_t pad;
    uint64_t gen;
    uint64_t writer_id;
};
struct efs_msg_meta_flush_begin_reply {
    uint8_t status;
    uint8_t pad[7];
    uint64_t committed_gen; /* server's committed gen; >= req gen means stale */
};

struct efs_msg_put_meta_reply {
    uint8_t status;
    uint32_t new_epoch;
};

struct efs_msg_list_nodes_reply {
    uint32_t node_count;
    struct efs_node nodes[EFS_MAX_NODES];
};

struct efs_msg_status_reply {
    uint64_t quota;
    uint64_t used;
    uint32_t state; /* efsd_server_state: 0=active, 3=draining, 4=drained, ... */
};

struct efs_msg_shrink_quota {
    uint64_t amount; /* bytes to subtract from the current quota */
};

struct efs_msg_node_left {
    efs_node_id_t node_id;
};

#define EFS_REMOVE_NODE_OK     0
#define EFS_REMOVE_NODE_ERROR  1
#define EFS_REMOVE_NODE_IN_PROGRESS 2
#define EFS_REMOVE_NODE_NOT_DRAINED 3

#define EFS_DRAIN_NODE_OK           0
#define EFS_DRAIN_NODE_ERROR        1
#define EFS_DRAIN_NODE_IN_PROGRESS  2

#define EFS_UNDRAIN_NODE_OK     0
#define EFS_UNDRAIN_NODE_ERROR  1

#define EFS_SHRINK_QUOTA_OK     0
#define EFS_SHRINK_QUOTA_ERROR  1
#define EFS_SHRINK_QUOTA_IN_PROGRESS 2

/* Comma-separated absolute roots to append. Existing paths are skipped. */
struct efs_msg_add_storage {
    char paths[EFS_MAX_PATH];
};

#define EFS_ADD_STORAGE_OK       0
#define EFS_ADD_STORAGE_ERROR    1
#define EFS_ADD_STORAGE_FULL     2
#define EFS_ADD_STORAGE_INVALID  3

struct efs_msg_add_storage_reply {
    uint8_t status;
    uint32_t path_count; /* node's storage_path_count after the call */
};

/* Wire values match enum efsd_server_state in server_internal.h */
#define EFS_NODE_STATE_ACTIVE    0
#define EFS_NODE_STATE_LEAVING   1
#define EFS_NODE_STATE_SHRINKING 2
#define EFS_NODE_STATE_DRAINING  3
#define EFS_NODE_STATE_DRAINED   4

struct efs_msg_create_export {
    char name[EFS_MAX_NAME];
    uint32_t chunk_size; /* 0 = EFS_DEFAULT_CHUNK_SIZE */
};

#define EFS_CREATE_EXPORT_OK                0
#define EFS_CREATE_EXPORT_ERROR             1
#define EFS_CREATE_EXPORT_EXISTS            2
/* Export was created/saved locally; peer meta replicate did not get quorum. */
#define EFS_CREATE_EXPORT_REPLICATE_FAILED  3

struct efs_msg_destroy_export {
    char name[EFS_MAX_NAME];
};

#define EFS_DESTROY_EXPORT_OK         0
#define EFS_DESTROY_EXPORT_ERROR      1
#define EFS_DESTROY_EXPORT_NOT_FOUND  2

struct efs_msg_join {
    char peer_host[64];
    uint16_t peer_port;
};

#define EFS_JOIN_OK     0
#define EFS_JOIN_ERROR  1

#define EFS_MAX_QUERY_USERS 256

struct efs_user_stat {
    uint32_t uid;
    uint64_t files;
    uint64_t bytes;
};

struct efs_msg_query_stats_reply {
    uint64_t total_files;
    uint64_t total_bytes;
    uint32_t user_count;
    struct efs_user_stat users[EFS_MAX_QUERY_USERS];
};

struct efs_msg_export_entry {
    efs_export_id_t id;
    char name[EFS_MAX_NAME];
};

struct efs_msg_list_exports_reply {
    uint32_t export_count;
    struct efs_msg_export_entry exports[EFS_MAX_EXPORTS];
};

#define EFS_INODE_RPC_OK         0
#define EFS_INODE_RPC_NOT_FOUND  1
#define EFS_INODE_RPC_EXIST      2
#define EFS_INODE_RPC_ERROR      3
#define EFS_INODE_RPC_QUOTA      4
#define EFS_INODE_RPC_BUSY       5
#define EFS_INODE_RPC_INVAL      6
/* Phase 2b: mutation sent to a non-primary node; client should re-resolve the
 * primary and retry. */
#define EFS_INODE_RPC_NOT_PRIMARY 7
/* rmdir on a directory that still has children. */
#define EFS_INODE_RPC_NOT_EMPTY  8
#define EFS_INODE_RPC_SYMLINK    9
#define EFS_INODE_RPC_DEEP      10

struct efs_msg_inode_lookup {
    efs_export_id_t export_id;
    efs_ino_t parent;
    char name[EFS_MAX_NAME];
};

#define EFS_LOOKUP_PATH_MAX_DEPTH 64
#define EFS_LOOKUP_PATH_F_ANCESTORS (1u << 0)
struct efs_msg_inode_lookup_path {
    efs_export_id_t export_id;
    uint32_t flags;
    char path[4096];
};
struct efs_lookup_path_anc {
    efs_ino_t ino;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
};
/* First two fields match efs_msg_inode_reply so rpc_send_recv_owner can
 * inspect NOT_PRIMARY without a separate decoder. */
struct efs_msg_inode_lookup_path_reply {
    uint8_t status;
    efs_node_id_t primary_id;
    struct efs_inode inode;
    uint32_t ancestor_count;
    struct efs_lookup_path_anc ancestors[EFS_LOOKUP_PATH_MAX_DEPTH];
};

#define EFS_HEAL_F_TABLES  (1u << 0) /* one or more tables still need rebuild */
#define EFS_HEAL_F_REBUILD (1u << 1) /* a page rebuild is in progress now */
struct efs_msg_heal_status_export {
    char name[EFS_MAX_NAME];
    uint64_t gen;
    uint32_t flags;
    uint32_t tables_need;
    uint32_t tables_total;
    uint32_t cur_shard;
    uint32_t pages_done;
    uint32_t pages_total;
    uint64_t elapsed_us;
};
struct efs_msg_heal_status_reply {
    uint32_t export_count;
    uint32_t healing; /* any export has flags set */
    struct efs_msg_heal_status_export exports[EFS_MAX_EXPORTS];
};

/* Piggyback an open hold on CREATE so create+open is one RPC. Release
 * still sends HOLD−1; skipping that leaks refs and last-link unlink
 * never deletes. */
#define EFS_CREATE_F_HOLD (1u << 0)

struct efs_msg_inode_create {
    efs_export_id_t export_id;
    efs_ino_t parent;
    char name[EFS_MAX_NAME];
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t flags;
};

/* Nested create on the target shard owner (parent owner already decided
 * the shard). The receiver allocates an ino from target_shard's class and
 * writes the full row; the parent owner writes only the dentry. */
struct efs_msg_inode_create_shard {
    efs_export_id_t export_id;
    efs_ino_t parent;
    char name[EFS_MAX_NAME];
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t target_shard;
    uint32_t flags;
    /* Phase 3b: 0 = allocate; else write this ino as a dentry on
     * target_shard (spread-dir hash shard, inode already allocated). */
    efs_ino_t ino;
};

struct efs_msg_inode_getattr {
    efs_export_id_t export_id;
    efs_ino_t ino;
};

/* Return dentries with this parent from the receiver's own shard table
 * (not the parent-owner table). Used to merge a spread directory. */
#define EFS_READDIR_F_LOCAL_ONLY (1u << 0)
struct efs_msg_inode_readdir {
    efs_export_id_t export_id;
    efs_ino_t parent;
    uint32_t max_ents;
    uint32_t flags;
    /* EFS_READDIR_F_LOCAL_ONLY: serve this shard's table. */
    uint32_t shard;
    /* Pagination cursor: return children with ino > after_ino, in ascending
     * ino order. A positional skip is NOT stable — remove_inode_slot
     * swap-compacts, so a concurrent unlink of ANOTHER dir on the same shard
     * shifts this dir's rows across a page boundary and an entry is skipped.
     * inos survive compaction, so an ino cursor is stable. 0 = from start. */
    uint64_t after_ino;
};

struct efs_msg_inode_unlink {
    efs_export_id_t export_id;
    efs_ino_t parent;
    char name[EFS_MAX_NAME];
    uint8_t is_dir;
};

struct efs_msg_inode_reply {
    uint8_t status;
    /* Phase 2b: on EFS_INODE_RPC_NOT_PRIMARY, the server's view of the
     * metadata primary (lowest-id live node) so the client can retry there. */
    efs_node_id_t primary_id;
    struct efs_inode inode;
};

#define EFS_READDIR_MAX 64
struct efs_msg_inode_readdir_reply {
    uint8_t status;
    uint32_t count;
    struct efs_inode ents[EFS_READDIR_MAX];
};

/* Phase 2b: rename/move an inode to a new parent + name. */
struct efs_msg_inode_rename {
    efs_export_id_t export_id;
    efs_ino_t ino;
    efs_ino_t new_parent;
    char new_name[EFS_MAX_NAME];
};

struct efs_msg_inode_rename_at {
    efs_export_id_t export_id;
    efs_ino_t old_parent;
    char old_name[EFS_MAX_NAME];
    efs_ino_t new_parent;
    char new_name[EFS_MAX_NAME];
};

/* Phase 2b: setattr mask bits — which fields to apply. */
#define EFS_SETATTR_MODE  (1u << 0)
#define EFS_SETATTR_UID   (1u << 1)
#define EFS_SETATTR_GID   (1u << 2)
#define EFS_SETATTR_SIZE  (1u << 3)
#define EFS_SETATTR_MTIME (1u << 4)
#define EFS_SETATTR_ATIME (1u << 5)
struct efs_msg_inode_setattr {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint32_t mask;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t mtime;
    uint32_t mtime_nsec;
    uint64_t atime;
};

/* Phase 2b: add a hard link (extra name) for an existing non-directory inode. */
struct efs_msg_inode_link {
    efs_export_id_t export_id;
    efs_ino_t src_ino;
    efs_ino_t new_parent;
    char new_name[EFS_MAX_NAME];
};

/* Nested nlink mutation on the child-row owner. */
struct efs_msg_inode_link_shard {
    efs_export_id_t export_id;
    efs_ino_t src_ino;
};

struct efs_msg_inode_unlink_shard {
    efs_export_id_t export_id;
    efs_ino_t src_ino;
};

/* Cross-client O_APPEND reservation; reply is struct efs_msg_inode_reply
 * with the inode AFTER the size advance (offset = inode.size - len). */
struct efs_msg_inode_append {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint64_t len;
};

/* flags: 1 = open (+1 ref), 0 = close (-1 ref). owner identifies the
 * client mount so close can drop that client's flock. */
struct efs_msg_inode_hold {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint32_t flags;
    uint64_t owner;
};

/* op is the flock(2) operation (LOCK_SH/EX/UN, optional LOCK_NB). */
struct efs_msg_inode_flock {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint32_t op;
    uint64_t owner;
};

/* Phase 2b: one written-chunk mapping record (matches efs_export_set_chunk). */
struct efs_chunk_rec {
    efs_ino_t ino;
    uint32_t chunk_index;
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
};

/* Phase 2b: one inode size/mtime update (the write path grows a file and bumps
 * mtime without a full inode upsert, so other fields are left untouched). */
struct efs_ino_size_rec {
    efs_ino_t ino;
    uint64_t size;
    uint64_t mtime;
    uint32_t mtime_nsec;
    /* EFS_INO_REC_F_TIMES: apply mtime/atime even if older, and grow
     * size despite an older mtime. ecopy futimens pins times locally and
     * piggybacks them on the close REPORT instead of a SETATTR RPC. */
    uint32_t flags;
    efs_ino_t pack_ino;
    uint32_t pack_off;
    uint32_t pack_len;
    uint64_t atime;
};

#define EFS_INO_REC_F_TIMES  0x1u

#define EFS_GETCHUNKS_MAX 64
struct efs_msg_inode_getchunks {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint32_t start; /* first chunk_index to consider */
    uint32_t max;
};

struct efs_msg_inode_getchunks_reply {
    uint8_t status;
    /* On NOT_PRIMARY: owner of the request's chunk group. */
    efs_node_id_t primary_id;
    uint32_t count;
    struct efs_chunk_rec recs[EFS_GETCHUNKS_MAX];
};

/* Phase 3b: drop mappings with chunk_index >= first_chunk. Reply is
 * struct efs_msg_inode_reply. */
struct efs_msg_inode_drop_chunks {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint32_t first_chunk;
};

/* Phase 2b: batched dirty-metadata report (replaces the client blob flush).
 * The request payload is this header, then `count` struct efs_chunk_rec, then
 * `ino_count` struct efs_ino_size_rec. When sync is set, the primary commits
 * the export (server_flush_fragmented_meta) before replying — this is the
 * fsync durability barrier now that clients no longer blob-flush. */
struct efs_msg_report_chunks {
    efs_export_id_t export_id;
    uint32_t count;     /* chunk recs */
    uint32_t sync;      /* 0 = async (mark dirty); 1 = commit before replying */
    uint32_t ino_count; /* inode size/mtime recs (after the chunk recs) */
};

struct efs_msg_upgrade_meta {
    char export_name[EFS_MAX_NAME];
    uint32_t shard_bits;
};

#define EFS_UPGRADE_OK     0
#define EFS_UPGRADE_ERROR  1
#define EFS_UPGRADE_NOT_FOUND 2

struct efs_msg_upgrade_meta_reply {
    uint8_t status;
    uint32_t shard_count;
};

/* RDMA QP bootstrap. Both ends create an RC QP first, then exchange the
 * addressing needed for the RTR transition. Native IB only: dlid + sl. */
#define EFS_RDMA_STATUS_OK          0
#define EFS_RDMA_STATUS_UNSUPPORTED 1

struct efs_msg_rdma_setup {
    uint32_t lid;      /* sender port LID */
    uint32_t qpn;      /* sender QP number */
    uint32_t psn;      /* sender initial PSN */
    uint32_t mtu;      /* sender active MTU (enum ibv_mtu value) */
    uint32_t buf_size; /* sender recv buffer size = max RDMA frame accepted */
};

struct efs_msg_rdma_setup_reply {
    uint32_t status;   /* EFS_RDMA_STATUS_* */
    uint32_t lid;
    uint32_t qpn;
    uint32_t psn;
    uint32_t mtu;
    uint32_t buf_size;
};

/* Send a single message. */
int efs_send_msg(int fd, uint8_t type, const void *payload, uint32_t payload_len);

/* Send a message whose payload is two contiguous logical parts (writev),
 * avoiding a bounce buffer when the on-wire payload is header+body. */
int efs_send_msg_parts(int fd, uint8_t type,
                       const void *part1, uint32_t part1_len,
                       const void *part2, uint32_t part2_len);

/* Receive a single message. Caller must free *payload with free(). */
int efs_recv_msg(int fd, uint8_t *type, void **payload, uint32_t *payload_len);

/* Zero-copy variant: receive a message whose payload is exactly
 * [1-byte status][hdr_len bytes header][body_len bytes body]. Status, header,
 * and body are read directly into caller-provided buffers (no malloc, no extra
 * copy). The wire payload length must equal 1 + hdr_len + body_len exactly,
 * else EFS_ERR_PROTO. On success *type and *status are set. */
int efs_recv_msg_into(int fd, uint8_t *type, uint8_t *status,
                      void *hdr, uint32_t hdr_len, void *body, uint32_t body_len);

/* Hot-path helper: receive a 1-byte status reply without malloc.
 * On success sets *type and *status. Returns EFS_OK or an error. */
int efs_recv_u8_reply(int fd, uint8_t *type, uint8_t *status);

/* ---- transport-dispatching variants (struct efs_conn, network.h) ----
 * Same semantics as the fd-based originals. With a live RDMA QP, frames up
 * to the pool buffer size go over the QP; larger frames (and GET_META /
 * GET_META_ROOT, whose replies are unbounded) use the TCP side-channel.
 * Replies follow the request's channel, so the receiver always knows where
 * to wait (c->recv_chan). */
struct efs_conn;

int efs_conn_send_msg(struct efs_conn *c, uint8_t type, const void *payload,
                      uint32_t payload_len);
int efs_conn_send_msg_parts(struct efs_conn *c, uint8_t type,
                            const void *part1, uint32_t part1_len,
                            const void *part2, uint32_t part2_len);
int efs_conn_recv_msg(struct efs_conn *c, uint8_t *type, void **payload,
                      uint32_t *payload_len);
int efs_conn_recv_msg_into(struct efs_conn *c, uint8_t *type, uint8_t *status,
                           void *hdr, uint32_t hdr_len,
                           void *body, uint32_t body_len);
int efs_conn_recv_u8_reply(struct efs_conn *c, uint8_t *type, uint8_t *status);

/* Wait for the next request on either channel. Returns EFS_CONN_TCP /
 * EFS_CONN_RDMA, or -1 on error / peer close. Pure-TCP conns return
 * EFS_CONN_TCP immediately (caller blocks in recv). Shared so the xprt
 * test cannot drift from the server conn thread. */
int efs_conn_wait_request(struct efs_conn *c);

/* For multi-conn reply polling (parallel PUT): arm/peek the reply channel.
 * Returns EFS_CONN_REPLY_READY when a reply is already available, -1 on
 * error, else an fd (>= 0) to poll for POLLIN. */
#define EFS_CONN_REPLY_READY (-2)
int efs_conn_reply_watch(struct efs_conn *c);
/* Same contract, but never spin-polls the CQ: for harvest loops that check
 * several conns per wait iteration. */
int efs_conn_reply_watch_quick(struct efs_conn *c);
/* Fixed-budget spin variant for the PUT reply wait. */
int efs_conn_reply_watch_us(struct efs_conn *c, int budget_us);

#endif
