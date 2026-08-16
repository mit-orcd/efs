#include "efs/metadata.h"
#include "src/client/client_internal.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <pthread.h>

int main(void)
{
    struct efs_export ex;
    efs_export_init(&ex, 1, "cap");
    assert(efs_export_fits_page_cap(&ex, 1, 0));
    assert(efs_export_fits_page_cap(&ex, 0, 1));

    /* A request that would need more than 16k inode pages must fail. */
    uint64_t too_many = ((uint64_t)EFS_META_INO_PAGE_MAX * EFS_META_PAGE_SIZE) /
                            EFS_INODE_COMPACT_SIZE +
                        1000;
    if (efs_export_fits_page_cap(&ex, too_many, 0)) {
        fprintf(stderr, "FAIL: huge extra_inodes still fits\n");
        return 1;
    }
    uint64_t too_chunks = ((uint64_t)EFS_META_CHUNK_PAGE_MAX * EFS_META_PAGE_SIZE) /
                              EFS_CHUNK_WIRE_SIZE +
                          1000;
    if (efs_export_fits_page_cap(&ex, 0, too_chunks)) {
        fprintf(stderr, "FAIL: huge extra_chunks still fits\n");
        return 1;
    }

    uint32_t ip = 0, cp = 0;
    efs_export_meta_page_usage(&ex, &ip, &cp);
    if (ip == 0) {
        fprintf(stderr, "FAIL: expected at least one inode page\n");
        return 1;
    }

    memset(&g_client, 0, sizeof(g_client));
    pthread_mutex_init(&g_client.lock, NULL);
    efs_export_init(&g_client.export, 1, "capc");
    int rc = efs_client_ensure_meta_room(too_many, 0);
    if (rc != EFS_ERR_QUOTA || g_client.last_err != EFS_ERR_QUOTA) {
        fprintf(stderr, "FAIL: ensure_meta_room rc=%d last=%d\n", rc,
                g_client.last_err);
        return 1;
    }
    efs_ino_t ino = efs_client_create(EFS_ROOT_INO, "x", S_IFREG | 0644, 0, 0);
    /* Room for one file is fine. */
    if (ino == 0) {
        fprintf(stderr, "FAIL: create of first file should succeed\n");
        return 1;
    }

    g_client.write_readonly = 1;
    if (efs_client_ensure_meta_room(1, 0) != EFS_ERR_BUSY) {
        fprintf(stderr, "FAIL: readonly should be BUSY\n");
        return 1;
    }

    /* Chunk-only cap checks must stay O(1) — a name scan of millions of
     * inodes was 96% of write CPU on the filled cluster. */
    for (int i = 0; i < 20000; i++) {
        if (!efs_export_fits_page_cap(&g_client.export, 0, 1)) {
            fprintf(stderr, "FAIL: one extra chunk should fit\n");
            return 1;
        }
    }

    printf("test_meta_cap: OK\n");
    efs_export_free(&ex);
    efs_export_free(&g_client.export);
    return 0;
}
