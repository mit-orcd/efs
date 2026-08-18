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
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "client_internal.h"

/* 4096 x 128 KiB = 512 MiB held for reuse at most; excess frees go to
 * free() so the pool cannot itself become a footprint problem. */
#define BUFPOOL_MAX_FREE 4096

static void *g_bp[BUFPOOL_MAX_FREE];
static int g_bp_n;
static pthread_mutex_t g_bp_mu = PTHREAD_MUTEX_INITIALIZER;

void *efs_buf_alloc(uint32_t len)
{
    if (len > EFS_CHUNK_SIZE)
        return malloc(len);
    pthread_mutex_lock(&g_bp_mu);
    if (g_bp_n > 0) {
        void *p = g_bp[--g_bp_n];
        pthread_mutex_unlock(&g_bp_mu);
        return p;
    }
    pthread_mutex_unlock(&g_bp_mu);
    return malloc(EFS_CHUNK_SIZE);
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
    pthread_mutex_unlock(&g_bp_mu);
    free(p);
}
