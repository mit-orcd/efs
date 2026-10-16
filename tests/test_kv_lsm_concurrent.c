/* Concurrent LSM readers against a writer that forces flushes and
 * compactions: every value a reader sees is well formed, a get never
 * returns a version older than one committed before it started, and
 * prefix scans stay ordered. Run under -fsanitize=thread too. */
#include "efs/kv.h"
#include "efs/kv_lsm.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NKEYS 1500u
#define VLEN 96u
#define READERS 6
static struct efs_kv *kv;
static uint64_t committed[NKEYS];
static int stop;
static uint64_t reads, scans, writes;

static uint32_t key_of(uint32_t i, int tomb, uint8_t k[8])
{
    k[0] = tomb ? 9 : 1; k[1] = 0; k[2] = (uint8_t)(i / 100); /* prefix groups */
    k[3] = (uint8_t)(i >> 16); k[4] = (uint8_t)(i >> 8); k[5] = (uint8_t)i;
    return 6;
}

static void value_of(uint64_t v, uint8_t *out)
{
    for (int b = 0; b < 8; b++) out[b] = (uint8_t)(v >> (56 - 8 * b));
    memset(out + 8, (int)(v & 0xff), VLEN - 8);
}

static uint64_t check_value(const uint8_t *p, uint32_t n)
{
    assert(n == VLEN);
    uint64_t v = 0;
    for (int b = 0; b < 8; b++) v = (v << 8) | p[b];
    for (uint32_t i = 8; i < VLEN; i++) assert(p[i] == (uint8_t)(v & 0xff));
    return v;
}

static void *writer(void *arg)
{
    (void)arg;
    uint8_t k[8], val[VLEN];
    for (uint64_t round = 1; !__atomic_load_n(&stop, __ATOMIC_ACQUIRE); round++) {
        for (uint32_t i = 0; i < NKEYS; i++) {
            uint32_t kl = key_of(i, 0, k);
            value_of(round, val);
            assert(efs_kv_put(kv, k, kl, val, VLEN) == EFS_OK);
            __atomic_store_n(&committed[i], round, __ATOMIC_RELEASE);
            /* A tombstone churn range readers only see as absent or well formed. */
            kl = key_of(i, 1, k);
            if ((i + round) % 3) assert(efs_kv_put(kv, k, kl, val, VLEN) == EFS_OK);
            else (void)efs_kv_del(kv, k, kl);
            __atomic_add_fetch(&writes, 2, __ATOMIC_RELAXED);
        }
        if (round % 2 == 0) (void)efs_kv_lsm_flush_nowait(kv);
        if (round % 5 == 0) (void)efs_kv_lsm_compact(kv);
    }
    return NULL;
}

struct scan_acc { uint8_t last[16]; uint32_t last_len; int have; uint32_t n; };
static int scan_cb(void *u, const uint8_t *key, uint32_t klen, const uint8_t *v, uint32_t vlen)
{
    struct scan_acc *a = u;
    if (a->have) {
        uint32_t m = klen < a->last_len ? klen : a->last_len;
        int c = memcmp(a->last, key, m);
        assert(c < 0 || (c == 0 && a->last_len < klen));
    }
    memcpy(a->last, key, klen); a->last_len = klen; a->have = 1; a->n++;
    (void)check_value(v, vlen);
    return 0;
}

static void *reader(void *arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg;
    uint8_t k[8], val[VLEN];
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        uint32_t i = (uint32_t)rand_r(&seed) % NKEYS;
        if (rand_r(&seed) % 16 == 0) {
            struct scan_acc a = {0};
            uint8_t pre[3] = {1, 0, (uint8_t)(i / 100)};
            assert(efs_kv_scan_prefix(kv, pre, 3, scan_cb, &a) == EFS_OK);
            uint8_t tpre[3] = {9, 0, (uint8_t)(i / 100)};
            struct scan_acc b = {0};
            assert(efs_kv_scan_prefix(kv, tpre, 3, scan_cb, &b) == EFS_OK);
            __atomic_add_fetch(&scans, 1, __ATOMIC_RELAXED);
            continue;
        }
        uint64_t before = __atomic_load_n(&committed[i], __ATOMIC_ACQUIRE);
        uint32_t kl = key_of(i, 0, k), n = VLEN;
        int rc = efs_kv_get(kv, k, kl, val, &n);
        if (before) {
            assert(rc == EFS_OK);
            assert(check_value(val, n) >= before);
        } else {
            assert(rc == EFS_OK || rc == EFS_ERR_NOT_FOUND);
            if (rc == EFS_OK) (void)check_value(val, n);
        }
        kl = key_of(i, 1, k); n = VLEN;
        rc = efs_kv_get(kv, k, kl, val, &n);
        assert(rc == EFS_OK || rc == EFS_ERR_NOT_FOUND);
        if (rc == EFS_OK) (void)check_value(val, n);
        __atomic_add_fetch(&reads, 2, __ATOMIC_RELAXED);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    unsigned secs = argc > 1 ? (unsigned)atoi(argv[1]) : 4;
    char dir[] = "/tmp/efs-kv-concurrent-XXXXXX";
    const char *tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/efs-kv-concurrent-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    char *d = mkdtemp(path);
    (void)dir;
    assert(d);
    efs_kv_lsm_bloom_enable(1);
    struct efs_kv_lsm_cfg cfg = {.sync_mode = EFS_KV_LSM_NOSYNC, .memtable_max = 96 * 1024, .l0_max = 2};
    kv = efs_kv_lsm_open(d, &cfg);
    assert(kv);
    pthread_t w, r[READERS];
    assert(!pthread_create(&w, NULL, writer, NULL));
    for (uintptr_t i = 0; i < READERS; i++) assert(!pthread_create(&r[i], NULL, reader, (void *)(i + 1)));
    sleep(secs);
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    pthread_join(w, NULL);
    for (int i = 0; i < READERS; i++) pthread_join(r[i], NULL);
    uint32_t l0 = 0, l1 = 0;
    assert(efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK);
    assert(l0 + l1 > 0); /* flushes happened under the readers */
    efs_kv_lsm_close(kv);
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", d);
    (void)system(cmd);
    printf("kv concurrent: %llu writes, %llu gets, %llu scans with flush/compaction; "
           "monotonic gets, well-formed values, ordered scans PASS\n",
           (unsigned long long)writes, (unsigned long long)reads, (unsigned long long)scans);
    return 0;
}
