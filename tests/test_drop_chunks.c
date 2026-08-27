#include "efs/metadata.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

/* Focused gate for the drop_chunks per-ino-count fast path: a full drop must
 * remove EVERY chunk for the ino (no leak) and a partial truncate must remove
 * exactly the chunks >= first_chunk (no over/under-drop), across dense,
 * sparse, and sharded layouts. */

static int failures = 0;

static uint64_t total_chunks(struct efs_export *ex)
{
    uint64_t n = ex->chunk_count;
    if (ex->shard_tabs)
        for (uint32_t i = 0; i < ex->shard_tab_cap; i++)
            if (ex->shard_tabs[i])
                n += ex->shard_tabs[i]->chunk_count;
    return n;
}

static void check(const char *label, uint64_t got, uint64_t want)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %llu want %llu\n", label,
                (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static void add_chunks(struct efs_export *ex, efs_ino_t ino,
                       const uint32_t *cis, int n)
{
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS] = {1, 2, 3};
    uint8_t cks[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
    memset(cks, 0xab, sizeof(cks));
    for (int i = 0; i < n; i++) {
        if (efs_export_set_chunk(ex, ino, cis[i], nodes, cks) != 0) {
            fprintf(stderr, "FAIL set_chunk ino=%llu ci=%u\n",
                    (unsigned long long)ino, cis[i]);
            failures++;
            return;
        }
    }
}

static void test_dense(void)
{
    struct efs_export ex;
    efs_export_init(&ex, 1, "dense");
    efs_ino_t f = efs_export_create(&ex, EFS_ROOT_INO, S_IFREG | 0644, 0, 0, "f");
    uint32_t cis[64];
    for (uint32_t i = 0; i < 64; i++)
        cis[i] = i;
    add_chunks(&ex, f, cis, 64);
    check("dense before", total_chunks(&ex), 64);
    efs_export_drop_chunks_from(&ex, f, 0);
    check("dense after", total_chunks(&ex), 0);
    struct efs_chunk_entry ce;
    check("dense gone", efs_export_get_chunk(&ex, f, 0, &ce) == 0, 0);
    efs_export_free(&ex);
}

static void test_sparse(void)
{
    struct efs_export ex;
    efs_export_init(&ex, 1, "sparse");
    efs_ino_t f = efs_export_create(&ex, EFS_ROOT_INO, S_IFREG | 0644, 0, 0, "f");
    /* Sparse: holes between present chunks, incl. past the probe headroom. */
    uint32_t cis[] = {0, 1, 5, 63, 64, 130, 1000, 50000};
    int n = sizeof(cis) / sizeof(cis[0]);
    add_chunks(&ex, f, cis, n);
    check("sparse before", total_chunks(&ex), (uint64_t)n);
    efs_export_drop_chunks_from(&ex, f, 0);
    check("sparse after", total_chunks(&ex), 0);
    efs_export_free(&ex);
}

static void test_truncate(void)
{
    struct efs_export ex;
    efs_export_init(&ex, 1, "trunc");
    efs_ino_t f = efs_export_create(&ex, EFS_ROOT_INO, S_IFREG | 0644, 0, 0, "f");
    uint32_t cis[32];
    for (uint32_t i = 0; i < 32; i++)
        cis[i] = i;
    add_chunks(&ex, f, cis, 32);
    efs_export_drop_chunks_from(&ex, f, 20); /* drop ci >= 20, keep 0..19 */
    check("truncate after", total_chunks(&ex), 20);
    struct efs_chunk_entry ce;
    check("truncate kept 0", efs_export_get_chunk(&ex, f, 0, &ce) == 0, 1);
    check("truncate kept 19", efs_export_get_chunk(&ex, f, 19, &ce) == 0, 1);
    check("truncate dropped 20", efs_export_get_chunk(&ex, f, 20, &ce) == 0, 0);
    check("truncate dropped 31", efs_export_get_chunk(&ex, f, 31, &ce) == 0, 0);
    efs_export_free(&ex);
}

static void test_multi(void)
{
    struct efs_export ex;
    efs_export_init(&ex, 1, "multi");
    efs_ino_t f = efs_export_create(&ex, EFS_ROOT_INO, S_IFREG | 0644, 0, 0, "f");
    efs_ino_t g = efs_export_create(&ex, EFS_ROOT_INO, S_IFREG | 0644, 0, 0, "g");
    uint32_t cis[16];
    for (uint32_t i = 0; i < 16; i++)
        cis[i] = i;
    add_chunks(&ex, f, cis, 16);
    add_chunks(&ex, g, cis, 16);
    check("multi before", total_chunks(&ex), 32);
    efs_export_drop_chunks_from(&ex, f, 0);
    check("multi after drop f", total_chunks(&ex), 16);
    struct efs_chunk_entry ce;
    check("multi g intact", efs_export_get_chunk(&ex, g, 7, &ce) == 0, 1);
    efs_export_free(&ex);
}

static void test_unlink_name(void)
{
    struct efs_export ex;
    efs_export_init(&ex, 1, "unl");
    efs_ino_t f = efs_export_create(&ex, EFS_ROOT_INO, S_IFREG | 0644, 0, 0, "f");
    uint32_t cis[40];
    for (uint32_t i = 0; i < 40; i++)
        cis[i] = i * 3; /* sparse-ish */
    add_chunks(&ex, f, cis, 40);
    check("unlink before", total_chunks(&ex), 40);
    if (efs_export_unlink_name(&ex, EFS_ROOT_INO, "f") != 0) {
        fprintf(stderr, "FAIL unlink_name\n");
        failures++;
    }
    check("unlink after", total_chunks(&ex), 0);
    efs_export_free(&ex);
}

static void test_sharded(void)
{
    struct efs_export ex;
    efs_export_init(&ex, 1, "shard");
    ex.root.shard_bits = 3;
    ex.root.shard_count = 8;
    ex.create_stride = 1;
    efs_ino_t f = efs_export_create(&ex, EFS_ROOT_INO, S_IFREG | 0644, 0, 0, "f");
    /* 200 chunks span multiple 64-chunk groups -> multiple shard tables. */
    uint32_t cis[200];
    for (uint32_t i = 0; i < 200; i++)
        cis[i] = i;
    add_chunks(&ex, f, cis, 200);
    check("sharded before", total_chunks(&ex), 200);
    efs_export_drop_chunks_from(&ex, f, 0);
    check("sharded after", total_chunks(&ex), 0);
    efs_export_free(&ex);
}

int main(void)
{
    test_dense();
    test_sparse();
    test_truncate();
    test_multi();
    test_unlink_name();
    test_sharded();
    if (failures) {
        fprintf(stderr, "drop_chunks: %d FAILURES\n", failures);
        return 1;
    }
    printf("drop_chunks: ALL PASS\n");
    return 0;
}
