#ifndef EFS_PROTOCOL_H
#define EFS_PROTOCOL_H

#include "efs/common.h"
#include "efs/opid.h"
#include "efs/metadata.h"
#include "efs/meta_cmd.h"
#include "efs/pack_ledger.h"
#include "efs/membership.h"
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

/* Ordinary PUT body: status + path + 64-bit verifier, fixed endian. */
#define EFS_PUT_REPLY_LEN 10u
static inline uint64_t efs_put_epoch_read(const uint8_t *p)
{ uint64_t v=0; for(unsigned i=0;i<8;i++) v=(v<<8)|p[i]; return v; }
static inline void efs_put_epoch_write(uint8_t *p, uint64_t v)
{ for(unsigned i=0;i<8;i++) p[i]=(uint8_t)(v>>(56-8*i)); }

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
    /* 16/17 retired: JOIN (efs-mgmt add-node; a node joins with efsd
     * --join, F4). See EFS_MSG_IS_RETIRED. */
    EFS_MSG_STATUS = 18,
    EFS_MSG_STATUS_REPLY = 19,
    /* 20/21 retired: REMOVE_NODE (node lifecycle is a control-plane op). */
    EFS_MSG_SHRINK_QUOTA = 22,
    EFS_MSG_SHRINK_QUOTA_REPLY = 23,
    /* 24 retired: NODE_LEFT (W132). Membership changes are Raft
     * configuration changes / JOIN; see EFS_MSG_IS_RETIRED. */
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
    /* 41/42 retired: ADD_STORAGE (a node's roots are fixed when its store
     * is created, F11). See EFS_MSG_IS_RETIRED. */
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
    /* 81-82 retired (HEAL_STATUS; see EFS_MSG_IS_RETIRED). */
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
    /* Empty transport reply; never a durable consensus ACK. */
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

    /* 99-100 retired (single-fragment GC_FRAGMENT; see EFS_MSG_IS_RETIRED). */
    /* Extended attribute on one inode. Request is struct efs_msg_xattr
     * followed by name[nlen] and value[vlen]. The reply begins with the
     * same status + primary_id layout as efs_msg_inode_reply so a client
     * can retry NOT_PRIMARY and BUSY, then nbytes and the bytes. */
    EFS_MSG_XATTR = 101,
    EFS_MSG_XATTR_REPLY = 102,
    /* Data-plane I/O counters: cumulative per op class since efsd start;
     * consumers diff. Reply is struct efs_msg_io_stats_reply. */
    EFS_MSG_IO_STATS = 103,
    EFS_MSG_IO_STATS_REPLY = 104,
    /* Human-facing build identity ("efsd v... (branch ..., built ... on
     * host)"); informational only, the HELLO gate stays EFS_BUILD_ID.
     * Reply is struct efs_msg_version_reply. */
    EFS_MSG_VERSION = 105,
    EFS_MSG_VERSION_REPLY = 106,
    EFS_MSG_INODE_WRITER_VIEW = 107,
    EFS_MSG_INODE_WRITER_VIEW_REPLY = 108,
    EFS_MSG_LANE_WRITER_VIEW = 109,
    EFS_MSG_LANE_WRITER_VIEW_REPLY = 110,
    EFS_MSG_LANE_BOOTSTRAP = 111,
    EFS_MSG_LANE_BOOTSTRAP_REPLY = 112,
    EFS_MSG_PUBLICATION = 113,
    EFS_MSG_PUBLICATION_REPLY = 114,
    EFS_MSG_PUBLICATION_STATUS = 115,
    EFS_MSG_PUBLICATION_STATUS_REPLY = 116,
    EFS_MSG_PUBLICATION_RETIRE = 117,
    EFS_MSG_PUBLICATION_RETIRE_REPLY = 118,
    EFS_MSG_GC_STATUS = 119,
    EFS_MSG_GC_STATUS_REPLY = 120,
    /* 121-122 retired (single-inode GC_INODE; see EFS_MSG_IS_RETIRED). */
    EFS_MSG_QUERY_SNAPSHOT = 123,
    EFS_MSG_QUERY_SNAPSHOT_REPLY = 124,
    /* Opt-in pending CREATE with exact result proof and retained routing. */
    EFS_MSG_CREATE_RETAINED = 125,
    EFS_MSG_CREATE_RETAINED_REPLY = 126,
    EFS_MSG_STORAGE_SESSION_FENCE = 127,
    EFS_MSG_STORAGE_SESSION_FENCE_REPLY = 128,
    /* 129/130 retired: the staged PUT_TICKETED opcode. Every data PUT is
     * EFS_MSG_PUT_CHUNK carrying an admitted ticket (K13). */
    /* 131-150 are reserved: never reuse. */
    /* Asynchronous PUT durability: COMMIT (client fsync) and stats. */
    EFS_MSG_DATA_COMMIT = 151,
    EFS_MSG_DATA_COMMIT_REPLY = 152,
    EFS_MSG_SYNC_STATS = 153,
    EFS_MSG_SYNC_STATS_REPLY = 154,
    /* W124: batched journal-first reclaim acceptance (one per node per batch). */
    EFS_MSG_GC_INODE_BATCH = 155,
    EFS_MSG_GC_INODE_BATCH_REPLY = 156,
    /* W125 packed segments (packer to storage node, and reads). */
    EFS_MSG_PUT_SEGMENT = 157,
    EFS_MSG_PUT_SEGMENT_REPLY = 158,
    EFS_MSG_GET_SEGMENT = 159,
    EFS_MSG_GET_SEGMENT_REPLY = 160,
    /* W125: journal-first reclaim acceptance of retired segments. */
    EFS_MSG_GC_SEGMENT_BATCH = 161,
    EFS_MSG_GC_SEGMENT_BATCH_REPLY = 162,
    /* W125: journal-first acceptance of superseded fragments (GC records). */
    EFS_MSG_GC_FRAG_BATCH = 163,
    EFS_MSG_GC_FRAG_BATCH_REPLY = 164,
    /* W125/W133: protocol declaration, the first message of every client
     * connection; nothing but HEARTBEAT is served before it is accepted. */
    EFS_MSG_CLIENT_HELLO = 165,
    EFS_MSG_CLIENT_HELLO_REPLY = 166,
    /* W125: request scheduled-maintenance compaction now (efs-mgmt
     * pack-compact); segments = 0 only reports the fragmentation summary. */
    EFS_MSG_PACK_COMPACT = 167,
    EFS_MSG_PACK_COMPACT_REPLY = 168,
    /* W145 repair: ask this node to resync one group from its leader's
     * snapshot (efs-mgmt raft-resync). Request struct efs_msg_raft_resync,
     * reply struct efs_msg_raft_resync_reply. */
    EFS_MSG_RAFT_RESYNC = 169,
    EFS_MSG_RAFT_RESYNC_REPLY = 170,
    /* W64 failover harness: make this node stop leading one group now
     * (efs-mgmt raft-step-down). With a target voter it is a leadership
     * transfer (TIMEOUT_NOW); without one the node merely steps down and
     * may win the next election again. Request struct
     * efs_msg_raft_step_down, reply struct efs_msg_raft_step_down_reply. */
    EFS_MSG_RAFT_STEP_DOWN = 171,
    EFS_MSG_RAFT_STEP_DOWN_REPLY = 172,
    /* K28/W151 (D38): linearizable pack-object read for a node that holds
     * a segment fragment but hosts no replica of the segment's group. A
     * hosting voter answers after its ReadIndex; anything else refuses.
     * Request struct efs_msg_pack_object_read, reply
     * struct efs_msg_pack_object_read_reply. */
    EFS_MSG_PACK_OBJECT_READ = 173,
    EFS_MSG_PACK_OBJECT_READ_REPLY = 174,
    /* W154 + K13: the group leader's rebuild of the one missing fragment of
     * a degraded stripe. Header EFS_REBUILD_PUT_HEADER_LEN: export:4, then
     * the degraded mark fixed-endian (EFS_REBUILD_MARK_LEN: ino:8
     * inode_gen:8 object_gen:8 ci:4 lane:1 missing:1 nodes:4*3
     * checksums:32*3); then the fragment. The object is published,
     * so no ticket authorizes it: storage stores it only when the durable
     * mark names exactly this fragment for this node. Reply: the
     * EFS_PUT_REPLY_LEN PUT reply. */
    EFS_MSG_REBUILD_PUT = 175,
    EFS_MSG_REBUILD_PUT_REPLY = 176,
    /* D31 + K13: the lane group leader's collection of one abandoned
     * fragment. Payload EFS_PUT_ORPHAN_DELETE_LEN: export:4, fragment:4,
     * then the 216-byte ticket. The member deletes fragment `fragment`
     * only on itself (nodes[fragment]), behind its own storage session
     * floor above the ticket's epoch, with the group's fresh orphan record
     * as authority and only while the stored bytes carry the ticket's
     * checksum. Reply struct efs_msg_put_orphan_delete_reply: OK when no
     * copy remains, durably. */
    EFS_MSG_PUT_ORPHAN_DELETE = 177,
    EFS_MSG_PUT_ORPHAN_DELETE_REPLY = 178,
    /* K33 live client view: this node's committed membership record, for a
     * mounted client to install a later revision without a remount
     * (efs/member_view.h). No request payload; reply
     * struct efs_msg_membership_reply sent as EFS_MEMBERSHIP_REPLY_HDR + len
     * bytes; rc EFS_ERR_UNFORMED before this node learned a formation. */
    EFS_MSG_MEMBERSHIP_READ = 245,
    EFS_MSG_MEMBERSHIP_READ_REPLY = 246,
};
#define EFS_MEMBERSHIP_REPLY_HDR 8u
struct efs_msg_membership_reply {
    int32_t rc;
    uint32_t len;                            /* record bytes that follow */
    uint8_t record[EFS_MEMBERSHIP_MAX_LEN];  /* efs_membership_encode */
};
#define EFS_PUT_ORPHAN_DELETE_LEN (8u + 216u)
struct efs_msg_put_orphan_delete_reply { int32_t rc; };
#define EFS_REBUILD_MARK_LEN (8u + 8u + 8u + 4u + 1u + 1u + \
                              4u * EFS_NUM_FRAGMENTS + EFS_HASH_SIZE * EFS_NUM_FRAGMENTS)
#define EFS_REBUILD_PUT_HEADER_LEN (4u + EFS_REBUILD_MARK_LEN)

/* Retired message IDs: no handler in this build. The server closes a
 * connection that sends one (handler.c retired_refuse); never reuse them. */
#define EFS_MSG_IS_RETIRED(t) \
    ((t) == 24 /* NODE_LEFT */ || \
     (t) == 16 || (t) == 17 /* JOIN (F4) */ || \
     (t) == 41 || (t) == 42 /* ADD_STORAGE (F11) */ || \
     (t) == 129 || (t) == 130 /* PUT_TICKETED (K13) */ || \
     (t) == 81 || (t) == 82 /* HEAL_STATUS */ || \
     (t) == 99 || (t) == 100 /* GC_FRAGMENT */ || \
     (t) == 121 || (t) == 122 /* GC_INODE */)

struct efs_msg_raft_resync {
    uint8_t group;
    uint8_t pad[7];
};

struct efs_msg_raft_resync_reply {
    int32_t rc; /* EFS_OK, EFS_ERR_NOT_PRIMARY (this node leads it), EFS_ERR_INVAL */
    uint32_t pad;
};

struct efs_msg_raft_step_down {
    uint8_t group;
    int8_t target; /* raft peer id (node-id - 1) to transfer to; -1 = plain step-down */
    uint8_t pad[6];
};

struct efs_msg_raft_step_down_reply {
    int32_t rc;      /* EFS_OK, EFS_ERR_NOT_PRIMARY (not leading), EFS_ERR_BUSY (transfer
                        target not caught up / group frozen), EFS_ERR_INVAL */
    int32_t raft_id; /* this node's raft peer id */
    uint64_t term;   /* the term it led when it stepped down */
};

struct efs_msg_pack_compact {
    uint32_t segments; /* output segments to build per led group; 0 = report only */
    uint32_t reserved;
};
/* Background packer counters of one node since start (all groups it led). */
struct efs_packer_status {
    uint64_t passes, candidates, segments, files_packed, files_cancelled,
             unindexed, read_errors, put_errors, admit_errors, abandoned,
             busy_passes, budget_waits, compacted, backfilled,
             abandon_scanned,    /* segment objects examined by the abandon sweep */
             abandon_actionable, /* of those, foreign admissions with pending records */
             abandon_refused,    /* candidates whose full decode failed (not cancelled) */
             base_segments,      /* packed-base segments read (one per segment per pass) */
             base_files;         /* files whose packed base came from those reads */
};
struct efs_msg_pack_compact_reply {
    int32_t rc;
    uint32_t led_groups;      /* metadata groups this node leads (bit per group index) */
    uint64_t compacted_total; /* records moved by compaction since start */
    /* Last fragmentation summary of the led groups (summed). */
    uint64_t segments, live_bytes, dead_bytes, holes, small;
    struct efs_packer_status packer;
};

/* Connection declaration (W125, tightened by W133). The first message on
 * a client connection is CLIENT_HELLO naming exactly EFS_PROTOCOL_VERSION;
 * there is one supported protocol, no minimum and no range. Until a
 * declaration has been accepted the server serves nothing but HEARTBEAT:
 * any other request is refused by closing the connection, with no reply
 * and one logged line, so an undeclared or mismatched client fails closed
 * instead of being served by accident. A declaration that names another
 * version is answered with EFS_ERR_PROTO and this version, then closed.
 * Daemon-to-daemon connections declare like clients before their own
 * HELLO/build-id join gate (peer_pool.c). A declaration is not
 * authentication.
 * Version history: 2 = packed references in GETCHUNKS (W125);
 * 3 = every request gated, exact version match (W133);
 * 4 = every metadata mutation carries a registered session (K5/W134). */
#define EFS_PROTOCOL_VERSION 5u
struct efs_msg_client_hello { uint8_t magic[4]; uint32_t version; uint64_t features; };
struct efs_msg_client_hello_reply { int32_t rc; uint32_t version; };
/* Send the declaration and check the reply; 0 when accepted. */
struct efs_conn;
int efs_conn_declare(struct efs_conn *c);
int efs_fd_declare(int fd);
/* Connect to an efsd and declare in one step; -1 (fd closed) when the
 * connection or the declaration fails. The declaration itself is bounded by
 * EFS_IO_TIMEOUT_MS; the fd is returned without socket timeouts, as
 * efs_connect_tcp returns it. */
int efs_connect_declared(const char *host, uint16_t port);

/* Server-side gate, one per accepted connection (handler.c). Every received
 * frame passes through efs_decl_gate_frame before dispatch. */
struct efs_decl_gate { int declared; };
#define EFS_GATE_DISPATCH 0 /* serve the request */
#define EFS_GATE_ANSWERED 1 /* a declaration: reply sent, keep the connection */
#define EFS_GATE_REFUSE   2 /* refused: close the connection without a reply */
/* 0 only for the two messages a connection may send before declaring:
 * CLIENT_HELLO (the declaration) and HEARTBEAT (liveness, no state). */
int efs_declaration_required(uint8_t type);
/* EFS_OK when payload is a CLIENT_HELLO for exactly EFS_PROTOCOL_VERSION. */
int efs_client_hello_check(const void *payload, uint32_t len);
int efs_decl_gate_frame(struct efs_decl_gate *g, struct efs_conn *c, uint8_t type,
                        const void *payload, uint32_t len);

/* W125 segment messages, big-endian byte layouts:
 * PUT   [group:1][fragment:1][0:2][geometry:4][id:8][generation:8][checksum:32]
 *       then geometry/2 payload bytes; reply efs_msg_segment_put_reply (rc).
 * GET   [group:1][fragment:1][0:2][geometry:4][id:8][generation:8];
 *       reply [rc:4][payload bytes:4][checksum:32] then the payload.
 * GC    [group:1][0:3][count:4] then count [id:8][generation:8];
 *       reply efs_msg_gc_inode_batch_reply (one rc per item).
 * FRAG  [export:4][count:4] then count
 *       [ino:8][generation:8][chunk:4][fragment:1][0:3][checksum:32];
 *       reply efs_msg_gc_inode_batch_reply. */
#define EFS_SEGMENT_PUT_HEADER 56u
#define EFS_SEGMENT_GET_HEADER 24u
#define EFS_SEGMENT_GET_REPLY 40u
#define EFS_GC_SEGMENT_BATCH_MAX 256u
#define EFS_GC_FRAG_BATCH_MAX 1024u
#define EFS_GC_FRAG_ITEM_BYTES 56u

/* Storage session seal/check. Reply echoes the exact identity and floor.
 * check_only never writes; missing/corrupt certificates are not success. */
struct efs_msg_storage_session_fence { uint8_t uuid[16]; uint32_t floor, check_only; };
struct efs_msg_storage_session_fence_reply { uint8_t uuid[16]; uint32_t floor; int32_t rc; };

struct efs_msg_segment_put_reply { int32_t rc; };
/* K28/W151: one encoded pack object (efs_pack_object_encode); equals
 * EFS_PACK_OBJECT_MAX_BYTES (pack_mapping.h, asserted in raft_host.c). */
#define EFS_PACK_OBJECT_READ_BYTES (128u + EFS_PACK_LEDGER_MAX_BYTES + EFS_HASH_SIZE)
struct efs_msg_pack_object_read { uint32_t group, reserved; uint64_t id, generation; };
/* rc: EFS_OK (object[0..len) is the encoded object), EFS_ERR_NOT_FOUND
 * (absent after the ReadIndex: proven gone), anything else refuses. */
struct efs_msg_pack_object_read_reply {
    uint32_t group;
    int32_t rc;
    uint64_t id, generation;
    uint32_t len, reserved;
    uint8_t object[EFS_PACK_OBJECT_READ_BYTES];
};
/* Request: header then count items; reply: count int32 results in order.
 * EFS_OK = durably journalled and fenced (accepted for reclaim); BUSY = not
 * accepted (journal full, fail-closed or recovering), retry later. */
#define EFS_GC_INODE_BATCH_MAX 1024u
struct efs_msg_gc_inode_batch {
    uint32_t export_id, count;
    struct { uint64_t ino, generation; } items[];
};
struct efs_msg_gc_inode_batch_reply {
    uint32_t count, reserved;
    int32_t rc[];
};

/* Runtime counters, reset on restart. Queue samples cover leader groups only;
 * reap_capped means reap_seen is a lower bound. Ages are monotonic durations,
 * UINT64_MAX means never observed. File bytes are unlinked lengths, not a
 * promise that the filesystem has released blocks held by another open fd. */
/* GC syscall counters, not device IOPS: buffered checksum reads can hit
 * page cache; unlink bytes stay zero (reclaimed bytes are reported separately).
 * Directory sync includes partial-pass and absent-file retries. */
#define EFS_GC_IO_CLASSES 3
struct efs_gc_io_stats {
    uint64_t ops, bytes, errors, us_sum;
};
/* W124 node reclaim. accepted/deleted/checkpointed count inodes since start;
 * journal_* describe unretired journal state; pending_bytes were credited to
 * quota at acceptance but are still on disk. fail_closed=1: journal loss was
 * detected and data PUTs are refused until recovery completes. */
struct efs_reclaim_status {
    uint64_t accepted, deleted, checkpointed, retired, busy;
    uint64_t journal_records, journal_segments, journal_epoch;
    uint64_t pending_bytes, credited_bytes, phys_avail_bytes, phys_refused;
    uint32_t fail_closed, recovering;
    uint64_t recovered;
};
/* Process-local observations, reset on restart. Namespace bytes are logical
 * metadata bytes observed at deletion, never physical reclaimed bytes. */
struct efs_expiry_status {
    uint32_t available, enabled, healthy, uncertain, leader_mask, sampled_mask;
    uint64_t removed_files, namespace_bytes, size_unknown_files, refused, errors, passes;
    uint64_t last_pass_age_ms, last_delete_age_ms;
    uint64_t due_lower_bound[2], sample_age_ms[2];
    uint32_t due_capped[2];
};

struct efs_msg_gc_status_reply {
    uint32_t version, node_id, stage, group;
    uint32_t sampled_mask, exports, reap_capped[2];
    uint64_t passes, pass_elapsed_ms, last_pass_age_ms, last_delete_age_ms;
    uint64_t removed_fragments, removed_file_bytes, removed_payload_bytes, data_usage_bytes;
    uint64_t delete_errors, mismatches, sweep_errors, reap_errors, ack_errors;
    uint64_t scan_errors, missing_exports;
    uint64_t pending[2], reap_seen[2], orphan_seen[2], first_reap[2], first_seen_age_ms[2];
    /* Version 2 appends checksum_read, unlink, dir_sync in that order. */
    struct efs_gc_io_stats io[EFS_GC_IO_CLASSES];
    /* Version 3 appends expiry; prior offsets remain stable. */
    struct efs_expiry_status expiry;
    /* Version 4 appends W124 node reclaim state. */
    struct efs_reclaim_status reclaim;
    /* Version 5 appends D25 TRUNC_PENDING inodes seen by the last pass. */
    uint64_t trunc_pending[2];
};

/* D25 read authority for write admission, including holes and new lanes.
 * Fixed bounded history; FileID and chunk identity must match the request.
 * This is a snapshot, not a promise against subsequent fences. */
struct efs_msg_inode_writer_view {
    efs_ino_t ino;
    uint64_t generation; /* zero discovers FileID; nonzero validates it */
    uint32_t chunk_index;
    uint32_t reserved;
};
struct efs_msg_inode_writer_view_reply {
    uint8_t status;
    efs_node_id_t primary_id;
    efs_ino_t ino;
    uint64_t generation;
    uint32_t chunk_index;
    uint32_t reserved;
    uint64_t authority_epoch;
    uint64_t oldest_complete_epoch;
    struct efs_fence_history history;
};
static inline int efs_writer_view_reply_valid(
    const struct efs_msg_inode_writer_view *req,
    const struct efs_msg_inode_writer_view_reply *reply)
{
    if (!req || !reply || reply->ino != req->ino ||
        (!reply->generation || (req->generation && reply->generation != req->generation)) ||
        reply->chunk_index != req->chunk_index || reply->reserved ||
        reply->oldest_complete_epoch > reply->authority_epoch ||
        efs_fence_history_valid(&reply->history) != EFS_OK)
        return EFS_ERR_PROTO;
    uint64_t floor = reply->authority_epoch;
    for (uint32_t i = reply->history.count; i > 0; --i) {
        uint64_t epoch = reply->history.entries[i - 1].epoch;
        if (epoch > reply->authority_epoch)
            return EFS_ERR_PROTO;
        if (epoch != floor)
            break;
        --floor;
    }
    return reply->oldest_complete_epoch >= floor ? EFS_OK : EFS_ERR_PROTO;
}

/* Exact FileID is mandatory: generation discovery belongs to cold bootstrap.
 * Geometry is echoed and validated, never inferred from absent chunk rows. */
struct efs_msg_lane_writer_view {
    efs_ino_t ino;
    uint64_t generation;
    uint32_t chunk_index;
    uint32_t chunk_size;
};
struct efs_msg_lane_writer_view_reply {
    struct efs_msg_inode_writer_view_reply view;
    uint32_t chunk_size;
    uint32_t reserved;
};
static inline int efs_lane_writer_view_reply_valid(
    const struct efs_msg_lane_writer_view *req,
    const struct efs_msg_lane_writer_view_reply *reply)
{
    if (!req || !reply || !req->generation || !efs_chunk_size_valid(req->chunk_size) ||
        reply->chunk_size != req->chunk_size || reply->reserved)
        return EFS_ERR_PROTO;
    struct efs_msg_inode_writer_view legacy = {req->ino, req->generation, req->chunk_index, 0};
    return efs_writer_view_reply_valid(&legacy, &reply->view);
}

/* Cold-only: the server validates requested geometry against this export.
 * The reply is efs_msg_lane_writer_view_reply; an exact FileID is mandatory. */
struct efs_msg_lane_bootstrap {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint64_t generation;
    uint32_t chunk_index;
    uint32_t chunk_size;
};

/* XATTR request. name[nlen] and value[vlen] follow this header with no
 * padding. op is EFS_XATTR_GET/SET/REMOVE/LIST. SET and REMOVE append the
 * mandatory EFS_SESS_WIRE_LEN mount session suffix (K5). sizeof is 24. */
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

/* Per-peer replication progress in raft-status (efs_raft_peer_progress).
 * Ages are Raft ticks (5 ms); UINT64_MAX = never. */
#define EFS_RAFT_STATUS_PEERS EFS_MAX_NODES /* == EFS_RAFT_MAX_PEERS (raft.h) */
struct efs_raft_peer_status {
    uint64_t next_index, match_index;
    uint64_t inflight, inflight_end;
    uint64_t inflight_age, ack_age;
    uint64_t batches_sent, send_again, send_blocked, send_errors;
    uint64_t rejects, reject_hint;
    uint64_t busy;      /* K22: busy refusals/advisories from this peer */
    uint64_t busy_hold; /* K22: 1 while entries are withheld from it */
};

struct efs_raft_group_status {
    uint8_t group;
    uint8_t role;    /* EFS_RAFT_FOLLOWER/CANDIDATE/LEADER */
    uint8_t hosted;  /* this node runs a replica of this group */
    uint8_t joint;   /* 1 while C_old,C_new overlapping (I18) */
    int32_t leader;  /* peer id, -1 unknown */
    uint32_t voters; /* committed voting-set bitmask (app_old) */
    uint8_t retired; /* K33: not a target voter; never campaigns or serves */
    uint8_t pad2[3];
    uint64_t term;
    uint64_t commit_index;
    uint64_t applied_index;
    /* W145: this replica's log tail and follower-side refusals, and the
     * replication progress it sees for each peer (filled for every peer;
     * meaningful on the leader). */
    uint64_t last_log_index;
    uint64_t last_log_term;
    uint64_t snap_index;
    uint64_t rejects_sent;
    uint64_t proto_drops;
    uint32_t npeers;
    uint32_t resync_pending; /* W145: efs-mgmt raft-resync requested, not yet installed */
    /* K22/W23 bound: entries past applied (committed or not) and its cap;
     * AppendEntries refused busy (follower), proposals refused BUSY (leader). */
    uint64_t unapplied, max_unapplied, busy_sent, admit_busy;
    /* K22: may not lead now; elections held; capacity step-downs. */
    uint64_t low_capacity, campaigns_held, capacity_stepdowns;
    struct efs_raft_peer_status peers[EFS_RAFT_STATUS_PEERS];
};

struct efs_msg_raft_status_reply {
    int32_t rc;
    uint32_t node_id;
    uint64_t kv_has_root; /* 1 once the mkfs command applied locally */
    uint64_t export_salt; /* valid when kv_has_root */
    uint32_t ngroups;
    uint32_t pad;
    /* K22/W23 node bound: KV unflushed memtable/WAL tail vs its derived
     * budget, L0 bytes vs cap, applies deferred at the budget, and
     * proposals refused at the 3/4 admission watermark. */
    uint64_t kv_mt_bytes, kv_mem_budget, kv_l0_bytes, kv_l0_cap;
    uint64_t kv_apply_deferred, admit_busy;
    struct efs_raft_group_status groups[EFS_RAFT_HOST_MAX_GROUPS];
};

/* GET_FEATURES reply: the export's current feature mask. */
#define EFS_FEATURES_OK        0
#define EFS_FEATURES_NOT_FOUND 1
#define EFS_FEATURES_UNAVAILABLE 2
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
    efs_node_id_t node_id; /* K33: 0 for a joining store (no node id yet) */
    char addr[64];
    uint16_t port;
    char storage_path[EFS_MAX_PATH];
    uint64_t quota; /* local storage quota in bytes; 0 = unlimited */
    uint64_t used;  /* bytes currently stored on this node */
    char build_id[EFS_BUILD_ID_LEN]; /* git build id; must match the cluster's */
    uint64_t token;   /* K33: the sender's member-store token (never 0) */
    uint64_t cluster; /* K33: its cluster identity (seed, promised or formed); 0 while joining */
    uint32_t flags;   /* EFS_HELLO_F_* */
};

/* K33: the sender's store holds a committed membership record (formed);
 * without it a sender with a cluster is a seed or holds a promise. */
#define EFS_HELLO_F_FORMED 1u

struct efs_msg_hello_ack {
    uint32_t epoch;
    efs_node_id_t assigned_id;
    uint32_t node_count;
    struct efs_node nodes[EFS_MAX_NODES];
    uint32_t reject_reason; /* EFS_HELLO_REJECT_*; valid when assigned_id == 0 */
    char build_id[EFS_BUILD_ID_LEN]; /* receiver's build id (for diagnostics) */
    /* K33, to a joining store (node_id 0): the cluster it is joining, and
     * a record: the formation proposal to promise (EFS_HELLO_PROMISE), or
     * the committed revision naming it (accepted: assigned_id is its id). */
    uint64_t cluster;
    uint32_t formed; /* 1: the cluster is formed (an addition is queued when pending) */
    uint32_t record_len;
    uint8_t record[EFS_MEMBERSHIP_MAX_LEN];
};

#define EFS_HELLO_REJECT_NONE    0
/* F14: a formed cluster of EFS_MAX_NODES members refuses a joining store;
 * the joiner exits. */
#define EFS_HELLO_REJECT_FULL    1
#define EFS_HELLO_REJECT_VERSION 2
/* W153/K33: the joiner can never be a member of this formed cluster: its
 * id or endpoint is a member with another store token, it belongs to
 * another cluster, or its store does not match. The joiner exits. */
#define EFS_HELLO_REJECT_FORMED  3
/* K33: the joiner is not a member yet; its addition is in progress (or
 * waits for a lower node id). The joiner retries HELLO until its store has
 * learned a committed revision naming it. Before formation: its id is held
 * by another store token (refused, exits) -- see EFS_HELLO_REJECT_FORMED. */
#define EFS_HELLO_REJECT_PENDING 4
/* K33 (M4'): to a joining store named in the seed's formation proposal:
 * persist a promise for `record` (it is node assigned_id), then say HELLO
 * with that id. */
#define EFS_HELLO_PROMISE        5

struct efs_msg_get_chunk {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint32_t chunk_index;
    uint32_t fragment_index; /* 0 = D1, 1 = D2, 2 = P */
    /* Candidate identity, nonzero; generation 0 names no object (NOT_FOUND). */
    uint64_t chunk_generation;
};

/* Reply payload: [status:1][checksum:32][fragment_data:data_len]
 * data_len = frame_payload_len - 1 - EFS_HASH_SIZE when status == OK. */
#define EFS_GET_CHUNK_OK     0
#define EFS_GET_CHUNK_NOT_FOUND 1
#define EFS_GET_CHUNK_ERROR  2
/* I25: the stored fragment exists but was refused (unknown format, another
 * identity under this name, or payload/record corruption). Never served;
 * the reader treats it as unavailable and decodes from verified siblings. */
#define EFS_GET_CHUNK_INTEGRITY 3

/* Data PUT frame (K13, W86): fixed-endian export id, fragment index and the
 * immutable admitted ticket (put_upload.h), then exactly ticket.fragment_len
 * bytes. There is no unticketed data PUT; storage refuses any other shape.
 * No path hint: an ambiguous retry must probe every storage root. */
#define EFS_PUT_HEADER_LEN 224u
/* Network bench only (EFS_MSG_BENCH_PUT): header + data_len bytes, discarded. */
struct efs_msg_bench_put {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint32_t chunk_index;
    uint32_t fragment_index;
    uint8_t checksum[EFS_HASH_SIZE];
    uint32_t data_len;
    uint64_t chunk_generation;
    uint32_t path_hint;
    /* uint8_t data[data_len]; */
};

/* PUT reply: EFS_PUT_REPLY_LEN bytes, status + path + boot verifier. */
#define EFS_PUT_CHUNK_OK     0
#define EFS_PUT_CHUNK_ERROR  1
#define EFS_PUT_CHUNK_QUOTA_EXCEEDED 2
#define EFS_PUT_CHUNK_BUSY 3 /* transient admission; no durable PUT receipt */
/* Terminal: the ticket is not ADMITTED (settled, retired, unknown) or its
 * session is fenced. Never retried with the same ticket. */
#define EFS_PUT_CHUNK_STALE 4

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
    uint32_t reserved; /* zero; aligns the capacity fields */
    uint64_t capacity_bytes;
    uint64_t available_bytes;
    uint64_t capacity_valid;
    /* W154: degraded stripes awaiting rebuild on the groups this node
     * leads (bit g/2 of degraded_sampled says groups[g/2] is current),
     * and this node's rebuild totals since start. */
    uint64_t degraded_chunks[2];
    uint32_t degraded_sampled;
    uint32_t reserved2;
    uint64_t rebuilt_fragments, rebuild_failures, rebuild_retired;
    /* D31: abandoned-upload records pending on the groups this node leads
     * (bit g/2 of put_orphan_sampled says groups[g/2] finished a scan),
     * and this node's collection totals since start. */
    uint64_t put_orphan_pending[2];
    uint32_t put_orphan_sampled;
    uint32_t reserved3;
    uint64_t put_orphan_collected, put_orphan_deleted, put_orphan_failures;
};

/* DATA_COMMIT: make every data fragment this server acknowledged before
 * the request durable (NFS COMMIT). Empty request. */
struct efs_msg_data_commit_reply {
    int32_t rc;           /* EFS_OK or EFS_ERR_IO (a durability barrier failed) */
    uint32_t roots;       /* async roots covered */
    uint64_t boot_epoch;  /* nonzero durability verifier; retained only after all-root clean stop */
};

/* SYNC_STATS: per storage root durability state. Empty request.
 * mode: 2 asynchronous acknowledgement; 0 root not attached. */
#define EFS_SYNC_STATS_ROOTS 8
struct efs_sync_root_stats {
    char root[160];
    uint32_t mode;
    uint32_t error;               /* barrier failed or recovery uncertain; COMMIT returns EIO */
    uint64_t unsynced_bytes;      /* acknowledged, not yet durable */
    uint64_t unsynced_fragments;
    uint64_t oldest_unsynced_ms;  /* age of the oldest unsynced fragment */
    uint64_t acked_bytes;         /* since start, async acknowledgements */
    uint64_t synced_bytes;        /* of those, made durable */
    uint32_t boot;                /* epoch = (boot, seq) */
    uint32_t prev_clean;          /* previous run stopped cleanly */
    uint64_t current_seq, durable_seq;
    uint64_t barriers, barrier_failures;
    uint64_t last_barrier_ms_ago, last_barrier_us;
    uint64_t recovery_scanned, recovery_bad;  /* after an unclean restart */
    uint32_t recovery_done, reserved;
};
struct efs_msg_sync_stats_reply {
    uint32_t count;
    uint32_t sync_window_ms;
    uint64_t sync_window_bytes;
    struct efs_sync_root_stats roots[EFS_SYNC_STATS_ROOTS];
};

/* IO_STATS reply: one row per op class (0 = GET_CHUNK handler, 1 =
 * PUT_CHUNK handler, 2 = writer-pool disk write, 3 = store read,
 * 4 = GC delete RPC). Counters are cumulative
 * since efsd start; p50_us is the median of the last 64 latencies. */
#define EFS_IO_STATS_CLASSES 5
struct efs_io_stats_class {
    uint64_t ops;
    uint64_t bytes;
    uint64_t errors;
    uint64_t us_sum;
    uint64_t us_max;
    uint64_t p50_us;
};
/* Process-wide RDMA message transport counters since start: connections
 * already closed plus a snapshot of the live ones (efs_rdma_totals). */
struct efs_io_stats_rdma {
    uint64_t live_conns;
    uint64_t data_frames_tx, data_bytes_tx, data_frames_rx, data_bytes_rx;
    uint64_t credit_stalls;
};
struct efs_msg_io_stats_reply {
    uint64_t uptime_us;
    struct efs_io_stats_class cls[EFS_IO_STATS_CLASSES];
    struct efs_io_stats_rdma rdma;
};

/* VERSION reply: fixed-size NUL-terminated string (efs_version_string of
 * the answering efsd), truncated if longer. Fixed size keeps the strict
 * reply_len == sizeof check valid. */
#define EFS_VERSION_REPLY_LEN 128
struct efs_msg_version_reply {
    char version[EFS_VERSION_REPLY_LEN];
};

struct efs_msg_shrink_quota {
    uint64_t amount; /* bytes to subtract from the current quota */
};

#define EFS_SHRINK_QUOTA_OK     0
#define EFS_SHRINK_QUOTA_ERROR  1
#define EFS_SHRINK_QUOTA_IN_PROGRESS 2

/* Only value on the wire: node lifecycle states went with the migration
 * engine. STATUS_REPLY always reports it. */
#define EFS_NODE_STATE_ACTIVE 0

#define EFS_MAX_QUERY_USERS 256

struct efs_user_stat {
    uint32_t uid;
    uint64_t files;
    uint64_t bytes;
};

/* Current QUERY_STATS_REPLY is one byte: logical query integration is
 * unavailable. Legacy struct replies are not authoritative statistics. */
#define EFS_QUERY_STATS_UNSUPPORTED 1u

/* W92 bounded server snapshots. Paths are export-relative UTF8/raw bytes,
 * newline separated; newline names fail ILSEQ. No partial-success replies.
 * Credentials use the same trusted mount boundary as inode RPCs; this does
 * not introduce an authentication protocol. Unknown versions fail explicitly.
 * Version 1 carries only the efs-fuse directory .stats aggregate
 * (operation STATS); a v1 FIND or LS is refused. */
#define EFS_QUERY_VERSION 1
/* Version 4: W91 command pages, ASCII JSON envelopes with hex-encoded
 * relative path bytes. Versions 2 (unpaged commands) and 3 (unpaged filtered
 * find) are retired and refused. */
#define EFS_QUERY_PAGE_VERSION 4
#define EFS_QUERY_GROUPS 128
#define EFS_QUERY_MAX_OUTPUT (16u << 20)
#define EFS_QUERY_MAX_VISITS 1000000u
#define EFS_QUERY_STATS 0
#define EFS_QUERY_FIND 1
#define EFS_QUERY_LS 2
struct efs_msg_query_snapshot {
    uint32_t version,operation;
    efs_ino_t root;
    uint32_t uid,gid,ngroups,match;
    uint32_t max_output,visit_budget;
    uint32_t groups[EFS_QUERY_GROUPS];
    char term[EFS_MAX_NAME];
};
/* Page filters. Size bounds are inclusive; modification timestamps are
 * strict ns bounds.
 * Unset fields MUST be zero; kind: 0 any, 1 regular, 2 directory, 3 symlink. */
#define EFS_QUERY_MIN_SIZE 1u
#define EFS_QUERY_MAX_SIZE 2u
#define EFS_QUERY_AFTER 4u
#define EFS_QUERY_BEFORE 8u
struct efs_query_filters {
    uint32_t flags,kind;
    uint64_t min_size,max_size,after_ns,before_ns;
};
/* v4 bounded pages. Cursor is a physical index position, not a result offset.
 * token=0 starts one retained generation; limit=0 closes it. */
struct efs_msg_query_snapshot_v4 {
    struct efs_msg_query_snapshot base;
    struct efs_query_filters filters;
    uint64_t token,cursor;
    uint32_t limit,nonce;
};
struct efs_msg_query_snapshot_reply {
    uint32_t version;
    int32_t status,refresh_error;
    uint16_t primary_id,reserved;
    uint32_t text_len,uid,gid,checked_dirs;
    uint64_t boot_id,generation,source_ns,built_ns,applied[2],terms[2];
    uint64_t mtime,atime,ctime;
    /* text_len bytes immediately follow; no terminating NUL on wire. */
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
/* Explicit metadata/output capacity refusal; never transient BUSY. */
#define EFS_INODE_RPC_OVERFLOW  13
/* Session refusals (K5): EFS_ERR_FENCED / EFS_ERR_UNREGISTERED. */
#define EFS_INODE_RPC_FENCED    14
#define EFS_INODE_RPC_UNREGISTERED 15

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

/* Piggyback an open hold on CREATE so create+open is one RPC. Release
 * still sends HOLD−1; skipping that leaks refs and last-link unlink
 * never deletes. */
#define EFS_CREATE_F_HOLD (1u << 0)
/* Opt-in pending adoption, requires exact op-id and established shard session. */
#define EFS_CREATE_F_PENDING (1u << 1)

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
    /* Successful LOOKUP/GETATTR only: node that collected the reply, not
     * the inode group's leader. A route hint, never cached read authority.
     * Uses former alignment padding; primary_id/inode offsets stay. */
    uint8_t read_node_id;
    /* Phase 2b: on EFS_INODE_RPC_NOT_PRIMARY, the server's view of the
     * metadata primary (lowest-id live node) so the client can retry there. */
    efs_node_id_t primary_id;
    struct efs_inode inode;
};

/* Separate framing keeps historical inode replies unchanged. A read-only
 * probe may discover routing; once routed, clients retain that shard across
 * ambiguity. known=1 means a ReadIndex-confirmed namespace receipt. */
struct efs_msg_create_retained {
    struct efs_msg_inode_create create;
    struct efs_opid_req q;
    uint32_t shard;
    uint8_t probe, routed;
};
struct efs_msg_create_retained_reply {
    struct efs_msg_inode_reply rpc;
    uint64_t generation;
    uint32_t shard;
    uint8_t routed, known;
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
 * Mandatory suffix (K5): EFS_APPEND_ACK_LEN = uuid[16] + native uint32
 * epoch + native uint64 seq + native uint64 contiguous ACK, selecting the
 * durable reservation receipt on the inode shard. Any other length is
 * refused UNREGISTERED. Retired identities fail closed; a full receipt
 * window refuses allocation. Do not grow this struct. */
struct efs_msg_inode_append {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint64_t len;
};
/* K15/W64: len | EFS_APPEND_ABANDON abandons the identity in the ACK suffix
 * (same uuid/epoch/seq, same length) after its reserve outcome stayed
 * unknown. The server retires the receipt (later reserve/replay is STALE)
 * and resolves a still-OPEN reservation as a hole. OK reply carries no
 * offset; inode.nlink = 1 when an OPEN reservation was retired. */
#define EFS_APPEND_ABANDON (1ull << 63)

/* flags: 1 = open (+1 ref), 0 = close (-1 ref). owner is the process/ofd
 * id. Mandatory EFS_SESS_WIRE_LEN suffix carries the mount session (uuid,
 * epoch); absent is refused UNREGISTERED (K5). Do not grow this struct. */
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
 * Session identity (K5): EFS_SESS_WIRE_LEN bytes (uuid[16] + native
 * uint32 epoch) after the struct, or after the range suffix. Mandatory for
 * every grant and release; GETLK is a read and needs none. Absent on a
 * grant or release is refused UNREGISTERED; there is no stand-in owner.
 * HOLD takes the same mandatory suffix after struct efs_msg_inode_hold. */
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
#define EFS_APPEND_ACK_LEN (EFS_APPEND_OPID_LEN + 8u) /* contiguous client ACK */
/* Directory mutations (INODE_CREATE incl. mkdir, INODE_UNLINK incl.
 * rmdir, INODE_LINK, INODE_RENAME_AT) and INODE_SETATTR carry a mandatory
 * EFS_OPID_WIRE_LEN suffix (opid.h: uuid[16] + native u32 epoch + native
 * u64 seq + native u64 contiguous ack) after the fixed struct (K5). The
 * (uuid, epoch) is the mount session; the op is I16: a retry with the same
 * (uuid, epoch, seq) returns the recorded verdict instead of EEXIST /
 * ENOENT for an op that already committed. Absent is refused
 * UNREGISTERED. Do not grow those structs. */
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
#define EFS_CHUNK_REC_F_CAPTURED_EPOCH 1u
#define EFS_CHUNK_REC_F_CAPTURED_FILEID 2u
#define EFS_CHUNK_REC_F_FRESH_OBJECT 4u
/* 8u is reserved: always refused. */
#define EFS_CHUNK_REC_F_RESERVED 8u
/* GETCHUNKS only (W125): the base is a packed record; packed_ref holds its
 * canonical reference and nodes/checksums are the segment's placement.
 * Never valid in a publication or REPORT. */
#define EFS_CHUNK_REC_F_PACKED 16u
#define EFS_CHUNK_REC_PACKED_REF_BYTES 96u
/* W154: bit 5 + i says fragment i of this PUT object was not durably
 * acknowledged (2+1: at most one). The publication records the stripe as
 * degraded and the group leader rebuilds the fragment when its node is
 * reachable. Publication and REPORT only; not part of the intent digest. */
#define EFS_CHUNK_REC_F_MISSING_SHIFT 5u
#define EFS_CHUNK_REC_F_MISSING_MASK (((1u << EFS_NUM_FRAGMENTS) - 1u) << EFS_CHUNK_REC_F_MISSING_SHIFT)
static inline uint32_t efs_chunk_rec_missing(uint32_t flags)
{
    return (flags & EFS_CHUNK_REC_F_MISSING_MASK) >> EFS_CHUNK_REC_F_MISSING_SHIFT;
}
/* A missing set the 2+1 code can rebuild: empty or exactly one fragment. */
static inline int efs_chunk_missing_valid(uint32_t mask)
{
    return mask < (1u << EFS_NUM_FRAGMENTS) && (mask & (mask - 1u)) == 0;
}
/* REPORT only (W77): the next record belongs to the same atomic publication
 * unit (one FUSE write request, or the closure of overlapping ones). A
 * unit's records are adjacent, of one inode, at most EFS_PUB_UNIT_MAX, and
 * publish all-or-nothing; the REPORT must carry EFS_REPORT_F_PUBID. */
#define EFS_CHUNK_REC_F_UNIT_NEXT 256u /* bit 8: bits 5..7 are the W154 missing mask */
#define EFS_PUB_UNIT_MAX 64u
_Static_assert((EFS_CHUNK_REC_F_UNIT_NEXT & EFS_CHUNK_REC_F_MISSING_MASK) == 0,
               "chunk-record flags: the W77 unit bit must not overlap the W154 missing mask");
struct efs_chunk_rec {
    efs_ino_t ino;
    uint32_t chunk_index;
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
    uint64_t base_gen;
    uint64_t chunk_generation;
    uint64_t file_generation; /* exact FileID: REPORT ticket FileID (always); CAPTURED_FILEID also binds the authority check */
    uint64_t publish_epoch; /* byte-backed view used by this PUT */
    uint32_t publish_flags;
    /* REPORT (K13): the admitted PUT ticket that names chunk_generation.
     * put_seq is the mount's PUT stream sequence (uuid/epoch come from the
     * REPORT session), file_generation is the ticket's FileID. Every
     * REPORT record carries one; zero only on GETCHUNKS. */
    uint32_t put_fragment_len;
    uint64_t put_seq;
    uint32_t put_member_mask;
    uint32_t put_pad; /* zero */
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
    struct efs_fence_view read_view; /* GETCHUNKS only; REPORT leaves zero */
    struct efs_chunk_delta deltas[EFS_CHUNK_DELTA_MAX];
    /* GETCHUNKS with EFS_CHUNK_REC_F_PACKED only. After deltas[], so the
     * variable-length REPORT encoding never carries it. */
    uint8_t packed_ref[EFS_CHUNK_REC_PACKED_REF_BYTES];
};

/* Per-publication transport status is separate from its durable verdict.
 * UNKNOWN (including NOT_FOUND) never authorizes dropping/rebasing ownership. */
#define EFS_PUBLICATION_APPLY_PENDING 5u /* mapping command committed; verdict unknown; retain bytes */
#define EFS_PUBLICATION_PENDING 4u /* durable intent only: retain bytes; fsync must wait */
#define EFS_PUBLICATION_UNKNOWN 0u
#define EFS_PUBLICATION_COMMITTED 1u
#define EFS_PUBLICATION_REJECTED 2u
#define EFS_PUBLICATION_RETIRED 3u /* do not drop/rebase dirty bytes from this state */
struct efs_msg_publication {
    struct efs_opid id;
    struct efs_chunk_rec rec;
    uint64_t size;
    /* Ticketed envelope: ticketed=1 binds the immutable pre-PUT identity
     * separately from CAS identity. The host refuses ticketed=0 (INVAL, K13);
     * the codec still parses it for the staged client cache tests. */
    struct efs_opid put_id;
    uint32_t put_fragment_len, put_member_mask, ticketed;
};
struct efs_msg_publication_reply {
    struct efs_msg_inode_reply rpc;
    uint32_t state;
    int32_t verdict;
    uint8_t digest[EFS_HASH_SIZE];
    /* Intent locator for PENDING; committed command for APPLY_PENDING;
     * applied mapping/verdict for terminal.
     * Never authorize fsync or byte release from these fields alone. */
    uint32_t receipt_group;
    uint64_t receipt_index, receipt_term;
};

/* Resolve the publication epoch before any registration/proposal side effect.
 * The apply-side fence check must still cover a race after this snapshot. */
static inline int efs_chunk_publish_epoch(const struct efs_chunk_rec *r,
                                          uint64_t current, uint64_t *out)
{
    if (!r || !out || (r->publish_flags &
        ~(EFS_CHUNK_REC_F_CAPTURED_EPOCH | EFS_CHUNK_REC_F_CAPTURED_FILEID |
          EFS_CHUNK_REC_F_FRESH_OBJECT | EFS_CHUNK_REC_F_MISSING_MASK |
          EFS_CHUNK_REC_F_UNIT_NEXT)) ||
        !efs_chunk_missing_valid(efs_chunk_rec_missing(r->publish_flags)) ||
        ((r->publish_flags & EFS_CHUNK_REC_F_CAPTURED_FILEID) &&
         (!(r->publish_flags & EFS_CHUNK_REC_F_CAPTURED_EPOCH) || !r->file_generation)))
        return EFS_ERR_INVAL;
    if ((r->publish_flags & EFS_CHUNK_REC_F_CAPTURED_EPOCH) &&
        r->publish_epoch != current)
        return EFS_ERR_STALE;
    *out = (r->publish_flags & EFS_CHUNK_REC_F_CAPTURED_EPOCH) ?
               r->publish_epoch : current;
    return EFS_OK;
}

/* Typed writers cannot adopt the receiving host's newer inode generation. */
static inline int efs_chunk_publish_authority(const struct efs_chunk_rec *r,
    uint64_t generation, uint64_t epoch, uint64_t *out)
{
    uint64_t captured;
    int rc = efs_chunk_publish_epoch(r, epoch, &captured);
    if (rc != EFS_OK) return rc;
    if ((r->publish_flags & EFS_CHUNK_REC_F_CAPTURED_FILEID) &&
        r->file_generation != generation)
        return EFS_ERR_STALE;
    if (!out) return EFS_ERR_INVAL;
    *out = captured;
    return EFS_OK;
}

/* GETCHUNKS masks must describe exactly the captured object list. Reject
 * malformed ranges before either staging adoption or fragment I/O. */
static inline int efs_chunk_rec_view_valid(const struct efs_chunk_rec *r)
{
    const struct efs_fence_view *v = &r->read_view;
    if (r->publish_flags & EFS_CHUNK_REC_F_RESERVED) return EFS_ERR_PROTO;
    if (r->publish_flags & EFS_CHUNK_REC_F_PACKED) {
        /* A packed base names exactly this file's chunk zero record: the
         * reference's inode (bytes 0-7) and record generation (24-31),
         * big-endian. The client decodes and validates the rest before use. */
        uint64_t ino = 0, gen = 0;
        for (int i = 0; i < 8; i++) {
            ino = ino << 8 | r->packed_ref[i];
            gen = gen << 8 | r->packed_ref[24 + i];
        }
        if (r->chunk_index || ino != r->ino || gen != r->chunk_generation)
            return EFS_ERR_PROTO;
    }
    if (r->delta_base_n > EFS_CHUNK_DELTA_MAX ||
        v->count != r->delta_base_n + 1 ||
        v->fence_epoch > v->revision ||
        !efs_chunk_size_valid(v->chunk_size))
        return EFS_ERR_PROTO;
    for (uint32_t i = 0; i < v->count; ++i) {
        const struct efs_fence_part *p = &v->parts[i];
        if (p->epoch > v->revision || p->off > v->chunk_size ||
            p->len > v->chunk_size - p->off)
            return EFS_ERR_PROTO;
        if (!i) {
            if (p->off || (!r->chunk_generation && p->len))
                return EFS_ERR_PROTO;
        } else {
            const struct efs_chunk_delta *d = &r->deltas[i - 1];
            if (p->off != d->off || p->len > d->len ||
                d->len > v->chunk_size - p->off)
                return EFS_ERR_PROTO;
        }
    }
    return EFS_OK;
}

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
    efs_ino_t reserved_ino;
    uint32_t reserved_off;
    uint32_t reserved_len;
    uint64_t atime;
};

#define EFS_INO_REC_F_TIMES  0x1u

#define EFS_GETCHUNKS_MAX 64
struct efs_msg_inode_getchunks {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint32_t start; /* first chunk_index to consider */
    uint32_t max;
    uint64_t generation; /* zero discovers; otherwise require this FileID */
};

struct efs_msg_inode_getchunks_reply {
    uint8_t status;
    /* On NOT_PRIMARY: owner of the request's chunk group. */
    efs_node_id_t primary_id;
    uint32_t count;
    efs_ino_t ino;
    uint64_t generation;
    uint64_t authority_epoch; /* inode epoch, including an empty result */
    struct efs_chunk_rec recs[EFS_GETCHUNKS_MAX];
};

/* On the wire a GETCHUNKS reply is exactly the header plus its `count`
 * records (each record is 1536 bytes; the full struct is 98 KiB). Fields
 * past recs[count] are never read, so a reply may be used in place or
 * zero-extended into a full struct. */
#define EFS_GETCHUNKS_REPLY_HDR_LEN \
    ((uint32_t)offsetof(struct efs_msg_inode_getchunks_reply, recs))

static inline uint32_t efs_getchunks_reply_len(uint32_t count)
{
    if (count > EFS_GETCHUNKS_MAX)
        count = EFS_GETCHUNKS_MAX;
    return EFS_GETCHUNKS_REPLY_HDR_LEN +
           count * (uint32_t)sizeof(struct efs_chunk_rec);
}

/* Length check before any field of a received reply is trusted. */
static inline int efs_getchunks_reply_len_ok(const void *reply, uint32_t len)
{
    uint32_t count;

    if (!reply || len < EFS_GETCHUNKS_REPLY_HDR_LEN)
        return EFS_ERR_PROTO;
    memcpy(&count, (const uint8_t *)reply +
                   offsetof(struct efs_msg_inode_getchunks_reply, count),
           sizeof(count));
    if (count > EFS_GETCHUNKS_MAX)
        return EFS_ERR_PROTO;
    if (len != efs_getchunks_reply_len(count))
        return EFS_ERR_PROTO;
    return EFS_OK;
}

/* Validate the whole fixed reply before exposing any records to the cache. */
static inline int efs_getchunks_reply_valid(
    const struct efs_msg_inode_getchunks *req,
    const struct efs_msg_inode_getchunks_reply *reply)
{
    if (!req || !reply || !req->ino || reply->ino != req->ino || !reply->generation ||
        (req->generation && req->generation != reply->generation) ||
        reply->count > EFS_GETCHUNKS_MAX)
        return EFS_ERR_PROTO;
    uint32_t max = req->max && req->max < EFS_GETCHUNKS_MAX ? req->max : EFS_GETCHUNKS_MAX;
    uint64_t end = ((uint64_t)req->start | (EFS_CHUNK_GROUP_SIZE - 1u)) + 1;
    if (reply->count > max)
        return EFS_ERR_PROTO;
    for (uint32_t i = 0; i < reply->count; ++i) {
        const struct efs_chunk_rec *r = &reply->recs[i];
        if (r->ino != req->ino || r->chunk_index < req->start ||
            r->chunk_index >= end ||
            (i && r->chunk_index <= reply->recs[i - 1].chunk_index) ||
            efs_chunk_rec_view_valid(r) != EFS_OK)
            return EFS_ERR_PROTO;
    }
    return EFS_OK;
}

/* Phase 3b: drop mappings with chunk_index >= first_chunk. Reply is
 * struct efs_msg_inode_reply. */

/* Phase 2b: batched dirty-metadata report (replaces the client blob flush).
 * The request payload is this header, then `count` packed chunk records, then
 * `ino_count` struct efs_ino_size_rec. When sync is set, the primary commits
 * the export (server_flush_fragmented_meta) before replying — this is the
 * fsync durability barrier now that clients no longer blob-flush. */
/* REPORT session suffix is UUID + big-endian epoch, independent of an
 * operation receipt. EFS_REPORT_F_SESSION is mandatory (K5): a REPORT
 * without it is refused UNREGISTERED. */
#define EFS_REPORT_F_SYNC 1u
#define EFS_REPORT_F_SESSION 2u
#define EFS_REPORT_SESSION_LEN 20u
/* W77: the REPORT's publication identity, minted by the client per built
 * REPORT and reused verbatim when that REPORT is re-sent, so a retry joins
 * the same request-wide decision. Wire: [mint_ns:8 BE][seq:8 BE] directly
 * before the session suffix. */
#define EFS_REPORT_F_PUBID 4u
#define EFS_REPORT_PUBID_LEN 16u
struct efs_report_pubid {
    uint64_t mint_ns;
    uint64_t seq;
};
static inline void efs_report_pubid_pack(uint8_t *p, const struct efs_report_pubid *id)
{
    for (unsigned i = 0; i < 8; i++) {
        p[i] = (uint8_t)(id->mint_ns >> (56 - 8*i));
        p[8+i] = (uint8_t)(id->seq >> (56 - 8*i));
    }
}
static inline int efs_report_pubid_unpack(struct efs_report_pubid *id, const uint8_t *p)
{
    id->mint_ns = id->seq = 0;
    for (unsigned i = 0; i < 8; i++) {
        id->mint_ns = (id->mint_ns << 8) | p[i];
        id->seq = (id->seq << 8) | p[8+i];
    }
    return id->mint_ns && id->seq;
}
/* W155: read-only receipt query. A REPORT with this flag carries no records
 * (count = ino_count = 0), the session and the publication id of an earlier
 * REPORT, and runs nothing: the host answers what its receipt for that
 * identity says (include/efs/request_receipt.h). The reply is
 * struct efs_msg_report_receipt_reply. */
#define EFS_REPORT_F_RECEIPT 8u
struct efs_msg_report_receipt_reply {
    struct efs_msg_inode_reply rpc; /* OK: the query was answered */
    uint32_t state;    /* EFS_RECEIPT_STATE_* */
    uint32_t outcome;  /* EFS_RECEIPT_OUT_* */
    int32_t rc;        /* the last attempt's rc on this host */
    uint32_t attempts; /* attempts executed on this host */
    uint64_t age_ms;   /* since the last attempt ended (0 while pending) */
};
static inline int efs_report_session_valid(const struct efs_opid *id)
{
    uint8_t any = 0;
    if (!id || !id->session_epoch) return 0;
    for (unsigned i = 0; i < 16; i++) any |= id->client_uuid[i];
    return any != 0;
}
static inline void efs_report_session_pack(uint8_t *p, const struct efs_opid *id)
{
    memcpy(p, id->client_uuid, 16);
    for (unsigned i = 0; i < 4; i++)
        p[16+i] = (uint8_t)(id->session_epoch >> (24 - 8*i));
}
static inline int efs_report_session_unpack(struct efs_opid *id, const uint8_t *p)
{
    memset(id, 0, sizeof(*id));
    memcpy(id->client_uuid, p, 16);
    for (unsigned i = 0; i < 4; i++) id->session_epoch = (id->session_epoch << 8) | p[16+i];
    id->seq = 1; /* Internal participant admission only; not a REPORT receipt. */
    return efs_report_session_valid(id);
}
struct efs_msg_report_chunks {
    efs_export_id_t export_id;
    uint32_t count;     /* chunk recs */
    uint32_t sync;      /* EFS_REPORT_F_*; optional session suffix after inode recs */
    uint32_t ino_count; /* inode size/mtime recs (after the chunk recs) */
};


#define EFS_UPGRADE_OK     0
#define EFS_UPGRADE_ERROR  1
#define EFS_UPGRADE_NOT_FOUND 2


/* RDMA QP bootstrap. Both ends create an RC QP first, then exchange the
 * addressing needed for the RTR transition. Native IB only: dlid + sl. */
#define EFS_RDMA_STATUS_OK          0
#define EFS_RDMA_STATUS_UNSUPPORTED 1

/* Segmented setup is explicitly encoded by rdma_message.h, not a native
 * addressing struct. Exact-version peers only; no legacy 20/44-byte setup. */

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
int efs_recv_put_reply(int fd, uint8_t *type, uint8_t *status, uint8_t *path, uint64_t *epoch);

/* ---- transport-dispatching variants (struct efs_conn, network.h) ----
 * Same semantics as the fd-based originals. With a live RDMA QP, all supported complete logical frames go over segmented RDMA. TCP is used
 * only before upgrade, or on explicitly non-upgraded connections.
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
/* PUT reply: status, root index, and mandatory big-endian boot verifier. */
int efs_conn_recv_put_reply(struct efs_conn *c, uint8_t *type, uint8_t *status,
                            uint8_t *path, uint64_t *epoch);

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
/* PUT-only watch consumes an incomplete TCP frame into bounded conn storage.
 * A READY result guarantees recv_put_reply cannot block on missing bytes.
 * RDMA preserves the existing CQ watch behavior (budget_us=0 is quick). */
int efs_conn_put_reply_watch(struct efs_conn *c, int budget_us);

#endif
