#!/usr/bin/env python3
"""Exercise the production inode lease edges, including failed and overlapping opens."""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
text = (root / 'src/client/efs_fuse.c').read_text()
def function(name):
    match = re.search(r'^static [^\n;]*\b' + name + r'\([^;]+?\)\n\{', text, re.M)
    if not match: raise RuntimeError(name)
    return text[match.start():text.index('\n}', match.end()) + 2]
refs = text[text.index('static pthread_mutex_t g_open_mu'):text.index('/* Returns 1 when this is the first open')]
source = r"""
#include "client_internal.h"
#include "efs/session.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
static _Thread_local uint64_t mock_deadline;
uint64_t efs_client_rpc_deadline_ms(void){return mock_deadline;}
void efs_client_rpc_set_deadline_ms(uint64_t d){mock_deadline=d;}
static uint64_t stats_now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
int efs_client_rpc_past_deadline(void){return mock_deadline && stats_now_ms()>=mock_deadline;}
struct efs_client g_client;
struct fuse_file_info { uint64_t fh, lock_owner;unsigned flock_release; };
typedef uint64_t fuse_ino_t;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int holds, drops, fail, block, entered, unblock, fileid_calls;
int efs_client_rpc_getchunks_fileid(efs_export_id_t ex,efs_ino_t ino,uint64_t gen,uint32_t start,struct efs_chunk_rec *recs,uint32_t *count,uint64_t *out) {
 fileid_calls++;(void)ex;(void)ino;(void)gen;(void)start;(void)recs;(void)count;*out=99;return EFS_OK;
}
int efs_client_lease_begin(struct efs_client_lease_request *r,efs_ino_t ino,uint64_t gen,unsigned flags,uint64_t token) {
 assert(!r->active&&gen==99&&!token);*r=(struct efs_client_lease_request){.ino=ino,.generation=gen,.flags=flags,.active=1};return EFS_OK;
}
int efs_client_lease_resume(struct efs_client_lease_request *r,int probe) {
 assert(!probe&&r->active);if(r->complete)return r->result;
 int open=!!(r->flags&EFS_LEASE_EDGE_OPEN);
 pthread_mutex_lock(&gate);
 if(!r->attempted){if(open)++holds;else ++drops;}
 r->attempted=1;
 if(block==(open?1:2)) {
   entered=1;pthread_cond_broadcast(&cv);
   while(!unblock)pthread_cond_wait(&cv,&gate);
 }
 int rc=fail?EFS_ERR_IO:EFS_OK;
 if(!rc){r->complete=1;r->result=rc;}
 pthread_mutex_unlock(&gate);return rc;
}
int efs_client_lease_reset(struct efs_client_lease_request *r) {
 assert(!r->active||r->complete);memset(r,0,sizeof(*r));return EFS_OK;
}
static efs_ino_t efs_file_ino(struct fuse_file_info *fi, efs_ino_t fallback) { return fi->fh ? fi->fh : fallback; }
static void efs_file_close(struct fuse_file_info *fi) { fi->fh = 0; }
static int virt_kind(fuse_ino_t ino) { return (ino & (7ULL << 60)) != 0; }
int efs_client_note_meta_change(int force) { (void)force; return EFS_OK; }
void efs_client_stage_evict_ino(efs_ino_t ino) { (void)ino; }
static void efs_fuse_log_err(const char *where,int rc,efs_ino_t ino,uint64_t offset,size_t size,const char *path){(void)where;(void)rc;(void)ino;(void)offset;(void)size;(void)path;}
static int efs_rc_to_errno(int rc){return rc?-EIO:0;}
int efs_client_rpc_flock(efs_export_id_t ex, efs_ino_t ino, uint32_t op, uint64_t owner) {
    (void)ex; (void)ino; (void)op; (void)owner; return EFS_OK;
}
""" + refs + '\n'.join(function(n) for n in ['efs_open_note', 'efs_open_set_leased', 'efs_open_retry_order', 'efs_close_note', 'efs_open_release_pending_run', 'efs_open_release_pending', 'wb_lock_budget','efs_open_lease_resume_wait','efs_open_acquire_fileid_run','efs_open_acquire_fileid_mode','efs_open_acquire_fileid','efs_open_acquire', 'efs_fuse_release_ino']) + r"""
static void release(void) { struct fuse_file_info fi = {.fh = 42}; assert(!efs_fuse_release_ino(42, &fi)); }
static void *opener(void *arg) { (void)arg; assert(efs_open_acquire(42) == EFS_OK); return NULL; }
static void *closer(void *arg) { (void)arg; release(); return NULL; }
static void wait_entered(void) {
    pthread_mutex_lock(&gate);
    while (!entered) pthread_cond_wait(&cv, &gate);
    pthread_mutex_unlock(&gate);
}
static void resume(void) {
    pthread_mutex_lock(&gate); unblock = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&gate);
}
int main(void) {
    struct fuse_file_info query = {.fh = 1ULL << 60};
    assert(!efs_fuse_release_ino(query.fh, &query));
    assert(!query.fh && !holds && !drops && !g_open_refs);
    assert(efs_open_acquire(42) == EFS_OK);
    assert(efs_open_acquire(42) == EFS_OK && holds == 1);
    release(); assert(drops == 0); release(); assert(drops == 1 && !g_open_refs);
    fail = 1; assert(efs_open_acquire(42) == EFS_ERR_IO && g_open_refs && !g_open_refs->n && g_open_refs->close_pending); fail = 0;
    pthread_t a, b;
    block = 1; entered = unblock = 0;
    assert(!pthread_create(&a, NULL, opener, NULL)); wait_entered();
    assert(pthread_mutex_trylock(open_edge_mu(42)) != 0);
    assert(!pthread_create(&b, NULL, opener, NULL)); resume();
    pthread_join(a, NULL); pthread_join(b, NULL);
    assert(g_open_refs->n == 2 && g_open_refs->leased);
    block = 0; release();
    block = 2; entered = unblock = 0;
    assert(!pthread_create(&a, NULL, closer, NULL)); wait_entered();
    assert(pthread_mutex_trylock(open_edge_mu(42)) != 0);
    assert(!pthread_create(&b, NULL, opener, NULL)); resume();
    pthread_join(a, NULL); pthread_join(b, NULL);
    assert(g_open_refs->n == 1 && g_open_refs->leased);
    block = 0; release(); assert(!g_open_refs);
    int fetched=fileid_calls;
    assert(!efs_open_acquire_fileid(42,99));
    assert(fileid_calls==fetched && g_open_refs->leased);
    release(); assert(!g_open_refs);
    puts("open leases: shared opens, failed HOLD, acquire/release serialization PASS");
}
"""
with tempfile.TemporaryDirectory(prefix='efs-open-lease-') as directory:
    path = Path(directory) / 'test.c'; path.write_text(source)
    command = shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread', '-I' + str(root / 'include'), '-I' + str(root / 'src/client')]
    command += shlex.split(os.environ.get('LEASE_TEST_CFLAGS', ''))
    subprocess.run(command + [str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True)
