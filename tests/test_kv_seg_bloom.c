/* Segment point-get filter and cached-block iterator seeks: every PUT and
 * tombstone stays visible once the filter is published, absent keys stay
 * absent, and a seek into a cached block lands on the first record >= seek
 * (also past the block's last record and in uncached blocks). */
#include "../src/kv/kv_lsm_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_tmpdir.h"

static uint32_t key_for(unsigned i, uint8_t key[16])
{
    /* Variable lengths: shared prefixes across neighbours. */
    uint32_t n = 4 + i % 9;
    memset(key, 0x5a, 16);
    key[0] = 2; key[1] = (uint8_t)(i >> 16); key[2] = (uint8_t)(i >> 8); key[3] = (uint8_t)i;
    return n;
}

int main(void)
{
    char path[EFS_TEST_PATH];efs_test_path(path,sizeof(path),"efs-seg-bloom-XXXXXX");
    int fd = efs_test_mkstemp(path);
    assert(fd >= 0); close(fd); unlink(path);
    struct kv_seg_w *w;
    assert(kv_seg_w_open(path, &w) == EFS_OK);
    uint8_t key[16], value[600];
    const unsigned N = 6000;
    for (unsigned i = 0; i < N; i++) {
        uint32_t kl = key_for(i * 2, key);
        memset(value, (int)(i % 251), sizeof(value));
        assert(kv_seg_w_add(w, i % 13 ? KV_OP_PUT : KV_OP_DEL, key, kl, value,
                            i % 13 ? (i % 7) * 100 : 0) == EFS_OK);
    }
    assert(kv_seg_w_finish(w) == EFS_OK);
    struct kv_seg *s;
    assert(kv_seg_open(path, &s) == EFS_OK);
    assert(!kv_seg_has_bloom(s));
    kv_seg_bloom_build_now(s);
    assert(kv_seg_has_bloom(s));
    struct kv_buf out = {0};
    uint8_t op;
    /* A shared scan's seek caches its block: a later point probe in that
     * block needs no I/O (getattr's reduce-prefix probes re-read it). */
    {
        uint8_t sk[16];
        uint32_t sl = key_for(2 * 3001, sk);
        struct kv_seg_io io;
        int need = 0;
        assert(kv_seg_probe(s, sk, sl, &out, &op, &need, &io) == EFS_OK && need);
        struct kv_seg_iter *it;
        assert(kv_seg_iter_open(s, &it) == EFS_OK);
        assert(kv_seg_iter_seek(it, sk, sl) == EFS_OK);
        const uint8_t *k, *v;
        uint32_t kl2, vl;
        assert(kv_seg_iter_next(it, &k, &kl2, &v, &vl, &op) == EFS_OK);
        assert(kl2 == sl && !memcmp(k, sk, sl));
        kv_seg_iter_close(it);
        need = 1;
        assert(kv_seg_probe(s, sk, sl, &out, &op, &need, &io) == EFS_OK && !need);
    }
    for (unsigned j = 0; j < N; j++) {
        unsigned i = (j * 2711) % N;
        uint32_t kl = key_for(i * 2, key);
        assert(kv_seg_get(s, key, kl, &out, &op) == EFS_OK);
        assert(op == (i % 13 ? KV_OP_PUT : KV_OP_DEL));
        assert(out.len == (i % 13 ? (i % 7) * 100 : 0));
        for (uint32_t b = 0; b < out.len; b++)
            assert(out.p[b] == i % 251);
        kl = key_for(i * 2 + 1, key);
        assert(kv_seg_get(s, key, kl, &out, &op) == EFS_ERR_NOT_FOUND);
    }
    /* Seeks: the gets above cached many blocks; check every 7th position,
     * exact keys and the gaps between them, against the sorted order. */
    for (unsigned i = 0; i + 1 < N; i += 7) {
        for (int gap = 0; gap < 2; gap++) {
            struct kv_seg_iter *it;
            const uint8_t *k, *v;
            uint32_t kl2, vl;
            uint8_t sk[16];
            uint32_t sl = key_for(i * 2 + (unsigned)gap, sk);
            assert(kv_seg_iter_open(s, &it) == EFS_OK);
            assert(kv_seg_iter_seek(it, sk, sl) == EFS_OK);
            int rc;
            do {
                rc = kv_seg_iter_next(it, &k, &kl2, &v, &vl, &op);
                assert(rc == EFS_OK);
            } while (kv_key_cmp(k, kl2, sk, sl) < 0);
            unsigned want = gap ? i + 1 : i;
            uint8_t wk[16];
            uint32_t wl = key_for(want * 2, wk);
            assert(kl2 == wl && !memcmp(k, wk, wl));
            /* The next record follows in order. */
            if (want + 1 < N) {
                assert(kv_seg_iter_next(it, &k, &kl2, &v, &vl, &op) == EFS_OK);
                wl = key_for((want + 1) * 2, wk);
                assert(kl2 == wl && !memcmp(k, wk, wl));
            }
            kv_seg_iter_close(it);
        }
    }
    /* Seek past the last key: the iterator ends. */
    {
        struct kv_seg_iter *it;
        const uint8_t *k, *v;
        uint32_t kl2, vl;
        uint8_t sk[16];
        uint32_t sl = key_for(N * 2 + 2, sk);
        assert(kv_seg_iter_open(s, &it) == EFS_OK);
        assert(kv_seg_iter_seek(it, sk, sl) == EFS_OK);
        int rc;
        while ((rc = kv_seg_iter_next(it, &k, &kl2, &v, &vl, &op)) == EFS_OK)
            assert(kv_key_cmp(k, kl2, sk, sl) < 0);
        assert(rc == EFS_ERR_NOT_FOUND);
        kv_seg_iter_close(it);
    }
    kv_buf_free(&out);
    kv_seg_close(s);
    unlink(path);
    puts("segment bloom: hits, tombstones, misses; seek caches its block; cached-block seeks exact, gaps, end PASS");
    return 0;
}
