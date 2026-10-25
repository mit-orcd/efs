#ifndef EFS_EXPIRY_H
#define EFS_EXPIRY_H
#include "efs/kv.h"
#include "efs/kv_key.h"
/* D33 durable expiry primitives. Call mutations under participant-log
 * serialization. The caller must honor transaction intents and combine these
 * items with namespace creation/deletion in the same atomic decision. */
#define EFS_EXPIRY_POLICY_BYTES 40u
#define EFS_EXPIRY_RECORD_BYTES 320u
#define EFS_EXPIRY_KEY_BYTES 19u
#define EFS_EXPIRY_INDEX_BYTES 27u
#define EFS_EXPIRY_PAGE_MAX 64u
struct efs_expiry_policy {
    uint64_t revision, ttl_ns, max_ns;
    uint32_t owner_uid;
    uint8_t enabled, allow_clear;
};
struct efs_expiry_record {
    uint64_t deadline_ns, revision, root, root_generation, policy_revision;
    uint64_t max_ns;
    uint8_t allow_clear;
    uint64_t parent;
    char name[EFS_MAX_NAME];
};
struct efs_expiry_change {
    struct efs_kv_item items[3];
    uint8_t record_key[EFS_EXPIRY_KEY_BYTES];
    uint8_t old_index[EFS_EXPIRY_INDEX_BYTES], new_index[EFS_EXPIRY_INDEX_BYTES];
    uint8_t value[EFS_EXPIRY_RECORD_BYTES];
    uint32_t count;
};
struct efs_expiry_due {
    uint64_t deadline_ns, ino, generation, revision, parent;
    uint64_t root, root_generation; /* policy source, for coverage */
    char name[EFS_MAX_NAME];
};
struct efs_expiry_page {
    struct efs_expiry_due rows[EFS_EXPIRY_PAGE_MAX];
    uint32_t count;
    uint8_t resume[EFS_EXPIRY_INDEX_BYTES];
    uint8_t more;
};
struct efs_expiry_clock { uint64_t wall, mono; uint8_t initialized, healthy, uncertain; };
/* D43 cluster expiry policy: one versioned row on shard 1 (absent: version
 * 0, off). Apply only from serialized Raft apply; CAS on the version. */
int efs_expiry_switch_get(struct efs_kv *, uint64_t *version, uint8_t *enabled);
int efs_expiry_switch_apply(struct efs_kv *, uint64_t expected, uint8_t enabled);
/* Enabled folder policies, cluster-wide: one row on each group anchor
 * (shards 1 and 2), absent = 0. Both rows change only in the transaction
 * that flips a folder policy's enabled flag, so they are always equal; a
 * create in a group reads its own anchor's row in serialized apply. */
#define EFS_EXPIRY_FOLDERS_KEY_BYTES 3u
void efs_expiry_folders_key(uint32_t anchor, uint8_t out[EFS_EXPIRY_FOLDERS_KEY_BYTES]);
int efs_expiry_folders_get(struct efs_kv *, uint32_t anchor, uint64_t *count);
/* Ordinary-path admission of a regular-file create while the cluster
 * policy is on: EFS_OK when the anchor's count is 0 and no transaction
 * holds the row; EFS_ERR_AGAIN when a create must take the expiry path. */
int efs_expiry_folders_fence(struct efs_kv *, uint32_t anchor);
/* A folder policy write: the policy row CAS and, when its enabled flag
 * flips, both anchors' counts. Validation matches efs_expiry_policy_set. */
struct efs_expiry_policy_change {
    uint8_t key[EFS_EXPIRY_KEY_BYTES];
    uint8_t old_value[EFS_EXPIRY_POLICY_BYTES], value[EFS_EXPIRY_POLICY_BYTES];
    uint32_t old_len; /* EFS_TXN_ABSENT: no row yet */
    int delta;        /* -1, 0 or +1 enabled folder policies */
    uint64_t folders; /* count after the change */
    uint8_t folders_key[2][EFS_EXPIRY_FOLDERS_KEY_BYTES];
    uint8_t folders_old[2][8], folders_new[2][8];
    uint32_t folders_old_len[2]; /* EFS_TXN_ABSENT: no row yet */
};
int efs_expiry_policy_plan(struct efs_kv *, uint64_t root, uint64_t gen,
                           uint32_t uid, uint64_t expected,
                           const struct efs_expiry_policy *,
                           struct efs_expiry_policy_change *);
/* 1 when these stored policy bytes (the record's source folder) are an
 * enabled policy: a deadline is acted on only while its source covers it. */
int efs_expiry_covered(const uint8_t *policy, uint32_t policy_len);
void efs_expiry_index_key(uint64_t ino,uint64_t gen,uint64_t deadline,uint8_t out[EFS_EXPIRY_INDEX_BYTES]);
void efs_expiry_policy_key(uint64_t root,uint64_t gen,uint8_t out[EFS_EXPIRY_KEY_BYTES]);
void efs_expiry_record_key(uint64_t ino,uint64_t gen,uint8_t out[EFS_EXPIRY_KEY_BYTES]);
int efs_expiry_policy_decode(const uint8_t *,uint32_t,struct efs_expiry_policy *);
int efs_expiry_policy_get(struct efs_kv *, uint64_t root, uint64_t gen,
                          struct efs_expiry_policy *);
int efs_expiry_policy_set(struct efs_kv *, uint64_t root, uint64_t gen,
                          uint32_t uid, uint64_t expected,
                          const struct efs_expiry_policy *);
int efs_expiry_get(struct efs_kv *, uint64_t ino, uint64_t gen,
                   struct efs_expiry_record *);
int efs_expiry_plan(struct efs_kv *, uint64_t ino, uint64_t gen,
                    uint64_t expected, const struct efs_expiry_record *,
                    struct efs_expiry_change *);
int efs_expiry_materialize(const struct efs_expiry_policy *, uint64_t root,
                           uint64_t root_gen, uint64_t now,
                           struct efs_expiry_record *);
int efs_expiry_renew(const struct efs_expiry_record *,
                     const struct efs_expiry_policy *, uint32_t uid,
                     uint32_t owner_uid, uint32_t nlink, uint32_t mode,
                     uint64_t now, uint64_t deadline,
                     struct efs_expiry_record *);
int efs_expiry_due_page(struct efs_kv *, uint32_t anchor, uint64_t now,
                        const uint8_t *after, struct efs_expiry_page *);
/* False on first observation, backwards clocks, or >1s wall/monotonic drift.
 * Uncertainty is latched until the operator re-establishes clock health. */
void efs_expiry_clock_reset(struct efs_expiry_clock *);
int efs_expiry_clock_sample(struct efs_expiry_clock *, uint64_t wall,
                           uint64_t mono);
#endif
