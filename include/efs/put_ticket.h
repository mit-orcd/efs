#ifndef EFS_PUT_TICKET_H
#define EFS_PUT_TICKET_H
#include "efs/kv.h"
#include "efs/meta_apply.h"
#include "efs/opid.h"

/* Durable PUT ownership (K13, W86, D31). Serialized apply only. Every data
 * PUT is preceded by an admitted ticket; its immutable body digest excludes
 * publication CAS/size/time. A ticket ends PUBLISHED (with its mapping) or
 * REJECTED (lost CAS, deleted file, or abandoned by its live owner), and the
 * owner retires it in sequence order behind a durable floor. A retired
 * REJECTED body moves to the orphan ledger; bodies are discarded only after
 * durable session revocation. Never authorize deletion from age, missing
 * state, or a local fence alone. */
#define EFS_PUT_TICKET_MAX 4096u /* live tickets per session/lane-shard stream */
#define EFS_PUT_TICKET_SETTLE_MAX 32u /* tickets in one SETTLE command */
#define EFS_PUT_TICKET_BATCH_MAX 32u /* tickets in one ADMIT_BATCH/SETTLE_BATCH (K32) */
#define EFS_PUT_TICKET_ADMITTED 1u
#define EFS_PUT_TICKET_PUBLISHED 2u
#define EFS_PUT_TICKET_RECLAIMING 3u
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
/* Both authoritative session and shard views must be colocated in this KV.
 * A distributed caller cannot supply an unverified cached global epoch. */
int efs_put_ticket_reclaim_begin(struct efs_kv *, const struct efs_put_ticket *);
/* Only after storage has durably fenced old PUTs and acknowledged deletion
 * on every member. PUBLISHED tickets need no body deletion. Advance only
 * over the first outstanding ticket; the compact floor survives retirement. */
int efs_put_ticket_delete_ack(struct efs_kv *, const struct efs_put_ticket *,
                              uint32_t member_bit);
int efs_put_ticket_retire(struct efs_kv *, const struct efs_put_ticket *);
/* Floor proof cannot reconstruct the publication verdict or release bytes. */
int efs_put_ticket_retired(struct efs_kv *,const struct efs_put_ticket *,int *);
/* State including RETIRED (seq at or below the floor). NOT_FOUND: never
 * admitted. Validates the stored body against this exact ticket. */
int efs_put_ticket_state(struct efs_kv *,const struct efs_put_ticket *,uint32_t *state);
/* ADMITTED -> REJECTED; idempotent on REJECTED. PUBLISHED is STALE (a
 * published ticket is never abandoned), RECLAIMING is BUSY. */
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
#endif
