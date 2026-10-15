#!/usr/bin/env python3
"""Read cache keeps admitting chunks once its bodies fill the speculative budget.

Cached bodies are speculative buffer allocations (half the buffer budget).
When the cache itself held that much, a refused allocation dropped the store
and no new chunk was ever cached (Oct 15 Intel, 32 KiB reads: two fragment
GETs per read). The production rdcache functions are compiled here against a
fixed-budget pool: stores past the budget must reuse LRU bodies, never exceed
the budget, never take pinned or pending ways, and drop cleanly when nothing
can be reused.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
s = (root / 'src/client/read.c').read_text()


def between(start, end):
    a = s.index(start)
    return s[a:s.index(end, a)]


def function(signature):
    a = s.index(signature)
    return s[a:s.index('\n}\n', a) + 3]


cache = between('#define RDCACHE_SLOTS', '/* Pressure can discard only')
code = (cache + function('static struct rdcache_ent *rdcache_find(') +
        between('#define RDCACHE_STEAL_WINDOW', 'static uint8_t *rdcache_steal_body(') +
        function('static uint8_t *rdcache_steal_body(') +
        function('static int rdcache_put_inner(') +
        function('int efs_rdcache_copy('))

source = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t efs_ino_t;
#define LEN 16384u
static unsigned budget, live, allocs, frees;
static void *efs_buf_alloc_prefetch(uint32_t n)
{
    assert(n == LEN);
    if (live >= budget)
        return NULL;
    live++; allocs++;
    return malloc(n);
}
static void efs_buf_free(void *p, uint32_t n)
{
    assert(p && n == LEN && live > 0);
    live--; frees++;
    free(p);
}
static uint64_t rdcache_map_gen(efs_ino_t ino, uint32_t ci) { (void)ino; (void)ci; return 7; }
''' + code + r'''
static unsigned cached_bodies(void)
{
    unsigned n = 0;
    for (unsigned s = 0; s < RDCACHE_SLOTS; s++)
        for (unsigned w = 0; w < RDCACHE_WAYS; w++)
            n += g_rdcache.e[s][w].data != NULL;
    return n;
}
static unsigned pending_ways(void)
{
    unsigned n = 0;
    for (unsigned s = 0; s < RDCACHE_SLOTS; s++)
        for (unsigned w = 0; w < RDCACHE_WAYS; w++)
            n += g_rdcache.e[s][w].pending != 0;
    return n;
}
static int cached(efs_ino_t ino, uint32_t ci)
{
    static uint8_t got[4096];
    if (efs_rdcache_copy(ino, ci, 4096, got, sizeof(got)) != 0)
        return 0;
    for (unsigned i = 0; i < sizeof(got); i++)
        assert(got[i] == (uint8_t)ci);
    return 1;
}
static void put(efs_ino_t ino, uint32_t ci)
{
    static uint8_t src[LEN];
    memset(src, (int)(uint8_t)ci, sizeof(src));
    (void)rdcache_put_inner(ino, ci, src, LEN, 0, 0);
}
int main(void)
{
    /* Budget of 8 bodies: the first 8 chunks allocate. */
    budget = 8;
    for (uint32_t ci = 0; ci < 8; ci++)
        put(42, ci);
    assert(live == 8 && cached_bodies() == 8);
    for (uint32_t ci = 0; ci < 8; ci++)
        assert(cached(42, ci));

    /* Pin chunk 0: a zero-copy reply is reading out of it. */
    uint32_t s0 = rdcache_slot(42, 0);
    struct rdcache_ent *pinned = rdcache_find(s0, 42, 0);
    assert(pinned);
    pinned->pins = 1;

    /* 200 more chunks: every one is cached right after its put, by reusing
     * an LRU body, and the pool never exceeds its budget. */
    for (uint32_t ci = 8; ci < 208; ci++) {
        put(42, ci);
        assert(cached(42, ci));
        assert(live <= budget && cached_bodies() == live);
    }
    assert(live == 8 && allocs == 8 && frees == 0);
    assert(cached(42, 0));          /* pinned: never stolen */
    pinned->pins = 0;

    /* A pending way (a fetch in flight for another key) is never stolen. */
    uint32_t sp = rdcache_slot(42, 207);
    struct rdcache_ent *pend = rdcache_find(sp, 42, 207);
    assert(pend);
    pend->pending = 1;
    for (uint32_t ci = 300; ci < 340; ci++)
        put(42, ci);
    assert(pend->data && pend->ino == 42 && pend->ci == 207);
    pend->pending = 0;

    /* Dense cache (the production failure: ~1280 bodies in 4096 slots):
     * sampled LRU must keep recent chunks while older ones are reused. */
    for (unsigned s = 0; s < RDCACHE_SLOTS; s++)
        for (unsigned w = 0; w < RDCACHE_WAYS; w++)
            if (g_rdcache.e[s][w].data) {
                efs_buf_free(g_rdcache.e[s][w].data, LEN);
                memset(&g_rdcache.e[s][w], 0, sizeof(g_rdcache.e[s][w]));
            }
    budget = 2048; allocs = frees = 0;
    for (uint32_t ci = 0; ci < 2048; ci++)
        put(44, ci);
    assert(live == 2048 && cached_bodies() == 2048);
    for (uint32_t ci = 2048; ci < 2560; ci++)
        put(44, ci);
    assert(live == 2048 && allocs == 2048 && frees == 0);
    for (uint32_t ci = 2560 - 256; ci < 2560; ci++)
        assert(cached(44, ci));     /* the newest 256 all survive */
    unsigned old = 0;
    for (uint32_t ci = 0; ci < 512; ci++)
        old += cached(44, ci);
    assert(old < 256);              /* reuse came mostly from the oldest */

    /* Nothing to reuse and no budget: the store is dropped, no pending
     * marks are left behind, no body leaks. */
    for (unsigned s = 0; s < RDCACHE_SLOTS; s++)
        for (unsigned w = 0; w < RDCACHE_WAYS; w++)
            if (g_rdcache.e[s][w].data) {
                efs_buf_free(g_rdcache.e[s][w].data, LEN);
                memset(&g_rdcache.e[s][w], 0, sizeof(g_rdcache.e[s][w]));
            }
    budget = 0;
    put(43, 1);
    assert(!cached(43, 1) && live == 0 && pending_ways() == 0);
    puts("read cache: admission past the speculative budget reuses LRU bodies, "
         "keeps pinned/pending ways, bounded pool and clean drop PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='efs-rdcache-steal-') as d:
    p = Path(d) / 'test.c'
    p.write_text(source)
    exe = Path(d) / 'test'
    cc = shlex.split(os.environ.get('CC', 'cc'))
    subprocess.run(cc + ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                         '-pthread', str(p), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# Both speculative allocations fall back to reusing a cached body.
assert 'spare = rdcache_steal_body(len);' in s
assert 'job->chunk = rdcache_steal_body(cs);' in s
