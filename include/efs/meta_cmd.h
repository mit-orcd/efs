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
#define EFS_MD_CMD_SETATTR 13 /* matches sim SIM_CMD_SETATTR */
#define EFS_MD_CMD_UTIMENS 14 /* matches sim SIM_CMD_UTIMENS */
#define EFS_MD_CMD_TRUNCATE 15 /* matches sim SIM_CMD_TRUNCATE */
#define EFS_MD_CMD_APPEND_RSV 16 /* matches sim SIM_CMD_APPEND_RSV */
#define EFS_MD_CMD_APPEND_RES 17 /* matches sim SIM_CMD_APPEND_RES */
#define EFS_MD_CMD_MKFS 18 /* [now:8][salt:8] — idempotent; salt absent = 0 */

#endif
