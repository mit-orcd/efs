#ifndef EFS_META_CMD_H
#define EFS_META_CMD_H

#include <stdint.h>

/* Raft log command namespace for the metadata engine (architecture.md
 * §7 / §10 step 10.5). Byte 0 is the command tag; multi-byte fields are
 * big-endian. The simulator (src/sim/sim_internal.h SIM_CMD_*) uses the
 * same value for every command both apply.
 *
 * Only the commands the production host applies belong here. The
 * simulator also keeps commands production has retired (SIM_CMD_APPEND_RES
 * 17); production refuses those tags (EFS_MD_CMD_IS_RETIRED). */

/* W86: [tag:1][immutable ticket:216]. STATUS is ReadIndex-only; ADMIT
 * is serialized apply. Reply salt is ticket state, never permission to PUT. */
/* REPORT envelope: [tag][uuid:16][epoch:4][legacy PUBLISH:202]*N.
 * Fences every mapping at serialized apply without reusing delta fields. */
/* Internal coordinator commands; public control RPC cannot forge ACKs. */
/* 45-51 and 55-57 are reserved for retired feature commands. Apply halts
 * on them; submit refuses. Retired tags are refused the same way:
 * APPEND_RES 17 (folded into APPEND_CATCHUP), LANE_FENCE 20 (D25 logical
 * truncate), LANE_SWEEP 21 and REAP_DONE 22, LANE_SWEEP_BATCH 33 and
 * REAP_BATCH 34 (replaced by the W124 *_MULTI forms). The refusal stays until a format-marker change retires these
 * values; do not assign them to new commands before that. */
#define EFS_MD_CMD_RESERVED_MIN 45
#define EFS_MD_CMD_RESERVED_MAX 51
#define EFS_MD_CMD_RESERVED2_MIN 55
#define EFS_MD_CMD_RESERVED2_MAX 57
#define EFS_MD_CMD_IS_RETIRED(c) \
    ((c) == 17 || (c) == 20 || (c) == 21 || (c) == 22 || (c) == 33 || (c) == 34)
#define EFS_MD_CMD_IS_RESERVED(c) \
    (((c) >= EFS_MD_CMD_RESERVED_MIN && (c) <= EFS_MD_CMD_RESERVED_MAX) || \
     ((c) >= EFS_MD_CMD_RESERVED2_MIN && (c) <= EFS_MD_CMD_RESERVED2_MAX) || \
     EFS_MD_CMD_IS_RETIRED(c))
/* Exact single-shard APPEND image envelope; no replica-local EOF selection. */
#define EFS_MD_APPEND_COMPLETE_ATOMIC_LEN 101u
#define EFS_MD_REQ_EXPIRY_CLOCK 63 /* local clock health only */
#define EFS_MD_REQ_EXPIRY_CLOCK_RESET 64 /* local operator uid4; no namespace mutation */
#define EFS_MD_REQ_EXPIRY_POLICY_INFO 68 /* policy read; OK hint carries enabled/clear flags */
#define EFS_MD_REQ_EXPIRY_ORIGIN 69 /* record root-generation/policy-revision/max; OK hint=clear */
#define EFS_MD_REQ_EXPIRY_PARENT 70 /* inode read: parent/generation/mode */
/* W124 batched reclaim, one entry per owning group:
 * LANE_SWEEP_MULTI [count:2] then per item [ino:8][gen:8][lane:1][n:1][ci:4]*n
 *   (reaped-inode sweep: no per-chunk GC records);
 * REAP_MULTI [count:2] then per item [ino:8][gen:8][n:1][len:1,key:31]*n.
 * Items apply independently; a refused item keeps its REAP marker. */
#define EFS_MD_CMD_LANE_SWEEP_MULTI 71
#define EFS_MD_CMD_REAP_MULTI 72
/* W125 background packer, applied by the segment's publication group
 * (payloads in pack_mapping.h): admit a segment, switch files into it,
 * cancel its remaining pending records. */
#define EFS_MD_CMD_PACK_ADMIT 73
#define EFS_MD_CMD_PACK_SWITCH 74
#define EFS_MD_CMD_PACK_CANCEL 75
#define EFS_MD_CMD_PACK_RETIRE 76
#define EFS_MD_CMD_PACK_UNINDEX 77
#define EFS_MD_CMD_PACK_INDEX 78
#define EFS_MD_REQ_EXPIRY_ID 67 /* inode8; authoritative generation/uid/mode/links */
#define EFS_MD_REQ_EXPIRY_ENABLE 66 /* renewal prefix + parent/name; absent-record CAS */
#define EFS_MD_REQ_EXPIRY_DELETE 65 /* guarded due FileID/name; receiver owns clock */
#define EFS_MD_CMD_CREATE_REFUSAL 62 /* shard4 + existing op-id trailer; EEXIST receipt */
#define EFS_MD_CMD_EXPIRY_POLICY 58 /* root/gen/uid/expected/TTL/max/flags:46 */
#define EFS_MD_CMD_EXPIRY_RENEW 59 /* ino/gen/uid/expected/leader-now/deadline:45 */
#define EFS_MD_REQ_EXPIRY_POLICY 60 /* root/gen:17; fresh revision/TTL/max */
#define EFS_MD_REQ_EXPIRY_RECORD 61 /* ino/gen:17; fresh revision/deadline/root */
/* Expiry requests and commands (D33). Excluded from the alpha product
 * (D42): a build without EFS_BUILD_EXPIRY refuses them, never proposes them. */
static inline int efs_md_is_expiry(uint8_t op)
{
    return op == EFS_MD_CMD_EXPIRY_POLICY || op == EFS_MD_CMD_EXPIRY_RENEW ||
           (op >= EFS_MD_REQ_EXPIRY_POLICY && op <= EFS_MD_REQ_EXPIRY_RECORD) ||
           op == EFS_MD_REQ_EXPIRY_CLOCK || op == EFS_MD_REQ_EXPIRY_CLOCK_RESET ||
           (op >= EFS_MD_REQ_EXPIRY_DELETE && op <= EFS_MD_REQ_EXPIRY_ID) ||
           op == EFS_MD_REQ_EXPIRY_POLICY_INFO || op == EFS_MD_REQ_EXPIRY_ORIGIN ||
           op == EFS_MD_REQ_EXPIRY_PARENT;
}
#define EFS_MD_CMD_APPEND_COMPLETE_ATOMIC 54 /* ino/gen/offset/UUID/epoch/content epoch/reservation: 101 bytes */
#define EFS_MD_CMD_APPEND_ATOMIC 53
#define EFS_MD_CMD_QUERY_POLICY 43 /* expected version:8, mask:4 */
#define EFS_MD_CMD_APPEND_CATCHUP 52 /* ino/gen/epoch/published EOF + optional REPORT session */
#define EFS_MD_REQ_QUERY_POLICY 44 /* ReadIndex only, reply index=version/salt=mask */
#define EFS_MD_CMD_REPORT_PUBLISH 41
/* Client settle (K13): ADMITTED -> REJECTED, then retire a PUBLISHED or
 * REJECTED ticket that heads its stream. One 216-byte ticket body. */
#define EFS_MD_CMD_PUT_TICKET_SETTLE 42
#define EFS_MD_CMD_PUT_TICKET_ADMIT 39
#define EFS_MD_REQ_PUT_TICKET_STATUS 40
#define EFS_MD_REQ_SESSION_APPEND_NEXT 38 /* RPC only: [uuid:16][epoch:4][shard:4] */
#define EFS_MD_REQ_CLOSE_APPEND_NEXT 37 /* RPC only: [ino:8][identity:36] */
#define EFS_MD_CMD_CLOSE_STEP 36 /* [ino:8][identity:36][phase:1], 46 bytes */
#define EFS_MD_REQ_SWEEP_NEXT 35 /* RPC only: leader selects exact keys; never log */
#define EFS_MD_CMD_CLOSE_CLEANUP 32 /* [ino:8][generation:8], retry deferred close */
#define EFS_MD_CMD_PUBLICATION_ADMIT 31 /* immutable durable pending intent */
#define EFS_MD_CMD_ORPHAN_REAP 30 /* [ino:8][generation:8], recheck leases/intents */
#define EFS_MD_CMD_CREATE 1 /* matches sim CMD_CREATE; see pack in raft_host.c */
#define EFS_MD_CMD_UNLINK  2 /* matches sim CMD_UNLINK */
#define EFS_MD_CMD_PUBLICATION_RETIRE 29 /* same immutable payload, exact acknowledgement */
#define EFS_MD_CMD_PUBLICATION 28 /* legacy publish payload + UUID/epoch/seq (28 B) */
#define EFS_MD_CMD_PUBLISH 3 /* matches sim CMD_PUBLISH */
#define EFS_MD_CMD_PREPARE 5 /* matches sim CMD_PREPARE */
#define EFS_MD_CMD_DECIDE  6 /* [txid:16][coord:4][dec:1][n:1][shard:4]*n */
#define EFS_MD_CMD_TXN_RETIRE 83 /* [txid:16][coord:4][now_ns:8]: every RESOLVE applied */
/* W154 on the group of the mark's anchor shard: [rebuilt:1][ino:8]
 * [inode_gen:8][object_gen:8][ci:4][lane:1][missing:1][nodes:4*3]
 * [checksums:32*3], 140 B. Retires one degraded mark (efs_meta_apply_degraded_clear). */
#define EFS_MD_CMD_DEGRADED_CLEAR 84
/* RPC only (K13 + W154), never logged: [mark:EFS_REBUILD_MARK_LEN] as in
 * EFS_MSG_REBUILD_PUT. OK when the mark's group, after a ReadIndex, holds
 * exactly this mark; NOT_FOUND when absent; STALE when it differs. */
#define EFS_MD_REQ_DEGRADED_CHECK 85
/* D31 (K13) on the ticket's lane-shard group: [frags:1][ticket:216], 218 B.
 * The members holding fragments `frags` deleted them durably; the apply
 * records them (efs_put_ticket_orphan_ack) and retires the orphan record
 * with the last one. Refused behind a missing shard rejection floor. */
#define EFS_MD_CMD_PUT_ORPHAN_ACK 86
#define EFS_MD_PUT_ORPHAN_ACK_LEN (2u + EFS_PUT_TICKET_WIRE_LEN)
/* RPC only, never logged: [ticket:216]. After a ReadIndex: OK (salt = the
 * acknowledged mask) when the orphan record exists and the shard's rejection
 * floor is above the ticket's epoch; NOT_FOUND without a record; BUSY while
 * the floor is not raised. A storage member's authority to delete. */
#define EFS_MD_REQ_PUT_ORPHAN_STATUS 87
#define EFS_MD_DEGRADED_CLEAR_LEN (1u + 1u + 8u + 8u + 8u + 4u + 1u + 1u + \
                                   4u * EFS_NUM_FRAGMENTS + EFS_HASH_SIZE * EFS_NUM_FRAGMENTS)
#define EFS_MD_CMD_RESOLVE 7
#define EFS_MD_CMD_DROP    8
/* K29: one entry carrying a transaction's PREPAREs, or its RESOLVEs, for
 * this group's shards: [TXN_BATCH][sub:1][n:2]([len:2][command])*n, sub is
 * PREPARE or RESOLVE and every command carries that tag. Apply runs each
 * command as its own entry would, in order, in the entry's one atomic KV
 * scope; the verdict is the first non-OK command verdict. */
/* W153/K33: host-only membership notice, never a log command
 * (EFS_MSG_RAFT_MKFS submit, any group byte): [tag][committed membership
 * record]. The member persists the record (member store) when it names
 * this store and extends what the store holds, derives the count and both
 * groups' voters from it and attaches the groups it votes in; the same or
 * an earlier revision is OK, another cluster or a divergent record FENCED. */
#define EFS_MD_REQ_FORMED 89
/* K33 node addition, group 0, the membership authority:
 * [tag][record] = the proposer's committed record plus one member with id
 * count + 1. Applied with EFS_MEMBERSHIP_EXTEND_ONE on the root anchor;
 * verdict OK (installed, or already part of the committed history),
 * FENCED (a competing revision committed) or UNFORMED. */
#define EFS_MD_CMD_MEMBERS 92 /* 90, 91 are K32's ticket batches */
/* K33: host-only, never a log command. A member that hosts no group-0
 * replica forwards an eligible joiner to a group-0 member:
 * [tag][id:4][port:2][token:8][alen:1][addr]. The receiver runs the
 * addition (MEMBERS, notice); the reply is its verdict. */
#define EFS_MD_REQ_MEMBER_ADD 93
#define EFS_MD_CMD_TXN_BATCH 88 /* K29; 86 is PUT_ORPHAN_ACK, 85 and 87 are requests */
/* K32 (K13 batching), on one Raft group: [tag][n:2][ticket:216]*n,
 * 2 <= n <= EFS_PUT_TICKET_BATCH_MAX, every ticket on a lane shard of the
 * entry's group and of one mount session. ADMIT_BATCH admits each ticket as
 * its own ADMIT would (no order; distinct sequences). SETTLE_BATCH carries
 * several streams' SETTLE runs: tickets sorted by (lane shard, sequence),
 * strictly increasing sequence per stream; each stream settles in order and
 * stops at its first ticket that cannot retire. Apply verdict is OK (IO and
 * NOMEM retry); the RPC answers salt = bit i set when ticket i is ADMITTED
 * (ADMIT_BATCH) or retired (SETTLE_BATCH), read after the apply. */
#define EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH 90
#define EFS_MD_CMD_PUT_TICKET_SETTLE_BATCH 91
/* D31 revoked-stream drain, on the lane shard's group, proposed by its
 * leader once the stream owner's revocation is proven:
 * [tag][uuid:16][epoch:4][lane shard:4], 25 B. Retires up to
 * EFS_PUT_TICKET_DRAIN_MAX tickets from the stream head into the orphan
 * ledger (efs_put_ticket_drain). Refused (BUSY) behind a missing shard
 * rejection floor or a pending K14 stage; a logical verdict. */
#define EFS_MD_CMD_PUT_TICKET_DRAIN 250
/* K35 (D42, W154) on the lane shard's group, proposed by a client fsync:
 * [tag][ino:8][inode_gen:8][object_gen:8][ci:4][fragment:1][node:4], 34 B.
 * COMMIT proved two fragments of a published object durable but not this
 * one (its node down, uncertain or of another incarnation): record the
 * object's degraded mark from its chunk row (efs_meta_apply_degraded_mark)
 * so the rebuild on return restores it. A logical verdict: OK marked (or
 * already marked so), NOT_FOUND no longer live, STALE the row disagrees. */
/* K37 (W154 for packed segments), on the segment's publication group,
 * proposed by that group's leader (packer or segment rebuild):
 * [tag][group:1][op:1][id:8][generation:8][mask:1][reason:1], 21 B. The
 * segment's degraded mark (EFS_KV_KIND_PACK_DEGRADED) is the one authority
 * for "this segment lacks fragments": MARK adds mask bits (a mark with two
 * bits is unrecoverable and never cleared by a rebuild); CLEAR deletes the
 * mark after a proven rebuild only when it still names exactly mask, one
 * fragment; RETIRE deletes it only when the segment is gone or holds no
 * live or pending record. Byte 1 is the group, as for every pack command.
 * Logical verdicts (STALE: nothing to do) never halt the log. */
#define EFS_MD_CMD_PACK_DEGRADED 247
#define EFS_MD_PACK_DEGRADED_LEN 21u
#define EFS_MD_PACK_DEGRADED_MARK 1u
#define EFS_MD_PACK_DEGRADED_CLEAR 2u
#define EFS_MD_PACK_DEGRADED_RETIRE 3u
#define EFS_MD_CMD_DEGRADED_MARK 249
#define EFS_MD_DEGRADED_MARK_LEN 34u
/* K36 verified recovery, on the group of the mark's anchor shard:
 * [ino:8][inode_gen:8][object_gen:8][ci:4][lane:1][missing:1][nodes:4*3]
 * [checksums:32*3] after the tag, 139 B. A recovering storage node dropped
 * its fragment of a published object (missing, torn or mismatching); the
 * apply writes the W154 degraded mark so the leader rebuilds it
 * (efs_meta_apply_recovery_mark). Verdicts OK, NOT_FOUND (object no longer
 * live) and STALE (another fragment already marked) are logical. */
#define EFS_MD_CMD_RECOVERY_MARK 248 /* 249 is K35's DEGRADED_MARK */
#define EFS_MD_RECOVERY_MARK_LEN (1u + 8u + 8u + 8u + 4u + 1u + 1u + \
                                  4u * EFS_NUM_FRAGMENTS + EFS_HASH_SIZE * EFS_NUM_FRAGMENTS)
#define EFS_MD_PUT_TICKET_DRAIN_LEN 25u
#define EFS_MD_TXN_BATCH_MAX 96u /* commands per entry */
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
#define EFS_MD_CMD_TRUNCATE 15 /* D25 logical truncate, exactly 86 B (protocol
                                * §1.2); matches sim SIM_CMD_TRUNCATE */
#define EFS_MD_CMD_APPEND_RSV 16 /* matches sim SIM_CMD_APPEND_RSV */
#define EFS_MD_CMD_MKFS 18 /* [now:8][salt:8] — idempotent; salt absent = 0 */
/* Cross-group lane support (a group's log only writes its own shards' keys,
 * so the first publish to a lane whose shard lives on ANOTHER group cannot
 * ride the publish entry's inode-row touch — see efs_meta_apply_publish).
 * ACTIVATE_LANE runs on the INODE group and only sets the active_lanes bit.
 * Idempotent. Tag 20 (the retired LANE_FENCE) is refused (IS_RETIRED). */
#define EFS_MD_CMD_ACTIVATE_LANE 19 /* [ino:8][mask:8][content_epoch:8]
                                     * (25 B); REPORT appends UUID/epoch (20 B) */
/* D25 BOOTSTRAP on a cross-group lane's group: create the lane stamp at the
 * inode epoch the proposer read, or adopt it; never lowers it. A lane-local
 * publish to a lane without a stamp answers BUSY until this commits. */
#define EFS_MD_CMD_BOOTSTRAP_LANE 79 /* [ino:8][gen:8][lane:1][epoch:8], 26 B */
/* D25 sweep (protocol §1.2, §4). TRUNC_SWEEP on the lane's group, the other
 * two on the inode's group. */
#define EFS_MD_CMD_TRUNC_SWEEP 80 /* [ino:8][gen:8][lane:1][target:8][cs:4]
                                   * [count:1][count x (epoch:8 size:8)] */
#define EFS_MD_CMD_SWEEP_DONE  81 /* [ino:8][gen:8][lane:1][target:8], 26 B */
#define EFS_MD_CMD_RETIRE      82 /* [ino:8][gen:8][target:8], 25 B */
/* Data-plane GC (spec L7). GC_ACK runs on the group whose anchor shard
 * holds the record: sets one fragment's ack bit, retiring the record when
 * all EFS_NUM_FRAGMENTS bits are set. An idempotent replay. Lane sweeps and
 * reap completions are LANE_SWEEP_MULTI / REAP_MULTI above. */
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

/* W140: the log commands this build applies. host_apply_dispatch
 * (src/server/raft_host.c) handles exactly these tags, and every proposal
 * path refuses any other tag before it reaches the log (one committed entry
 * with an unknown tag halts the group at apply, W127). The dispatcher
 * checks the same predicate first and still halts on anything a handler
 * cannot decode. Add a tag here only together with its dispatch arm;
 * tests/test_submit_unknown_tag.py fails when the two drift. RPC-only
 * requests (EFS_MD_REQ_*) and the host-only CFG are never log commands and
 * never belong here. */
#define EFS_MD_CMD_APPLIED(X) \
    X(CREATE) X(UNLINK) X(PUBLISH) X(PREPARE) X(DECIDE) X(RESOLVE) X(DROP) \
    X(SESSION) X(DIR) X(LOCK) X(SETATTR) X(UTIMENS) X(TRUNCATE) \
    X(APPEND_RSV) X(MKFS) X(ACTIVATE_LANE) X(GC_ACK) X(RMDIR) X(SALT) \
    X(XATTR) X(PUBLICATION) X(PUBLICATION_RETIRE) X(ORPHAN_REAP) \
    X(PUBLICATION_ADMIT) X(CLOSE_CLEANUP) X(CLOSE_STEP) X(PUT_TICKET_ADMIT) \
    X(REPORT_PUBLISH) X(PUT_TICKET_SETTLE) X(QUERY_POLICY) \
    X(APPEND_CATCHUP) X(APPEND_ATOMIC) X(APPEND_COMPLETE_ATOMIC) \
    X(EXPIRY_POLICY) X(EXPIRY_RENEW) X(CREATE_REFUSAL) \
    X(LANE_SWEEP_MULTI) X(REAP_MULTI) \
    X(PACK_ADMIT) X(PACK_SWITCH) X(PACK_CANCEL) X(PACK_RETIRE) \
    X(PACK_UNINDEX) X(PACK_INDEX) \
    X(BOOTSTRAP_LANE) X(TRUNC_SWEEP) X(SWEEP_DONE) X(RETIRE) X(TXN_RETIRE) \
    X(DEGRADED_CLEAR) X(PUT_ORPHAN_ACK) X(TXN_BATCH) \
    X(PUT_TICKET_ADMIT_BATCH) X(PUT_TICKET_SETTLE_BATCH) X(MEMBERS) \
    X(PUT_TICKET_DRAIN) X(DEGRADED_MARK) X(RECOVERY_MARK) X(PACK_DEGRADED)

/* 1 when byte 0 of a log command names a command this build applies. */
static inline int efs_md_cmd_applicable(uint8_t tag)
{
    switch (tag) {
#define EFS_MD_CMD_APPLIED_CASE(name) case EFS_MD_CMD_##name:
    EFS_MD_CMD_APPLIED(EFS_MD_CMD_APPLIED_CASE)
#undef EFS_MD_CMD_APPLIED_CASE
        return 1;
    default:
        return 0;
    }
}

#endif
