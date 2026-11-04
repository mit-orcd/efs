/* Task AX: cost of a shared-attribute update on a hard-linked file against the
 * number of unrelated rows loaded in the same table.
 *
 *   gcc -O2 -Iinclude scripts/evidence/ax/ax_alias_bench.c libefs.a -lpthread -lm -o ax_alias_bench
 *   ./ax_alias_bench ROWS ALIASES [DIRS]
 *
 * ROWS unrelated files spread over DIRS directories (0: all in the root), one
 * hard-linked file with ALIASES names and one single-link control. Prints the
 * mean ms/op of chmod, truncate (set_size), link and unlink on the hard-linked
 * file, and chmod/truncate on the control. */
#include "efs/metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#define TROOT EFS_ROOT_INO
#define OPS 100

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    uint64_t n = strtoull(argv[1], 0, 10);
    int aliases = atoi(argv[2]);
    int dirs = argc > 3 ? atoi(argv[3]) : 0;
    struct efs_export ex;
    char nm[32];
    efs_export_init(&ex, 1, "t");
    for (int d = 0; d < dirs; d++) {
        snprintf(nm, sizeof(nm), "d%d", d);
        efs_export_create_with_ino(&ex, 2 + d, TROOT, S_IFDIR | 0755, 0, 0, nm);
    }
    efs_ino_t base = 2 + (efs_ino_t)dirs;
    for (uint64_t i = 0; i < n; i++) {
        snprintf(nm, sizeof(nm), "f%llu", (unsigned long long)i);
        efs_export_create_with_ino(&ex, base + i, dirs ? 2 + i % dirs : TROOT,
                                   S_IFREG | 0644, 0, 0, nm);
    }
    efs_ino_t h = base + n, s = base + n + 1;
    efs_export_create_with_ino(&ex, h, TROOT, S_IFREG | 0644, 0, 0, "hl");
    efs_export_create_with_ino(&ex, s, TROOT, S_IFREG | 0644, 0, 0, "single");
    for (int k = 1; k < aliases; k++) {
        snprintf(nm, sizeof(nm), "hl%d", k);
        if (efs_export_link(&ex, h, TROOT, nm) != EFS_OK)
            return 1;
    }
    double t0, chmod_h = 0, chmod_s = 0, size_h = 0, size_s = 0, link_h = 0, unlink_h = 0;
    for (int i = 0; i < OPS; i++) {
        t0 = now_ms(); efs_export_set_mode(&ex, h, i & 1 ? 0644 : 0600); chmod_h += now_ms() - t0;
        t0 = now_ms(); efs_export_set_mode(&ex, s, i & 1 ? 0644 : 0600); chmod_s += now_ms() - t0;
        t0 = now_ms(); efs_export_set_size(&ex, h, i & 1 ? 4096 : 0); size_h += now_ms() - t0;
        t0 = now_ms(); efs_export_set_size(&ex, s, i & 1 ? 4096 : 0); size_s += now_ms() - t0;
        t0 = now_ms(); efs_export_link(&ex, h, TROOT, "tmp"); link_h += now_ms() - t0;
        t0 = now_ms(); efs_export_unlink_name_ex(&ex, TROOT, "tmp", 0); unlink_h += now_ms() - t0;
    }
    printf("rows=%llu aliases=%d dirs=%d ms/op: chmod=%.4f truncate=%.4f link=%.4f unlink=%.4f"
           " chmod-single=%.4f truncate-single=%.4f\n",
           (unsigned long long)n, aliases, dirs, chmod_h / OPS, size_h / OPS,
           link_h / OPS, unlink_h / OPS, chmod_s / OPS, size_s / OPS);
    efs_export_free(&ex);
    return 0;
}
