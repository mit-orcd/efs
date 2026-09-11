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

#define EFS_MD_CMD_CREATE 1 /* matches sim CMD_CREATE; see pack in raft_host.c */
#define EFS_MD_CMD_UNLINK  2 /* matches sim CMD_UNLINK */
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
#define EFS_MD_CMD_ACTIVATE_LANE 19 /* [ino:8][lane:1] */
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

#endif
