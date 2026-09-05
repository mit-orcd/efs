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

#define EFS_MD_CMD_MKFS 18 /* [now:8][salt:8] — idempotent; salt absent = 0 */

#endif
