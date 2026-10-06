#ifndef EFS_WRITER_STATE_H
#define EFS_WRITER_STATE_H
#include "efs/writer_ranges.h"

/* Dynamically allocated cache sidecar; charge sizeof this object to the
 * metadata hard bound before write admission. Caller owns serialization. */
struct efs_writer_state {
    struct efs_writer_ranges ranges;
    struct efs_writer_publication publication;
    int has_publication;
};
static inline int efs_writer_state_owned(const struct efs_writer_state *state)
{
    return state && (state->ranges.bytes.count || state->has_publication);
}
struct efs_writer_state *efs_writer_state_alloc(uint32_t chunk_size);
/* Caller holds the cache slot lock. NULL creates a budgeted sidecar only
 * after authority/range validation; failures leave both body and ownership
 * unchanged. The caller must not attach this to legacy accepted bytes whose
 * admission authority was never captured. src may overlap body. */
int efs_writer_state_write(struct efs_writer_state **state,
                            const struct efs_msg_inode_writer_view_reply *view,
                            uint8_t *body, uint32_t body_len,
                            uint32_t off, const uint8_t *src, uint32_t len);
/* Caller holds the same lock for body and ranges. Output storage is
 * caller-budgeted and must not overlap the cache body or state. Snapshot
 * contains only surviving owned bytes; overlay it onto a masked peer base,
 * never publish it as a whole image. Errors leave both outputs unchanged. */
int efs_writer_state_snapshot(const struct efs_writer_state *state,
                              const struct efs_msg_inode_writer_view_reply *view,
                              const struct efs_msg_inode_getchunks_reply *base,
                              const uint8_t *body, uint32_t body_len,
                              struct efs_writer_plan *out_plan,
                              uint8_t *out_body);
/* BUSY preserves the sidecar whenever accepted bytes/publication are owned. */
int efs_writer_state_free(struct efs_writer_state *state);
int efs_writer_state_put(struct efs_writer_state *state,
                          const struct efs_writer_plan *plan,
                          uint64_t object_generation, uint64_t snapshot_sequence);
int efs_writer_state_report(struct efs_writer_state *state,
                             uint64_t object_generation, uint64_t snapshot_sequence,
                             int verdict);
#endif
