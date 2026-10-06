#!/usr/bin/env python3
"""Run production D27 registry, cycle verdict and file-handle helpers."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
w=(root/'src/client/write.c').read_text()
f=(root/'src/client/efs_fuse.c').read_text()
def function(text,name):
    m=re.search(r'^(?:static )?[^\n;]*\b'+name+r'\([^;]+?\)\n\{',text,re.M)
    if not m: raise RuntimeError(name)
    return text[m.start():text.index('\n}',m.end())+2]
registry=w[w.index('#define WB_ERROR_BUCKETS'):w.index('static void dcache_cycle_sent(const struct efs_chunk_rec *r, uint64_t seq);')]
entry=w[w.index('struct dcache_ent {'):w.index('\n};',w.index('struct dcache_ent {'))+3]
handles=f[f.index('#define EFS_FILE_FH_TAG'):f.index('static int virt_kind(')]
source=r'''
#include "efs/protocol.h"
#include "efs/metadata.h"
#include "efs/wb_recovery.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
static int allocation_failure;
static void *test_calloc(size_t n,size_t z) {
    if(allocation_failure) return NULL;
    return calloc(n,z);
}
#define calloc test_calloc
'''+registry+r'''
#define DCACHE_NR 32
#define DTRACE(...) ((void)0)
'''+entry+r'''
static struct dcache_ent slots[4];
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static uint32_t dcache_slot(efs_ino_t ino,uint32_t ci) {(void)ino;return ci%4;}
static pthread_mutex_t *dcache_mu(uint32_t s) {(void)s;return &mu;}
static void dcache_ensure(void) {}
static struct dcache_ent *dcache_find_meta(uint32_t s,efs_ino_t ino,uint32_t ci) {
    return slots[s].ino==ino&&slots[s].ci==ci?&slots[s]:NULL;
}
static void dcache_pin_add(struct dcache_ent *e) {e->pin_held=1;}
static void dcache_pin_release(struct dcache_ent *e) {if(!e->stalled)e->pin_held=0;}
void efs_buf_free(void *p,uint32_t n) {(void)n;free(p);}
static int dcache_body_to_rdcache(struct dcache_ent *e,uint8_t **p,uint32_t *n) {
    (void)e;(void)p;(void)n;return 0;
}
static void dcache_img_to_rdcache(efs_ino_t ino,uint32_t ci,uint8_t *p,uint32_t n) {
    (void)ino;(void)ci;(void)n;free(p);
}
'''+function(w,'dcache_cycle_sent')+'\n'+function(w,'dcache_cycle_verdict')+'\n'+function(w,'dcache_note_committed')+r'''
struct fuse_file_info {uint64_t fh;};
'''+handles+r'''
static void prepare_cycle(uint32_t ci,uint64_t mutation,uint64_t seq) {
    struct dcache_ent *e=&slots[ci];
    e->ino=42;e->ci=ci;e->mutation=mutation;e->object_seq=seq;
    e->object_gen=100+ci;e->pending_seq=seq;e->pending_sent=0;
    e->pending_cycle=(struct efs_wb_cycle_state){.operation=mutation,
        .generation=7,.epoch=2,.spans=1,.valid=1};
}
static void send_cycle(uint32_t ci,uint64_t seq) {
    struct efs_chunk_rec r={.ino=42,.chunk_index=ci};
    dcache_cycle_sent(&r,seq);
}
int main(void) {
    struct fuse_file_info d1={0},d2={0},d3={0};
    assert(!efs_file_open(&d1,42));assert(!efs_file_open(&d2,42));
    assert(d1.fh!=d2.fh&&efs_file_ino(&d1,0)==42);
    struct fuse_file_info virtual_file={.fh=1ULL<<62};
    assert(efs_file_ino(&virtual_file,0)==virtual_file.fh);
    struct fuse_file_info duplicate=d1;
    assert(efs_file_handle(&duplicate)==efs_file_handle(&d1));
    struct efs_chunk_entry ce={.generation=7,.ndelta=1};
    ce.read_view.fence_epoch=2;
    for(unsigned ci=0;ci<2;ci++) {
        for(unsigned i=0;i<8;i++) {
            prepare_cycle(ci,1,10+i);
            assert(!dcache_cycle_verdict(42,ci,&ce,10+i)); /* not sent */
            send_cycle(ci,9+i); /* not the snapshot we actually rebased */
            assert(!dcache_cycle_verdict(42,ci,&ce,10+i));
            send_cycle(ci,10+i);
            assert(dcache_cycle_verdict(42,ci,&ce,10+i)==(i==7));
            assert(!dcache_cycle_verdict(42,ci,&ce,10+i)); /* no double-count */
        }
        assert(slots[ci].stalled&&slots[ci].pin_held);
    }
    assert(efs_wb_inode_stalled(42));assert(!efs_wb_inode_stalled(43));
    assert(efs_file_sync_error(&d1,42,1,EFS_OK)==EFS_ERR_IO);
    assert(efs_file_sync_error(&duplicate,42,1,EFS_OK)==EFS_ERR_IO);
    assert(!efs_file_open(&d3,42)); /* reopened description is also sticky */
    assert(efs_file_sync_error(&d3,42,1,EFS_OK)==EFS_ERR_IO);
    efs_file_close(&d3);
    assert(!dcache_note_committed(42,0,100,100,16)); /* wrong snapshot */
    assert(efs_wb_inode_stalled(42));
    assert(dcache_note_committed(42,0,100,100,17));
    assert(!slots[0].stalled&&!slots[0].pin_held&&slots[1].stalled);
    assert(efs_file_sync_error(&d1,42,1,EFS_OK)==EFS_ERR_IO);
    assert(dcache_note_committed(42,1,101,101,17));
    assert(!efs_wb_inode_stalled(42));
    assert(efs_file_sync_error(&d1,42,1,EFS_OK)==EFS_OK);
    assert(efs_file_sync_error(&d1,42,0,EFS_OK)==EFS_OK);
    assert(efs_file_sync_error(&d2,42,0,EFS_OK)==EFS_ERR_IO);
    assert(efs_file_sync_error(&d2,42,0,EFS_OK)==EFS_OK);
    assert(!efs_file_open(&d3,42));
    assert(efs_file_sync_error(&d3,42,0,EFS_OK)==EFS_OK);
    efs_file_close(&d1);efs_file_close(&d2);efs_file_close(&d3);
    assert(!g_wb_errors[42%WB_ERROR_BUCKETS]);
    for(unsigned i=0;i<1000;i++) {
        prepare_cycle(2,1,100+i);send_cycle(2,100+i);
        ce.generation=8+i;
        assert(!dcache_cycle_verdict(42,2,&ce,100+i));
    }
    assert(!slots[2].stalled&&!efs_wb_inode_stalled(42));
    ce.generation=7;
    prepare_cycle(2,1,2000);send_cycle(2,2000);slots[2].mutation=2;
    assert(!dcache_cycle_verdict(42,2,&ce,2000));
    allocation_failure=1;
    assert(efs_file_open(&d1,42)==-ENOMEM);
    assert(!wb_error_record_add(42));
    assert(!g_wb_errors[42%WB_ERROR_BUCKETS]);
    allocation_failure=0;
    puts("D27 runtime: sent snapshots, contention, mutations, sticky descriptions and partial recovery PASS");
}
'''
source=source.replace('#include <stdint.h>','#include <stdint.h>\n#include <errno.h>')
with tempfile.TemporaryDirectory(prefix='efs-wb-runtime-') as directory:
    path=Path(directory)/'test.c';path.write_text(source)
    command=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include')]
    command+=shlex.split(os.environ.get('WB_RUNTIME_CFLAGS',''))
    subprocess.run(command+[str(path),'-o',str(path.with_suffix(''))],check=True)
    subprocess.run([str(path.with_suffix(''))],check=True)
