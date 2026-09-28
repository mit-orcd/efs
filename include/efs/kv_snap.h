#ifndef EFS_KV_SNAP_H
#define EFS_KV_SNAP_H

#include "efs/kv.h"

/* Group-filtered view of the applied KV for Raft InstallSnapshot.
 *
 * Not a new store format: the blob is the existing LSM WAL item payload
 * (kv_lsm_internal.h): u32 nitems, then nitems ×
 * (u8 op · u32 klen · u32 vlen · key · val). Only live PUTs.
 * Keys are selected by shard prefix → efs_raft_shard_group (odd = 0,
 * even = 2). One engine holds every group; a snap must not carry the
 * other group's keys and must replace only its own namespace.
 *
 * max_bytes is the in-memory export cap used by tests. A snapshot that
 * does not fit is written by efs_kv_lsm_view_export and shipped as
 * InstallSnapshot chunks (architecture.md §5). */

int efs_kv_group_export(struct efs_kv *kv, uint8_t group, uint32_t max_bytes,
                        uint8_t **data, uint32_t *len);
int efs_kv_group_import(struct efs_kv *kv, uint8_t group, const uint8_t *data,
                        uint32_t len);
/* Read the whole file and efs_kv_group_import it. */
int efs_kv_group_import_file(struct efs_kv *kv, uint8_t group, const char *path);
/* Diff incoming against a local image already in the same blob format
 * (a pinned-view export). Does not scan or write the live store.
 * *diff points into *hold_a / *hold_b; the caller batches, then frees
 * all three. *hold_a and *hold_b are the two file images. */
int efs_kv_group_import_prepare(struct efs_kv *kv, uint8_t group,
                                const char *incoming, const char *local,
                                struct efs_kv_item **diff, uint32_t *n,
                                uint8_t **hold_a, uint8_t **hold_b);

#endif
