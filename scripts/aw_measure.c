/* Task AW measurement: child-vector scan cost per unlink/rename.
 * Build with the TEMP-AW counter patch (scripts/aw-instrument.patch):
 *   gcc -O2 -Iinclude -Isrc/common -Ideps/blake3 -o /tmp/aw_measure \
 *     scripts/aw_measure.c libefs.a -lpthread -lm -ldl
 * Run: /tmp/aw_measure D [SAMPLE]   (D children, SAMPLE unlinks + renames)
 */
#include "efs/metadata.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

extern uint64_t g_aw_cmps; /* TEMP-AW */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void stats(const char *what, uint64_t D, unsigned n, uint64_t ns,
                  uint64_t cmps)
{
    printf("aw-measure op=%s D=%llu n=%u total_ms=%.3f ns_per_op=%llu cmps_per_op=%llu\n",
           what, (unsigned long long)D, n, ns / 1e6,
           (unsigned long long)(n ? ns / n : 0),
           (unsigned long long)(n ? cmps / n : 0));
}

static int verify_cb(struct efs_export *ex, uint64_t slot, void *arg)
{
    uint64_t *mm = arg;
    struct efs_inode c;
    if (efs_export_lookup(ex, mm[2], efs_export_inode_name(ex, slot), &c) != 0)
        return 0;
    uint64_t lo = c.mtime;
    if (c.ctime < lo) lo = c.ctime;
    if (c.atime < lo) lo = c.atime;
    uint64_t hi = c.mtime;
    if (c.ctime > hi) hi = c.ctime;
    if (c.atime > hi) hi = c.atime;
    if (!mm[0] || lo < mm[0]) mm[0] = lo;
    if (hi > mm[1]) mm[1] = hi;
    return 0;
}

int main(int argc, char **argv)
{
    uint64_t D = argc > 1 ? strtoull(argv[1], NULL, 10) : 1000;
    unsigned sample = argc > 2 ? (unsigned)atoi(argv[2]) : 200;
    struct efs_export ex;
    efs_export_init(&ex, 1, "aw");
    efs_ino_t dir = efs_export_create_with_ino(&ex, 2, EFS_ROOT_INO,
                                               S_IFDIR | 0755, 0, 0, "d");
    assert(dir == 2);
    char name[32];
    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < D; i++) {
        snprintf(name, sizeof(name), "f%08llu", (unsigned long long)i);
        if (!efs_export_create_with_ino(&ex, i + 100, dir, S_IFREG | 0644,
                                        0, 0, name)) {
            fprintf(stderr, "create failed at %llu\n", (unsigned long long)i);
            return 1;
        }
        /* create_with_ino stamps no times; give each child distinct entry
         * times so exact narrowing and the skip path are exercised. */
        struct efs_inode rec;
        if (efs_export_get_inode(&ex, i + 100, &rec) == 0) {
            rec.mtime = rec.ctime = rec.atime = i + 1;
            if (efs_export_upsert_inode(&ex, &rec) != 0) {
                fprintf(stderr, "upsert failed at %llu\n", (unsigned long long)i);
                return 1;
            }
        }
    }
    printf("aw-measure loaded D=%llu create_ms=%.1f staged_child_bytes=%llu bytes_per_child=%llu\n",
           (unsigned long long)D, (now_ns() - t0) / 1e6,
           (unsigned long long)ex.staged_child_bytes,
           (unsigned long long)(D ? ex.staged_child_bytes / D : 0));
    { struct efs_inode di0;
      assert(efs_export_get_inode(&ex, dir, &di0) == 0);
      printf("aw-measure initial parent_tmin=%llu tmax=%llu tree_tmin=%llu tree_tmax=%llu\n",
             (unsigned long long)di0.imm_tmin, (unsigned long long)di0.imm_tmax,
             (unsigned long long)di0.tree_tmin, (unsigned long long)di0.tree_tmax); }

    /* Unlink in pseudo-random order (Knuth's coprime multiplier is a
     * permutation mod D): most removed children hold no time extreme. */
    unsigned done = 0;
    uint64_t ns = 0;
    g_aw_cmps = 0;
    for (unsigned k = 0; k < sample; k++) {
        uint64_t i = (k * 2654435761ull + 12345) % D;
        snprintf(name, sizeof(name), "f%08llu", (unsigned long long)i);
        uint64_t op0 = now_ns();
        int rc = efs_export_unlink_name_ex(&ex, dir, name, 0);
        ns += now_ns() - op0;
        if (rc) {
            fprintf(stderr, "unlink %s rc=%d\n", name, rc);
            return 1;
        }
        done++;
    }
    stats("unlink-random", D, done, ns, g_aw_cmps);

    /* Rename a spread of the survivors (child_idx_del + add). */
    done = 0;
    ns = 0;
    g_aw_cmps = 0;
    for (uint64_t i = 1; i < D && done < sample; i += (D / sample) + 1) {
        snprintf(name, sizeof(name), "f%08llu", (unsigned long long)i);
        if (efs_export_lookup(&ex, dir, name, &(struct efs_inode){0}) != 0)
            continue;
        char nn[32];
        snprintf(nn, sizeof(nn), "r%08llu", (unsigned long long)i);
        uint64_t op0 = now_ns();
        int rc = efs_export_rename_at(&ex, dir, name, dir, nn);
        ns += now_ns() - op0;
        if (rc) {
            fprintf(stderr, "rename %s rc=%d\n", name, rc);
            return 1;
        }
        done++;
    }
    stats("rename", D, done, ns, g_aw_cmps);

    /* Worst case for exact narrowing: delete in creation (time) order, so
     * every removed child holds the current tmin and must rescan. */
    done = 0;
    ns = 0;
    g_aw_cmps = 0;
    for (uint64_t i = 0; i < D && done < sample; i++) {
        snprintf(name, sizeof(name), "f%08llu", (unsigned long long)i);
        if (efs_export_lookup(&ex, dir, name, &(struct efs_inode){0}) != 0)
            continue;
        uint64_t op0 = now_ns();
        int rc = efs_export_unlink_name_ex(&ex, dir, name, 0);
        ns += now_ns() - op0;
        if (rc) {
            fprintf(stderr, "ordered unlink %s rc=%d\n", name, rc);
            return 1;
        }
        done++;
    }
    stats("unlink-ordered", D, done, ns, g_aw_cmps);

    /* Correctness: the parent's bounds must equal the true bounds over the
     * remaining children — a skipped-but-needed rescan would show up here. */
    struct efs_inode di;
    assert(efs_export_get_inode(&ex, dir, &di) == 0);
    uint64_t mm[3] = {0, 0, dir};
    efs_export_foreach_child(&ex, dir, verify_cb, mm);
    printf("aw-measure verify D=%llu parent_tmin=%llu true_tmin=%llu parent_tmax=%llu true_tmax=%llu\n",
           (unsigned long long)D,
           (unsigned long long)di.imm_tmin, (unsigned long long)mm[0],
           (unsigned long long)di.imm_tmax, (unsigned long long)mm[1]);
    efs_export_free(&ex);
    return 0;
}
