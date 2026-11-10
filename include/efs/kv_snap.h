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
/* Streaming snapshot import (K108). The incoming image and a local image of
 * the same group, both in the blob format above and both sorted and unique (as
 * efs_kv_lsm_view_export writes them), are merged into a diff applied in
 * bounded batches. Open reads each file once, before any write, and refuses
 * an image that is malformed, out of order, outside the group or holds
 * reserved rows (EFS_ERR_PROTO). Each step builds and applies one batch of at
 * most 1024 keys / 1 MiB of key+value bytes and merges at most 16384 records;
 * a failed batch is applied again by the next step. Memory held between steps
 * is two read windows and one batch, independent of the image sizes. The files
 * may be unlinked after open. A half-applied import is not resumed: a new open
 * diffs the incoming image against the then-current local image. */
struct efs_kv_import;
int efs_kv_import_open(struct efs_kv *kv, uint8_t group, const char *incoming,
                       const char *local, struct efs_kv_import **out);
/* *done = 1 once every record is merged and the last batch applied. */
int efs_kv_import_step(struct efs_kv_import *im, int *done);
/* Records per image, the high-water of the importer's own buffers and the
 * keys of the diff built so far. Any pointer may be NULL. */
void efs_kv_import_stat(const struct efs_kv_import *im, uint32_t *n_incoming,
                        uint32_t *n_local, uint64_t *peak_bytes,
                        uint64_t *n_diff);
void efs_kv_import_close(struct efs_kv_import *im);

#endif
