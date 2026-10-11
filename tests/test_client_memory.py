#!/usr/bin/env python3
"""Run actual cache ownership functions with filesystem/RPC stubs.
No Linux FUSE or network required; not a substitute for cold-read load gates.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import sys
root = Path(__file__).resolve().parents[1]
w = Path(os.environ.get('EFS_CLIENT_MEMORY_WRITE_SOURCE', root / 'src/client/write.c')).read_text()
f = (root / 'src/client/efs_fuse.c').read_text()
r = (root / 'src/client/read.c').read_text()
def function(s, name):
    m = re.search(r'^(?:static )?[^\n;]*\b' + name + r'\([^;]+?\)\n\{', s, re.M)
    if not m:
        raise RuntimeError(name)
    return s[m.start():s.index('\n}', m.end()) + 2]
def block(s, start):
    a = s.index(start)
    return s[a:s.index('\n};', a) + 3]
source = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include "client_internal.h"
#include "efs/writer_state.h"
#include "efs/publication.h"
#include "efs/put_ticket.h"
#include "efs/kv_key.h"
#include "efs/write_extent.h"
#include <limits.h>
struct efs_client g_client;
int efs_rdma_zc_region_add(void *p, size_t n) { (void)p; (void)n; return 0; }
#include "bufpool.c"
#include "writer_state.c"
/* Hash implementation is covered by the durable LSM test; this harness faults
 * the actual cache identity/ownership transitions without SIMD linkage. */
void efs_hash(const void *input,size_t n,uint8_t out[EFS_HASH_SIZE]) {
    const uint8_t *p=input;memset(out,0,EFS_HASH_SIZE);
    for(size_t i=0;i<n;i++)out[i%EFS_HASH_SIZE]=(uint8_t)((out[i%EFS_HASH_SIZE]*33)^p[i]);
}
/* Packed record encoding uses the real canonical codec; this legacy-only
 * ownership harness does not create segment bodies. */
int efs_segment_geometry(size_t n, size_t *out) { (void)n; (void)out; return EFS_ERR_INVAL; }
#define put packed_put
#define get packed_get
#include "packed_segment.c"
#undef put
#undef get
#include "publication.c"
int efs_client_publication_id(uint64_t seq,struct efs_opid *out) {
    memset(out,0,sizeof(*out));out->client_uuid[0]=1;out->session_epoch=1;out->seq=seq;return EFS_OK;
}
#define DCACHE_NR 32
#define DCACHE_SLOTS 65536
#define DCACHE_SHARDS 64
#define DTRACE(...) ((void)0)
'''+block(w, 'struct dcache_ent {')+r'''
static struct { struct dcache_ent e[DCACHE_SLOTS]; pthread_mutex_t shard[DCACHE_SHARDS]; } g_dcache;
static uint64_t dirty_bytes;
static void dcache_ensure(void) {}
static void dcache_pin_add(struct dcache_ent *e) { e->pin_held=1; }
static void wb_error_record_resolve(efs_ino_t ino) { (void)ino; }
static void dcache_pin_release(struct dcache_ent *e) { e->pin_held=0; }
static void dcache_dirty_unlink(struct dcache_ent *e, uint32_t s) { (void)s; e->on_dirty=0; }
static void dcache_dirty_link(struct dcache_ent *e, uint32_t s) { (void)s; e->on_dirty=1; }
static void dcache_set_dirty(struct dcache_ent *e, uint32_t s) { e->dirty=1; dcache_dirty_link(e,s); }
static void dcache_note_dirty_bytes(int64_t n) { dirty_bytes+=n; }
static uint64_t dcache_seq_now(void) { return 1; }
static uint64_t dcache_seq_next(struct dcache_ent *e) { return ++e->snap_seq; }
static void dcache_account_extra(efs_ino_t ino, uint32_t ci) { (void)ino;(void)ci; }
static void dcache_add_range(struct dcache_ent *e,uint32_t off,uint32_t len) { e->nrange=1;e->roff[0]=off;e->rlen[0]=len; }
void efs_export_present_add(struct efs_export *ex,efs_ino_t ino,int which,int delta) { (void)ex;(void)ino;(void)which;(void)delta; }
'''
ticket_source=(root/'src/meta/put_ticket.c').read_text()
for name in ['wr32','wr64','ticket_shard','ticket_key','efs_put_ticket_encode','efs_put_ticket_from_pub']:
    source+='\n'+function(ticket_source,name)
for name in ['dcache_slot','dcache_mu','dcache_find','dcache_find_meta','dcache_metadata_idle','dcache_chain_reuse','dcache_keep_on_drop','efs_dcache_bind_writer','dcache_typed_admission_idle','efs_dcache_write_lane','dcache_snapshot_lane','efs_dcache_snapshot_lane','efs_dcache_snapshot_publication','efs_dcache_begin_publication','efs_dcache_finish_publication','efs_dcache_publication_report','efs_dcache_complete_publication','efs_dcache_publication_request_ticketed','efs_dcache_publication_request','efs_dcache_consume_publication','efs_dcache_publication_result','dcache_take']:
    source += '\n'+function(w,name)
source += '\n'+function(w,'efs_dcache_trim_metadata')
source += '\n'+block(w, 'struct dcache_init {')
for name in ['dcache_apply_init','dcache_store_owned_locked','dcache_drop_locked','dcache_body_to_rdcache','dcache_install_image','dcache_flush_keep']:
    source += '\n'+function(w,name)
source += '\n'+block(w, 'struct chunk_put_job {')+'\n'+block(w, 'struct flush_pipe {')
source += r'''
static unsigned pipe_windows_closed;
static void put_win_close(efs_ino_t ino) { (void)ino; pipe_windows_closed++; }
static int put_pool_run(struct chunk_put_job *jobs, uint32_t n) {
    for (uint32_t i=0;i<n;i++) jobs[i].rc=EFS_ERR_IO;
    return EFS_ERR_IO;
}
'''
for name in ['flush_pipe_init','flush_pipe_drain','flush_pipe_add']:
    source += '\n'+function(w,name)
source += '\n'+function(w,'efs_dcache_admit_lane_write')+'\n'+function(w,'dcache_range_covered')+'\n'+function(w,'efs_dcache_overlay_lane')+'\n'+function(w,'efs_dcache_retire_clipped')
source += r'''
static int dcache_store_owned(efs_ino_t ino, uint32_t ci, uint8_t *p, uint32_t len, const struct dcache_init *in) {
    pthread_mutex_t *mu=dcache_mu(dcache_slot(ino,ci));
    pthread_mutex_lock(mu);
    int rc=dcache_store_owned_locked(ino,ci,p,len,in);
    pthread_mutex_unlock(mu);
    if (!rc) dcache_account_extra(ino,ci);
    return rc;
}
static void dcache_img_to_rdcache(efs_ino_t ino,uint32_t ci,uint8_t *img,uint32_t len) { (void)ino;(void)ci;efs_buf_free(img,len); }
'''+function(w,'dcache_note_committed')+'\n'+function(w,'efs_dcache_pressure_ino')
source += r'''
static uint64_t deadline;
uint64_t efs_client_rpc_deadline_ms(void) {return deadline;}
static uint64_t stats_now_ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
int efs_client_rpc_past_deadline(void) {return deadline&&stats_now_ms()>=deadline;}
static uint32_t fuse_chunk_size(void) { return EFS_CHUNK_SIZE; }
static int flush_error, drains, trims;
void efs_rdcache_trim(void) { trims++; }
static int efs_append_flush_report(void *fi, efs_ino_t ino) {
    (void)fi;
    drains++;
    if (flush_error) return flush_error;
    for (uint32_t s=0;s<DCACHE_SLOTS;s++)
        for (struct dcache_ent *e=&g_dcache.e[s];e;e=e->next) {
            if (e->ino!=ino || !e->data) continue;
            if (e->dirty) { dirty_bytes-=e->len; e->dirty=0; e->on_dirty=0; }
            e->pin_held=0;
            e->object_gen=9;
            e->object_seq=4;
            e->object_delta_len=1;
            assert(dcache_note_committed(ino,e->ci,0,9,4));
        }
    return EFS_OK;
}
'''+function(f,'fuse_validate_write_extent')+'\n'+function(f,'fuse_write_admit')
source += r'''
static struct dcache_ent *entry(uint32_t ci) { return dcache_find_meta(dcache_slot(1,ci),1,ci); }
int main(void) {
    deadline=stats_now_ms();assert(fuse_write_admit(1)==-EAGAIN);deadline=0;
    uint64_t limit=(uint64_t)UINT32_MAX*EFS_CHUNK_SIZE;
    assert(!fuse_validate_write_extent(0,1));
    assert(fuse_validate_write_extent(-1,1)==-EINVAL);
    assert(fuse_validate_write_extent(0,(size_t)INT_MAX+1)==-EFBIG);
    assert(!fuse_validate_write_extent((off_t)(limit-1),1));
    assert(fuse_validate_write_extent((off_t)(limit-1),2)==-EFBIG);
    assert(efs_write_extent_valid(UINT64_MAX,2,EFS_CHUNK_SIZE)==EFS_ERR_INVAL);
    assert(efs_write_extent_valid(0,1,1)==EFS_ERR_INVAL);
    setenv("EFS_DCACHE_HARD_BYTES","33554432",1);
    setenv("EFS_DCACHE_DRAIN_BYTES","33554432",1);
    for(int i=0;i<DCACHE_SHARDS;i++) assert(!pthread_mutex_init(&g_dcache.shard[i],NULL));
    struct dcache_init in={0,0,0,0,1};
    /* More distinct sparse chunks than fit in the hard limit cannot install. */
    for(uint32_t ci=0;ci<256;ci++) {
        uint8_t *p=efs_buf_alloc(EFS_CHUNK_SIZE); assert(p); p[0]=42;
        assert(!dcache_store_owned(1,ci,p,EFS_CHUNK_SIZE,&in));
    }
    assert(!efs_buf_alloc(EFS_CHUNK_SIZE));
    assert(dirty_bytes==(32ull<<20));
    assert(efs_dcache_pressure_ino(0)==1 && !efs_dcache_pressure_ino(1));
    flush_error=EFS_ERR_IO;
    assert(fuse_write_admit(1)==-EIO);
    assert(drains==1 && trims==1 && dirty_bytes==(32ull<<20));
    assert(entry(0)->data[0]==42 && g_reserved==0 && g_meta_reserved==0);
    flush_error=EFS_ERR_QUOTA;
    assert(fuse_write_admit(1)==-ENOSPC);
    assert(entry(0)->data[0]==42 && dirty_bytes==(32ull<<20));
    flush_error=0;
    assert(!fuse_write_admit(1));
    efs_buf_unreserve();
    assert(!g_live && !dirty_bytes && !g_reserved && !g_meta_reserved);
    /* Drain before exhaustion: this request still fits the hard bound,
     * but retaining the entire backlog would outlive a bounded live PUT. */
    for(uint32_t ci=0;ci<128;ci++) {
        uint8_t *body=efs_buf_alloc(EFS_CHUNK_SIZE);assert(body);
        assert(!dcache_store_owned(1,ci,body,EFS_CHUNK_SIZE,&in));
    }
    int before_drains=drains;
    assert(efs_buf_should_drain());
    assert(!fuse_write_admit(1));
    assert(drains>before_drains && !g_live && !dirty_bytes);
    efs_buf_unreserve();
    /* Same generation, newer snapshot: older REPORT must retain all ranges. */
    uint8_t *p=efs_buf_alloc(EFS_CHUNK_SIZE); assert(p); p[0]=43;
    assert(!dcache_store_owned(1,0,p,EFS_CHUNK_SIZE,&in));
    struct dcache_ent *e=entry(0); e->dirty=0;e->pin_held=0;e->on_dirty=0;
    e->object_gen=9;e->object_seq=6;e->object_delta_len=1;
    assert(!dcache_note_committed(1,0,0,9,5));
    assert(e->data==p && e->nrange==1 && dcache_keep_on_drop(e));
    dcache_drop_locked(1,0,1);
    assert(e->data==p);
    /* A concurrent patch stays owned even after its preceding PUT commits. */
    e->dirty=1;
    assert(dcache_note_committed(1,0,0,9,6));
    assert(e->data==p && e->nrange==1 && dcache_keep_on_drop(e));
    e->dirty=0;
    assert(dcache_note_committed(1,0,0,9,6));
    assert(!e->data && !e->nrange && !dcache_keep_on_drop(e) && !g_live);
    /* Full-image bytes survive a successful PUT until exact REPORT ACK. */
    p=efs_buf_alloc(EFS_CHUNK_SIZE);assert(p);p[0]=77;
    struct dcache_init full={.full=1};
    assert(!dcache_store_owned(1,0,p,EFS_CHUNK_SIZE,&full));
    e=entry(0);e->dirty=0;e->on_dirty=0;e->object_gen=12;e->object_seq=20;
    dcache_flush_keep(1,0,p,EFS_CHUNK_SIZE,1,20);
    assert(e->data==p&&e->data[0]==77&&e->pin_held&&dcache_keep_on_drop(e));
    e->stalled=1;
    dcache_drop_locked(1,0,0);assert(e->data==p); /* even explicit cache drop */
    e->stalled=0;
    assert(!dcache_note_committed(1,0,12,12,19));assert(e->data==p);
    e->writer=efs_writer_state_alloc(EFS_CHUNK_SIZE);assert(e->writer);
    e->writer->ranges.bytes.count=1;
    assert(dcache_note_committed(1,0,12,12,20));
    assert(e->data==p && e->writer && dcache_keep_on_drop(e));
    e->writer->ranges.bytes.count=0;
    assert(efs_writer_state_free(e->writer)==EFS_OK);e->writer=NULL;
    assert(dcache_note_committed(1,0,12,12,20));
    assert(!e->data&&!g_live);
    /* Failed PUT re-dirties the retained body; caller frees its snapshot. */
    p=efs_buf_alloc(EFS_CHUNK_SIZE); assert(p); p[0]=44;
    assert(!dcache_store_owned(1,0,p,EFS_CHUNK_SIZE,&in));
    e=entry(0);e->dirty=0;e->on_dirty=0;dirty_bytes-=EFS_CHUNK_SIZE;
    uint8_t *failed_copy=efs_buf_alloc(EFS_CHUNK_SIZE);assert(failed_copy);
    memcpy(failed_copy,p,EFS_CHUNK_SIZE);
    dcache_flush_keep(1,0,failed_copy,EFS_CHUNK_SIZE,0,7);
    assert(e->data==p && e->dirty && e->data[0]==44);
    assert(g_live==2*EFS_CHUNK_SIZE);
    efs_buf_free(failed_copy,EFS_CHUNK_SIZE);
    assert(g_live==EFS_CHUNK_SIZE);
    dcache_drop_locked(1,0,0);
    assert(!g_live);
    /* A full pipe fails while the next chunk already owns a clean snapshot.
     * Its retained body must be re-dirtied even though it never entered PUT. */
    struct flush_pipe failed_pipe;flush_pipe_init(&failed_pipe);
    pipe_windows_closed=0;
    uint64_t pipe_dirty_before=dirty_bytes;
    for(uint32_t j=0;j<=EFS_WRITE_PIPELINE;j++) {
        uint32_t ci=300+j;
        uint8_t *body=efs_buf_alloc(EFS_CHUNK_SIZE);assert(body);body[0]=91;
        assert(!dcache_store_owned(1,ci,body,EFS_CHUNK_SIZE,&in));
        struct dcache_ent *incoming=entry(ci);
        incoming->dirty=0;incoming->on_dirty=0;dirty_bytes-=EFS_CHUNK_SIZE;
        uint8_t *snapshot=efs_buf_alloc(EFS_CHUNK_SIZE);assert(snapshot);
        memcpy(snapshot,body,EFS_CHUNK_SIZE);
        int rc=flush_pipe_add(&failed_pipe,1,ci,snapshot,EFS_CHUNK_SIZE,0,7,0,0,NULL);
        assert(rc==(j==EFS_WRITE_PIPELINE?EFS_ERR_IO:EFS_OK));
    }
    assert(failed_pipe.n==0 && pipe_windows_closed==EFS_WRITE_PIPELINE+1);
    assert(dirty_bytes==pipe_dirty_before+(EFS_WRITE_PIPELINE+1)*EFS_CHUNK_SIZE);
    for(uint32_t j=0;j<=EFS_WRITE_PIPELINE;j++) {
        uint32_t ci=300+j;struct dcache_ent *retained=entry(ci);
        assert(retained->dirty && retained->on_dirty && retained->data[0]==91);
        dcache_drop_locked(1,ci,0);
    }
    assert(dirty_bytes==pipe_dirty_before && !g_live);
    /* Typed ownership alone protects against legacy drop/replacement/reuse. */
    uint32_t owned_slot=dcache_slot(1,0);
    struct dcache_ent *owned=&g_dcache.e[owned_slot];
    owned->ino=1;owned->ci=0;
    owned->writer=efs_writer_state_alloc(EFS_CHUNK_SIZE);assert(owned->writer);
    owned->writer->ranges.bytes.count=1;
    assert(dcache_keep_on_drop(owned) && !dcache_metadata_idle(owned));
    dcache_drop_locked(1,0,0);assert(owned->writer && owned->ino==1);
    p=efs_buf_alloc(EFS_CHUNK_SIZE);assert(p);
    assert(dcache_take(owned,1,0,p,EFS_CHUNK_SIZE)==EFS_ERR_BUSY);
    assert(!owned->data);efs_buf_free(p,EFS_CHUNK_SIZE);
    owned->writer->ranges.bytes.count=0;owned->writer->has_publication=1;
    dcache_drop_locked(1,0,0);assert(owned->writer);
    owned->writer->has_publication=0;
    dcache_drop_locked(1,0,0);assert(!owned->writer && !g_metadata);
    struct dcache_ent *idle=efs_buf_metadata_alloc(sizeof(*idle));assert(idle);
    idle->writer=efs_writer_state_alloc(EFS_CHUNK_SIZE);assert(idle->writer);
    owned->next=idle;
    assert(dcache_chain_reuse(owned_slot)==idle && !idle->writer);
    assert(g_metadata==sizeof(*idle));
    idle->writer=efs_writer_state_alloc(EFS_CHUNK_SIZE);assert(idle->writer);
    efs_dcache_trim_metadata();assert(!owned->next && !g_metadata);
    /* Binding must not invent authority for previously accepted bytes. */
    owned->ino=1;owned->ci=0;owned->data=efs_buf_alloc(EFS_CHUNK_SIZE);
    owned->len=EFS_CHUNK_SIZE;assert(owned->data);owned->data[0]=77;
    struct efs_msg_inode_writer_view_reply view={.ino=1,.generation=7};
    owned->dirty=1;
    assert(efs_dcache_bind_writer(1,7,0,&view)==EFS_ERR_BUSY && !owned->writer);
    owned->dirty=0;owned->pin_held=1;
    assert(efs_dcache_bind_writer(1,7,0,&view)==EFS_ERR_BUSY && !owned->writer);
    owned->pin_held=0;view.generation=8;
    assert(efs_dcache_bind_writer(1,7,0,&view)!=EFS_OK && !owned->writer);
    view.generation=7;
    assert(efs_dcache_bind_writer(1,7,0,&view)==EFS_OK);
    assert(owned->writer->ranges.ino==1 && owned->writer->ranges.generation==7);
    assert(owned->data[0]==77 && !owned->dirty);
    owned->writer->ranges.bytes.chunk_size*=2;
    struct efs_writer_state bad_geometry=*owned->writer;
    assert(efs_dcache_bind_writer(1,7,0,&view)==EFS_ERR_INVAL);
    assert(!memcmp(owned->writer,&bad_geometry,sizeof(bad_geometry)) && owned->data[0]==77);
    owned->writer->ranges.bytes.chunk_size/=2;
    uint64_t charged_metadata=g_metadata;
    assert(efs_dcache_bind_writer(1,7,0,&view)==EFS_OK && g_metadata==charged_metadata);
    view.authority_epoch=1;view.history.count=1;
    view.history.entries[0]=(struct efs_content_fence){1,100};
    assert(efs_dcache_bind_writer(1,7,0,&view)==EFS_OK);
    view.authority_epoch=0;view.history.count=0;
    assert(efs_dcache_bind_writer(1,7,0,&view)==EFS_ERR_STALE);
    view.authority_epoch=1;view.history.count=1;
    view.history.entries[0]=(struct efs_content_fence){1,100};
    struct efs_msg_lane_writer_view_reply cached_view={.view=view,.chunk_size=owned->len};
    uint8_t patch[4]={1,2,3,4};
    uint8_t original_byte=owned->data[10];
    assert(efs_dcache_write_lane(1,8,0,&cached_view,10,patch,4)!=EFS_OK);
    assert(!owned->dirty && owned->data[10]==original_byte);
    assert(efs_dcache_write_lane(1,7,0,&cached_view,10,patch,4)==EFS_OK);
    assert(owned->dirty && owned->pin_held && !memcmp(owned->data+10,patch,4));
    uint8_t *cached_copy=malloc(owned->len);assert(cached_copy);
    struct efs_writer_plan cached_plan;
    struct efs_msg_inode_getchunks_reply *cached_base=calloc(1,sizeof(*cached_base));assert(cached_base);
    cached_base->ino=1;cached_base->generation=7;cached_base->authority_epoch=1;
    assert(efs_dcache_snapshot_lane(1,7,0,&cached_view,cached_base,&cached_plan,cached_copy,owned->len)==EFS_OK);
    assert(cached_copy[0]==0 && !memcmp(cached_copy+10,patch,4));
    assert(efs_dcache_snapshot_publication(1,7,0,&cached_view,cached_base,&cached_plan,cached_copy,owned->len)==EFS_OK);
    uint64_t sequence=cached_plan.cache_sequence;assert(sequence);
    assert(efs_dcache_begin_publication(1,8,0,&cached_plan,10)==EFS_ERR_INVAL);
    assert(efs_dcache_begin_publication(1,7,0,&cached_plan,10)==EFS_OK);
    struct efs_chunk_rec uploaded={.ino=1,.chunk_generation=10},report;
    for(unsigned i=0;i<EFS_NUM_FRAGMENTS;i++) uploaded.nodes[i]=i+1;
    assert(efs_dcache_complete_publication(1,7,0,10,sequence,EFS_OK)==EFS_ERR_BUSY);
    assert(efs_dcache_finish_publication(1,7,0,10,sequence,EFS_OK,&uploaded)==EFS_OK);
    assert(efs_dcache_publication_report(1,7,0,10,sequence,&report)==EFS_OK);
    assert(report.file_generation==7 && report.publish_flags==3 && report.base_gen==0);
    assert(efs_dcache_publication_report(1,7,0,10,sequence,(void *)owned->data)==EFS_ERR_INVAL);
    assert(efs_dcache_complete_publication(1,7,0,10,sequence,EFS_ERR_STALE)==EFS_ERR_STALE);
    assert(owned->writer->has_publication && owned->dirty && owned->pin_held);

    assert(efs_dcache_snapshot_lane(1,7,0,&cached_view,cached_base,&cached_plan,cached_copy,owned->len)==EFS_ERR_BUSY);
    struct efs_msg_publication request;
    assert(efs_dcache_publication_request(1,7,0,10,sequence,100,&request)==EFS_OK);
    assert(request.size==100);
    assert(efs_dcache_publication_request(1,7,0,10,sequence,999,&request)==EFS_OK && request.size==100);
    assert(efs_dcache_publication_request(1,7,0,10,sequence,999,(void *)owned->data)==EFS_ERR_INVAL);
    struct efs_msg_publication_reply outcome={0};
    assert(efs_dcache_publication_result(1,7,0,10,sequence,&outcome)==EFS_ERR_BUSY);
    assert(owned->writer->has_publication && owned->dirty && owned->pin_held);
    outcome.state=EFS_PUBLICATION_PENDING;outcome.verdict=EFS_META_PUBLICATION_PENDING;
    memcpy(outcome.digest,owned->writer->publication.result_digest,EFS_HASH_SIZE);
    assert(efs_dcache_publication_result(1,7,0,10,sequence,&outcome)==EFS_ERR_BUSY);
    assert(owned->writer->has_publication && owned->dirty && owned->pin_held && owned->data);
    outcome.state=EFS_PUBLICATION_APPLY_PENDING;outcome.verdict=EFS_META_PUBLICATION_PENDING;
    outcome.receipt_index=55;outcome.receipt_term=8;
    assert(efs_dcache_publication_result(1,7,0,10,sequence,&outcome)==EFS_ERR_BUSY);
    assert(owned->writer->has_publication && owned->dirty && owned->pin_held && owned->data);
    outcome.state=EFS_PUBLICATION_RETIRED;outcome.verdict=EFS_META_PUBLICATION_RETIRED;
    memcpy(outcome.digest,owned->writer->publication.result_digest,EFS_HASH_SIZE);
    assert(efs_dcache_publication_result(1,7,0,10,sequence,&outcome)==EFS_ERR_BUSY);
    assert(owned->writer->has_publication && owned->dirty && owned->pin_held && owned->data);
    memset(outcome.digest,0,sizeof(outcome.digest));
    outcome.state=EFS_PUBLICATION_REJECTED;outcome.verdict=EFS_ERR_STALE;
    assert(efs_dcache_publication_result(1,7,0,10,sequence,&outcome)==EFS_ERR_STALE);
    assert(owned->writer->has_publication && owned->dirty && owned->pin_held);
    memcpy(outcome.digest,owned->writer->publication.result_digest,EFS_HASH_SIZE);
    assert(efs_dcache_publication_result(1,7,0,10,sequence,&outcome)==EFS_ERR_STALE);
    assert(!owned->writer->has_publication && owned->dirty && owned->pin_held && owned->data);
    assert(owned->writer->ranges.bytes.count && !owned->object_gen);
    assert(efs_dcache_snapshot_publication(1,7,0,&cached_view,cached_base,&cached_plan,cached_copy,owned->len)==EFS_OK);
    sequence=cached_plan.cache_sequence;
    struct efs_opid put_identity;assert(!efs_client_publication_id(sequence,&put_identity));
    uint64_t ticket_object=efs_meta_candidate_gen(put_identity.client_uuid,1,sequence,0,0);
    uploaded.chunk_generation=ticket_object;uploaded.publish_flags=EFS_CHUNK_REC_F_FRESH_OBJECT;
    assert(efs_dcache_begin_publication(1,7,0,&cached_plan,ticket_object)==EFS_OK);
    assert(efs_dcache_finish_publication(1,7,0,ticket_object,sequence,EFS_OK,&uploaded)==EFS_OK);
    assert(!efs_dcache_publication_report(1,7,0,ticket_object,sequence,&report));
    struct efs_meta_pub ticket_pub;assert(!efs_publication_from_rec(&report,101,&ticket_pub));
    ticket_pub.publication_id=put_identity;ticket_pub.put_id=put_identity;
    ticket_pub.put_fragment_len=EFS_MIN_CHUNK_SIZE/2;ticket_pub.put_member_mask=7;
    struct efs_put_ticket ticket;assert(!efs_put_ticket_from_pub(&ticket_pub,&ticket));
    assert(!efs_dcache_publication_request_ticketed(1,7,0,ticket_object,sequence,101,&ticket,&request));
    assert(request.ticketed && request.size==101 && request.put_id.seq==sequence);
    uint8_t bound_digest[EFS_HASH_SIZE];memcpy(bound_digest,owned->writer->publication.result_digest,EFS_HASH_SIZE);
    ticket.body_digest[0]^=1;
    assert(efs_dcache_publication_request_ticketed(1,7,0,ticket_object,sequence,999,&ticket,&request)==EFS_ERR_STALE);
    assert(efs_dcache_publication_request(1,7,0,ticket_object,sequence,999,&request)==EFS_ERR_STALE);
    assert(!memcmp(bound_digest,owned->writer->publication.result_digest,EFS_HASH_SIZE));
    ticket.body_digest[0]^=1;
    assert(!efs_dcache_publication_request_ticketed(1,7,0,ticket_object,sequence,999,&ticket,&request) && request.size==101);
    outcome.state=EFS_PUBLICATION_COMMITTED;outcome.verdict=EFS_OK;
    memcpy(outcome.digest,owned->writer->publication.result_digest,EFS_HASH_SIZE);
    assert(efs_dcache_publication_result(1,7,0,ticket_object,sequence,&outcome)==EFS_OK);
    assert(!owned->dirty && !owned->pin_held && !owned->data);
    free(cached_base);free(cached_copy);

    dcache_drop_locked(1,0,0);assert(!owned->writer && !g_metadata && !g_live);
    /* The actual retained publication adapter must not strip a packed mapping
     * or reconstruct its retry from mutable cache placement. */
    cached_view=(struct efs_msg_lane_writer_view_reply){.view={.ino=777,.generation=7},
        .chunk_size=EFS_MIN_CHUNK_SIZE};
    assert(!efs_dcache_admit_lane_write(777,7,0,&cached_view,0,patch,4));
    struct dcache_ent *packed_entry=dcache_find_meta(dcache_slot(777,0),777,0);
    struct efs_msg_inode_getchunks_reply *packed_base=calloc(1,sizeof(*packed_base));assert(packed_base);
    packed_base->ino=777;packed_base->generation=7;
    uint8_t packed_image[EFS_MIN_CHUNK_SIZE];struct efs_writer_plan packed_plan;
    assert(!efs_dcache_snapshot_publication(777,7,0,&cached_view,packed_base,
        &packed_plan,packed_image,sizeof(packed_image)));
    sequence=packed_plan.cache_sequence;
    struct efs_chunk_rec packed_put={.ino=777,.chunk_generation=77,
        .publish_flags=EFS_CHUNK_REC_F_FRESH_OBJECT|EFS_CHUNK_REC_F_PACKED_BASE};
    for(unsigned i=0;i<3;i++)packed_put.nodes[i]=i+1;
    packed_put.packed=(struct efs_pack_ref){.ino=777,.inode_generation=7,.record_generation=77,
        .segment_id=9,.segment_generation=10,.geometry=8192,.offset=4096,.len=4,.publication_group=2};
    packed_put.packed.digest[0]=1;
    assert(!efs_dcache_begin_publication(777,7,0,&packed_plan,77));
    packed_put.packed.content_epoch=1;
    assert(efs_dcache_finish_publication(777,7,0,77,sequence,EFS_OK,&packed_put)==EFS_ERR_INVAL);
    assert(!packed_entry->writer->publication_ready && packed_entry->dirty);
    packed_put.packed.content_epoch=0;
    struct efs_chunk_rec good_pack_put=packed_put;
    for(unsigned bad=0;bad<6;bad++) {
        packed_put=good_pack_put;
        if(bad==0)packed_put.packed.inode_generation++;
        if(bad==1)packed_put.packed.record_generation++;
        if(bad==2)packed_put.packed.len++;
        if(bad==3)packed_put.packed.offset=8191;
        if(bad==4)packed_put.publish_flags &= ~EFS_CHUNK_REC_F_FRESH_OBJECT;
        if(bad==5)packed_put.publish_flags |= 16u;
        assert(efs_dcache_finish_publication(777,7,0,77,sequence,EFS_OK,&packed_put)==EFS_ERR_INVAL);
        assert(!packed_entry->writer->publication_ready && packed_entry->dirty &&
            packed_entry->writer->ranges.bytes.count && !memcmp(packed_entry->data,patch,4));
    }
    packed_put=good_pack_put;
    assert(!efs_dcache_finish_publication(777,7,0,77,sequence,EFS_OK,&packed_put));
    memset(packed_entry->object_nodes,0,sizeof(packed_entry->object_nodes));
    packed_entry->object_publish_flags=0;
    assert(!efs_dcache_publication_report(777,7,0,77,sequence,&report));
    assert(report.publish_flags==15 && report.nodes[0]==1 &&
        !memcmp(&report.packed,&packed_put.packed,sizeof(report.packed)));
    assert(!efs_dcache_publication_request(777,7,0,77,sequence,4,&request));
    struct efs_msg_publication immutable_pack_request=request;
    packed_put.packed.segment_id++;
    assert(efs_dcache_finish_publication(777,7,0,77,sequence,EFS_OK,&packed_put)==EFS_ERR_BUSY);
    assert(!efs_dcache_publication_request(777,7,0,77,sequence,999,&request));
    assert(!memcmp(&request,&immutable_pack_request,sizeof(request)));
    /* Exactly rejected packed publication releases its old token, while the
     * accepted body, sparse ownership and cache charge remain for rebase. */
    outcome=(struct efs_msg_publication_reply){.state=EFS_PUBLICATION_REJECTED,.verdict=EFS_ERR_STALE};
    memcpy(outcome.digest,packed_entry->writer->publication.result_digest,EFS_HASH_SIZE);
    int packed_consumed=0;outcome.digest[0]^=1;
    assert(efs_dcache_consume_publication(777,7,0,77,sequence,&outcome,&packed_consumed)==EFS_ERR_STALE&&!packed_consumed);
    assert(packed_entry->writer->has_publication&&packed_entry->dirty&&packed_entry->pin_held);
    outcome.digest[0]^=1;
    assert(efs_dcache_consume_publication(777,7,0,77,sequence,&outcome,&packed_consumed)==EFS_ERR_STALE&&packed_consumed);
    assert(!packed_entry->writer->has_publication&&packed_entry->dirty&&packed_entry->pin_held&&
        packed_entry->writer->ranges.bytes.count&&!memcmp(packed_entry->data,patch,4));
    assert(!efs_dcache_snapshot_publication(777,7,0,&cached_view,packed_base,
        &packed_plan,packed_image,sizeof(packed_image))&&!memcmp(packed_image,patch,4));
    sequence=packed_plan.cache_sequence;
    struct efs_chunk_rec rebased_put={.ino=777,.chunk_generation=78,
        .publish_flags=EFS_CHUNK_REC_F_FRESH_OBJECT};
    memcpy(rebased_put.nodes,good_pack_put.nodes,sizeof(rebased_put.nodes));
    assert(!efs_dcache_begin_publication(777,7,0,&packed_plan,78));
    assert(!efs_dcache_finish_publication(777,7,0,78,sequence,EFS_OK,&rebased_put));
    assert(!efs_dcache_publication_request(777,7,0,78,sequence,4,&request));
    assert(!(request.rec.publish_flags&EFS_CHUNK_REC_F_PACKED_BASE));
    assert(memcmp(&request.id,&immutable_pack_request.id,sizeof(request.id)));
    outcome=(struct efs_msg_publication_reply){.state=EFS_PUBLICATION_COMMITTED};
    memcpy(outcome.digest,packed_entry->writer->publication.result_digest,EFS_HASH_SIZE);
    assert(!efs_dcache_publication_result(777,7,0,78,sequence,&outcome));
    assert(!packed_entry->dirty && !packed_entry->pin_held && !packed_entry->data);
    free(packed_base);dcache_drop_locked(777,0,0);assert(!g_metadata && !g_live);
    /* Fresh typed admission reserves exact sparse ownership; gaps never become
     * readable just because the body exists. Failure cannot adopt legacy bytes. */
    cached_view=(struct efs_msg_lane_writer_view_reply){.view={.ino=555,.generation=7,.chunk_index=0}};
    cached_view.chunk_size=EFS_MIN_CHUNK_SIZE;
    assert(efs_dcache_admit_lane_write(555,7,0,&cached_view,10,patch,4)==EFS_OK);
    struct dcache_ent *fresh=dcache_find_meta(dcache_slot(555,0),555,0);
    assert(fresh && fresh->writer && fresh->dirty && fresh->pin_held && !fresh->have_base);
    assert(dcache_range_covered(fresh,10,4) && !dcache_range_covered(fresh,9,5));
    assert(efs_dcache_admit_lane_write(555,7,0,&cached_view,20,patch,4)==EFS_OK);
    assert(!dcache_range_covered(fresh,10,14));
    assert(efs_dcache_admit_lane_write(555,8,0,&cached_view,10,patch,4)!=EFS_OK);
    struct efs_msg_inode_getchunks_reply *read_base=calloc(1,sizeof(*read_base));assert(read_base);
    read_base->ino=555;read_base->generation=7;
    uint8_t read_image[EFS_MIN_CHUNK_SIZE];memset(read_image,0x33,sizeof(read_image));
    assert(efs_dcache_overlay_lane(555,7,0,&cached_view,read_base,read_image,sizeof(read_image))==EFS_OK);
    assert(!memcmp(read_image+10,patch,4) && read_image[14]==0x33 && !memcmp(read_image+20,patch,4));
    cached_view.view.authority_epoch=1;cached_view.view.history.count=1;
    cached_view.view.history.entries[0]=(struct efs_content_fence){1,12};read_base->authority_epoch=1;
    memset(read_image,0x33,sizeof(read_image));
    assert(efs_dcache_overlay_lane(555,7,0,&cached_view,read_base,read_image,sizeof(read_image))==EFS_OK);
    assert(!memcmp(read_image+10,patch,2) && read_image[12]==0x33 && read_image[20]==0x33);
    cached_view.view.oldest_complete_epoch=1;
    memset(read_image,0x33,sizeof(read_image));
    assert(efs_dcache_overlay_lane(555,7,0,&cached_view,read_base,read_image,sizeof(read_image))==EFS_ERR_STALE);
    for(unsigned i=0;i<sizeof(read_image);i++)assert(read_image[i]==0x33);
    cached_view.view.oldest_complete_epoch=0;read_base->generation=8;
    assert(efs_dcache_overlay_lane(555,7,0,&cached_view,read_base,read_image,sizeof(read_image))==EFS_ERR_STALE);
    read_base->generation=7;
    assert(efs_dcache_overlay_lane(555,7,0,&cached_view,read_base,fresh->data,fresh->len)==EFS_ERR_INVAL);
    assert(efs_dcache_retire_clipped(555,7,0,&cached_view,read_base)==EFS_ERR_BUSY);
    cached_view.view.history.entries[0].size=9;
    assert(efs_dcache_retire_clipped(555,7,0,&cached_view,read_base)==EFS_OK);
    assert(!fresh->data && !fresh->dirty && !fresh->pin_held && !efs_writer_state_owned(fresh->writer));
    /* Idle typed body reuse preserves extra accounting for the same FileID;
     * it never transfers that obligation to another generation. */
    fresh->present_extra=1;cached_view.view.generation=8;
    assert(efs_dcache_admit_lane_write(555,8,0,&cached_view,10,patch,4)==EFS_ERR_BUSY);
    assert(!fresh->data && fresh->present_extra);
    cached_view.view.generation=7;
    assert(!efs_dcache_admit_lane_write(555,7,0,&cached_view,10,patch,4));
    assert(fresh->data && fresh->present_extra && efs_writer_state_owned(fresh->writer));
    cached_view.view.authority_epoch=2;cached_view.view.history.count=2;
    cached_view.view.history.entries[1]=(struct efs_content_fence){2,0};read_base->authority_epoch=2;
    assert(!efs_dcache_retire_clipped(555,7,0,&cached_view,read_base));
    assert(!fresh->data && fresh->present_extra);
    free(read_base);
    dcache_drop_locked(555,0,0);assert(!g_metadata && !g_live);
    /* Snapshot outputs must never overwrite body/state through partial aliases. */
    uint32_t cs=EFS_MIN_CHUNK_SIZE;
    uint8_t *body=malloc(cs+1), *copy=malloc(cs); assert(body && copy);
    memset(body,77,cs+1);
    struct efs_writer_state *writer=NULL;
    view=(struct efs_msg_inode_writer_view_reply){.ino=1,.generation=7};
    struct efs_msg_lane_writer_view_reply lane_view={.view=view,.chunk_size=cs*2};
    assert(efs_writer_state_write_lane(&writer,&lane_view,body,cs,0,body,8)==EFS_ERR_PROTO);
    assert(!writer && body[0]==77);
    lane_view.chunk_size=cs;lane_view.reserved=1;
    assert(efs_writer_state_write_lane(&writer,&lane_view,body,cs,0,body,8)!=EFS_OK);
    assert(!writer);lane_view.reserved=0;
    assert(efs_writer_state_write_lane(&writer,&lane_view,body,cs,0,body,8)==EFS_OK);
    struct efs_msg_inode_getchunks_reply *base=calloc(1,sizeof(*base));assert(base);
    base->ino=1;base->generation=7;
    struct efs_writer_plan plan, saved_plan;memset(&plan,0x5a,sizeof(plan));saved_plan=plan;
    assert(efs_writer_state_snapshot(writer,&view,base,body,cs,&plan,body+1)==EFS_ERR_INVAL);
    assert(body[0]==77 && body[cs]==77 && !memcmp(&plan,&saved_plan,sizeof(plan)));
    assert(efs_writer_state_snapshot(writer,&view,base,body,cs,(void *)copy,copy)==EFS_ERR_INVAL);
    assert(efs_writer_state_snapshot(writer,&view,base,body,cs,&plan,(void *)writer)==EFS_ERR_INVAL);
    assert(efs_writer_state_snapshot(writer,&view,base,body,cs,&plan,copy)==EFS_OK);
    assert(copy[0]==77 && copy[8]==0);
    saved_plan=plan;lane_view.chunk_size=cs*2;
    assert(efs_writer_state_snapshot_lane(writer,&lane_view,base,body,cs,&plan,copy)==EFS_ERR_PROTO);
    assert(!memcmp(&plan,&saved_plan,sizeof(plan)) && copy[0]==77);
    lane_view.chunk_size=cs;
    assert(efs_writer_state_snapshot_lane(writer,&lane_view,base,body,cs,&plan,copy)==EFS_OK);
    ++base->generation;
    assert(efs_writer_state_snapshot_lane(writer,&lane_view,base,body,cs,&plan,copy)==EFS_ERR_STALE);
    --base->generation;
    struct efs_writer_plan forged=plan;
    forged.original.ranges[0].len=forged.surviving.ranges[0].len=4;
    assert(efs_writer_state_put(writer,&forged,19,2)==EFS_ERR_STALE);
    assert(!writer->has_publication && writer->ranges.bytes.ranges[0].len==8);
    assert(efs_writer_state_begin_put(writer,&plan,19,2)==EFS_OK);
    assert(efs_writer_state_report(writer,19,2,EFS_OK)==EFS_ERR_BUSY);
    assert(efs_writer_state_finish_put(writer,20,2,EFS_OK)==EFS_ERR_STALE);
    assert(efs_writer_state_finish_put(writer,19,2,EFS_ERR_NET)==EFS_ERR_NET);
    assert(!writer->has_publication && writer->ranges.bytes.count);
    assert(efs_writer_state_begin_put(writer,&plan,19,3)==EFS_OK);
    assert(efs_writer_state_finish_put(writer,19,3,EFS_OK)==EFS_OK);
    assert(efs_writer_state_finish_put(writer,19,3,EFS_ERR_NET)==EFS_ERR_BUSY);
    assert(writer->has_publication && writer->publication_ready);

    assert(efs_writer_state_report(writer,19,3,EFS_OK)==EFS_OK);
    assert(efs_writer_state_write_lane(&writer,&lane_view,body,cs,500,body,4)==EFS_OK);
    assert(efs_writer_state_retire_clipped(writer,&lane_view,base)==EFS_ERR_BUSY);
    lane_view.view.authority_epoch=1;lane_view.view.history.count=1;
    lane_view.view.history.entries[0]=(struct efs_content_fence){1,100};
    base->authority_epoch=1;
    assert(efs_writer_state_retire_clipped(writer,&lane_view,base)==EFS_OK);
    assert(!efs_writer_state_owned(writer) && writer->ranges.observed_epoch==1);
    assert(efs_writer_state_free(writer)==EFS_OK);free(body);free(copy);free(base);
    /* Saturate the real metadata allocator with published, body-less nodes.
     * An unresolved and a stalled node must survive pressure reclamation. */
    struct dcache_ent *head=&g_dcache.e[0];
    while (1) {
        struct dcache_ent *n=efs_buf_metadata_alloc(sizeof(*n));
        if (!n) break;
        n->ino=77;n->object_gen=9;n->object_seq=2;
        n->committed_object=9;n->committed_seq=2;
        n->next=head->next;head->next=n;
    }
    struct dcache_ent *pending=head->next;
    struct dcache_ent *stalled=pending->next;
    pending->committed_seq=1;stalled->stalled=1;
    assert(!g_live && g_metadata>=(8ull<<20)-sizeof(*pending));
    assert(efs_buf_reserve_request(1,4096)==EFS_ERR_BUSY);
    assert(!fuse_write_admit(1));efs_buf_unreserve();
    assert(head->next==pending && pending->next==stalled && !stalled->next);
    assert(g_metadata==2*sizeof(*pending));
    pending->committed_seq=2;stalled->stalled=0;
    efs_dcache_trim_metadata();assert(!g_metadata && !head->next);
    /* Local congestion is retryable, never a filesystem-full verdict. */
    assert(fuse_write_admit(16ull<<20)==-EAGAIN);
    puts("test_client_memory: OK (sparse bound, failed PUT/pressure retention, commit identity)");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-memory-test-') as d:
    p=Path(d)/'test.c';p.write_text(source)
    cmd=[os.environ.get('CC','cc'),'-std=gnu11','-D_GNU_SOURCE','-O1','-g','-pthread',f'-I{root}/include',f'-I{root}/src/common',f'-I{root}/src/client',str(p),str(root/'src/common/common.c'),str(root/'src/kv/kv_key.c'),str(root/'src/sim/opid.c'),'-o',str(p.with_suffix(''))]
    if '--sanitize' in sys.argv: cmd[1:1]=['-fsanitize=address,undefined']
    subprocess.run(cmd,check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)

# Exercise the actual read-cache trim independently: pending reads and
# zero-copy reply pins must not lose their body under writer pressure.
source2 = source[:source.index('#define DCACHE_NR')] + r[r.index('#define RDCACHE_SLOTS'):r.index('static struct rdcache_ent *rdcache_find')] + r"""
int main(void) {
    rdcache_ensure();
    for (int i=0;i<3;i++) {
        struct rdcache_ent *e=&g_rdcache.e[i][0];
        e->data=efs_buf_alloc(EFS_CHUNK_SIZE); assert(e->data);
        e->len=EFS_CHUNK_SIZE;e->ino=1;
    }
    g_rdcache.e[1][0].pins=1;
    g_rdcache.e[2][0].pending=1;
    efs_rdcache_trim();
    assert(!g_rdcache.e[0][0].data);
    assert(g_rdcache.e[1][0].data && g_rdcache.e[2][0].data);
    assert(g_live==2*EFS_CHUNK_SIZE);
    g_rdcache.e[1][0].pins=0;g_rdcache.e[2][0].pending=0;
    efs_rdcache_trim();assert(!g_live);
    puts("test_read_pressure: OK (pin and pending-read ownership)");
}
"""
with tempfile.TemporaryDirectory(prefix='efs-read-pressure-') as d:
    p=Path(d)/'test.c';p.write_text(source2)
    cmd=[os.environ.get('CC','cc'),'-std=gnu11','-D_GNU_SOURCE','-O1','-g','-pthread',f'-I{root}/include',f'-I{root}/src/common',f'-I{root}/src/client',str(p),str(root/'src/common/common.c'),'-o',str(p.with_suffix(''))]
    if '--sanitize' in sys.argv: cmd[1:1]=['-fsanitize=address,undefined']
    subprocess.run(cmd,check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
