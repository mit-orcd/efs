#ifndef EFS_PUT_TICKET_H
#define EFS_PUT_TICKET_H
#include "efs/kv.h"
#include "efs/meta_apply.h"
#include "efs/opid.h"

/* Staged metadata state machine. Serialized apply only. A ticket precedes
 * PUT; its immutable body digest excludes publication CAS/size/time. Never
 * authorize deletion from age, missing state, or a local fence alone. */
#define EFS_PUT_TICKET_MAX 128u
#define EFS_PUT_TICKET_ADMITTED 1u
#define EFS_PUT_TICKET_PUBLISHED 2u
#define EFS_PUT_TICKET_RECLAIMING 3u
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
#endif
