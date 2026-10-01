/*
 * Client chunk-buffer pool.
 *
 * The dcache/rdcache/flush/read paths used to malloc+free a 128 KiB buffer
 * per chunk operation. Across ~200 FUSE/RDMA threads glibc fanned that churn
 * into ~300 per-thread arenas that never release memory — 16.9 GB of a
 * 27 GB efs-fuse RSS was pure arena bloat. Recycling fixed-size buffers
 * through a bounded free-list keeps the hot path out of malloc entirely.
 *
 * Buffers are EFS_CHUNK_SIZE (128 KiB); callers needing more get plain
 * malloc and must say so at free time via the same length, which selects
 * the pool vs free() path deterministically.
 */
#include <efs/common.h>
#include <efs/rdma.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "client_internal.h"

/* One slab is 256 chunks (32 MiB, one malloc → one mmap). The pool
 * holds at most the dcache dirty cap (2 GiB) plus a pipeline of
 * chunks: 64 slabs. A 1 GiB sequential write then maps ~32 times,
 * not once per chunk (Oct 1: 8193 mmap in the write phase). */
#define BUF_SLAB_N 256
#define BUF_SLAB_MAX 64
#define BUFPOOL_MAX_FREE (BUF_SLAB_N * BUF_SLAB_MAX)

static void *g_bp[BUFPOOL_MAX_FREE];
static void *g_slabs[BUF_SLAB_MAX];
static int g_bp_n;
static int g_bp_slabs;
static pthread_mutex_t g_bp_mu = PTHREAD_MUTEX_INITIALIZER;

static int buf_in_slab(const void *p)
{
    int i;
    for (i = 0; i < g_bp_slabs; i++) {
        const uint8_t *b = g_slabs[i];
        if (!b)
            continue;
        if (p >= (const void *)b &&
            p < (const void *)(b + (size_t)BUF_SLAB_N * EFS_CHUNK_SIZE))
            return 1;
    }
    return 0;
}

void *efs_buf_alloc(uint32_t len)
{
    void *slab;
    int i;

    if (len > EFS_CHUNK_SIZE)
        return malloc(len);
    pthread_mutex_lock(&g_bp_mu);
    if (g_bp_n > 0) {
        void *p = g_bp[--g_bp_n];
        pthread_mutex_unlock(&g_bp_mu);
        return p;
    }
    if (g_bp_slabs >= BUF_SLAB_MAX) {
        pthread_mutex_unlock(&g_bp_mu);
        return malloc(EFS_CHUNK_SIZE);
    }
    /* Reserve this slab's index under the lock. Two threads carving at
     * once each own a distinct slot; writing g_slabs[g_bp_slabs - 1]
     * after the malloc could record both slabs in one slot and leave
     * the other unknown to buf_in_slab (then free() of a slab interior). */
    int idx = g_bp_slabs++;
    pthread_mutex_unlock(&g_bp_mu);
    slab = malloc((size_t)BUF_SLAB_N * EFS_CHUNK_SIZE);
    pthread_mutex_lock(&g_bp_mu);
    if (!slab) {
        /* The index stays reserved (NULL slab, never matched by
         * buf_in_slab); the cap is 64 and this is a malloc failure. */
        pthread_mutex_unlock(&g_bp_mu);
        return malloc(EFS_CHUNK_SIZE);
    }
    g_slabs[idx] = slab;
    for (i = 1; i < BUF_SLAB_N && g_bp_n < BUFPOOL_MAX_FREE; i++)
        g_bp[g_bp_n++] = (uint8_t *)slab + (size_t)i * EFS_CHUNK_SIZE;
    pthread_mutex_unlock(&g_bp_mu);
    /* W39: a PUT fragment that lives in a slab is sent as a second SGE
     * straight from here (no copy into the conn's send buffer). The slab
     * is never freed, so the registration is for the process lifetime. */
    (void)efs_rdma_zc_region_add(slab, (size_t)BUF_SLAB_N * EFS_CHUNK_SIZE);
    return slab;
}

void efs_buf_free(void *p, uint32_t len)
{
    if (!p)
        return;
    if (len > EFS_CHUNK_SIZE) {
        free(p);
        return;
    }
    pthread_mutex_lock(&g_bp_mu);
    if (g_bp_n < BUFPOOL_MAX_FREE) {
        g_bp[g_bp_n++] = p;
        pthread_mutex_unlock(&g_bp_mu);
        return;
    }
    int slab = buf_in_slab(p);
    pthread_mutex_unlock(&g_bp_mu);
    /* A slab interior is not a malloc result. Dropping it loses one
     * chunk until process exit; free() would corrupt the heap. */
    if (!slab)
        free(p);
}
