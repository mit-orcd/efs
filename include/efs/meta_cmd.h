#ifndef EFS_META_CMD_H
#define EFS_META_CMD_H

/* Raft log command namespace for the metadata engine (architecture.md
 * §7 / §10 step 10.5). Byte 0 is the command tag; multi-byte fields are
 * big-endian. This is ONE namespace shared by the simulator
 * (src/sim/sim_internal.h SIM_CMD_*) and the production raft host
 * (src/server/raft_host.c) — the values must never diverge.
 *
 * Only the commands the production host applies belong here. The sim's
 * remaining SIM_CMD_* values converge onto this header when the
 * coordinator side is lifted out of src/sim (production adoption P2). */

/* Exact bounded key set, so retrying an applied entry cannot consume the
 * next batch. Historical LANE_SWEEP retains whole-lane replay semantics. */
/* W86: [tag:1][immutable ticket:216]. STATUS is ReadIndex-only; ADMIT
 * is serialized apply. Reply salt is ticket state, never permission to PUT. */
/* REPORT envelope: [tag][uuid:16][epoch:4][legacy PUBLISH:202]*N.
 * Fences every mapping at serialized apply without reusing delta fields. */
/* Internal coordinator commands; public control RPC cannot forge ACKs. */
/* 45-51 were packing commands/requests, removed 2026-10-16
 * (archive/packing-containers-20261016). Never reuse; apply refuses them. */
#define EFS_MD_CMD_RETIRED_PACK_ADMIT 45
#define EFS_MD_REQ_RETIRED_PACK_STATUS 46
#define EFS_MD_CMD_RETIRED_PACK_UPLOAD_RETIRE 47
#define EFS_MD_CMD_RETIRED_PACK_GC_ACK 48
#define EFS_MD_CMD_RETIRED_PACK_CANCEL 49
#define EFS_MD_CMD_RETIRED_PACK_GC_RETIRE 50
#define EFS_MD_REQ_RETIRED_PACK_GC_VIEW 51
/* Exact single-shard APPEND image envelope; no replica-local EOF selection. */
#define EFS_MD_APPEND_COMPLETE_ATOMIC_LEN 101u
#define EFS_MD_REQ_EXPIRY_CLOCK 63 /* local clock health only */
#define EFS_MD_REQ_EXPIRY_CLOCK_RESET 64 /* local operator uid4; no namespace mutation */
#define EFS_MD_REQ_EXPIRY_POLICY_INFO 68 /* policy read; OK hint carries enabled/clear flags */
#define EFS_MD_REQ_EXPIRY_ORIGIN 69 /* record root-generation/policy-revision/max; OK hint=clear */
#define EFS_MD_REQ_EXPIRY_PARENT 70 /* inode read: parent/generation/mode */
#define EFS_MD_REQ_EXPIRY_ID 67 /* inode8; authoritative generation/uid/mode/links */
#define EFS_MD_REQ_EXPIRY_ENABLE 66 /* renewal prefix + parent/name; absent-record CAS */
#define EFS_MD_REQ_EXPIRY_DELETE 65 /* guarded due FileID/name; receiver owns clock */
#define EFS_MD_CMD_CREATE_REFUSAL 62 /* shard4 + existing op-id trailer; EEXIST receipt */
#define EFS_MD_CMD_EXPIRY_POLICY 58 /* root/gen/uid/expected/TTL/max/flags:46 */
#define EFS_MD_CMD_EXPIRY_RENEW 59 /* ino/gen/uid/expected/leader-now/deadline:45 */
#define EFS_MD_REQ_EXPIRY_POLICY 60 /* root/gen:17; fresh revision/TTL/max */
#define EFS_MD_REQ_EXPIRY_RECORD 61 /* ino/gen:17; fresh revision/deadline/root */
#define EFS_MD_REQ_REPAIR_SEAL_AUTHORITY 57 /* ReadIndex-only exact eligible job; no seal receipt */
#define EFS_MD_REQ_REPAIR_WRITE_AUTHORITY 56 /* ReadIndex-only canonical job; never a data receipt */
#define EFS_MD_CMD_REPAIR_JOB 55 /* staged: op1, canonical job260, arg4; no RPC/worker admission */
#define EFS_MD_CMD_APPEND_COMPLETE_ATOMIC 54 /* ino/gen/offset/UUID/epoch/content epoch/reservation: 101 bytes */
#define EFS_MD_CMD_APPEND_ATOMIC 53
#define EFS_MD_CMD_QUERY_POLICY 43 /* expected version:8, mask:4 */
#define EFS_MD_CMD_APPEND_CATCHUP 52 /* ino/gen/epoch/published EOF + optional REPORT session */
#define EFS_MD_REQ_QUERY_POLICY 44 /* ReadIndex only, reply index=version/salt=mask */
#define EFS_MD_CMD_REPORT_PUBLISH 41
#define EFS_MD_CMD_PUT_TICKET_RETIRE_PUBLISHED 42
#define EFS_MD_CMD_PUT_TICKET_ADMIT 39
#define EFS_MD_REQ_PUT_TICKET_STATUS 40
#define EFS_MD_REQ_SESSION_APPEND_NEXT 38 /* RPC only: [uuid:16][epoch:4][shard:4] */
#define EFS_MD_REQ_CLOSE_APPEND_NEXT 37 /* RPC only: [ino:8][identity:36] */
#define EFS_MD_CMD_CLOSE_STEP 36 /* [ino:8][identity:36][phase:1], 46 bytes */
#define EFS_MD_REQ_SWEEP_NEXT 35 /* RPC only: leader selects exact keys; never log */
#define EFS_MD_CMD_REAP_BATCH 34 /* [ino:8][gen:8][n:1][len:1,key:31]*n */
#define EFS_MD_CMD_LANE_SWEEP_BATCH 33 /* [ino:8][gen:8][lane:1][n:1][ci:4]*n */
#define EFS_MD_CMD_CLOSE_CLEANUP 32 /* [ino:8][generation:8], retry deferred close */
#define EFS_MD_CMD_PUBLICATION_ADMIT 31 /* immutable durable pending intent */
#define EFS_MD_CMD_ORPHAN_REAP 30 /* [ino:8][generation:8], recheck leases/intents */
#define EFS_MD_CMD_CREATE 1 /* matches sim CMD_CREATE; see pack in raft_host.c */
#define EFS_MD_CMD_UNLINK  2 /* matches sim CMD_UNLINK */
#define EFS_MD_CMD_PUBLICATION_RETIRE 29 /* same immutable payload, exact acknowledgement */
#define EFS_MD_CMD_PUBLICATION 28 /* legacy publish payload + UUID/epoch/seq (28 B) */
#define EFS_MD_CMD_PUBLISH 3 /* matches sim CMD_PUBLISH */
#define EFS_MD_CMD_PREPARE 5 /* matches sim CMD_PREPARE */
#define EFS_MD_CMD_DECIDE  6
#define EFS_MD_CMD_RESOLVE 7
#define EFS_MD_CMD_DROP    8
#define EFS_MD_CMD_SESSION 9 /* matches sim SIM_CMD_SESSION */
#define EFS_MD_SESS_CREATE      1
#define EFS_MD_SESS_REGISTER    2
/* sub=0 is a host ReadIndex GET (not a log command; 10.5c-35a). */
#define EFS_MD_SESS_BEGIN       3
#define EFS_MD_SESS_FENCE_LOC   4
#define EFS_MD_SESS_ACK         5
#define EFS_MD_SESS_FINISH      6
#define EFS_MD_SESS_ESTABLISH   7
#define EFS_MD_SESS_LEASE_OPEN  8
#define EFS_MD_SESS_LEASE_CLOSE 9
#define EFS_MD_SESS_LEASE_DROP  10
#define EFS_MD_SESS_RECLAIM     11
#define EFS_MD_SESS_LEASE_EDGE  12 /* staged guarded edge, fixed 66-byte command */
/* Staged bounded lease/lock phase; append reservations are a separate phase.
 * NEXT is RPC-only; leader selects exact keys before writing BATCH to Raft.
 * [tag,sub,uuid:16,epoch:4,shard:4] + BATCH [n:1][len:1,key:65]*n.
 * Reply salt=1 means this phase is empty; salt=0 means one batch progressed. */
#define EFS_MD_SESS_LEASE_EDGE_DEFERRED 16 /* staged atomic close obligation */
#define EFS_MD_SESS_LEASE_PROBE_DEFERRED 17 /* read-only matching identity domain */
#define EFS_MD_SESS_REGISTER_ESTABLISH 19 /* atomic admission, same Raft group only */
#define EFS_MD_SESS_LEASE_LOCK_NEXT 14
#define EFS_MD_SESS_LEASE_LOCK_BATCH 15
#define EFS_MD_SESS_STORAGE_FENCE 18 /* RPC only: uuid/floor, seal every configured storage member */
#define EFS_MD_SESS_LEASE_PROBE 13 /* host-only ReadIndex; never a log command */
/* Both use [tag:1][sub:1][uuid:16][epoch:4][seq:8][ack:8]
 * [ino:8][generation:8][edge_flags:4][pending_token:8]. Route to the
 * inode shard group. Reply rc is the stored verdict; salt=1 means known.
 * Unknown PROBE returns BUSY/salt=0 and cannot acquire or retire ownership.
 * Requires an explicitly established session; production FUSE is not active. */
#define EFS_MD_SESS_LEASE_EDGE_LEN 66u
#define EFS_MD_CMD_DIR    11 /* matches sim SIM_CMD_DIR; layout-epoch */
#define EFS_MD_DIR_BEGIN   1
#define EFS_MD_DIR_MIGRATE 2
#define EFS_MD_DIR_FINISH  3
#define EFS_MD_CMD_LOCK   12 /* matches sim SIM_CMD_LOCK; flock/fcntl */
#define EFS_MD_LOCK_GRANT   1
#define EFS_MD_LOCK_RELEASE 2
#define EFS_MD_CMD_SETATTR 13 /* matches sim SIM_CMD_SETATTR */
#define EFS_MD_CMD_UTIMENS 14 /* matches sim SIM_CMD_UTIMENS */
#define EFS_MD_CMD_TRUNCATE 15 /* matches sim SIM_CMD_TRUNCATE */
#define EFS_MD_CMD_APPEND_RSV 16 /* matches sim SIM_CMD_APPEND_RSV */
#define EFS_MD_CMD_APPEND_RES 17 /* matches sim SIM_CMD_APPEND_RES */
#define EFS_MD_CMD_MKFS 18 /* [now:8][salt:8] — idempotent; salt absent = 0 */
/* Cross-group lane support (a group's log only writes its own shards' keys,
 * so the first publish to a lane whose shard lives on ANOTHER group cannot
 * ride the publish entry's inode-row touch — see efs_meta_apply_publish).
 * ACTIVATE_LANE runs on the INODE group and only sets the active_lanes bit;
 * LANE_FENCE runs on the LANE's group and carries one lane's share of a
 * truncate (epoch fence + range delete). Both idempotent. */
#define EFS_MD_CMD_ACTIVATE_LANE 19 /* [ino:8][lane:1]  (10 B)
                                     * or [ino:8][mask:8] (17 B); REPORT
                                     * appends UUID/epoch (20 B) to either form */
#define EFS_MD_CMD_LANE_FENCE    20 /* [ino:8][gen:8][lane:1][epoch:8][size:8]
                                     * [tail_ci:4][has_tail:1] */
/* Data-plane GC (spec L7). LANE_SWEEP runs on the LANE's group: deletes the
 * lane's chunk keys (emitting one GC record per chunk on the group's anchor
 * shard), then the lane key. REAP_DONE runs on the INODE's group: clears
 * leftover append state and deletes the reap marker after every lane of a
 * dead inode was swept. GC_ACK runs on the group whose anchor shard holds
 * the record: sets one fragment's ack bit, retiring the record when all
 * EFS_NUM_FRAGMENTS bits are set. All three are idempotent replays. */
#define EFS_MD_CMD_LANE_SWEEP  21 /* [ino:8][gen:8][lane:1] */
#define EFS_MD_CMD_REAP_DONE   22 /* [ino:8][gen:8] */
#define EFS_MD_CMD_GC_ACK      23 /* [cnt:2][(ino:8)(gen:8)(lane:1)(ci:4)
                                   * (frag:1)]*cnt */
/* Host-only membership (I18). Not a log command — the leader calls
 * efs_raft_change, which appends JOINT/COLD itself. Byte 1 is the sub. */
#define EFS_MD_CMD_CFG 24
#define EFS_MD_CFG_NOTE   1 /* persist desired + attach learner (C_old) */
#define EFS_MD_CFG_CHANGE 2 /* NOTE every peer, then efs_raft_change */
/* Same-group RMDIR: one apply of efs_meta_apply_rmdir when parent, dentry,
 * and child shards share a Raft group. Encoding matches UNLINK. */
#define EFS_MD_CMD_RMDIR 25
/* Export-salt record for a group that did not apply MKFS. MKFS writes the
 * salt keyed on the ROOT shard (group 0), but every MKDIR scatters via
 * efs_kv_mkdir_shard(parent, name, salt) on EVERY group — a node hosting
 * only group 2 must apply the same salt or its placement diverges (apply
 * PROTO skip -> missing rows -> later EIO). mkfs proposes this to group 2
 * after the group-0 MKFS commits; the record anchors on the group's anchor
 * shard (efs_kv_anchor_shard) so group snapshots carry it. Idempotent: a
 * matching existing record is OK, a mismatch is EFS_ERR_PROTO (never
 * overwrite — placement is derived from this value). */
#define EFS_MD_CMD_SALT 26 /* [anchor:4][salt:8] */
/* One extended-attribute update on the inode's shard. Apply reads the
 * current blob and folds this op in, so two names on one inode do not
 * lose each other. [op:1][ino:8][now:8][flags:4][nlen:2][vlen:4][name][value]
 * op is EFS_XATTR_SET or EFS_XATTR_REMOVE. A user-visible error (NODATA,
 * EXIST) is the apply verdict; it must not halt the log. */
#define EFS_MD_CMD_XATTR 27
#define EFS_XATTR_SET    1
#define EFS_XATTR_REMOVE 2
#define EFS_XATTR_GET    3 /* read path; not a log command */
#define EFS_XATTR_LIST   4
#define EFS_XATTR_CREATE  0x1u /* Linux XATTR_CREATE */
#define EFS_XATTR_REPLACE 0x2u /* Linux XATTR_REPLACE */
#define EFS_XATTR_NAME_MAX  255
#define EFS_XATTR_VALUE_MAX 4096
#define EFS_XATTR_BLOB_MAX  16384

/* LEASE_LOCK_NEXT reply: BUSY with zero index/term and this salt certifies
 * selection/admission refused before any cleanup command was proposed.
 * An ordinary BUSY (including an uncertain submitted batch) has no marker. */
#define EFS_MD_SESSION_CLEANUP_RETRY_SALT 2ull
/* SESSION_APPEND_NEXT uses a distinct certificate for its read-only
 * admission/authority/selection refusal, before transaction callbacks.
 * It never certifies a completion or retirement transaction's BUSY. */
#define EFS_MD_SESSION_APPEND_RETRY_SALT 3ull

#endif
