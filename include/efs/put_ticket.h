#ifndef EFS_PUT_TICKET_H
#define EFS_PUT_TICKET_H
#include "efs/kv.h"
#include "efs/meta_apply.h"
#include "efs/opid.h"

/* Durable PUT ownership (K13, W86, D31). Serialized apply only. Every data
 * PUT is preceded by an admitted ticket; its immutable body digest excludes
 * publication CAS/size/time. A ticket ends PUBLISHED (with its mapping) or
 * REJECTED (lost CAS, deleted file, or abandoned by its live owner), and the
 * owner retires it in sequence order behind a durable floor; once the owner
 * is revoked, the group leader drains what it left the same way
 * (efs_put_ticket_drain). A retired REJECTED body moves to the orphan
 * ledger; bodies are discarded only after durable session revocation. Never authorize deletion from age, missing
 * state, or a local fence alone. */
#define EFS_PUT_TICKET_MAX 4096u /* live tickets per session/lane-shard stream */
#define EFS_PUT_TICKET_SETTLE_MAX 32u /* tickets in one SETTLE command */
#define EFS_PUT_TICKET_BATCH_MAX 32u /* tickets in one ADMIT_BATCH/SETTLE_BATCH (K32) */
#define EFS_PUT_TICKET_ADMITTED 1u
#define EFS_PUT_TICKET_PUBLISHED 2u
/* 3 was RECLAIMING (per-member deletion on the stream itself); bodies are
 * collected from the orphan ledger only. A stored 3 is refused (PROTO). */
#define EFS_PUT_TICKET_RETIRED 4u /* transport floor proof only; never stored as a ticket */
#define EFS_PUT_TICKET_REJECTED 5u /* terminal, never published; body retained */
#define EFS_PUT_TICKET_KEY_LEN 51u
#define EFS_PUT_TICKET_VAL_LEN 180u
struct efs_put_ticket {
    struct efs_opid id;
    efs_ino_t ino;
    uint64_t inode_gen;
    uint32_t ci;
    uint64_t object;
    uint8_t body_digest[EFS_HASH_SIZE];
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
    uint32_t delta_off, delta_len, coding_profile, fragment_len;
    uint32_t member_mask; /* configured-member slots, captured at admission */
};
/* Fixed endian transport, no ABI padding or caller-supplied state. Decode
 * validates the complete immutable body before any authority/admission work. */
#define EFS_PUT_TICKET_WIRE_LEN 216u
int efs_put_ticket_encode(const struct efs_put_ticket *,uint8_t[EFS_PUT_TICKET_WIRE_LEN]);
int efs_put_ticket_decode(const uint8_t *,uint32_t,struct efs_put_ticket *);
int efs_put_ticket_from_pub(const struct efs_meta_pub *, struct efs_put_ticket *);
int efs_put_ticket_admit(struct efs_kv *, const struct efs_put_ticket *);
/* Internal inspection only. An external PUT validator must authoritatively
 * check the session and require ADMITTED; neither absence nor PUBLISHED is a
 * data-plane write permit. */
int efs_put_ticket_get(struct efs_kv *, const struct efs_put_ticket *, uint32_t *state);
/* Builds a PUBLISHED write for the caller's atomic mapping batch. */
int efs_put_ticket_publish_item(struct efs_kv *, const struct efs_put_ticket *,
                                uint8_t key[EFS_PUT_TICKET_KEY_LEN],
                                uint8_t val[EFS_PUT_TICKET_VAL_LEN],
                                struct efs_kv_item *);
/* Floor proof cannot reconstruct the publication verdict or release bytes. */
int efs_put_ticket_retired(struct efs_kv *,const struct efs_put_ticket *,int *);
/* State including RETIRED (seq at or below the floor). NOT_FOUND: never
 * admitted. Validates the stored body against this exact ticket. */
int efs_put_ticket_state(struct efs_kv *,const struct efs_put_ticket *,uint32_t *state);
/* ADMITTED -> REJECTED; idempotent on REJECTED. PUBLISHED is STALE (a
 * published ticket is never abandoned). */
int efs_put_ticket_reject(struct efs_kv *,const struct efs_put_ticket *);
/* Owner settle: reject an ADMITTED ticket, then retire a PUBLISHED/REJECTED
 * ticket that heads its stream (BUSY otherwise; the reject persists). A
 * retired REJECTED body is recorded in the orphan ledger. Idempotent once
 * retired. Requires the owner's active session on the lane shard. */
int efs_put_ticket_settle(struct efs_kv *,const struct efs_put_ticket *);
/* D31 orphan ledger. A retired REJECTED ticket's body is recorded under its
 * group's anchor shard, so the group leader's collector scans one prefix:
 * [anchor:2][PUT_ORPHAN][lane shard:2][uuid:16][epoch:4][seq:8][ino:8]
 * [inode_gen:8][ci:4]. The value is the ticket value with the fragments
 * whose member acknowledged deletion. The record is removed in the apply
 * that acknowledges the last fragment, never earlier. */
#define EFS_PUT_ORPHAN_KEY_LEN 53u
#define EFS_PUT_ORPHAN_ALL ((1u << EFS_NUM_FRAGMENTS) - 1u)
/* OK when this retired REJECTED ticket's body is recorded and not yet
 * collected; *acked (optional) gets the acknowledged fragment mask. */
int efs_put_ticket_orphan_get(struct efs_kv *,const struct efs_put_ticket *,uint32_t *acked);
int efs_put_ticket_orphan(struct efs_kv *,const struct efs_put_ticket *);
/* Serialized apply of the collector's acknowledgement for fragments `frags`
 * (bit i = fragment i on nodes[i], deleted and durable on that member).
 * Refused (BUSY) until this lane shard's durable rejection floor is above
 * the ticket's epoch. OK and idempotent once the record is retired; the
 * record is retired only when every fragment is acknowledged. NOT_FOUND
 * when no orphan was ever recorded (live, published or unknown ticket). */
int efs_put_ticket_orphan_ack(struct efs_kv *,const struct efs_put_ticket *,uint32_t frags);
/* Scan helpers: the group's orphan prefix, and one record back to its
 * ticket and acknowledged mask (validated; PROTO on any mismatch). */
void efs_put_ticket_orphan_prefix(uint32_t anchor,uint8_t prefix[3]);
int efs_put_ticket_orphan_decode(const uint8_t *key,uint32_t kl,const uint8_t *val,
                                 uint32_t vl,struct efs_put_ticket *,uint32_t *acked);
/* D31 revoked-stream drain. Every session/lane-shard stream that holds a
 * ticket is indexed on its group's anchor shard, written and removed in the
 * same batch as the stream's live count goes 0 -> 1 and 1 -> 0:
 * [anchor:2][PUT_STREAM][uuid:16][epoch:4][lane shard:2], value [1:4]. The
 * group leader scans one prefix to find streams whose owner was revoked,
 * instead of every lane shard. */
#define EFS_PUT_STREAM_KEY_LEN 25u
#define EFS_PUT_TICKET_DRAIN_MAX 64u /* tickets one DRAIN apply retires */
struct efs_put_stream {
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t epoch;
    uint32_t shard; /* lane shard */
};
void efs_put_ticket_stream_prefix(uint32_t anchor,uint8_t prefix[3]);
int efs_put_ticket_stream_decode(const uint8_t *key,uint32_t kl,const uint8_t *val,uint32_t vl,
                                 struct efs_put_stream *);
/* OK while the stream holds a ticket (indexed), NOT_FOUND once empty. */
int efs_put_ticket_stream_live(struct efs_kv *,const struct efs_put_stream *);
/* Serialized apply of EFS_MD_CMD_PUT_TICKET_DRAIN, at most `max` tickets
 * from the stream head, in sequence order, behind the floor:
 *   - refused (BUSY) until this lane shard's durable rejection floor is
 *     above the epoch: no ADMIT, PUBLISH, REPORT or SETTLE of the epoch
 *     applies here any more;
 *   - PUBLISHED heads retire (floor advances, no orphan record);
 *   - REJECTED heads retire into the orphan ledger, as SETTLE does;
 *   - ADMITTED heads are abandoned (REJECTED) and retire into the ledger,
 *     except while a K14 stage is pending on that chunk: its RESOLVE may
 *     still publish this object, so the drain stops there.
 * OK when it retired at least one ticket or the stream is empty (*moved
 * counts them); BUSY when nothing could retire; idempotent. Bodies are
 * deleted only by the orphan collector, under its own four conditions. */
int efs_put_ticket_drain(struct efs_kv *,const struct efs_put_stream *,uint32_t max,
                         uint32_t *moved);
#endif
