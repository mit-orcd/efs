#ifndef EFS_PROTOCOL_H
#define EFS_PROTOCOL_H

#include "efs/common.h"
#include "efs/metadata.h"
#include "efs/meta_cmd.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Length-prefixed TCP frames: 4 bytes length (network order), 1 byte type,
 * length-1 bytes payload. Encode/decode lives in efs/wire.h; this header
 * is the type catalog plus the I/O send/recv declarations. */

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
    /* 8..11 retired: GET_META / PUT_META (old whole-table metadata engine). */
    EFS_MSG_LIST_NODES = 12,
    EFS_MSG_LIST_NODES_REPLY = 13,
    /* 14/15 retired: CREATE_EXPORT (mkfs is EFS_MSG_RAFT_MKFS). */
    EFS_MSG_JOIN = 16,
    EFS_MSG_JOIN_REPLY = 17,
    EFS_MSG_STATUS = 18,
    EFS_MSG_STATUS_REPLY = 19,
    /* 20/21 retired: REMOVE_NODE (node lifecycle is a control-plane op). */
    EFS_MSG_SHRINK_QUOTA = 22,
    EFS_MSG_SHRINK_QUOTA_REPLY = 23,
    EFS_MSG_NODE_LEFT = 24,
    EFS_MSG_QUERY_STATS = 25,
    EFS_MSG_QUERY_STATS_REPLY = 26,
    /* 27..32 retired: LIST_EXPORTS, DRAIN_NODE, UNDRAIN_NODE. */
    /* Network bench: same payload as PUT_CHUNK; server ACKs and discards. */
    EFS_MSG_BENCH_PUT = 33,
    EFS_MSG_BENCH_PUT_REPLY = 34,
    /* 35/36 retired: DESTROY_EXPORT. */
    /* Per-export feature switches (.stats/.find). Server-owned. */
    /* 37/38 retired: SET_FEATURES (features are not persisted). */
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
    /* 53/54 retired: UPGRADE_META (old-engine shard rehash). */
    /* RDMA QP bootstrap over the TCP conn; unknown-type/error -> stay TCP. */
    EFS_MSG_RDMA_SETUP = 55,
    EFS_MSG_RDMA_SETUP_REPLY = 56,
    /* 57..62 retired: META_FLUSH_BEGIN, GET_META_ROOT, and rename-by-ino
     * (the hosted rename is EFS_MSG_INODE_RENAME_AT). */
    /* Server-owned metadata mutations. Replies reuse struct
     * efs_msg_inode_reply (status + resulting inode). */
    EFS_MSG_INODE_SETATTR = 63,
    EFS_MSG_INODE_SETATTR_REPLY = 64,
    EFS_MSG_INODE_LINK = 65,
    EFS_MSG_INODE_LINK_REPLY = 66,
    /* Client reports written-chunk mappings (ino → nodes + checksums) to the
     * shard's Raft group, batched; the apply layer publishes them. The reply
     * is struct efs_msg_inode_reply (status + primary_id; inode unused). */
    EFS_MSG_REPORT_CHUNKS = 67,
    EFS_MSG_REPORT_CHUNKS_REPLY = 68,
    /* Reads: peer pulls chunk mappings for an inode it just looked up. */
    EFS_MSG_INODE_GETCHUNKS = 69,
    EFS_MSG_INODE_GETCHUNKS_REPLY = 70,
    /* Cross-client O_APPEND: reserve the next `len` bytes on the inode's
     * shard and return the reserved offset in reply.size - len. The client
     * then writes the data there via the normal dcache/PUT/REPORT path.
     * Without this, two clients each appended at their own stale cached end
     * and tore or lost each other's lines. */
    EFS_MSG_INODE_APPEND = 71,
    EFS_MSG_INODE_APPEND_REPLY = 72,
    /* 73..78 retired: the CREATE/LINK/UNLINK shard fan-out. A file create
     * is one Raft entry; the multi-shard cases are transactions. */
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
    /* 89..92 retired: DROP_CHUNKS fan-out (chunk lifetime is the KV apply
     * layer, spec L7) and the two-phase metadata root commit. */

    /* Production Raft host — the only metadata engine.
     * EFS_MSG_RAFT carries one efs_raft_msg encoded by
     * efs_wire_raft_encode (wire.h); the reply is a delivery ack only —
     * Raft tolerates loss, so processing is asynchronous. */
    EFS_MSG_RAFT = 93,
    EFS_MSG_RAFT_REPLY = 94,
    /* Propose on a group leader (scratch/mgmt + host cross-group submit).
     * Empty payload: idempotent MKFS on the ROOT group's leader (not the
     * export mkfs path). Non-empty: byte 0 is the group id; the rest is a
     * log command (or empty = ReadIndex). Session GET is sub=0 of
     * EFS_MD_CMD_SESSION (not a log command); salt carries epoch/state/
     * touched. EFS_MD_CMD_CFG is host-only (I18): persist desired, attach
     * a learner with C_old, then efs_raft_change — not a log command.
     * Same reply shape. Not a new opcode — 10.5c-24 reuses this
     * so a coordinator can submit to a group it does not lead without
     * inventing EFS_MSG_RAFT_PROPOSE. */
    EFS_MSG_RAFT_MKFS = 95,
    EFS_MSG_RAFT_MKFS_REPLY = 96,
    /* Per-group role/term/commit/applied readout for the smoke gate. */
    EFS_MSG_RAFT_STATUS = 97,
    EFS_MSG_RAFT_STATUS_REPLY = 98,

    /* Data-plane GC (spec L7): the reaper asks the node holding a dead
     * generation's fragment to delete it, proven by the fragment's
     * checksum sidecar (fragment paths do not carry the generation, so
     * the conditional delete never removes a newer generation's bytes).
     * Idempotent: an absent fragment is a success. */
    EFS_MSG_GC_FRAGMENT = 99,
    EFS_MSG_GC_FRAGMENT_REPLY = 100,
    /* Extended attribute on one inode. Request is struct efs_msg_xattr
     * followed by name[nlen] and value[vlen]. The reply begins with the
     * same status + primary_id layout as efs_msg_inode_reply so a client
     * can retry NOT_PRIMARY and BUSY, then nbytes and the bytes. */
    EFS_MSG_XATTR = 101,
    EFS_MSG_XATTR_REPLY = 102,
};

/* GC_FRAGMENT request: delete fragment `fragment_index` of chunk
 * `chunk_index` of `ino` iff its stored checksum sidecar equals
 * `checksum` (the dead generation's fragment hash from the GC record). */
struct efs_msg_gc_fragment {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint32_t chunk_index;
    uint32_t fragment_index;
    uint8_t checksum[EFS_HASH_SIZE];
    uint64_t chunk_generation; /* 0 = legacy `{ci}.{fi}` */
};

/* status: 0 = the dead fragment is gone (deleted / already absent / a
 * newer generation occupies the slot); nonzero = try again later. */
struct efs_msg_gc_fragment_reply {
    uint8_t status;
    uint8_t pad[7];
};

/* XATTR request. name[nlen] and value[vlen] follow this header with no
 * padding. op is EFS_XATTR_GET/SET/REMOVE/LIST. sizeof is 24. */
struct efs_msg_xattr {
    efs_export_id_t export_id;
    uint32_t flags;
    efs_ino_t ino;
    uint32_t vlen;
    uint16_t nlen;
    uint8_t op;
    uint8_t pad;
};

/* First two fields match efs_msg_inode_reply so the client retry loop can
 * read status and primary_id. Only the first hdr+nbytes bytes are sent. */
struct efs_msg_xattr_reply {
    uint8_t status;
    efs_node_id_t primary_id;
    uint32_t nbytes;
    uint8_t data[EFS_XATTR_BLOB_MAX];
};

/* RAFT_MKFS reply. rc: EFS_OK (accepted at index, and applied on this
 * leader for a non-empty submit / ReadIndex-ready for a group-only
 * payload), EFS_ERR_NOT_PRIMARY (not the leader; leader_hint is the
 * peer id or -1), EFS_ERR_INVAL (host off). Empty-payload MKFS is still
 * fire-and-forget at the leader (poll RAFT_STATUS for applied). */
struct efs_msg_raft_mkfs_reply {
    int32_t rc;
    int32_t leader_hint;
    uint64_t index;
    uint64_t salt; /* this process's mkfs salt candidate */
    uint64_t term; /* term of the entry at index (forwarded commands); the
                    * forwarder matches (index, term) in its apply ring */
};

#define EFS_RAFT_HOST_MAX_GROUPS 4

struct efs_raft_group_status {
    uint8_t group;
    uint8_t role;    /* EFS_RAFT_FOLLOWER/CANDIDATE/LEADER */
    uint8_t hosted;  /* this node runs a replica of this group */
    uint8_t joint;   /* 1 while C_old,C_new overlapping (I18) */
    int32_t leader;  /* peer id, -1 unknown */
    uint32_t voters; /* committed voting-set bitmask (app_old) */
    uint32_t pad2;
    uint64_t term;
    uint64_t commit_index;
    uint64_t applied_index;
};

struct efs_msg_raft_status_reply {
    int32_t rc;
    uint32_t node_id;
    uint64_t kv_has_root; /* 1 once the mkfs command applied locally */
    uint64_t export_salt; /* valid when kv_has_root */
    uint32_t ngroups;
    uint32_t pad;
    struct efs_raft_group_status groups[EFS_RAFT_HOST_MAX_GROUPS];
};

/* GET_FEATURES reply: the export's current feature mask. */
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
    /* Candidate identity. 0 = legacy `{ci}.{fi}` object (pre-W1 files). */
    uint64_t chunk_generation;
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
    /* Candidate identity. 0 = write the legacy `{ci}.{fi}` name. */
    uint64_t chunk_generation;
    /* W14.4: 0 = no hint (probe every root). Else the storage-root index
     * from the previous PUT of this (ino, chunk, fragment), plus one.
     * EFS_PATH_HINT_NEW means this generation has never been PUT: the
     * server creates on the least-queued root and does not access().
     * A retry of a failed reply sends 0, not NEW. */
    uint32_t path_hint;
    /* uint8_t data[data_len]; */
};

#define EFS_PATH_HINT_NEW    0xffffffffu

#define EFS_PUT_CHUNK_OK     0
#define EFS_PUT_CHUNK_ERROR  1
#define EFS_PUT_CHUNK_QUOTA_EXCEEDED 2

#define EFS_BENCH_PUT_OK     0
#define EFS_BENCH_PUT_ERROR  1

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

/* Only value on the wire: node lifecycle states went with the migration
 * engine. STATUS_REPLY always reports it. */
#define EFS_NODE_STATE_ACTIVE 0

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
/* REPORT CAS lost: the client's RMW base is no longer committed. */
#define EFS_INODE_RPC_STALE     11
/* getxattr/removexattr of a name that is not set. Not ENOENT. */
#define EFS_INODE_RPC_NODATA    12

struct efs_msg_inode_lookup {
    efs_export_id_t export_id;
    efs_ino_t parent;
    char name[EFS_MAX_NAME];
};

#define EFS_LOOKUP_PATH_MAX_DEPTH 64
#define EFS_LOOKUP_PATH_F_ANCESTORS (1u << 0)
/* `start` is the directory the path is relative to; 0 means the export root.
 * Resolving relative to an arbitrary ancestor lets the client walk a path
 * deeper than EFS_LOOKUP_PATH_MAX_DEPTH in ceil(depth/64) round trips while
 * still collecting every ancestor for the exec-permission check. */
struct efs_msg_inode_lookup_path {
    efs_export_id_t export_id;
    uint32_t flags;
    efs_ino_t start;
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
    /* flock_token; CREATE_F_HOLD writes this into the Raft CREATE uuid so
     * apply can LEASE_OPEN in the same entry. Close still uses HOLD. */
    uint64_t owner;
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
     * inos survive compaction, so an ino cursor is stable. 0 = from start.
     *
     * Raft host: the KV scans a directory in NAME order, so
     * the ino cursor cannot work (name order != ino order — entries would
     * be skipped). The raft path uses (after_src, after_name) instead: the
     * exact resume cookie from the previous reply's next_src/next_name.
     * 0/"" = from start. */
    uint64_t after_ino;
    uint32_t after_src;
    char after_name[EFS_MAX_NAME];
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
    /* Raft host resume cookie: pass back verbatim as after_src/after_name.
     * next_done=1 means the directory scan is exhausted. */
    uint32_t next_src;
    uint32_t next_done;
    char next_name[EFS_MAX_NAME];
};

/* Rename/move a specific dentry to a new parent + name. */
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
    uint32_t atime_nsec;
};

/* Phase 2b: add a hard link (extra name) for an existing non-directory inode. */
struct efs_msg_inode_link {
    efs_export_id_t export_id;
    efs_ino_t src_ino;
    efs_ino_t new_parent;
    char new_name[EFS_MAX_NAME];
};

/* Nested nlink mutation on the child-row owner. */


/* Cross-client O_APPEND reservation; reply is struct efs_msg_inode_reply
 * with inode.size = THIS reservation's end (eof+len), not the live
 * watermark. offset = inode.size - len. Two concurrent reserves must
 * not share an offset.
 * Optional suffix: EFS_SESS_WIRE_LEN (uuid[16] + native uint32 epoch)
 * or EFS_APPEND_OPID_LEN (that plus native uint64 seq). seq!=0 makes
 * a retried reserve I16 (same offset). Absent keeps the zero stand-in.
 * Do not grow this struct. */
struct efs_msg_inode_append {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint64_t len;
};

/* flags: 1 = open (+1 ref), 0 = close (-1 ref). owner is the process/ofd
 * id. Optional EFS_SESS_WIRE_LEN suffix carries (uuid, epoch); absent
 * keeps the stand-in. Do not grow this struct. */
struct efs_msg_inode_hold {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint32_t flags;
    uint64_t owner;
};

/* op is the flock(2) operation (LOCK_SH/EX/UN, optional LOCK_NB).
 * EFS_FLOCK_FCNTL selects the record-lock domain (classic fcntl);
 * without it the opcode is the flock(2) domain. An optional suffix of
 * EFS_FLOCK_RANGE_LEN bytes (two native uint64_t start,end, half-open)
 * selects a byte range; absent ⇒ whole file [0, ~0]. Do not grow this
 * struct — live FUSE flock sends sizeof(struct efs_msg_inode_flock).
 * EFS_FLOCK_GETLK is F_GETLK: a leader read, no Raft entry. Reply is
 * struct efs_msg_inode_reply: nlink=0 (F_UNLCK) or SH/EX, size=start,
 * ctime=end, ino=blocker owner id. EFS_FLOCK_WAIT (with EX/SH) is a
 * blocking wait: a conflicting grant is queued at the leader (FIFO,
 * leader memory, not Raft state) and the held RPC's reply IS the grant;
 * leader loss replies NOT_PRIMARY and the client re-issues.
 * Optional session identity (10.5c-35b): EFS_SESS_WIRE_LEN bytes
 * (uuid[16] + native uint32 epoch) after the struct, or after the
 * range suffix. Absent ⇒ stand-in (owner packed into uuid, epoch 1).
 * HOLD takes the same optional suffix after struct efs_msg_inode_hold. */
#define EFS_FLOCK_SH 1u
#define EFS_FLOCK_EX 2u
#define EFS_FLOCK_NB 4u
#define EFS_FLOCK_UN 8u
#define EFS_FLOCK_FCNTL 16u
#define EFS_FLOCK_GETLK 32u
#define EFS_FLOCK_WAIT 64u
#define EFS_FLOCK_RANGE_LEN 16u
#define EFS_SESS_WIRE_LEN 20u
#define EFS_APPEND_OPID_LEN (EFS_SESS_WIRE_LEN + 8u)
/* Directory mutations (INODE_CREATE incl. mkdir, INODE_UNLINK incl.
 * rmdir, INODE_LINK, INODE_RENAME_AT) take an optional EFS_OPID_WIRE_LEN
 * suffix (opid.h: uuid[16] + native u32 epoch + native u64 seq + native
 * u64 contiguous ack) after the fixed struct. With it the op is I16: a
 * retry with the same (uuid, epoch, seq) returns the recorded verdict
 * instead of EEXIST / ENOENT for an op that already committed. Absent
 * keeps the unprotected behaviour. Do not grow those structs. */
#define EFS_DIROP_OPID_LEN 36u
struct efs_msg_inode_flock {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint32_t op;
    uint64_t owner;
};

/* Phase 2b: one written-chunk mapping record (matches efs_export_set_chunk).
 * REPORT: base_gen is the committed generation the writer patched
 * (0 = empty slot; EFS_CHUNK_BASE_UNCOND = aligned last-writer-wins).
 * chunk_generation is the object name used for PUT `{ci}.{fi}.{gen}` —
 * the host must store this as the committed generation so GET can find
 * the fragments (recomputing a hash here drifted from the client).
 * GETCHUNKS: both fields are the committed generation of the mapping. */
#define EFS_CHUNK_BASE_UNCOND UINT64_MAX
struct efs_chunk_rec {
    efs_ino_t ino;
    uint32_t chunk_index;
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
    uint64_t base_gen;
    uint64_t chunk_generation;
    /* delta_len > 0: this rec is one immutable span [delta_off, +len),
     * not a full-chunk CAS. chunk_generation names a full-chunk object;
     * readers copy only that range. delta_len == 0: full-chunk publish
     * or the base image in a GETCHUNKS reply. delta_base_n /
     * delta_base_seq are the delta list the writer observed (full CAS)
     * or the list attached to this base image (GETCHUNKS). deltas[] is
     * meaningful on GETCHUNKS. */
    uint32_t delta_off;
    uint32_t delta_len;
    uint32_t delta_base_n;
    uint32_t delta_pad;
    uint64_t delta_base_seq;
    struct efs_chunk_delta deltas[EFS_CHUNK_DELTA_MAX];
};

/* W35: on the wire a rec is the head (through delta_base_seq) plus
 * delta_base_n span records. A full image sends no delta array, so a
 * 1 GiB REPORT is ~1.4 MB instead of 10 MB. The in-memory struct is
 * unchanged. Same ABI on every node (one build id). */
static inline uint32_t efs_chunk_rec_delta_n(const struct efs_chunk_rec *r)
{
    uint32_t n = r ? r->delta_base_n : 0;
    if (n > EFS_CHUNK_DELTA_MAX)
        n = EFS_CHUNK_DELTA_MAX;
    return n;
}

static inline size_t efs_chunk_rec_wire_size(const struct efs_chunk_rec *r)
{
    return offsetof(struct efs_chunk_rec, deltas) +
           (size_t)efs_chunk_rec_delta_n(r) * sizeof(struct efs_chunk_delta);
}

/* Returns the bytes written, 0 if dst is short. */
static inline size_t efs_chunk_rec_pack(uint8_t *dst, size_t cap,
                                        const struct efs_chunk_rec *r)
{
    size_t head = offsetof(struct efs_chunk_rec, deltas);
    size_t n = efs_chunk_rec_wire_size(r);
    uint32_t dn;

    if (!dst || !r || n > cap)
        return 0;
    memcpy(dst, r, head);
    dn = efs_chunk_rec_delta_n(r);
    if (dn)
        memcpy(dst + head, r->deltas, (size_t)dn * sizeof(struct efs_chunk_delta));
    return n;
}

/* Unpack `count` recs. *used is the byte length consumed. -1 if short
 * or delta_base_n is past the cap. */
static inline int efs_chunk_recs_unpack(const uint8_t *src, size_t len,
                                        uint32_t count,
                                        struct efs_chunk_rec *out,
                                        size_t *used)
{
    size_t off = 0;
    size_t head = offsetof(struct efs_chunk_rec, deltas);
    uint32_t i;

    if (used)
        *used = 0;
    for (i = 0; i < count; i++) {
        uint32_t n;

        if (!out)
            return -1;
        memset(&out[i], 0, sizeof(out[i]));
        if (off + head > len)
            return -1;
        memcpy(&out[i], src + off, head);
        n = out[i].delta_base_n;
        if (n > EFS_CHUNK_DELTA_MAX)
            return -1;
        off += head;
        if (n) {
            size_t dsz = (size_t)n * sizeof(struct efs_chunk_delta);
            if (off + dsz > len)
                return -1;
            memcpy(out[i].deltas, src + off, dsz);
            off += dsz;
        }
    }
    if (used)
        *used = off;
    return 0;
}

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


#define EFS_UPGRADE_OK     0
#define EFS_UPGRADE_ERROR  1
#define EFS_UPGRADE_NOT_FOUND 2


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
int efs_recv_put_reply(int fd, uint8_t *type, uint8_t *status, uint8_t *path);

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
/* PUT reply: status byte, then an optional storage-root index (0xff = none).
 * A one-byte reply leaves *path as 0xff. */
int efs_conn_recv_put_reply(struct efs_conn *c, uint8_t *type, uint8_t *status,
                            uint8_t *path);

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
