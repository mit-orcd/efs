#!/usr/bin/env python3
"""Client staging LRU: touch order, O(1) index, pinned second chance, pass cost."""
from sanitize_env import asan_runtime_available
from pathlib import Path
import os, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = Path(os.environ.get('EFS_STAGE_EVICT_SOURCE', root/'src/client/stage_evict.c')).read_text()
def section(start, end):
    a=s.index(start); return s[a:s.index(end,a)]
code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef uint64_t efs_ino_t;
''' + section('struct lru_node {', '/* Pin providers') \
    + section('static uint64_t lru_hash(', '/* ------------------------- evictor thread') \
    + section('#define EVICT_BATCH', '/* Pin check WITHOUT') + r'''
/* evict_pass collaborators: a pin set and an eviction log. */
static uint8_t pinned[1 << 20];
static efs_ino_t evicted_log[1 << 16]; static unsigned nevicted;
static int stage_pinned(efs_ino_t ino){return pinned[ino];}
static int evict_one(efs_ino_t ino, uint64_t tick)
{
    pthread_mutex_lock(&g_lru_mu);
    uint64_t cur=lru_tick_locked(ino);
    if(cur && cur!=tick){pthread_mutex_unlock(&g_lru_mu);return 0;}
    lru_remove_locked(ino);
    pthread_mutex_unlock(&g_lru_mu);
    evicted_log[nevicted++]=ino; return 1;
}
''' + section('/* One pass over the SCAN_BATCH oldest', 'static void stage_compact(') + r'''
/* Reference model: tick per ino, 0 = absent. */
#define N 200000
static uint64_t ref[N+1]; static uint64_t ref_clock; static uint64_t ref_count;
static void ref_touch(efs_ino_t i){if(!ref[i])ref_count++;ref[i]=++ref_clock;}
static void ref_remove(efs_ino_t i){if(ref[i])ref_count--;ref[i]=0;}
static void check_against_ref(void)
{
    /* Same set, list in strictly increasing tick, index finds every ino. */
    uint64_t n=0,last=0;
    pthread_mutex_lock(&g_lru_mu);
    for(uint32_t x=g_lru_nodes[0].next;x;x=g_lru_nodes[x].next){
        struct lru_node *e=&g_lru_nodes[x];
        assert(e->ino>0 && e->ino<=N);
        assert(e->tick>last); last=e->tick;
        assert(ref[e->ino]);
        int64_t sl=lru_slot_locked(e->ino); assert(sl>=0 && g_lru_slots[sl]==x);
        n++;
    }
    assert(n==g_lru_count && n==ref_count);
    for(efs_ino_t i=1;i<=N;i++) assert((lru_slot_locked(i)>=0)==(ref[i]!=0));
    pthread_mutex_unlock(&g_lru_mu);
}
static uint64_t rnd=88172645463325252ull;
static uint64_t xr(void){rnd^=rnd<<13;rnd^=rnd>>7;rnd^=rnd<<17;return rnd;}
static double now_s(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(void)
{
    /* 1. Random touch/remove against the model; ticks advance in lockstep
     *    because both clocks count the same touches. */
    for(int round=0;round<6;round++){
        for(int k=0;k<200000;k++){
            efs_ino_t i=1+xr()%N;
            if(xr()%4){efs_client_stage_touch(i);ref_touch(i);}
            else{pthread_mutex_lock(&g_lru_mu);lru_remove_locked(i);pthread_mutex_unlock(&g_lru_mu);ref_remove(i);}
        }
        check_against_ref();
    }
    /* 2. Oldest-first read matches the model's smallest ticks. */
    struct lru_node old[SCAN_BATCH];
    uint32_t got=lru_oldest(old,SCAN_BATCH); assert(got==SCAN_BATCH);
    for(uint32_t j=0;j<got;j++){
        assert(ref[old[j].ino]==old[j].tick);
        uint64_t older=0; for(efs_ino_t i=1;i<=N;i++) if(ref[i] && ref[i]<old[j].tick) older++;
        assert(older==j);
    }
    /* 3. Requeue moves only an untouched entry; a touched one keeps its place. */
    efs_ino_t a=old[0].ino,b=old[1].ino;
    lru_requeue(a,old[0].tick);
    pthread_mutex_lock(&g_lru_mu);
    uint32_t newest=g_lru_nodes[0].prev;
    assert(g_lru_nodes[newest].ino==a && g_lru_nodes[newest].tick>old[0].tick);
    ref[a]=g_lru_nodes[newest].tick; ref_clock=g_lru_clock;
    pthread_mutex_unlock(&g_lru_mu);
    efs_client_stage_touch(b); ref_touch(b);
    uint64_t bt=ref[b]; lru_requeue(b,old[1].tick); /* stale tick: no-op */
    pthread_mutex_lock(&g_lru_mu);
    newest=g_lru_nodes[0].prev;
    assert(g_lru_nodes[newest].ino==b && lru_tick_locked(b)==bt);
    assert(g_lru_nodes[g_lru_nodes[newest].prev].ino==a);
    pthread_mutex_unlock(&g_lru_mu);
    check_against_ref();
    /* 4. A pass evicts unpinned oldest first, requeues pinned ones and
     *    never touches newer entries. */
    got=lru_oldest(old,SCAN_BATCH);
    for(uint32_t j=0;j<got;j+=2) pinned[old[j].ino]=1;
    int rq=0; nevicted=0;
    int ev=evict_pass(&rq);
    assert(ev==EVICT_BATCH && nevicted==EVICT_BATCH);
    for(int j=0;j<ev;j++) assert(evicted_log[j]==old[2*j+1].ino);
    assert(rq==EVICT_BATCH); /* the pinned ones met before the 64th eviction */
    struct lru_node now[EVICT_BATCH];
    lru_oldest(now,EVICT_BATCH);
    assert(now[0].ino==old[2*EVICT_BATCH].ino); /* rest of the window is next */
    for(uint32_t j=0;j<got;j++) pinned[old[j].ino]=0;
    /* 5. A pass costs O(batch), not O(entries): 2M staged inos, 2000
     *    passes. The old full-table scan did 2000 x 8M slot reads here. */
    for(efs_ino_t i=1;i<=N;i++) {pthread_mutex_lock(&g_lru_mu);lru_remove_locked(i);pthread_mutex_unlock(&g_lru_mu);}
    for(efs_ino_t i=1;i<=2000000;i++) efs_client_stage_touch(i+1000000);
    double t0=now_s();
    for(int k=0;k<2000;k++){ got=lru_oldest(old,SCAN_BATCH); assert(got==SCAN_BATCH);
        for(uint32_t j=0;j<got;j++) lru_requeue(old[j].ino,old[j].tick); }
    double dt=now_s()-t0;
    pthread_mutex_lock(&g_lru_mu); assert(g_lru_count==2000000); pthread_mutex_unlock(&g_lru_mu);
#ifndef EFS_LEAK_WRAPPED
    assert(dt<1.0);
#endif
    printf("stage LRU: model order, index, requeue, pass order and O(batch) passes (2000 passes over 2M in %.3fs) PASS\n",dt);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-stage-lru-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    # make leak-check sets EFS_LEAK_WRAP (memcheck): run only the plain build
    # under it, without the wall-clock bound memcheck's slowdown would break.
    wrap=shlex.split(os.environ.get('EFS_LEAK_WRAP',''))
    variants = [['-DEFS_LEAK_WRAPPED']] if wrap else [[]]
    if not wrap and asan_runtime_available():
        variants.append(['-fsanitize=address,undefined','-fno-omit-frame-pointer'])
    for extra in variants:
        exe=p.with_suffix('.asan' if '-fno-omit-frame-pointer' in extra else '')
        subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-O2','-Wall','-Wextra','-Werror','-pthread',*extra,str(p),'-o',str(exe)],check=True)
        subprocess.run([*wrap,str(exe)],check=True)
