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
w = (root / 'src/client/write.c').read_text()
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
struct efs_client g_client;
int efs_rdma_zc_region_add(void *p, size_t n) { (void)p; (void)n; return 0; }
#include "bufpool.c"
#include "writer_state.c"
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
static void dcache_account_extra(efs_ino_t ino, uint32_t ci) { (void)ino;(void)ci; }
static void dcache_add_range(struct dcache_ent *e,uint32_t off,uint32_t len) { e->nrange=1;e->roff[0]=off;e->rlen[0]=len; }
void efs_export_present_add(struct efs_export *ex,efs_ino_t ino,int which,int delta) { (void)ex;(void)ino;(void)which;(void)delta; }
'''
for name in ['dcache_slot','dcache_mu','dcache_find','dcache_find_meta','dcache_metadata_idle','dcache_chain_reuse','dcache_keep_on_drop','efs_dcache_bind_writer','dcache_take']:
    source += '\n'+function(w,name)
source += '\n'+function(w,'efs_dcache_trim_metadata')
source += '\n'+block(w, 'struct dcache_init {')
for name in ['dcache_apply_init','dcache_store_owned_locked','dcache_drop_locked','dcache_body_to_rdcache','dcache_install_image','dcache_flush_keep']:
    source += '\n'+function(w,name)
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
'''+function(f,'fuse_write_admit')
source += r'''
static struct dcache_ent *entry(uint32_t ci) { return dcache_find_meta(dcache_slot(1,ci),1,ci); }
int main(void) {
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
    assert(!dcache_flush_keep(1,0,p,EFS_CHUNK_SIZE,1,20,0));
    assert(e->data==p&&e->data[0]==77&&dcache_keep_on_drop(e));
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
    /* Failed full-image PUT restores the stolen body without a new charge. */
    p=efs_buf_alloc(EFS_CHUNK_SIZE); assert(p); p[0]=44;
    assert(!dcache_store_owned(1,0,p,EFS_CHUNK_SIZE,&in));
    e=entry(0);e->data=NULL;e->len=0;e->dirty=0;e->on_dirty=0;
    assert(dcache_flush_keep(1,0,p,EFS_CHUNK_SIZE,0,7,1)==1);
    assert(e->data==p && e->dirty && e->data[0]==44);
    assert(g_live==EFS_CHUNK_SIZE);
    dcache_drop_locked(1,0,0);
    assert(!g_live);
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
    uint64_t charged_metadata=g_metadata;
    assert(efs_dcache_bind_writer(1,7,0,&view)==EFS_OK && g_metadata==charged_metadata);
    view.authority_epoch=1;view.history.count=1;
    view.history.entries[0]=(struct efs_content_fence){1,100};
    assert(efs_dcache_bind_writer(1,7,0,&view)==EFS_OK);
    view.authority_epoch=0;view.history.count=0;
    assert(efs_dcache_bind_writer(1,7,0,&view)==EFS_ERR_STALE);
    dcache_drop_locked(1,0,0);assert(!owned->writer && !g_metadata && !g_live);
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
    cmd=[os.environ.get('CC','cc'),'-std=gnu11','-D_GNU_SOURCE','-O1','-g','-pthread',f'-I{root}/include',f'-I{root}/src/common',f'-I{root}/src/client',str(p),str(root/'src/common/common.c'),'-o',str(p.with_suffix(''))]
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
