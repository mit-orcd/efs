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
int efs_writer_state_write(struct efs_writer_state **slot,
                            const struct efs_msg_inode_writer_view_reply *view,
                            uint8_t *body, uint32_t body_len,
                            uint32_t off, const uint8_t *src, uint32_t len)
{
    if (!slot || !body || !src || !efs_chunk_size_valid(body_len))
        return EFS_ERR_INVAL;
    struct efs_writer_state *state = *slot;
    if (state && state->ranges.bytes.chunk_size != body_len)
        return EFS_ERR_INVAL;
    struct efs_writer_ranges next = state ? state->ranges :
        (struct efs_writer_ranges){.bytes.chunk_size = body_len};
    int rc = efs_writer_ranges_admit(&next, view, off, len);
    if (rc != EFS_OK)
        return rc;
    if (!state) {
        state = efs_writer_state_alloc(body_len);
        if (!state)
            return EFS_ERR_NOMEM;
    }
    /* No remaining failure point after accepted ownership becomes visible. */
    memmove(body + off, src, len);
    state->ranges = next;
    *slot = state;
    return EFS_OK;
}

int efs_writer_state_snapshot(const struct efs_writer_state *state,
                              const struct efs_msg_inode_writer_view_reply *view,
                              const struct efs_msg_inode_getchunks_reply *base,
                              const uint8_t *body, uint32_t body_len,
                              struct efs_writer_plan *out_plan,
                              uint8_t *out_body)
{
    if (!state || !body || !out_body || !out_plan || body == out_body ||
        !efs_chunk_size_valid(body_len) || state->ranges.bytes.chunk_size != body_len)
        return EFS_ERR_INVAL;
    struct efs_writer_plan plan;
    int rc = efs_writer_ranges_plan_base(&state->ranges, view, base, &plan);
    if (rc != EFS_OK)
        return rc;
    /* Successful planning validated every surviving range before output. */
    memset(out_body, 0, body_len);
    for (uint32_t i = 0; i < plan.surviving.count; ++i) {
        const struct efs_fence_part *p = &plan.surviving.ranges[i];
        memcpy(out_body + p->off, body + p->off, p->len);
    }
    *out_plan = plan;
    return EFS_OK;
}

int efs_writer_state_free(struct efs_writer_state *state)
{
    if (!state)
        return EFS_OK;
    if (efs_writer_state_owned(state))
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
