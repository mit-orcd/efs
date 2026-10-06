#!/usr/bin/env python3
"""Compile production replay/PUT code with deterministic fragment/RPC stubs.
Keep byte images separate from metadata to expose W38's false observation.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source_path = Path(os.environ.get("FOLD_TEST_WRITE_SOURCE", root / "src/client/write.c"))
write = source_path.read_text()

def function(name):
    match = re.search(r"^static [^\n;]*\b" + name + r"\([^;]+?\)\n\{", write, re.M)
    if not match:
        raise RuntimeError(name)
    return write[match.start():write.index("\n}", match.end()) + 2]

start = write.index("struct dcache_ent {")
entry = write[start:write.index("\n};", start) + 3]
source = r'''
#include "client_internal.h"
#include "efs/erasure.h"
#include "efs/placement.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define DCACHE_NR 32
#define DTRACE(...) ((void)0)
#define CS EFS_MIN_CHUNK_SIZE
struct efs_client g_client;
''' + entry + r'''
static struct dcache_ent slot;
static struct efs_chunk_entry map, remote, sent_obs;
static uint8_t remote_bytes[CS], sent[CS], uploaded[CS];
static uint64_t sent_bg;
static pthread_mutex_t slot_mu=PTHREAD_MUTEX_INITIALIZER, io_mu=PTHREAD_MUTEX_INITIALIZER;
static unsigned captures, notes, dirty_marks, replacements;
static uint32_t noted_off, noted_len, noted_n;
static uint64_t noted_seq;
static int put_error, add_error, race;
static void dcache_ensure(void) {}
static uint32_t dcache_slot(efs_ino_t ino,uint32_t ci) { (void)ino;(void)ci;return 0; }
static pthread_mutex_t *dcache_mu(uint32_t s) { (void)s;return &slot_mu; }
static pthread_mutex_t *dcache_io_mu(uint32_t s) { (void)s;return &io_mu; }
static struct dcache_ent *dcache_find(uint32_t s,efs_ino_t ino,uint32_t ci) {
    (void)s;return slot.ino==ino && slot.ci==ci && slot.data ? &slot : NULL;
}
static struct dcache_ent *dcache_find_meta(uint32_t s,efs_ino_t ino,uint32_t ci) { return dcache_find(s,ino,ci); }
static uint64_t dcache_seq_next(struct dcache_ent *e) { return ++e->snap_seq; }
static void dcache_set_dirty(struct dcache_ent *e,uint32_t s) { (void)s;e->dirty=1; }
static void dcache_pin_add(struct dcache_ent *e) { e->pin_held=1; }
static void dcache_pin_release(struct dcache_ent *e) { e->pin_held=0; }
static void dcache_note_dirty_bytes(int64_t bytes) { (void)bytes; }
static int dcache_trace_on(void) { return 0; }
void *efs_buf_alloc(uint32_t n) { return malloc(n); }
void efs_buf_free(void *p,uint32_t n) { (void)n;free(p); }
static int export_chunk_copy(efs_ino_t ino,uint32_t ci,struct efs_chunk_entry *out) {
    (void)ino;(void)ci;*out=map;return 0;
}
static int export_chunk_exists(efs_ino_t ino,uint32_t ci) { (void)ino;(void)ci;return 1; }
static void export_forget_unreported(efs_ino_t ino,uint32_t ci) { (void)ino;(void)ci; }
int efs_client_fetch_published_chunk_obs(efs_ino_t ino,uint32_t ci,uint8_t *buf,
                                       uint32_t len,struct efs_chunk_entry *obs,int *have) {
    (void)ino;(void)ci;assert(len==CS);*obs=remote;*have=1;memcpy(buf,remote_bytes,CS);return EFS_OK;
}
static int dcache_put_now(efs_ino_t ino,uint32_t ci,const uint8_t *buf,uint32_t len,
                          uint64_t bg,uint64_t seq,uint32_t off,uint32_t span,
                          const struct efs_chunk_entry *obs) {
    (void)ino;(void)ci;(void)seq;assert(len==CS && !off && !span && obs);
    ++captures;memcpy(sent,buf,CS);sent_obs=*obs;sent_bg=bg;return EFS_OK;
}
int efs_encode_chunk(const uint8_t *buf,size_t len,size_t stripe,uint8_t *frags[EFS_NUM_FRAGMENTS]) {
    (void)buf;(void)len;memset(frags[2],0,stripe/2);return 0;
}
void efs_place_fragments(const struct efs_node *nodes,uint32_t count,efs_ino_t ino,
                         uint32_t ci,efs_node_id_t out[EFS_NUM_FRAGMENTS]) {
    (void)nodes;(void)count;(void)ino;(void)ci;out[0]=1;out[1]=2;out[2]=3;
}
static int efs_bytes_are_zero(const uint8_t *p,size_t n) { (void)p;(void)n;return 0; }
static void hash_write_fragments(const uint8_t *p[EFS_NUM_FRAGMENTS],uint32_t n,uint32_t cs,
                                 int z,uint64_t c,uint64_t a,uint64_t b,
                                 uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE]) {
    (void)p;(void)n;(void)cs;(void)z;(void)c;(void)a;(void)b;memset(sums,42,EFS_NUM_FRAGMENTS*EFS_HASH_SIZE);
}
int efs_client_put_fragments_parallel(efs_ino_t ino,uint32_t ci,
                                    efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                                    const uint8_t *buf[EFS_NUM_FRAGMENTS],uint32_t len,
                                    const uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE]) {
    (void)ino;(void)ci;(void)nodes;(void)buf;(void)len;(void)sums;
    assert(len==CS/2);memcpy(uploaded,buf[0],len);memcpy(uploaded+len,buf[1],len);
    if (race) {
        map = remote;
    }
    return put_error;
}
void efs_rdcache_invalidate(efs_ino_t ino,uint32_t ci) { (void)ino;(void)ci; }
int efs_client_ensure_meta_room(uint64_t a,uint64_t b) { (void)a;(void)b;return EFS_OK; }
int efs_export_needs_chunk_grow(const struct efs_export *ex) { (void)ex;return 0; }
static void export_reserve_chunks_locked(uint64_t n) { (void)n; }
static uint64_t chunk_candidate_gen(const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                                   const uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE],uint32_t ci) {
    (void)nodes;(void)sums;(void)ci;return 42;
}
void efs_client_lock_dir(efs_ino_t ino) { (void)ino; }
void efs_client_unlock_dir(efs_ino_t ino) { (void)ino; }
int efs_client_set_chunk(struct efs_export *ex,efs_ino_t ino,uint32_t ci,
                         const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                         const uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE]) {
    (void)ex;(void)ino;(void)ci;(void)nodes;(void)sums;++replacements;return EFS_OK;
}
int efs_export_set_chunk_gen(struct efs_export *ex,efs_ino_t ino,uint32_t ci,uint64_t gen) {
    (void)ex;(void)ino;(void)ci;map.generation=gen;map.ndelta=0;map.delta_seq=0;return EFS_OK;
}
int efs_export_add_chunk_delta(struct efs_export *ex,efs_ino_t ino,uint32_t ci,
                              const struct efs_chunk_delta *d) {
    (void)ex;(void)ino;(void)ci;if(add_error)return add_error;
    if(map.ndelta>=EFS_CHUNK_DELTA_MAX)return EFS_ERR_NOMEM;
    map.deltas[map.ndelta++]=*d;return EFS_OK;
}
static void putid_note(efs_ino_t ino,uint32_t ci,uint64_t obj,uint64_t seq,
                       const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                       const uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE],
                       uint32_t off,uint32_t len,uint32_t n,uint64_t newest,
                       const struct efs_chunk_entry *obs) {
    (void)ino;(void)ci;(void)obj;(void)seq;(void)nodes;(void)sums;
    if (obs && obs->read_view.count) {
        assert(obs->read_view.fence_epoch == 6);
    }
    ++notes;noted_off=off;noted_len=len;noted_n=n;noted_seq=newest;
}
void efs_client_mark_chunk_dirty(efs_ino_t ino,uint32_t ci) { (void)ino;(void)ci;++dirty_marks; }
void efs_client_mark_ino_dirty(efs_ino_t ino) { (void)ino; }
static void report_landed_note(efs_ino_t ino) { (void)ino; }
''' + "\n".join(function(name) for name in ["chunk_obs_has_live_spans",
    "dcache_install_image", "dcache_flush_keep", "dcache_replay_stale_ex_budgeted",
    "dcache_put_now_budgeted"]) + r'''
static void reset(void) {
    memset(&slot,0,sizeof(slot));memset(&map,0,sizeof(map));memset(&remote,0,sizeof(remote));
    slot.ino=1;slot.len=CS;slot.data=malloc(CS);assert(slot.data);
    memset(slot.data,'A',CS);slot.have_base=1;slot.object_gen=7;slot.base_gen=7;
    map.generation=remote.generation=7;remote.ndelta=1;remote.delta_seq=9;
    remote.deltas[0].generation=8;remote.deltas[0].off=500;remote.deltas[0].len=4256;
    memset(remote_bytes,'A',CS);memset(remote_bytes+500,'P',4256);
    captures=notes=dirty_marks=replacements=0;put_error=add_error=race=0;
}
static void replay_tests(void) {
    for(int ranges=0;ranges<2;++ranges) {
        reset();slot.nrange=ranges;slot.roff[0]=100;slot.rlen[0]=64;
        if(ranges)memset(slot.data+100,'N',64);
        assert(dcache_replay_stale_ex_budgeted(1,0,0)==EFS_OK && captures==1);
        assert(sent_obs.ndelta==1 && sent_obs.delta_seq==9 && sent_bg==7);
        for(unsigned i=0;i<CS;++i)
            assert(sent[i]==(i>=500 && i<4756 ? 'P' : ranges && i>=100 && i<164 ? 'N' : 'A'));
        free(slot.data);
    }
    reset();remote.deltas[0].len=0;memset(slot.data,'N',CS);
    assert(dcache_replay_stale_ex_budgeted(1,0,0)==EFS_OK && sent[0]=='N');
    assert(sent_obs.ndelta==1);free(slot.data); /* tombstone carries no bytes */
}
static void put_tests(void) {
    struct efs_chunk_entry before;
    reset();remote.ndelta=EFS_CHUNK_DELTA_MAX;race=1;
    assert(dcache_put_now_budgeted(1,0,slot.data,CS,7,1,0,0,NULL)==EFS_OK);
    assert(notes==1 && noted_n==0 && noted_seq==0);free(slot.data);
    reset();map=remote;map.ndelta=EFS_CHUNK_DELTA_MAX;
    assert(dcache_put_now_budgeted(1,0,slot.data,CS,EFS_CHUNK_BASE_UNCOND,1,0,0,NULL)==EFS_OK);
    assert(notes==1 && noted_n==EFS_CHUNK_DELTA_MAX && noted_seq==9);free(slot.data);
    reset();map=remote;before=map;add_error=EFS_ERR_NOMEM;
    slot.object_seq=17;slot.nrange=1;slot.roff[0]=100;slot.rlen[0]=64;
    memset(slot.data+100,'N',64);
    assert(dcache_put_now_budgeted(1,0,slot.data,CS,7,1,100,64,NULL)==EFS_ERR_STALE);
    assert(!notes && !dirty_marks && !replacements && !memcmp(&map,&before,sizeof(map)));
    assert(!slot.object_delta_len && slot.object_seq==17);
    assert(!dcache_flush_keep(1,0,slot.data,CS,0,1,0));
    assert(slot.dirty && slot.nrange==1 && slot.data[100]=='N');free(slot.data);
    reset();map=remote;map.ndelta=EFS_CHUNK_DELTA_MAX;add_error=EFS_ERR_NOMEM;
    remote.read_view.count=1;remote.read_view.fence_epoch=6;
    before=remote; /* image fetched against this list, table subsequently grew */
    memcpy(slot.data,remote_bytes,CS);
    assert(dcache_put_now_budgeted(1,0,slot.data,CS,7,1,100,64,&before)==EFS_OK);
    assert(notes==1 && !noted_off && !noted_len && noted_n==1 && noted_seq==9);
    assert(uploaded[500]=='P');
    assert(slot.object_publish_epoch==6 &&
           slot.object_publish_flags==EFS_CHUNK_REC_F_CAPTURED_EPOCH);free(slot.data);
    reset();put_error=EFS_ERR_IO;
    assert(dcache_put_now_budgeted(1,0,slot.data,CS,7,1,100,64,NULL)==EFS_ERR_IO);
    assert(!notes && !dirty_marks && !replacements);free(slot.data);
}
int main(void) {
    assert(!pthread_mutex_init(&g_client.idx_mu,NULL));
    replay_tests();put_tests();
    puts("fold observation: peer bytes, tombstones, PUT races, chain-full fallback and failures PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="efs-fold-") as directory:
    path = Path(directory) / "test.c"
    path.write_text(source)
    command = shlex.split(os.environ.get("CC", "cc"))
    command += ["-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wextra",
                "-Werror", "-Wno-unused-variable", "-pthread",
                "-I" + str(root / "include"), "-I" + str(root / "src/client")]
    command += shlex.split(os.environ.get("FOLD_TEST_CFLAGS", ""))
    subprocess.run(command + [str(path), "-o", str(path.with_suffix(""))], check=True)
    subprocess.run([str(path.with_suffix(""))], check=True)
