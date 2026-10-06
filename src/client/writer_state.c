#include "client_internal.h"
#include "efs/writer_state.h"
#include <string.h>

struct efs_writer_state *efs_writer_state_alloc(uint32_t chunk_size)
{
    if (!efs_chunk_size_valid(chunk_size))
        return NULL;
    struct efs_writer_state *state = efs_buf_metadata_alloc(sizeof(*state));
    if (state) {
        memset(state, 0, sizeof(*state));
        state->ranges.bytes.chunk_size = chunk_size;
    }
    return state;
}
int efs_writer_state_free(struct efs_writer_state *state)
{
    if (!state)
        return EFS_OK;
    if (state->ranges.bytes.count || state->has_publication)
        return EFS_ERR_BUSY;
    efs_buf_metadata_free(state, sizeof(*state));
    return EFS_OK;
}
int efs_writer_state_put(struct efs_writer_state *state,
                          const struct efs_writer_plan *plan,
                          uint64_t object_generation, uint64_t snapshot_sequence)
{
    if (!state || !plan)
        return EFS_ERR_INVAL;
    if (state->has_publication)
        return EFS_ERR_BUSY;
    if (state->ranges.ino != plan->ino || state->ranges.generation != plan->generation ||
        state->ranges.chunk_index != plan->chunk_index)
        return EFS_ERR_STALE;
    int rc = efs_writer_publication_bind(plan, object_generation, snapshot_sequence,
                                         &state->publication);
    if (rc == EFS_OK)
        state->has_publication = 1;
    return rc;
}
int efs_writer_state_report(struct efs_writer_state *state,
                             uint64_t object_generation, uint64_t snapshot_sequence,
                             int verdict)
{
    if (!state || !state->has_publication)
        return EFS_ERR_INVAL;
    int rc = efs_writer_publication_complete(&state->ranges, &state->publication,
                                              object_generation, snapshot_sequence, verdict);
    /* A matching committed old publication is finished even if a concurrent
     * rewrite prevents clearing ownership. Its newer bytes need a new PUT. */
    if (verdict == EFS_OK && object_generation == state->publication.object_generation &&
        snapshot_sequence == state->publication.snapshot_sequence) {
        state->has_publication = 0;
        memset(&state->publication, 0, sizeof(state->publication));
    }
    return rc;
}
