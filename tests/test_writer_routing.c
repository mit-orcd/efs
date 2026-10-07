/* Exercise the actual writer implementation with an in-memory disk boundary. */
#include "../src/server/writer.c"
#include <assert.h>
#include <stdio.h>

__thread uint64_t efs_tls_chunk_gen;
static int roots[64], calls[2], fail_thread;
static unsigned delay_us;

int efsd_pthread_create(pthread_t *tid, void *(*fn)(void *), void *arg)
{
    if (fail_thread) return EAGAIN;
    return pthread_create(tid, NULL, fn, arg);
}
uint64_t efs_iostats_now_us(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
void efs_iostats_add(int cls, uint64_t bytes, uint64_t us, int err)
{ (void)cls; (void)bytes; (void)us; (void)err; }
void efs_hash_zero_fragment_len(size_t len, uint8_t out[EFS_HASH_SIZE])
{ (void)len; memset(out, 0, EFS_HASH_SIZE); }
int server_find_fragment_root(struct efsd_server *s, struct efs_export *ex,
    efs_ino_t ino, uint32_t ci, uint32_t fi)
{
    (void)s; (void)ex; (void)ino; (void)fi;
    return efs_tls_path_hint == EFS_PATH_HINT_SKIP || ci >= 64 ? -1 : roots[ci];
}
int server_write_fragment_with_sum_sync(struct efsd_server *s, struct efs_export *ex,
    efs_ino_t ino, uint32_t ci, uint32_t fi, const uint8_t *data,
    uint32_t len, const uint8_t checksum[EFS_HASH_SIZE])
{
    (void)s; (void)ex; (void)ino; (void)fi; (void)data; (void)len; (void)checksum;
    if (ci >= 64) return EFS_ERR_IO;
    if (delay_us) usleep(delay_us);
    assert(efs_tls_write_root >= 0 && efs_tls_write_root < 2);
    if (roots[ci] >= 0) assert(roots[ci] == efs_tls_write_root);
    roots[ci] = efs_tls_write_root;
    __atomic_fetch_add(&calls[efs_tls_write_root], 1, __ATOMIC_RELAXED);
    return EFS_OK;
}
int server_write_fragment_sync(struct efsd_server *s, struct efs_export *ex,
    efs_ino_t ino, uint32_t ci, uint32_t fi, const uint8_t *data, uint32_t len)
{ return server_write_fragment_with_sum_sync(s, ex, ino, ci, fi, data, len, NULL); }

struct producer { struct efsd_server *s; struct efs_export *ex; unsigned first, span, passes; };
static void *produce(void *arg)
{
    struct producer *p = arg;
    uint8_t data[64] = {1}, sum[EFS_HASH_SIZE] = {1};
    for (unsigned pass = 0; pass < p->passes; pass++) {
        efs_tls_path_hint = pass ? -1 : EFS_PATH_HINT_SKIP;
        for (unsigned ci = p->first; ci < p->first + p->span; ci++)
            assert(server_write_fragment_with_sum(p->s, p->ex, 123, ci, 0,
                   data, sizeof(data), sum) == EFS_OK);
    }
    return NULL;
}

int main(void)
{
    struct efsd_server s = {0}; struct efs_export ex = {0};
    uint8_t data[64] = {1}, sum[EFS_HASH_SIZE] = {1};
    s.storage_path_count = 2; s.id = 250;
    for (int nw = 0; nw <= 2; nw += 2) {
        memset(roots, -1, sizeof(roots)); memset(calls, 0, sizeof(calls));
        s.nwriters = nw;
        assert(server_writer_pool_start(&s) == 0);
        server_writer_stats_reset(1);
        pthread_t threads[4]; struct producer producers[4];
        for (unsigned i = 0; i < 4; i++) {
            producers[i] = (struct producer){ &s, &ex, i * 16, 16, 2 };
            assert(pthread_create(&threads[i], NULL, produce, &producers[i]) == 0);
        }
        for (unsigned i = 0; i < 4; i++) pthread_join(threads[i], NULL);
        assert(calls[0] > 0 && calls[1] > 0);
        assert(server_write_fragment_with_sum(&s, &ex, 123, 65, 0,
               data, sizeof(data), sum) == EFS_ERR_IO);
        struct efs_writer_stats st; server_writer_stats_snapshot(&st);
        assert(st.jobs == 129 && st.active == 0 && st.peak_active >= 1 && st.peak_active <= 4);
        assert(st.queued == (nw ? 129 : 0));
        server_writer_stats_reset(0);
        server_writer_pool_stop(&s);
        server_writer_pool_stop(&s);
    }
    /* Exercise the fallback class under sustained one/two-slot contention:
     * every producer completes, accounting drains and routing stays stable. */
    for (int nw = 1; nw <= 2; nw++) {
        memset(roots, -1, sizeof(roots)); memset(calls, 0, sizeof(calls));
        s.nwriters = nw; delay_us = 100;
        assert(server_writer_pool_start(&s) == 0);
        server_writer_stats_reset(1);
        pthread_t threads[32]; struct producer producers[32];
        for (unsigned i = 0; i < 32; i++) {
            producers[i] = (struct producer){ &s, &ex, i * 2, 2, 32 };
            assert(pthread_create(&threads[i], NULL, produce, &producers[i]) == 0);
        }
        for (unsigned i = 0; i < 32; i++) pthread_join(threads[i], NULL);
        struct efs_writer_stats st; server_writer_stats_snapshot(&st);
        assert(st.jobs == 2048 && st.active == 0 && st.fallback > 0);
        assert(st.queued == st.jobs && calls[0] + calls[1] == 2048);
        assert(st.peak_active > 1 && st.peak_active <= 32);
        server_writer_stats_reset(0); server_writer_pool_stop(&s);
    }
    delay_us = 0;
    s.nwriters = 2; fail_thread = 1;
    assert(server_writer_pool_start(&s) != 0);
    fail_thread = 0; s.nwriters = 0;
    assert(server_writer_pool_start(&s) == 0);
    server_writer_pool_stop(&s);
    puts("writer routing: inline/pool spread, overwrite identity, statistics and lifecycle PASS");
    return 0;
}
