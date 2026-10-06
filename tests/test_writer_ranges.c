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
    assert(plan.base_absent && plan.surviving.ranges[0].len == 100);
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
    assert(efs_writer_ranges_plan_base(&writer, &view, base, &plan) == EFS_OK && plan.base_absent);
    --base->recs[0].chunk_index;
    assert(!memcmp(&plan, &saved, sizeof(plan)));
    assert(efs_writer_ranges_plan_base(&writer, &view, base, &plan) == EFS_OK && !plan.base_absent);
    free(base);
}

static void test_publication_identity(void)
{
    struct efs_writer_ranges writer = {0}, saved;
    struct efs_writer_plan plan;
    struct efs_writer_publication publication, sentinel;
    struct efs_msg_inode_writer_view_reply view = authority(0);
    writer.bytes.chunk_size = 1024;
    assert(efs_writer_ranges_admit(&writer, &view, 0, 100) == EFS_OK);
    assert(efs_writer_ranges_plan(&writer, &view, 0, &plan) == EFS_OK);
    memset(&sentinel, 0xa5, sizeof(sentinel)); publication = sentinel;
    struct efs_writer_plan bad = plan;
    bad.surviving.ranges[0].off=100;
    assert(efs_writer_publication_bind(&bad,100,10,&publication)==EFS_ERR_INVAL);
    assert(!memcmp(&publication,&sentinel,sizeof(publication)));
    bad=plan;bad.surviving.ranges[0].epoch=1;
    assert(efs_writer_publication_bind(&bad,100,10,&publication)==EFS_ERR_INVAL);
    bad=plan;bad.original.ranges[0].epoch=bad.surviving.ranges[0].epoch=1;
    assert(efs_writer_publication_bind(&bad,100,10,&publication)==EFS_ERR_INVAL);
    bad=plan;bad.base_absent=2;
    assert(efs_writer_publication_bind(&bad,100,10,&publication)==EFS_ERR_INVAL);
    assert(efs_writer_publication_bind(&plan, 0, 10, &publication) == EFS_ERR_INVAL);
    assert(!memcmp(&publication, &sentinel, sizeof(publication)));
    assert(efs_writer_publication_bind(&plan, 1000, 10, &publication) == EFS_OK);
    saved = writer;
    assert(efs_writer_publication_complete(&writer, &publication, 1000, 10, EFS_ERR_IO) == EFS_ERR_IO);
    assert(efs_writer_publication_complete(&writer, &publication, 1001, 10, EFS_OK) == EFS_ERR_STALE);
    assert(efs_writer_publication_complete(&writer, &publication, 1000, 11, EFS_OK) == EFS_ERR_STALE);
    assert(!memcmp(&writer, &saved, sizeof(writer)));
    assert(efs_writer_ranges_admit(&writer, &view, 0, 1) == EFS_OK);
    assert(efs_writer_publication_complete(&writer, &publication, 1000, 10, EFS_OK) == EFS_ERR_STALE);
    assert(writer.bytes.count);
    assert(efs_writer_ranges_plan(&writer, &view, 0, &plan) == EFS_OK);
    assert(efs_writer_publication_bind(&plan, 1002, 12, &publication) == EFS_OK);
    assert(efs_writer_publication_complete(&writer, &publication, 1002, 12, EFS_OK) == EFS_OK);
    assert(!writer.bytes.count);
    assert(efs_writer_ranges_admit(&writer, &view, 0, 100) == EFS_OK);
    view.authority_epoch = 1; view.history.count = 1;
    view.history.entries[0] = (struct efs_content_fence){1, 100};
    assert(efs_writer_ranges_plan(&writer, &view, 1, &plan) == EFS_OK);
    assert(efs_writer_publication_bind(&plan, 1003, 13, &publication) == EFS_OK);
    assert(efs_writer_publication_complete(&writer, &publication, 1003, 13, EFS_OK) == EFS_OK);
    assert(writer.observed_epoch == 1);
    view = authority(0);
    assert(efs_writer_ranges_admit(&writer, &view, 0, 1) == EFS_ERR_STALE);

}

static void test_materialized_publication(void)
{
    uint32_t cs=EFS_MIN_CHUNK_SIZE;
    uint8_t *peer=malloc(cs+1),*owned=malloc(cs),*out=malloc(cs);
    struct efs_msg_inode_getchunks_reply *base=calloc(1,sizeof(*base));
    assert(peer && owned && out && base);
    memset(peer,'P',cs+1);memset(owned,'L',cs);memset(out,0x5a,cs);
    struct efs_writer_ranges writer={0};writer.bytes.chunk_size=cs;
    struct efs_msg_inode_writer_view_reply view=authority(0);
    assert(efs_writer_ranges_admit(&writer,&view,20,4)==EFS_OK);
    assert(efs_writer_ranges_admit(&writer,&view,500,4)==EFS_OK);
    view.authority_epoch=1;view.history.count=1;
    view.history.entries[0]=(struct efs_content_fence){1,100};
    base->ino=100;base->generation=7;base->authority_epoch=1;base->count=1;
    struct efs_chunk_rec *r=&base->recs[0];r->ino=100;r->chunk_generation=5;
    r->base_gen=5;r->delta_base_seq=3;
    r->read_view=(struct efs_fence_view){.chunk_size=cs,.count=1,.fence_epoch=1,.revision=1};
    r->read_view.parts[0]=(struct efs_fence_part){.len=100};
    struct efs_writer_plan plan;
    assert(efs_writer_ranges_plan_base(&writer,&view,base,&plan)==EFS_OK);
    assert(plan.base_bound && plan.base_generation==5 && plan.base_sequence==3);
    ++r->chunk_generation;
    assert(efs_writer_plan_materialize(&plan,base,peer,owned,out,cs)==EFS_ERR_STALE);
    assert(out[0]==0x5a);--r->chunk_generation;
    assert(efs_writer_plan_materialize(&plan,base,peer,owned,peer+1,cs)==EFS_ERR_INVAL);
    assert(efs_writer_plan_materialize(&plan,base,peer,owned,out,cs)==EFS_OK);
    for(uint32_t i=0;i<cs;i++) assert(out[i]==(i>=20 && i<24?'L':i<100?'P':0));
    struct efs_chunk_rec put={.ino=100,.chunk_generation=17}, report;
    for(unsigned i=0;i<EFS_NUM_FRAGMENTS;i++) put.nodes[i]=i+1;
    assert(efs_writer_plan_report(&plan,&put,&report)==EFS_OK);
    assert(report.file_generation==7 && report.publish_epoch==1 && report.base_gen==5 &&
           report.delta_base_seq==3 && report.publish_flags==3 && !report.delta_len);
    size_t n=efs_chunk_rec_wire_size(&report),used=0;uint8_t *wire=malloc(n);assert(wire);
    assert(efs_chunk_rec_pack(wire,n,&report)==n);
    memset(&put,0,sizeof(put));
    assert(efs_chunk_recs_unpack(wire,n,1,&put,&used)==0 && used==n);
    assert(put.file_generation==7 && put.publish_flags==3);
    free(wire);free(peer);free(owned);free(out);free(base);
}

int main(void)
{
    test_materialized_publication();
    test_base_identity();
    test_publication_identity();
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
