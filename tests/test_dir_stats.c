#include "efs/metadata.h"
#include "efs/common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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

    efs_export_free(&ex);

    if (failures == 0) {
        printf("test_dir_stats: OK\n");
        return 0;
    }
    printf("test_dir_stats: %d failures\n", failures);
    return 1;
}
