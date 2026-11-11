#!/usr/bin/env python3
"""Every exit of a flusher closes the PUT window it opened.

The flusher functions are cut out of src/client/write.c and compiled with the
REAL src/client/put_window.c; the cache, the PUT and the RPCs are stubs.
put_win_open/put_win_close are counted by wrappers around the real ones.
Usage: test_put_window_callers.py [scenario ...]   (default: all)
Scenarios: ok fail retired drain nomem fetch fetch_retired
After each: put_win_wait(ino, 0) == 0 (no window left) and opens == closes.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parent))
from sanitize_env import cflags, timeout, wrap  # noqa: E402

root = Path(__file__).resolve().parents[1]
write = Path(os.environ.get('WRITER_TEST_SOURCE', root / 'src/client/write.c')).read_text()
SCENARIOS = ['ok', 'fail', 'retired', 'drain', 'nomem', 'fetch', 'fetch_retired']


def function(name):
    m = re.search(r'^static [^\n;]*\b' + name + r'\([^;]+?\)\n\{', write, re.M)
    if not m:
        raise RuntimeError(name)
    return write[m.start():write.index('\n}', m.end()) + 2]


def block(start):
    a = write.index(start)
    return write[a:write.index('\n};', a) + 3]


source = r'''
#include "client_internal.h"
#include "put_window.h"
#include "efs/writer_state.h"
#include "efs/put_ticket.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define DCACHE_NR 32
#define DTRACE(...) ((void)0)
#define CS EFS_MIN_CHUNK_SIZE
''' + block('struct dcache_ent {') + '\n' + block('struct chunk_put_job {') + r'''
static struct { struct dcache_ent e[1]; } g_dcache;
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER, io=PTHREAD_MUTEX_INITIALIZER;
/* counted wrappers around the real put_window.c */
static unsigned opens, closes;
static void t_open(efs_ino_t i) { opens++; put_win_open(i); }
static void t_close(efs_ino_t i) { closes++; put_win_close(i); }
#define put_win_open t_open
#define put_win_close t_close
uint64_t efs_client_rpc_deadline_ms(void) { return 0; }
static int fetch_error, no_row, put_rc, job0_rc, gone, alloc_fail_at, allocs;
static int64_t dirty_bytes;
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
static void dcache_install_image(struct dcache_ent *e,uint8_t *b,uint32_t n,uint64_t q) { (void)q;memcpy(e->data,b,n); }
static int dcache_chain_has(uint32_t s,const struct dcache_ent *e) { (void)s;return e==&g_dcache.e[0]; }
static int dcache_retire_gone(efs_ino_t ino,uint32_t ci,int rc) { (void)ino;(void)ci;return gone&&rc!=EFS_OK; }
void *efs_buf_alloc(uint32_t n) { return ++allocs==alloc_fail_at?NULL:malloc(n); }
void efs_buf_free(void *p,uint32_t n) { (void)n;free(p); }
int efs_client_pull_chunks_range_absent(efs_ino_t ino,uint32_t lo,uint32_t hi,uint8_t *absent) { (void)ino;(void)lo;(void)hi;*absent=no_row;return EFS_OK; }
int efs_client_fetch_published_chunk_obs(efs_ino_t ino,uint32_t ci,uint8_t *buf,uint32_t len,struct efs_chunk_entry *obs,int *have) {
    (void)ino;(void)ci;if(fetch_error)return fetch_error;
    memset(obs,0,sizeof(*obs));memset(buf,0,len);*have=0;return EFS_OK;
}
static void span_of(efs_ino_t ino,uint32_t ci,uint64_t bg,uint8_t nr,const uint32_t *off,const uint32_t *len,uint32_t cs,uint32_t *o,uint32_t *l) {
    (void)ino;(void)ci;(void)bg;(void)nr;(void)off;(void)len;(void)cs;*o=*l=0;
}
static int dcache_put_now(efs_ino_t ino,uint32_t ci,const uint8_t *body,uint32_t len,uint64_t bg,uint64_t seq,uint32_t off,uint32_t span,const struct efs_chunk_entry *obs) {
    (void)ino;(void)ci;(void)body;(void)len;(void)bg;(void)seq;(void)off;(void)span;(void)obs;return put_rc;
}
static int put_pool_run(struct chunk_put_job *jobs,uint32_t batch) {
    for(uint32_t i=0;i<batch;i++)jobs[i].rc=(i==0&&job0_rc)?job0_rc:put_rc;
    return EFS_OK;
}
'''
# struct flush_pipe sits between its functions; assemble in source order
source += function('dcache_full_overwrite') + '\n' + function('dcache_fetch_merge_base') + '\n'
source += function('dcache_steal_dirty_budgeted') + '\n' + function('dcache_flush_keep') + '\n'
source += function('dcache_flush_slot_inner_budgeted') + '\n' + block('struct flush_pipe {') + '\n'
source += function('flush_pipe_init') + '\n' + function('flush_pipe_drain') + '\n' + function('flush_pipe_add') + r'''
static void reset(void) {
    struct dcache_ent *e=&g_dcache.e[0];free(e->data);
    *e=(struct dcache_ent){.ino=1,.ci=0,.len=CS,.have_base=1,.dirty=1,.on_dirty=1,
      .pin_held=1,.base_gen=7,.object_gen=7,.nrange=2};
    e->data=malloc(CS);assert(e->data);memset(e->data,'O',CS);
    e->roff[0]=20;e->rlen[0]=4;e->roff[1]=500;e->rlen[1]=4;
    fetch_error=no_row=put_rc=job0_rc=gone=alloc_fail_at=allocs=0;dirty_bytes=CS;
}
/* steal one snapshot and queue it; returns steal's rc, or add's when stolen */
static int steal_add(struct flush_pipe *p,int *added) {
    uint8_t *copy;uint32_t len,doff,dlen;uint64_t bg,seq;struct efs_chunk_entry obs;int have;
    g_dcache.e[0].dirty=1;
    int rc=dcache_steal_dirty_budgeted(1,0,&copy,&len,&bg,&seq,&doff,&dlen,&obs,&have);
    *added=0;if(rc!=1)return rc;
    *added=1;return flush_pipe_add(p,1,0,copy,len,bg,seq,doff,dlen,have?&obs:NULL);
}
static void check(const char *what) {
    /* no window left, and every open was closed exactly once */
    if(put_win_wait(1,0)!=0||opens!=closes||!opens) {
        fprintf(stderr,"FAIL %s: opens=%u closes=%u wait=%d\n",what,opens,closes,put_win_wait(1,0));
        exit(1);
    }
    assert(efs_dcache_put_win_wait(1)==0);
    opens=closes=0;
}
static void pipe_one(int rc0) { /* steal, queue, drain */
    struct flush_pipe p;int added;flush_pipe_init(&p);
    assert(steal_add(&p,&added)==EFS_OK&&added);
    (void)rc0;(void)flush_pipe_drain(&p);
}
int main(int argc,char **argv) {
    const char *sc=argc>1?argv[1]:"";
    reset();
    if(!strcmp(sc,"ok")) {
        pipe_one(0);check("ok pipe");
        reset();assert(dcache_flush_slot_inner_budgeted(0,1,1)==EFS_OK);check("ok direct");
    } else if(!strcmp(sc,"fail")) {
        put_rc=EFS_ERR_IO;pipe_one(0);check("fail pipe");
        assert(g_dcache.e[0].dirty);
        reset();put_rc=EFS_ERR_IO;assert(dcache_flush_slot_inner_budgeted(0,1,1)==EFS_ERR_IO);
        check("fail direct");assert(g_dcache.e[0].dirty);
    } else if(!strcmp(sc,"retired")) {
        put_rc=EFS_ERR_NOT_FOUND;gone=1;pipe_one(0);check("retired pipe");
        reset();put_rc=EFS_ERR_NOT_FOUND;gone=1;
        (void)dcache_flush_slot_inner_budgeted(0,1,1);check("retired direct");
    } else if(!strcmp(sc,"drain")) {
        /* a full pipe whose first job failed: the drain inside flush_pipe_add fails */
        struct flush_pipe p;int added;flush_pipe_init(&p);job0_rc=EFS_ERR_IO;
        for(int i=0;i<EFS_WRITE_PIPELINE;i++)assert(steal_add(&p,&added)==EFS_OK&&added);
        assert(steal_add(&p,&added)==EFS_ERR_IO&&added);
        assert(p.n==0);check("drain full pipe");
    } else if(!strcmp(sc,"nomem")) {
        uint8_t *c;uint32_t l,d,dl;uint64_t b,q;struct efs_chunk_entry o;int h;
        alloc_fail_at=2; /* the base buffer, after the copy */
        assert(dcache_steal_dirty_budgeted(1,0,&c,&l,&b,&q,&d,&dl,&o,&h)==EFS_ERR_NOMEM);
        check("nomem steal");assert(g_dcache.e[0].dirty);
        reset();alloc_fail_at=2;
        assert(dcache_flush_slot_inner_budgeted(0,1,1)==EFS_ERR_NOMEM);check("nomem direct");
    } else if(!strcmp(sc,"fetch")) {
        uint8_t *c;uint32_t l,d,dl;uint64_t b,q;struct efs_chunk_entry o;int h;
        fetch_error=EFS_ERR_BUSY;
        assert(dcache_steal_dirty_budgeted(1,0,&c,&l,&b,&q,&d,&dl,&o,&h)==EFS_ERR_BUSY);
        check("fetch steal");
        reset();fetch_error=EFS_ERR_BUSY;
        assert(dcache_flush_slot_inner_budgeted(0,1,1)==EFS_ERR_BUSY);check("fetch direct");
    } else if(!strcmp(sc,"fetch_retired")) {
        fetch_error=EFS_ERR_NOT_FOUND;gone=1;
        (void)dcache_flush_slot_inner_budgeted(0,1,1);check("fetch retired direct");
    } else {
        fprintf(stderr,"unknown scenario '%s'\n",sc);return 2;
    }
    free(g_dcache.e[0].data);
    printf("put window callers: %s PASS\n",sc);
    return 0;
}
'''
names = sys.argv[1:] or SCENARIOS
with tempfile.TemporaryDirectory(prefix='efs-put-window-callers-') as directory:
    path = Path(directory) / 'test.c'
    path.write_text(source)
    cmd = shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-D_GNU_SOURCE', '-O1', '-g',
        '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation', '-pthread',
        '-I' + str(root / 'include'), '-I' + str(root / 'src/client')]
    cmd += cflags() + shlex.split(os.environ.get('WRITER_TEST_CFLAGS', ''))
    subprocess.run(cmd + [str(path), str(root / 'src/client/put_window.c'),
                          '-o', str(path.with_suffix(''))], check=True)
    for n in names:
        subprocess.run(wrap() + [str(path.with_suffix('')), n], check=True, timeout=timeout(5))
