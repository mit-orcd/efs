#include "efs/wb_recovery.h"
#include "efs/protocol.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct efs_wb_error_state e = {0};
    uint64_t d1 = 0, d2 = 0;
    efs_wb_error_add(&e);
    assert(efs_wb_error_sync(&e, &d1, 0));
    assert(efs_wb_error_sync(&e, &d1, 0));
    uint64_t reopened = e.sequence;
    assert(efs_wb_error_sync(&e, &reopened, 0));
    efs_wb_error_add(&e);
    assert(efs_wb_error_resolve(&e));
    assert(efs_wb_error_sync(&e, &d1, 1)); /* one record still dirty */
    assert(efs_wb_error_resolve(&e));
    assert(!efs_wb_error_sync(&e, &d1, 1));
    assert(!efs_wb_error_sync(&e, &d1, 0));
    assert(efs_wb_error_sync(&e, &d2, 0)); /* unobserved historical error */
    assert(!efs_wb_error_sync(&e, &d2, 0));
    uint64_t d3 = e.sequence;
    assert(!efs_wb_error_sync(&e, &d3, 0));
    assert(!efs_wb_error_resolve(&e));

    struct efs_wb_cycle_state c = {0};
    for (int i = 0; i < 7; ++i)
        assert(!efs_wb_cycle_complete(&c, 10, 20, 30, 2));
    assert(efs_wb_cycle_complete(&c, 10, 20, 30, 2));
    assert(!efs_wb_cycle_complete(&c, 10, 21, 30, 2));
    assert(!efs_wb_cycle_complete(&c, 10, 21, 31, 2));
    assert(!efs_wb_cycle_complete(&c, 10, 21, 31, 3));
    assert(!efs_wb_cycle_complete(&c, 11, 21, 31, 3));
    for (unsigned i = 0; i < 1000; ++i)
        assert(!efs_wb_cycle_complete(&c, 11, 22+i, 31, 3));
    efs_wb_cycle_reset(&c);
    assert(!c.valid);

    struct efs_chunk_rec r = {0};
    uint64_t epoch = 99;
    assert(efs_chunk_publish_epoch(&r, 7, &epoch) == EFS_OK && epoch == 7);
    r.publish_flags = EFS_CHUNK_REC_F_CAPTURED_EPOCH;
    assert(efs_chunk_publish_epoch(&r, 0, &epoch) == EFS_OK && epoch == 0);
    r.publish_flags=EFS_CHUNK_REC_F_CAPTURED_FILEID;r.file_generation=7;
    assert(efs_chunk_publish_authority(&r,7,0,&epoch)==EFS_ERR_INVAL);
    r.publish_flags|=EFS_CHUNK_REC_F_CAPTURED_EPOCH;
    epoch=99;
    assert(efs_chunk_publish_authority(&r,8,0,&epoch)==EFS_ERR_STALE && epoch==99);
    assert(efs_chunk_publish_authority(&r,7,0,&epoch)==EFS_OK && epoch==0);
    r.file_generation=0;
    assert(efs_chunk_publish_authority(&r,7,0,&epoch)==EFS_ERR_INVAL);
    r.publish_flags=EFS_CHUNK_REC_F_CAPTURED_EPOCH;epoch=99;
    r.publish_epoch = 4;
    epoch = 99;
    assert(efs_chunk_publish_epoch(&r, 5, &epoch) == EFS_ERR_STALE && epoch == 99);
    assert(efs_chunk_publish_epoch(&r, 3, &epoch) == EFS_ERR_STALE && epoch == 99);
    assert(efs_chunk_publish_epoch(&r, 4, &epoch) == EFS_OK && epoch == 4);
    r.publish_flags |= EFS_CHUNK_REC_F_FRESH_OBJECT;
    assert(efs_chunk_publish_epoch(&r, 4, &epoch) == EFS_OK);
    r.publish_flags |= 8;
    assert(efs_chunk_publish_epoch(&r, 4, &epoch) == EFS_ERR_INVAL);
    puts("writeback recovery: sticky errors, partial resolution, progress and captured epoch PASS");
    return 0;
}
