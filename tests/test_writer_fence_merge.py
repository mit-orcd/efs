#!/usr/bin/env python3
"""Exercise both production flush paths against unchanged-object fences.
RPC and immutable-byte transport are mocked; cache ownership code is real.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
write = Path(os.environ.get('WRITER_TEST_SOURCE', root / 'src/client/write.c')).read_text()
def function(name):
    m = re.search(r'^static [^\n;]*\b' + name + r'\([^;]+?\)\n\{', write, re.M)
    if not m:
        raise RuntimeError(name)
    return write[m.start():write.index('\n}', m.end()) + 2]
a = write.index('struct dcache_ent {')
entry = write[a:write.index('\n};', a) + 3]
source = r'''
#include "client_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define DCACHE_NR 32
#define DTRACE(...) ((void)0)
#define CS EFS_MIN_CHUNK_SIZE
''' + entry + r'''
static struct { struct dcache_ent e[1]; } g_dcache;
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER, io=PTHREAD_MUTEX_INITIALIZER;
static unsigned pulls, fetches, uploads, installs;
static int pull_error, fetch_error, windows, no_row;
static int64_t dirty_bytes;
static struct efs_chunk_entry sent_obs;
static uint64_t sent_bg;
static uint8_t sent[CS];
static uint32_t dcache_slot(efs_ino_t ino,uint32_t ci) { (void)ino;(void)ci;return 0; }
static pthread_mutex_t *dcache_mu(uint32_t s) { (void)s;return &mu; }
static pthread_mutex_t *dcache_io_mu(uint32_t s) { (void)s;return &io; }
static struct dcache_ent *dcache_find(uint32_t s,efs_ino_t ino,uint32_t ci) {
    (void)s;struct dcache_ent *e=&g_dcache.e[0];return e->ino==ino&&e->ci==ci?e:NULL;
}
static struct dcache_ent *dcache_find_meta(uint32_t s,efs_ino_t ino,uint32_t ci) { return dcache_find(s,ino,ci); }
static uint64_t dcache_seq_next(struct dcache_ent *e) { return ++e->snap_seq; }
static void dcache_dirty_unlink(struct dcache_ent *e,uint32_t s) { (void)s;e->on_dirty=0; }
static void dcache_set_dirty(struct dcache_ent *e,uint32_t s) { (void)s;e->dirty=e->on_dirty=1; }
static void dcache_note_dirty_bytes(int64_t n) { dirty_bytes+=n; }
static void dcache_pin_add(struct dcache_ent *e) { e->pin_held=1; }
static void dcache_pin_release(struct dcache_ent *e) { e->pin_held=0; }
static void put_win_open(efs_ino_t ino) { (void)ino;windows++; }
static void put_win_close(efs_ino_t ino) { (void)ino;windows--; }
static int dcache_chain_has(uint32_t s,const struct dcache_ent *e) { (void)s;return e==&g_dcache.e[0]; }
void *efs_buf_alloc(uint32_t n) { return malloc(n); }
void efs_buf_free(void *p,uint32_t n) { (void)n;free(p); }
int efs_client_pull_chunks_range_absent(efs_ino_t ino,uint32_t lo,uint32_t hi,uint8_t *absent) {
    assert(ino==1&&lo==0&&hi==1);pulls++;*absent=no_row?1:0;return pull_error;
}
int efs_client_fetch_published_chunk_obs(efs_ino_t ino,uint32_t ci,uint8_t *buf,
       uint32_t len,struct efs_chunk_entry *obs,int *have) {
    assert(ino==1&&ci==0&&len==CS&&pulls);fetches++;
    if(fetch_error)return fetch_error;
    memset(obs,0,sizeof(*obs));memset(buf,0,len);*have=!no_row;
    if(!no_row) {
        /* Same generation as cached body, but fence cuts its old tail. */
        obs->generation=7;obs->read_view.count=1;
        obs->read_view.fence_epoch=2;obs->read_view.revision=2;
        obs->read_view.parts[0].len=100;
        memset(buf,'P',100);
    }
    return EFS_OK;
}
static void span_of(efs_ino_t ino,uint32_t ci,uint64_t bg,uint8_t nr,
        const uint32_t *off,const uint32_t *len,uint32_t cs,uint32_t *o,uint32_t *l) {
    (void)ino;(void)ci;(void)bg;(void)nr;(void)off;(void)len;(void)cs;*o=*l=0;
}
static int dcache_put_now(efs_ino_t ino,uint32_t ci,const uint8_t *body,uint32_t len,
        uint64_t bg,uint64_t seq,uint32_t off,uint32_t span,const struct efs_chunk_entry *obs) {
    (void)seq;assert(ino==1&&ci==0&&len==CS&&!off&&!span);uploads++;
    memcpy(sent,body,len);sent_bg=bg;
    memset(&sent_obs,0,sizeof(sent_obs));if(obs)sent_obs=*obs;
    return EFS_OK;
}
static void dcache_install_image(struct dcache_ent *e,uint8_t *body,uint32_t len,uint64_t seq) {
    (void)seq;memcpy(e->data,body,len);installs++;
}
''' + '\n' + function('dcache_full_overwrite') + '\n'
if 'static int dcache_fetch_merge_base(' in write:
    source += function('dcache_fetch_merge_base') + '\n'
else:
    source += r'''
int efs_client_pull_chunks_range(efs_ino_t ino,uint32_t lo,uint32_t hi) {
    uint8_t absent;return efs_client_pull_chunks_range_absent(ino,lo,hi,&absent);
}
static int export_chunk_exists(efs_ino_t ino,uint32_t ci) { (void)ino;(void)ci;return !no_row; }
static int export_chunk_copy(efs_ino_t ino,uint32_t ci,struct efs_chunk_entry *out) {
    (void)ino;(void)ci;memset(out,0,sizeof(*out));out->generation=7;return no_row?-1:0;
}
''' + function('dcache_need_published_merge') + '\n'
source += function('dcache_steal_dirty_budgeted') + '\n'
source += function('dcache_flush_slot_inner_budgeted') + r'''
static void reset(void) {
    struct dcache_ent *e=&g_dcache.e[0];free(e->data);memset(e,0,sizeof(*e));
    *e=(struct dcache_ent){.ino=1,.ci=0,.len=CS,.have_base=1,.dirty=1,
      .on_dirty=1,.pin_held=1,.base_gen=7,.object_gen=7,.nrange=2};
    e->data=malloc(CS);assert(e->data);memset(e->data,'O',CS);
    e->roff[0]=20;e->rlen[0]=4;memset(e->data+20,'A',4);
    e->roff[1]=500;e->rlen[1]=4;memset(e->data+500,'B',4);
    pulls=fetches=uploads=installs=0;pull_error=fetch_error=windows=no_row=0;dirty_bytes=CS;
}
static void verify(const uint8_t *p) {
    for(unsigned i=0;i<CS;i++) {
        unsigned char expected=no_row?0:(i<100?'P':0);
        if(i>=20&&i<24)expected='A';if(i>=500&&i<504)expected='B';
        assert(p[i]==expected);
    }
}
static int steal(uint8_t **copy,uint64_t *bg,struct efs_chunk_entry *obs,int *have) {
    uint32_t len,off,span;uint64_t seq;int drop;
    return dcache_steal_dirty_budgeted(1,0,copy,&len,bg,&seq,&off,&span,&drop,obs,have);
}
int main(void) {
    uint8_t *copy;uint64_t bg;struct efs_chunk_entry obs;int have;
    for(int path=0;path<2;path++) {
        for(int absent=0;absent<2;absent++) {
            reset();no_row=absent;
            if(!path) {
                assert(steal(&copy,&bg,&obs,&have)==1);verify(copy);
                assert(have&&bg==(absent?0:7));
                assert(obs.read_view.fence_epoch==(absent?0:2));free(copy);put_win_close(1);
            } else {
                assert(dcache_flush_slot_inner_budgeted(0,1,1)==EFS_OK);verify(sent);
                assert(sent_bg==(absent?0:7));assert(sent_obs.read_view.fence_epoch==(absent?0:2));
                assert(uploads==1&&installs==1);
            }
            assert(pulls==1&&fetches==(unsigned)!absent&&!windows);
        }
        for(int which=0;which<2;which++) {
            reset();if(which)fetch_error=EFS_ERR_NOMEM;else pull_error=EFS_ERR_BUSY;
            int expected=which?EFS_ERR_NOMEM:EFS_ERR_BUSY;
            int rc=path?dcache_flush_slot_inner_budgeted(0,1,1):steal(&copy,&bg,&obs,&have);
            assert(rc==expected&&!uploads&&!windows&&dirty_bytes==CS);
            assert(g_dcache.e[0].dirty&&g_dcache.e[0].pin_held&&g_dcache.e[0].data[700]=='O');
            assert(pulls==1&&fetches==(unsigned)which);
        }
        reset();g_dcache.e[0].base_gen=EFS_CHUNK_BASE_UNCOND;g_dcache.e[0].nrange=0;
        pull_error=EFS_ERR_BUSY;
        if(path)assert(dcache_flush_slot_inner_budgeted(0,1,1)==EFS_OK);
        else {assert(steal(&copy,&bg,&obs,&have)==1&&!have);free(copy);put_win_close(1);}
        assert(!pulls&&!fetches&&!windows);
    }
    free(g_dcache.e[0].data);
    puts("writer fence merge: pipeline/direct masks, absent CAS, failure retention and full overwrite PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-writer-fence-') as directory:
    path = Path(directory) / 'test.c';path.write_text(source)
    cmd = shlex.split(os.environ.get('CC','cc')) + ['-std=gnu11','-D_GNU_SOURCE','-O1','-g',
        '-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-pthread',
        '-I'+str(root/'include'),'-I'+str(root/'src/client')]
    cmd += shlex.split(os.environ.get('WRITER_TEST_CFLAGS',''))
    subprocess.run(cmd+[str(path),'-o',str(path.with_suffix(''))],check=True)
    subprocess.run([str(path.with_suffix(''))],check=True)
