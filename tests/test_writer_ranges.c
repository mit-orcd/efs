#include "efs/writer_ranges.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct efs_msg_inode_writer_view_reply authority(uint64_t epoch)
{
    struct efs_msg_inode_writer_view_reply v = {0};
    v.ino = 100; v.generation = 7; v.authority_epoch = epoch;
    return v;
}
static void test_base_identity(void)
{
    struct efs_writer_ranges writer = {0};
    struct efs_msg_inode_writer_view_reply view = authority(0);
    struct efs_msg_inode_getchunks_reply *base = calloc(1, sizeof(*base));
    struct efs_writer_plan plan, saved;
    assert(base);
    writer.bytes.chunk_size = EFS_MIN_CHUNK_SIZE;
    assert(efs_writer_ranges_admit(&writer, &view, 0, 200) == EFS_OK);
    view.authority_epoch = 1; view.history.count = 1;
    view.history.entries[0] = (struct efs_content_fence){1, 100};
    base->ino = writer.ino; base->generation = writer.generation; base->authority_epoch = 1;
    assert(efs_writer_ranges_plan_base(&writer, &view, base, &plan) == EFS_OK);
    assert(plan.surviving.ranges[0].len == 100);
    saved = plan;
    base->authority_epoch = 0;
    assert(efs_writer_ranges_plan_base(&writer, &view, base, &plan) == EFS_ERR_STALE);
    base->authority_epoch = 1; ++base->generation;
    assert(efs_writer_ranges_plan_base(&writer, &view, base, &plan) == EFS_ERR_STALE);
    --base->generation; base->count = 1;
    base->recs[0].ino = writer.ino; base->recs[0].read_view.chunk_size = EFS_MIN_CHUNK_SIZE;
    base->recs[0].read_view.count = 1;
    assert(efs_writer_ranges_plan_base(&writer, &view, base, &plan) == EFS_ERR_STALE);
    base->recs[0].read_view.fence_epoch = base->recs[0].read_view.revision = 1;
    ++base->recs[0].chunk_index;
    assert(efs_writer_ranges_plan_base(&writer, &view, base, &plan) == EFS_ERR_STALE);
    --base->recs[0].chunk_index;
    assert(!memcmp(&plan, &saved, sizeof(plan)));
    assert(efs_writer_ranges_plan_base(&writer, &view, base, &plan) == EFS_OK);
    free(base);
}

int main(void)
{
    test_base_identity();
    struct efs_writer_ranges writer = {0}, saved;
    struct efs_writer_plan plan, sentinel;
    struct efs_msg_inode_writer_view_reply v = authority(0);
    writer.bytes.chunk_size = 1024;
    assert(efs_writer_ranges_admit(&writer, &v, 0, 700) == EFS_OK);
    assert(writer.generation == 7 && writer.bytes.ranges[0].epoch == 0);
    v.authority_epoch = 1; v.history.count = 1;
    v.history.entries[0] = (struct efs_content_fence){1, 100};
    assert(efs_writer_ranges_admit(&writer, &v, 500, 100) == EFS_OK);
    v.authority_epoch = 2; v.history.count = 2;
    v.history.entries[1] = (struct efs_content_fence){2, 700};
    assert(efs_writer_ranges_plan(&writer, &v, 2, &plan) == EFS_OK);
    assert(plan.surviving.count == 2 && plan.surviving.ranges[0].len == 100 &&
           plan.surviving.ranges[1].off == 500 && plan.surviving.ranges[1].len == 100 &&
           plan.surviving.ranges[1].epoch == 1 && plan.publish_epoch == 2);
    uint8_t base[1024], local[1024];
    memset(base, 'P', sizeof(base)); memset(local, 'L', sizeof(local));
    assert(efs_dirty_ranges_overlay(&plan.surviving, base, sizeof(base), local) == EFS_OK);
    for (unsigned i = 0; i < sizeof(base); ++i)
        assert(base[i] == ((i < 100 || (i >= 500 && i < 600)) ? 'L' : 'P'));
    /* Peer bytes under a clipped local suffix survive; failed revalidation
     * neither changes the caller's plan nor recaptures accepted byte ages. */
    saved = writer; memset(&sentinel, 0xa5, sizeof(sentinel));
    struct efs_writer_plan failed = sentinel;
    v.oldest_complete_epoch = 1;
    assert(efs_writer_ranges_plan(&writer, &v, 2, &failed) == EFS_ERR_STALE);
    assert(!memcmp(&failed, &sentinel, sizeof(failed)) && !memcmp(&writer, &saved, sizeof(saved)));
    v.oldest_complete_epoch = 0;
    assert(efs_writer_ranges_plan(&writer, &v, 1, &failed) == EFS_ERR_STALE);
    assert(!memcmp(&failed, &sentinel, sizeof(failed)));
    ++v.generation;
    assert(efs_writer_ranges_admit(&writer, &v, 10, 1) == EFS_ERR_STALE);
    assert(!memcmp(&writer, &saved, sizeof(saved)));
    --v.generation;
    assert(efs_writer_ranges_admit(&writer, &v, 500, 1) == EFS_OK);
    assert(efs_writer_ranges_ack(&writer, &plan) == EFS_ERR_STALE && writer.bytes.count);
    assert(efs_writer_ranges_plan(&writer, &v, 2, &plan) == EFS_OK);
    assert(efs_writer_ranges_ack(&writer, &plan) == EFS_OK && !writer.bytes.count);
    assert(writer.observed_epoch == 2);
    v = authority(0);
    assert(efs_writer_ranges_admit(&writer, &v, 10, 1) == EFS_ERR_STALE);
    saved = writer;
    v.authority_epoch = 2; v.oldest_complete_epoch = 2;
    ++v.chunk_index;
    assert(efs_writer_ranges_admit(&writer, &v, 10, 1) == EFS_ERR_STALE);
    --v.chunk_index; ++v.ino;
    assert(efs_writer_ranges_admit(&writer, &v, 10, 1) == EFS_ERR_STALE);
    --v.ino; v.history.count = EFS_FENCE_HISTORY_MAX + 1;
    assert(efs_writer_ranges_admit(&writer, &v, 10, 1) == EFS_ERR_PROTO);
    assert(!memcmp(&writer, &saved, sizeof(saved)));
    v = authority(0);
    struct efs_writer_ranges empty = {0}; empty.bytes.chunk_size = 1024;
    saved = empty;
    assert(efs_writer_ranges_admit(&empty, &v, 1024, 1) == EFS_ERR_INVAL);
    assert(!memcmp(&empty, &saved, sizeof(saved)) && !empty.ino);
    /* Disjoint range exhaustion is transactional, including FileID binding. */
    memset(&writer, 0, sizeof(writer)); writer.bytes.chunk_size = 1024;
    for (uint32_t i = 0; i < EFS_DIRTY_RANGE_MAX; ++i)
        assert(efs_writer_ranges_admit(&writer, &v, 2 * i, 1) == EFS_OK);
    saved = writer;
    assert(efs_writer_ranges_admit(&writer, &v, 2 * EFS_DIRTY_RANGE_MAX, 1) == EFS_ERR_BUSY);
    assert(!memcmp(&writer, &saved, sizeof(saved)));
    /* A late chunk entirely beyond the fence owns no publishable bytes,
     * but its original snapshot remains the acknowledgement identity. */
    memset(&writer, 0, sizeof(writer)); writer.bytes.chunk_size = 1024;
    v.chunk_index = 1;
    assert(efs_writer_ranges_admit(&writer, &v, 0, 100) == EFS_OK);
    v.authority_epoch = 1; v.history.count = 1;
    v.history.entries[0] = (struct efs_content_fence){1, 100};
    assert(efs_writer_ranges_plan(&writer, &v, 1, &plan) == EFS_OK);
    assert(!plan.surviving.count && plan.original.count == 1);
    puts("writer ranges: FileID admission, coherent planning, fence clipping and exact ACK PASS");
}
