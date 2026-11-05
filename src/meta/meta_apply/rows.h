#ifndef EFS_META_APPLY_ROWS_H
#define EFS_META_APPLY_ROWS_H

#include "apply_internal.h"

/* rows.c: the write checks every mutation goes through, and the codecs of
 * the rows every area reads.
 *
 * Owns
 *   meta_write_*       the only KV writes of the module. meta_write_check
 *                      refuses a key a prepared EXCL image or a staged
 *                      publication holds (EFS_ERR_BUSY), so every mutation
 *                      honours transactions without knowing about them;
 *                      meta_write_batch_as(dry) runs the checks and writes
 *                      nothing (a W77 PREPARE). One batch is one atomic
 *                      efs_kv_batch: a refused or failed batch wrote nothing.
 *   meta_cleanup_*     as meta_write_*, inside efs_kv_lsm_cleanup_scope so
 *                      the query catalog picks the deletion up at rebuild.
 *   struct lane_rec    a write lane's high-water marks (layout below).
 *   pack_* / unpack_*  inode, lane, dentry, lane authority and chunk values
 *                      (base image plus the delta trailer); unpack returns
 *                      EFS_ERR_PROTO on a short or malformed value and never
 *                      reads past `n`.
 *
 * Locks: none of its own. Every mutation runs inside the participant's
 *   serialized Raft apply (or a coordinator-checked transaction step), and
 *   the KV is the only shared state.
 *   rows.c calls pub_stage_owner (pub_stage.h) from the write check: the one
 *   place the bottom layer reaches up, because a staged publication owns
 *   its chunk key until resolved.
 *
 * Lifetime: values are decoded into caller storage; nothing is retained. */

/* A write lane's high-water marks — the values stat() reduces over (§7.3).
 *
 * The first 24 bytes are deliberately the same triple a transaction's
 * commutative reduction carries (`struct efs_txn_reduce`), so a pending
 * committed reduction folds into a materialized lane without either side
 * knowing the other's layout. Anything after that is the lane owner's.
 *
 * `mtime_gen` records which utimens generation `max_mtime` belongs to. Only
 * utimens can move a timestamp backwards, so it bumps the inode row's
 * generation and a stat ignores lane mtimes stamped under an older one —
 * which invalidates every stale lane mtime at once instead of rewriting 64
 * lanes. `fenced_epoch` is the content-epoch fence a truncate installs.
 * `present` counts the lane's live chunk rows (D17): a publish that creates
 * a row adds one, a truncate's range delete subtracts what it dropped, and
 * getattr sums it across lanes for st_blocks. */
struct lane_rec {
    uint64_t max_end;
    uint64_t max_mtime;
    uint64_t max_ctime;
    uint64_t seq;
    uint64_t fenced_epoch;
    uint64_t mtime_gen;
    uint64_t append_bar; /* live reservation watermark; 0 = no barrier */
    uint64_t present;
    /* D25 truncate sweep (protocol §4.2): the target fence epoch being
     * swept, the next chunk index to examine, and the highest target this
     * lane was completely swept for. */
    uint64_t sweep_target;
    uint64_t sweep_next_ci;
    uint64_t swept_epoch;
};

int meta_write_check(struct efs_kv *kv, const uint8_t *key, uint32_t kl);
int meta_write_batch_as(struct efs_kv *kv, const struct efs_kv_item *items,
                        uint32_t count, const struct efs_txid *self, int dry);
int meta_write_batch(struct efs_kv *kv, const struct efs_kv_item *items,
                      uint32_t count);
int meta_write_put(struct efs_kv *kv, const uint8_t *key, uint32_t kl,
                    const uint8_t *value, uint32_t vl);
int meta_write_del(struct efs_kv *kv, const uint8_t *key, uint32_t kl);
int meta_cleanup_del(struct efs_kv *kv, const uint8_t *key, uint32_t kl);
int meta_cleanup_batch(struct efs_kv *kv, const struct efs_kv_item *items,
                       uint32_t count);
void pack_lane(uint8_t *p, const struct lane_rec *l);
int unpack_lane(const uint8_t *p, uint32_t n, struct lane_rec *l);
int lane_adopt(struct lane_rec *ln, uint64_t epoch);
int dir_lane_stamp(struct efs_kv *kv, const struct efs_meta_row *dir,
                   const char *name, uint64_t now,
                   uint8_t *k_ln, uint32_t *kl, uint8_t *v_ln);
int dseq_bump(struct efs_kv *kv, uint32_t shard, efs_ino_t dir,
              uint8_t lane, uint8_t *k, uint32_t *kl, uint8_t *v);
void pack_inode(uint8_t *p, const struct efs_meta_row *r);
int unpack_inode(const uint8_t *p, uint32_t n, struct efs_meta_row *r);
int content_fence_key(efs_ino_t ino, uint64_t gen, uint8_t authority,
                       uint8_t *key, uint32_t *len);
int fence_history_read(struct efs_kv *kv, efs_ino_t ino,
                         uint64_t gen, uint8_t authority,
                         efs_txn_coord_fn coord, void *ctx,
                         struct efs_txn_pending *pend,
                         struct efs_fence_history *out);
uint64_t writer_history_floor(uint64_t epoch, const struct efs_fence_history *h);
int lane_authority_key(efs_ino_t ino, uint64_t gen, uint8_t lane,
                        uint8_t *key, uint32_t *kl);
void pack_lane_authority(uint8_t *v, uint32_t cs, uint64_t epoch, uint64_t floor);
int unpack_lane_authority(const uint8_t *v, uint32_t len, uint32_t *cs,
                           uint64_t *epoch, uint64_t *floor);
void pack_dentry(uint8_t *p, const struct efs_meta_dentry *d);
int unpack_dentry(const uint8_t *p, uint32_t n, struct efs_meta_dentry *d);
int kv_get_copy(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                uint8_t *buf, uint32_t cap, uint32_t *out_len);
uint32_t chunk_header_size(const uint8_t *p, uint32_t n);
int unpack_chunk(const uint8_t *p, uint32_t n, struct efs_meta_chunk *ch);
uint32_t trailer_n(const uint8_t *v, uint32_t n);
int trailer_at(const uint8_t *v, uint32_t n, uint32_t i,
               struct efs_meta_delta *d);
int ranges_overlap(uint32_t a, uint32_t al, uint32_t b, uint32_t bl);
uint32_t pack_chunk_value(uint8_t *dst, const struct efs_meta_chunk *ch,
                          const struct efs_meta_delta *deltas,
                          uint32_t n);
int chunk_aliases(const struct efs_meta_chunk *a,
                  const struct efs_meta_chunk *b);

#endif
