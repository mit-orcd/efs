/* Unit test for the client staging-table eviction primitives
 * (client-cache design Part A): efs_export_forget_ino +
 * efs_export_staged_bytes + efs_export_compact.
 *
 * No cluster needed: the client staging table is a plain in-memory
 * efs_export. forget_ino must drop EVERY staged trace of an ino — chunk
 * recs, the full row on the ino's shard tab, the dentry stub on the
 * parent's tab, and extra hardlink rows — while leaving unrelated rows
 * untouched. Everything it drops is re-fetchable from the servers, so
 * "gone from the cache" is the only observable effect here.
 */
#include "efs/metadata.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TROOT EFS_ROOT_INO

static void stage_file(struct efs_export *ex, efs_ino_t ino, efs_ino_t parent,
                       const char *name, uint32_t nchunks)
{
    /* Mirror the client create dual-apply: full row on the ino's shard tab,
     * dentry stub on the parent's tab when the two differ. */
    struct efs_export *ctab = efs_export_table_for_ino(ex, ino);
    struct efs_export *ptab = efs_export_table_for_ino(ex, parent);
    if (!ctab)
        ctab = ex;
    assert(efs_export_create_with_ino(ctab, ino, parent, S_IFREG | 0644,
                                      0, 0, name) == ino);
    if (ptab && ptab != ctab)
        (void)efs_export_create_with_ino(ptab, ino, parent, S_IFREG | 0644,
                                         0, 0, name);
    for (uint32_t c = 0; c < nchunks; c++) {
        efs_node_id_t nodes[EFS_NUM_FRAGMENTS] = {1, 2, 3};
        uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
        memset(sums, (int)(c + 1), sizeof(sums));
        assert(efs_export_set_chunk(ex, ino, c, nodes, sums) == EFS_OK);
    }
}

static int chunk_present(struct efs_export *ex, efs_ino_t ino, uint32_t ci)
{
    struct efs_chunk_entry ce;
    return efs_export_get_chunk(ex, ino, ci, &ce) == EFS_OK;
}

int main(void)
{
    struct efs_export ex;
    efs_export_init(&ex, 1, "t");
    /* Sharded like the live client, but small: 3 bits exercises the
     * shard-tab fan-out without pre-creating 4096 tables. Tabs are created
     * lazily by efs_export_table_for_ino (single-threaded test = no locks). */
    ex.root.shard_bits = 3;
    ex.root.shard_count = 8;

    /* A directory on shard 0 (ino 8: low 3 bits = 0). */
    assert(efs_export_create_with_ino(&ex, 8, TROOT, S_IFDIR | 0755,
                                      0, 0, "d") == 8);

    /* Files landing on different shards (low bits of the ino). */
    stage_file(&ex, 9, 8, "f1", 3);  /* shard 1 */
    stage_file(&ex, 10, 8, "f2", 2); /* shard 2 */
    stage_file(&ex, 11, 8, "f3", 0); /* shard 3, no chunks */

    /* A hardlink row for ino 9 under a second name (same parent dir). */
    assert(efs_export_link(&ex, 9, 8, "f1alias") == EFS_OK);

    uint64_t before = efs_export_staged_bytes(&ex);
    assert(before > 0);

    /* Sanity: everything staged is findable. */
    struct efs_inode out;
    assert(efs_export_get_inode(&ex, 9, &out) == EFS_OK);
    assert(efs_export_lookup(&ex, 8, "f1", &out) == EFS_OK);
    assert(efs_export_lookup(&ex, 8, "f1alias", &out) == EFS_OK);
    assert(chunk_present(&ex, 9, 0) && chunk_present(&ex, 9, 2));
    assert(chunk_present(&ex, 10, 0) && chunk_present(&ex, 10, 1));
    assert(efs_export_ino_has_chunks(&ex, 9));
    assert(!efs_export_ino_has_chunks(&ex, 11));

    /* Chunk maps drop first. The row and the name stay, so a later
     * report of a still-dirty file still finds them. ino 10 is clean
     * here; the evictor refuses a dirty ino before it calls this. */
    efs_export_drop_chunks_from(&ex, 10, 0);
    assert(!chunk_present(&ex, 10, 0) && !chunk_present(&ex, 10, 1));
    assert(!efs_export_ino_has_chunks(&ex, 10));
    assert(efs_export_get_inode(&ex, 10, &out) == EFS_OK);
    assert(efs_export_lookup(&ex, 8, "f2", &out) == EFS_OK);

    /* Forget ino 9: both name rows (f1, f1alias) and all 3 chunk recs go. */
    efs_export_forget_ino(&ex, 9);
    assert(efs_export_get_inode(&ex, 9, &out) != EFS_OK);
    assert(efs_export_lookup(&ex, 8, "f1", &out) != EFS_OK);
    assert(efs_export_lookup(&ex, 8, "f1alias", &out) != EFS_OK);
    assert(!chunk_present(&ex, 9, 0) && !chunk_present(&ex, 9, 2));

    /* The other files and the dir are untouched. */
    assert(efs_export_get_inode(&ex, 10, &out) == EFS_OK);
    assert(efs_export_lookup(&ex, 8, "f2", &out) == EFS_OK);
    assert(!chunk_present(&ex, 10, 0) && !chunk_present(&ex, 10, 1));
    assert(efs_export_get_inode(&ex, 8, &out) == EFS_OK);

    /* Forgetting an ino that was never staged is a no-op. */
    efs_export_forget_ino(&ex, 1234567);
    assert(efs_export_get_inode(&ex, 10, &out) == EFS_OK);

    /* Forget the rest. staged_bytes measures CAPACITY (slabs, arenas,
     * indexes) — eviction frees logically, compact reclaims the emptied
     * slabs and over-grown indexes. */
    efs_export_forget_ino(&ex, 10);
    efs_export_forget_ino(&ex, 11);
    efs_export_forget_ino(&ex, 8);
    assert(efs_export_get_inode(&ex, 10, &out) != EFS_OK);
    assert(efs_export_get_inode(&ex, 11, &out) != EFS_OK);
    assert(efs_export_get_inode(&ex, 8, &out) != EFS_OK);
    efs_export_compact(&ex);
    uint64_t after = efs_export_staged_bytes(&ex);
    assert(after < before);

    /* Compact must not corrupt the table: a fresh create afterwards still
     * works and is findable. */
    stage_file(&ex, 17, TROOT, "post", 1); /* shard 1 */
    assert(efs_export_get_inode(&ex, 17, &out) == EFS_OK);
    assert(efs_export_lookup(&ex, TROOT, "post", &out) == EFS_OK);
    assert(chunk_present(&ex, 17, 0));

    /* efs_export_evict_ino: the targeted two-phase evictor. Pass 1 drops
     * the chunk maps and keeps the rows; pass 2 drops the rows. */
    assert(efs_export_create_with_ino(&ex, 16, TROOT, S_IFDIR | 0755,
                                      0, 0, "d2") == 16);   /* shard 0 */
    stage_file(&ex, 25, 16, "small", 3);                    /* shard 1 */
    /* Size drives the chunk-group tabs visited; set it like a stat would. */
    assert(efs_export_set_size(&ex, 25, 3ull * ex.chunk_size) == EFS_OK);
    assert(efs_export_evict_ino(&ex, 25) == 1);
    assert(!chunk_present(&ex, 25, 0) && !chunk_present(&ex, 25, 2));
    assert(efs_export_get_inode(&ex, 25, &out) == EFS_OK);
    assert(efs_export_lookup(&ex, 16, "small", &out) == EFS_OK);
    assert(efs_export_evict_ino(&ex, 25) == 2);
    assert(efs_export_get_inode(&ex, 25, &out) != EFS_OK);
    assert(efs_export_lookup(&ex, 16, "small", &out) != EFS_OK);
    /* A hard link falls back to the fan-out and still drops both names. */
    stage_file(&ex, 26, 16, "h1", 1);                       /* shard 2 */
    assert(efs_export_link(&ex, 26, 16, "h2") == EFS_OK);
    assert(efs_export_evict_ino(&ex, 26) == 1);
    assert(efs_export_evict_ino(&ex, 26) == 2);
    assert(efs_export_lookup(&ex, 16, "h1", &out) != EFS_OK);
    assert(efs_export_lookup(&ex, 16, "h2", &out) != EFS_OK);
    /* A file whose chunk groups reach every shard fans out too. */
    stage_file(&ex, 27, 16, "big", 8u << EFS_CHUNK_GROUP_SHIFT); /* shard 3 */
    assert(efs_export_set_size(&ex, 27,
                               (8ull << EFS_CHUNK_GROUP_SHIFT) * ex.chunk_size)
           == EFS_OK);
    assert(efs_export_evict_ino(&ex, 27) == 1);
    for (uint32_t c = 0; c < (8u << EFS_CHUNK_GROUP_SHIFT); c += 7)
        assert(!chunk_present(&ex, 27, c));
    assert(efs_export_evict_ino(&ex, 27) == 2);
    assert(efs_export_get_inode(&ex, 27, &out) != EFS_OK);
    /* Unknown ino: nothing to do, table intact. */
    assert(efs_export_evict_ino(&ex, 7654321) == 2);
    assert(efs_export_get_inode(&ex, 17, &out) == EFS_OK);

    efs_export_free(&ex);
    printf("test_stage_evict: OK\n");
    return 0;
}
