#ifndef EFS_META_APPLY_GC_H
#define EFS_META_APPLY_GC_H

#include "apply_internal.h"

/* gc.c: GC and reap records, the two per-anchor counts, degraded marks
 * (W154) and recovery marks (K36).
 *
 * Owns
 *   gc_wm_*            the D26 pending-GC watermark per anchor (1 = odd
 *                      group, 2 = even group), under gc_wm_mu.
 *   dg_wm_*            the W154 degraded-mark count per anchor, under
 *                      dg_wm_mu.
 *   Both are process memory, unknown until the host's rebuild derives them
 *   from one prefix count (process start, snapshot import). While unknown,
 *   inserts accumulate and removals are dropped, so a count can only
 *   overshoot; a derive that finds the prefix empty clamps it to zero. A
 *   count moves only after its batch committed (never on a dry PREPARE).
 *
 * Locks: gc_wm_mu and dg_wm_mu are leaf mutexes, never held across a KV call
 *   or another lock. Mutations otherwise run inside the serialized apply.
 *
 * Exports inside the module
 *   gc_queue           queue one GC record for a dead chunk generation into
 *                      a caller batch (caller storage indexed by the global
 *                      item number); EFS_ERR_NOMEM when the batch is full,
 *                      EFS_ERR_BUSY for a packed object (its space goes
 *                      back through the pack ledger, never per-inode GC).
 *   degraded_key, degraded_retire
 *                      the mark key, and the queued DEL of a superseded
 *                      object's mark (skipped when the count is known zero;
 *                      *removed counts the queued deletes).
 *   degraded_note      adjust the count after a written batch (publish.c).
 *   evidence_ok        a publication's evidence: nonzero candidate, K2F1
 *                      profile (else EFS_ERR_STALE), distinct nonzero
 *                      fragment nodes (else EFS_ERR_INVAL). */

int gc_queue(struct efs_kv_item *it, uint32_t *n, uint32_t cap,
             uint8_t (*gc_keys)[EFS_KV_KEY_MAX],
             uint8_t (*gc_vals)[EFS_META_GC_VAL],
             efs_ino_t ino, uint8_t lane, uint32_t ci,
             const struct efs_meta_chunk *dead);
void degraded_note(uint32_t anchor, int delta);
int degraded_key(efs_ino_t ino, uint64_t object_gen, uint8_t lane,
                 uint32_t ci, uint8_t *key, uint32_t *kl);
int degraded_retire(struct efs_kv *kv, struct efs_kv_item *it, uint32_t *n,
                    uint32_t cap, uint8_t (*keys)[EFS_KV_KEY_MAX], uint32_t *nk,
                    efs_ino_t ino, uint8_t lane, uint32_t ci, uint64_t gen,
                    uint32_t *removed);
int evidence_ok(const struct efs_meta_pub *p);

#endif
