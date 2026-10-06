/* Standalone: cc -Iinclude -Isrc/common -pthread tests/test_bufpool.c -o /tmp/test_bufpool */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
static int fail_malloc;
static void *test_malloc(size_t n) { return fail_malloc ? NULL : malloc(n); }
int efs_rdma_zc_region_add(void *p, size_t n) { (void)p; (void)n; return 0; }
#define malloc test_malloc
#include "../src/client/bufpool.c"
#undef malloc

static void check(uint64_t expected)
{
    uint64_t live, reserved, backing, limit;
    efs_buf_budget_stats(&live, &reserved, &backing, &limit);
    assert(live == expected);
    assert(live + reserved <= limit);
    assert(backing <= limit);
}
static void *churn(void *arg)
{
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        if (efs_buf_reserve(EFS_CHUNK_SIZE * 2))
            continue;
        void *p = efs_buf_alloc(1);
        assert(p);
        efs_buf_unreserve();
        efs_buf_free(p, 1);
    }
    return NULL;
}
static void *abandon_credit(void *unused)
{
    (void)unused;
    assert(!efs_buf_reserve_request(1u << 20, 4096));
    return NULL; /* pthread-key destructor must release both reservations */
}
int main(void)
{
    setenv("EFS_DCACHE_HARD_BYTES", "33554432", 1);
    setenv("EFS_DCACHE_DRAIN_BYTES", "33554432", 1);
    fail_malloc = 1;
    assert(!efs_buf_alloc(1));
    check(0);
    fail_malloc = 0;
    void *large = efs_buf_alloc(1u << 20);
    assert(large);
    check(1u << 20);
    efs_buf_free(large, 1u << 20);
    check(0);
    setenv("EFS_TEST_BUDGET", "33554432junk", 1);
    assert(budget_env("EFS_TEST_BUDGET", 17, 1, UINT64_MAX)==17);
    setenv("EFS_TEST_BUDGET", "-1", 1);
    assert(budget_env("EFS_TEST_BUDGET", 17, 1, UINT64_MAX)==17);
    assert(efs_buf_reserve(33ull << 20)==EFS_ERR_BUSY);
    /* Reservation excludes competing allocators, and consumes actual pool
     * capacity even for a one-byte sparse patch/parity buffer. */
    assert(efs_buf_reserve(32ull << 20) == 0);
    assert(!efs_buf_alloc(64u << 20));
    void *p[512];
    for (int i = 0; i < 256; i++) {
        p[i] = efs_buf_alloc(1);
        assert(p[i]);
    }
    efs_buf_unreserve();
    check(32ull << 20);
    assert(!efs_buf_alloc(1));
    assert(efs_buf_reserve(EFS_CHUNK_SIZE) == EFS_ERR_BUSY);
    /* Writer exhaustion cannot consume the dedicated drain reserve. */
    efs_buf_drain_enter();
    efs_buf_drain_enter();
    for (int i = 256; i < 512; i++) {
        p[i] = efs_buf_alloc(EFS_CHUNK_SIZE);
        assert(p[i]);
    }
    assert(!efs_buf_alloc(1));
    efs_buf_drain_leave();
    efs_buf_drain_leave();
    check(64ull << 20);
    /* Ownership transfer doesn't reduce the charge. Retained failed bodies
     * cannot permit admission until their actual allocation is released. */
    assert(efs_buf_reserve(1) == EFS_ERR_BUSY);
    for (int i = 0; i < 512; i++) efs_buf_free(p[i], EFS_CHUNK_SIZE);
    check(0);
    assert(g_bp_n == 512);
    /* An accepted writer's reservation remains usable while another thread
     * consumes the drain reserve. Previously alloc rechecked the hard cap
     * against drain bytes and failed despite fully reserved credit. */
    assert(!efs_buf_reserve(32ull << 20));
    efs_buf_drain_enter();
    for (int i=0;i<256;i++) { p[i]=efs_buf_alloc(1); assert(p[i]); }
    efs_buf_drain_leave();
    assert(g_live==(32ull<<20) && g_reserved==(32ull<<20));
    for (int i=256;i<512;i++) { p[i]=efs_buf_alloc(1); assert(p[i]); }
    assert(!g_reserved && g_live==(64ull<<20));
    assert(!efs_buf_alloc(1)); /* no credit cannot spend drain capacity */
    efs_buf_unreserve();
    for (int i=0;i<512;i++) efs_buf_free(p[i],EFS_CHUNK_SIZE);
    check(0);
    pthread_t workers[16];
    for (int i = 0; i < 16; i++) assert(!pthread_create(&workers[i], NULL, churn, NULL));
    for (int i = 0; i < 16; i++) assert(!pthread_join(workers[i], NULL));
    check(0);
    assert(g_reserved == 0 && g_bp_n == 512);
    pthread_t retired;
    assert(!pthread_create(&retired, NULL, abandon_credit, NULL));
    assert(!pthread_join(retired, NULL));
    assert(!g_reserved && !g_meta_reserved);
    assert(!efs_buf_reserve_request(1u << 20, 8u << 20));
    void *meta = efs_buf_metadata_alloc(1024);
    assert(meta && !efs_buf_metadata_alloc(8u << 20));
    efs_buf_unreserve();
    efs_buf_metadata_free(meta, 1024);
    assert(!g_metadata && !g_meta_reserved);
    void *m[32];
    for (int i = 0; i < 32; i++) { m[i] = efs_buf_metadata_alloc(256u << 10); assert(m[i]); }
    assert(!efs_buf_metadata_alloc(1));
    for (int i = 0; i < 32; i++) efs_buf_metadata_free(m[i], 256u << 10);
    assert(g_metadata == 0);
    puts("test_bufpool: OK (hard bound, reserve, failures, concurrency, metadata)");
    return 0;
}
