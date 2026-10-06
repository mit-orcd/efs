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
/* BUSY preserves the sidecar whenever accepted bytes/publication are owned. */
int efs_writer_state_free(struct efs_writer_state *state);
int efs_writer_state_put(struct efs_writer_state *state,
                          const struct efs_writer_plan *plan,
                          uint64_t object_generation, uint64_t snapshot_sequence);
int efs_writer_state_report(struct efs_writer_state *state,
                             uint64_t object_generation, uint64_t snapshot_sequence,
                             int verdict);
#endif
