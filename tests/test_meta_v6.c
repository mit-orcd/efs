#include "efs/metadata.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

static int failures = 0;

static void expect_str(const char *label, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s: got '%s' want '%s'\n", label, got, want);
        failures++;
    }
}

int main(void)
{
    struct efs_export ex;
    efs_export_init(&ex, 7, "v6");
    efs_ino_t d = efs_export_create(&ex, EFS_ROOT_INO, S_IFDIR | 0755, 0, 0, "dir");
    efs_ino_t f = efs_export_create(&ex, d, S_IFREG | 0644, 0, 0, "short.jpg");
    if (!d || !f) {
        fprintf(stderr, "FAIL create\n");
        return 1;
    }
    efs_export_set_size(&ex, f, 1234);
    /* "/" + "dir" + "short.jpg" packed dentries */
    uint64_t want_dent = (2 + 1) + (2 + 3) + (2 + 9);
    if (ex.dentry_bytes != want_dent) {
        fprintf(stderr, "FAIL dentry_bytes %llu want %llu\n",
                (unsigned long long)ex.dentry_bytes,
                (unsigned long long)want_dent);
        failures++;
    }

    char *blob = NULL;
    size_t blen = 0;
    if (efs_export_serialize(&ex, &blob, &blen) != 0) {
        fprintf(stderr, "FAIL serialize\n");
        return 1;
    }
    uint32_t ver = 0;
    memcpy(&ver, blob + 4, 4);
    if (ver != EFS_META_EFSM_V6) {
        fprintf(stderr, "FAIL efsm version %u\n", ver);
        failures++;
    }
    /* Compact + short names must beat the v5 420 B/inode blob. */
    size_t v5 = (size_t)EFS_META_HDR_SIZE +
                ex.inode_count * EFS_INODE_WIRE_SIZE +
                ex.chunk_count * EFS_CHUNK_WIRE_SIZE;
    if (blen >= v5) {
        fprintf(stderr, "FAIL v6 blob %zu not smaller than v5 %zu\n", blen, v5);
        failures++;
    }

    struct efs_export ex2;
    efs_export_init(&ex2, 0, "");
    if (efs_export_deserialize(&ex2, blob, blen) != 0) {
        fprintf(stderr, "FAIL deserialize\n");
        failures++;
    } else {
        struct efs_inode out;
        if (efs_export_lookup(&ex2, d, "short.jpg", &out) != 0) {
            fprintf(stderr, "FAIL lookup after v6 round-trip\n");
            failures++;
        } else {
            expect_str("name", out.name, "short.jpg");
            if (out.size != 1234) {
                fprintf(stderr, "FAIL size %llu\n",
                        (unsigned long long)out.size);
                failures++;
            }
        }
        if (ex2.efsm_version != EFS_META_EFSM_V6) {
            fprintf(stderr, "FAIL efsm_version %u\n", ex2.efsm_version);
            failures++;
        }
        if (ex2.dentry_bytes != want_dent) {
            fprintf(stderr, "FAIL dentry_bytes after load %llu want %llu\n",
                    (unsigned long long)ex2.dentry_bytes,
                    (unsigned long long)want_dent);
            failures++;
        }
    }

    char stats[1024];
    struct efs_inode root;
    efs_export_get_inode(&ex, EFS_ROOT_INO, &root);
    if (efs_export_format_stats_ex(&ex, &root, stats, sizeof(stats)) <= 0 ||
        !strstr(stats, "meta_ino_pages=") ||
        !strstr(stats, "meta_ino_pages_max=16384")) {
        fprintf(stderr, "FAIL stats:\n%s\n", stats);
        failures++;
    }

    if (efs_export_shard_of(1, 0) != 0 || efs_export_shard_of(1ULL << 10, 10) != 1) {
        fprintf(stderr, "FAIL shard_of\n");
        failures++;
    }
    if (efs_meta_shard_table_ino(0) != EFS_META_TABLE_INO ||
        efs_meta_shard_table_ino(3) != EFS_META_TABLE_INO + 3) {
        fprintf(stderr, "FAIL shard table ino\n");
        failures++;
    }

    efs_export_free(&ex2);
    efs_export_free(&ex);
    free(blob);
    if (failures) {
        printf("test_meta_v6: %d failures\n", failures);
        return 1;
    }
    printf("test_meta_v6: OK\n");
    return 0;
}
