/* Debug: dump an export's saved EFSR root (metadata.bin) — generation,
 * page count, and extra-shard descriptors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "efs/metadata.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <metadata.bin>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror("open");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        perror("read");
        return 1;
    }
    fclose(f);

    if (!efs_meta_blob_is_root(buf, (uint32_t)len)) {
        fprintf(stderr, "not a root blob (len=%ld)\n", len);
        return 1;
    }
    struct efs_export_root root;
    memset(&root, 0, sizeof(root));
    if (efs_export_root_deserialize(&root, buf, (size_t)len) != EFS_OK) {
        fprintf(stderr, "deserialize failed\n");
        return 1;
    }
    printf("export id=%u name=%s gen=%llu version=%u pages=%u next_ci=%u "
           "shard_bits=%u shard_count=%u next_ino=%llu\n",
           root.id, root.name, (unsigned long long)root.generation,
           root.version, root.page_count, root.next_ci, root.shard_bits,
           root.shard_count, (unsigned long long)root.next_ino);
    printf("extra_shard_count=%u\n", root.extra_shard_count);
    for (uint32_t i = 0; i < root.extra_shard_count; i++) {
        struct efs_export_root *xr = &root.extra_roots[i];
        printf("  shard %u: gen=%llu pages=%u next_ci=%u next_ino=%llu\n",
               root.extra_shard_ids ? root.extra_shard_ids[i] : 0,
               (unsigned long long)xr->generation, xr->page_count,
               xr->next_ci, (unsigned long long)xr->next_ino);
    }
    efs_export_root_free(&root);
    free(buf);
    return 0;
}
