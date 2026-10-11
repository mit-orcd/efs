/* Private randomized files-backend soak. No metadata admission or mounted
 * throughput claim: exercises concurrent immutable PUT/GET, EC, fences and GC. */
#define _XOPEN_SOURCE 700
#include "../../src/server/store.c"
#include "efs/packed_segment.h"
#include <assert.h>
#include <ftw.h>
#include <pthread.h>
#include <time.h>
static struct efsd_server server;
static uint64_t next_id, cohorts, payload_bytes, fragment_bytes;
static unsigned wave;
static int cleanup(const char *p, const struct stat *s, int t, struct FTW *f)
{ (void)s; (void)t; (void)f; return remove(p); }
static uint32_t random32(uint64_t *state)
{ *state ^= *state << 13; *state ^= *state >> 7; *state ^= *state << 17; return (uint32_t)*state; }
static void *worker(void *arg)
{
    uint64_t rng = (uint64_t)(uintptr_t)arg + 1u + wave * 7919u;
    uint8_t *image = malloc(EFS_SEGMENT_MAX_SIZE), *decoded = malloc(EFS_SEGMENT_MAX_SIZE), *payload = malloc(32 * 4096);
    uint8_t *fragments, *read; assert(image && decoded && payload);
    assert(!posix_memalign((void **)&fragments, 4096, 3u * EFS_SEGMENT_MAX_SIZE / 2));
    assert(!posix_memalign((void **)&read, 4096, 3u * EFS_SEGMENT_MAX_SIZE / 2));
    for (unsigned turn = 0; turn < 16; turn++) {
        struct efs_pack_record records[32]; struct efs_pack_ref refs[32];
        uint32_t count = random32(&rng) % 32 + 1, total = 0;
        uint64_t id = __atomic_add_fetch(&next_id, 1, __ATOMIC_RELAXED);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t len = random32(&rng) % 2048 + 1;
            uint8_t *p = payload + 4096 * i;
            for (uint32_t j = 0; j < len; j++) p[j] = random32(&rng);
            records[i] = (struct efs_pack_record){id * 128 + i * 2 + 1, 7, 0, id + i + 1, len, p}; total += len;
        }
        size_t size; assert(!efs_pack_build(id, wave + 1, 0, records, count, image, EFS_SEGMENT_MAX_SIZE, &size, refs));
        uint32_t fl = size / 2; uint8_t *f[] = {fragments, fragments + fl, fragments + 2 * fl};
        uint8_t *r[] = {read, read + fl, read + 2 * fl}, sum[3][EFS_HASH_SIZE];
        assert(!efs_encode_segment(image, size, size, f));
        for (unsigned i = 0; i < 3; i++) {
            efs_hash(f[i], fl, sum[i]);
            assert(!server_write_packed_fragment(&server, 1, id, wave + 1, i, f[i], fl, sum[i]));
            assert(!server_write_packed_fragment(&server, 1, id, wave + 1, i, f[i], fl, sum[i]));
            uint32_t cap = fl; uint8_t actual[32];
            assert(!server_read_packed_fragment(&server, 1, id, wave + 1, i, fl, r[i], &cap, actual));
            assert(cap == fl && !memcmp(actual, sum[i], 32));
        }
        unsigned missing = random32(&rng) % 3;
        memset(r[missing], 0, fl); assert(!efs_decode_segment(r, size, (missing + 1) % 3, (missing + 2) % 3, missing, decoded, size));
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *p; uint32_t len;
            assert(!efs_pack_read(decoded, size, refs + i, &p, &len));
            assert(len == records[i].len && !memcmp(p, records[i].data, len));
        }
        assert(!server_fence_packed_object(&server, 1, id, wave + 1));
        assert(server_write_packed_fragment(&server, 1, id, wave + 1, 0, f[0], fl, sum[0]) == EFS_ERR_STALE);
        for (unsigned i = 0; i < 3; i++) {
            assert(!server_delete_packed_fragment(&server, 1, id, wave + 1, i, fl, sum[i]));
            assert(!server_delete_packed_fragment(&server, 1, id, wave + 1, i, fl, sum[i]));
        }
        __atomic_add_fetch(&cohorts, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&payload_bytes, total, __ATOMIC_RELAXED);
        __atomic_add_fetch(&fragment_bytes, 3u * fl, __ATOMIC_RELAXED);
    }
    free(image); free(decoded); free(payload); free(fragments); free(read); return NULL;
}
int main(int argc, char **argv)
{
    unsigned duration = argc > 1 ? strtoul(argv[1], NULL, 10) : 60;
    const char *base = argc > 2 ? argv[2] : "/tmp";
    assert(duration && duration <= 3600); uint64_t start = (uint64_t)time(NULL);
    do {
        char root[8192]; snprintf(root, sizeof(root), "%s/efs-pack-soak-XXXXXX", base); assert(mkdtemp(root));
        memset(&server, 0, sizeof(server)); server.id = 1; server.node_count = 1; server.nodes[0].id = 1;
        server.direct_io = wave % 2; server.storage_path_count = 2; server.quota = 32u << 20;
        pthread_mutex_init(&server.lock, NULL);
        for (unsigned i = 0; i < 2; i++) { snprintf(server.storage_paths[i], EFS_MAX_PATH, "%s/r%u", root, i); assert(!mkdir(server.storage_paths[i], 0755)); }
        strcpy(server.storage_path, server.storage_paths[0]); assert(!server_storage_engine_init(&server));
        pthread_t threads[8];
        for (unsigned i = 0; i < 8; i++) assert(!pthread_create(threads + i, NULL, worker, (void *)(uintptr_t)i));
        for (unsigned i = 0; i < 8; i++) assert(!pthread_join(threads[i], NULL));
        assert(!server.nodes[0].used);
        server_storage_engine_close(&server); assert(!server_storage_engine_init(&server)); server_init_local_usage(&server);
        assert(!server.nodes[0].used); server_storage_engine_close(&server); pthread_mutex_destroy(&server.lock);
        assert(!nftw(root, cleanup, 64, FTW_DEPTH | FTW_PHYS)); wave++;
        printf("wave=%u cohorts=%llu payload=%llu fragment_bytes=%llu elapsed=%llu\n", wave,
            (unsigned long long)cohorts, (unsigned long long)payload_bytes, (unsigned long long)fragment_bytes,
            (unsigned long long)((uint64_t)time(NULL) - start)); fflush(stdout);
    } while ((uint64_t)time(NULL) - start < duration);
    puts("concurrent packed storage soak: randomized records, buffered/direct, exact retries, EC fallback, fences, GC and cold quota PASS");
}
