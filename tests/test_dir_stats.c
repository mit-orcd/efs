#include "efs/metadata.h"
#include "efs/common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <sys/stat.h>

static int failures = 0;

static void expect_u64(const char *label, uint64_t got, uint64_t want)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %llu want %llu\n", label,
                (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static struct efs_inode get_dir(struct efs_export *ex, efs_ino_t ino)
{
    struct efs_inode out;
    memset(&out, 0, sizeof(out));
    if (efs_export_get_inode(ex, ino, &out) != 0)
        failures++;
    return out;
}

int main(void)
{
    struct efs_export ex;
    efs_export_init(&ex, 1, "stats");

    efs_ino_t d1 = efs_export_create(&ex, EFS_ROOT_INO, S_IFDIR | 0755, 0, 0, "d1");
    efs_ino_t d2 = efs_export_create(&ex, d1, S_IFDIR | 0755, 0, 0, "d2");
    efs_ino_t f1 = efs_export_create(&ex, d1, S_IFREG | 0644, 0, 0, "f1");
    efs_ino_t f2 = efs_export_create(&ex, d2, S_IFREG | 0644, 0, 0, "f2");
    if (!d1 || !d2 || !f1 || !f2) {
        fprintf(stderr, "FAIL create\n");
        return 1;
    }

    efs_export_set_size(&ex, f1, 100);
    efs_export_set_size(&ex, f2, 50);

    struct efs_inode root = get_dir(&ex, EFS_ROOT_INO);
    expect_u64("root.imm_dirs", root.imm_dirs, 1);
    expect_u64("root.imm_files", root.imm_files, 0);
    expect_u64("root.tree_dirs", root.tree_dirs, 2);
    expect_u64("root.tree_files", root.tree_files, 2);
    expect_u64("root.tree_bytes", root.tree_bytes, 150);

    struct efs_inode id1 = get_dir(&ex, d1);
    expect_u64("d1.imm_dirs", id1.imm_dirs, 1);
    expect_u64("d1.imm_files", id1.imm_files, 1);
    expect_u64("d1.imm_bytes", id1.imm_bytes, 100);
    expect_u64("d1.tree_dirs", id1.tree_dirs, 1);
    expect_u64("d1.tree_files", id1.tree_files, 2);
    expect_u64("d1.tree_bytes", id1.tree_bytes, 150);

    struct efs_inode id2 = get_dir(&ex, d2);
    expect_u64("d2.imm_files", id2.imm_files, 1);
    expect_u64("d2.imm_bytes", id2.imm_bytes, 50);
    expect_u64("d2.tree_files", id2.tree_files, 1);
    expect_u64("d2.tree_bytes", id2.tree_bytes, 50);

    /* Reserved name */
    if (efs_export_create(&ex, EFS_ROOT_INO, S_IFREG | 0644, 0, 0, EFS_STATS_NAME) != 0) {
        fprintf(stderr, "FAIL .stats create should be rejected\n");
        failures++;
    }

    /* Rename f2 up to d1 */
    if (efs_export_rename(&ex, f2, d1, "f2moved") != 0) {
        fprintf(stderr, "FAIL rename\n");
        failures++;
    }
    id1 = get_dir(&ex, d1);
    id2 = get_dir(&ex, d2);
    root = get_dir(&ex, EFS_ROOT_INO);
    expect_u64("after rename d1.imm_files", id1.imm_files, 2);
    expect_u64("after rename d1.imm_bytes", id1.imm_bytes, 150);
    expect_u64("after rename d2.imm_files", id2.imm_files, 0);
    expect_u64("after rename d2.tree_bytes", id2.tree_bytes, 0);
    expect_u64("after rename root.tree_bytes", root.tree_bytes, 150);

    /* Unlink f1 */
    if (efs_export_unlink_name(&ex, d1, "f1") != 0) {
        fprintf(stderr, "FAIL unlink\n");
        failures++;
    }
    id1 = get_dir(&ex, d1);
    root = get_dir(&ex, EFS_ROOT_INO);
    expect_u64("after unlink d1.imm_files", id1.imm_files, 1);
    expect_u64("after unlink d1.tree_bytes", id1.tree_bytes, 50);
    expect_u64("after unlink root.tree_bytes", root.tree_bytes, 50);

    /* Format text */
    char text[512];
    int n = efs_export_format_stats(&id1, text, sizeof(text));
    if (n <= 0 || !strstr(text, "imm_files=1") || !strstr(text, "tree_bytes=50")) {
        fprintf(stderr, "FAIL format_stats:\n%s\n", text);
        failures++;
    }

    /* Serialize / deserialize round-trip */
    char *blob = NULL;
    size_t blen = 0;
    if (efs_export_serialize(&ex, &blob, &blen) != 0) {
        fprintf(stderr, "FAIL serialize\n");
        failures++;
    } else {
        struct efs_export ex2;
        efs_export_init(&ex2, 0, "");
        if (efs_export_deserialize(&ex2, blob, blen) != 0) {
            fprintf(stderr, "FAIL deserialize\n");
            failures++;
        } else {
            struct efs_inode r2 = get_dir(&ex2, EFS_ROOT_INO);
            expect_u64("deser root.tree_bytes", r2.tree_bytes, 50);
            expect_u64("deser root.tree_files", r2.tree_files, 1);
        }
        efs_export_free(&ex2);
        free(blob);
    }

    /* Full recompute matches incremental */
    efs_export_recompute_rollups(&ex);
    id1 = get_dir(&ex, d1);
    expect_u64("recompute d1.tree_bytes", id1.tree_bytes, 50);

    /* Deferred rollups: norollup then ensure restores dir bytes. */
    efs_ino_t f3 = efs_export_create(&ex, d1, S_IFREG | 0644, 0, 0, "f3");
    if (!f3) {
        fprintf(stderr, "FAIL create f3\n");
        failures++;
    } else {
        efs_export_set_size_norollup(&ex, f3, 25);
        if (!ex.rollups_stale) {
            fprintf(stderr, "FAIL expected rollups_stale after norollup\n");
            failures++;
        }
        efs_export_ensure_rollups(&ex);
        if (ex.rollups_stale) {
            fprintf(stderr, "FAIL rollups_stale still set after ensure\n");
            failures++;
        }
        id1 = get_dir(&ex, d1);
        root = get_dir(&ex, EFS_ROOT_INO);
        expect_u64("norollup d1.tree_bytes", id1.tree_bytes, 75);
        expect_u64("norollup root.tree_bytes", root.tree_bytes, 75);
    }

    /* Child-vec table at capacity used to leave needs_inode_grow stuck
     * because reserve_inodes ignored it — create then spun and failed. */
    ex.child_vec_cap = ex.child_vec_count;
    if (!efs_export_needs_inode_grow(&ex)) {
        fprintf(stderr, "FAIL expected needs_grow when child vecs full\n");
        failures++;
    }
    if (efs_export_reserve_inodes(&ex, 64) != 0) {
        fprintf(stderr, "FAIL reserve_inodes with full child vecs\n");
        failures++;
    }
    if (efs_export_needs_inode_grow(&ex)) {
        fprintf(stderr, "FAIL needs_grow after child-vec reserve "
                "(count=%llu cap=%llu)\n",
                (unsigned long long)ex.child_vec_count,
                (unsigned long long)ex.child_vec_cap);
        failures++;
    }
    efs_ino_t extra = efs_export_create(&ex, EFS_ROOT_INO, S_IFDIR | 0755, 0, 0,
                                       "extra-after-vec-full");
    if (!extra) {
        fprintf(stderr, "FAIL create after child-vec reserve\n");
        failures++;
    }

    /* Two ino namespaces with the same low counters must both resolve.
     * A raw (ino & mask) index collapses tag<<40 and walks the first
     * namespace's run on every second-namespace lookup. */
    {
        struct efs_export ns;
        efs_export_init(&ns, 2, "ns");
        efs_ino_t a = efs_export_create_with_ino(&ns, (1ULL << 40) | 5,
                                                EFS_ROOT_INO, S_IFREG | 0644,
                                                0, 0, "a");
        efs_ino_t b = efs_export_create_with_ino(&ns, (2ULL << 40) | 5,
                                                EFS_ROOT_INO, S_IFREG | 0644,
                                                0, 0, "b");
        struct efs_inode ia, ib;
        if (!a || !b ||
            efs_export_get_inode(&ns, a, &ia) != 0 ||
            efs_export_get_inode(&ns, b, &ib) != 0 ||
            ia.ino != a || ib.ino != b ||
            strcmp(ia.name, "a") != 0 || strcmp(ib.name, "b") != 0) {
            fprintf(stderr, "FAIL two-namespace ino index a=%llu b=%llu\n",
                    (unsigned long long)a, (unsigned long long)b);
            failures++;
        }
        efs_export_free(&ns);
    }

    /* ino==0 + leftover name must not be listed (readdir vs lookup split). */
    {
        efs_ino_t g = efs_export_create(&ex, d1, S_IFREG | 0644, 0, 0, "ghost.jpg");
        char *blob = NULL;
        size_t blen = 0;
        if (!g || efs_export_serialize(&ex, &blob, &blen) != 0) {
            fprintf(stderr, "FAIL ghost create/serialize\n");
            failures++;
        } else {
            int found = 0;
            size_t compact_off = EFS_META_HDR_SIZE;
            size_t compact_bytes = (size_t)ex.inode_count * EFS_INODE_COMPACT_SIZE;
            if (compact_off + compact_bytes < blen) {
                const char *dents = blob + compact_off + compact_bytes;
                size_t dent_off = 0;
                size_t dent_lim = blen - (compact_off + compact_bytes);
                for (uint64_t i = 0; i < ex.inode_count && dent_off + 2 <= dent_lim;
                     i++) {
                    uint16_t ln = 0;
                    memcpy(&ln, dents + dent_off, 2);
                    dent_off += 2;
                    if (dent_off + ln > dent_lim)
                        break;
                    if (ln == 9 && memcmp(dents + dent_off, "ghost.jpg", 9) == 0) {
                        memset(blob + compact_off + i * EFS_INODE_COMPACT_SIZE,
                               0, 8);
                        found = 1;
                        break;
                    }
                    dent_off += ln;
                }
            }
            struct efs_export loaded;
            struct efs_inode chk;
            if (!found || efs_export_deserialize(&loaded, blob, blen) != 0) {
                fprintf(stderr, "FAIL ghost load found=%d\n", found);
                failures++;
            } else if (efs_export_lookup(&loaded, d1, "ghost.jpg", &chk) == 0) {
                fprintf(stderr, "FAIL ghost lookup should miss\n");
                failures++;
                efs_export_free(&loaded);
            } else if (efs_export_lookup(&loaded, d1, "f3", &chk) != 0) {
                fprintf(stderr, "FAIL f3 missing after dropping ino==0\n");
                failures++;
                efs_export_free(&loaded);
            } else {
                efs_export_free(&loaded);
            }
        }
        free(blob);
    }

    efs_export_free(&ex);

    if (failures == 0) {
        printf("test_dir_stats: OK\n");
        return 0;
    }
    printf("test_dir_stats: %d failures\n", failures);
    return 1;
}
