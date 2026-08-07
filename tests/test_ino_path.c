#include "efs/common.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect_seg(const char *label, efs_ino_t ino, int idx, const char *want)
{
    char seg[EFS_INO_PATH_SEGS][5];
    efs_ino_path_segments(ino, seg);
    if (strcmp(seg[idx], want) != 0) {
        fprintf(stderr, "FAIL %s ino=%llu seg[%d]: got %s want %s\n",
                label, (unsigned long long)ino, idx, seg[idx], want);
        failures++;
    }
}

static void expect_path(const char *label, efs_ino_t ino, const char *want_joined)
{
    char seg[EFS_INO_PATH_SEGS][5];
    char got[64];
    efs_ino_path_segments(ino, seg);
    /* High digit first, matching on-disk order. */
    snprintf(got, sizeof(got), "%s/%s/%s/%s/%s",
             seg[4], seg[3], seg[2], seg[1], seg[0]);
    if (strcmp(got, want_joined) != 0) {
        fprintf(stderr, "FAIL %s ino=%llu: got %s want %s\n",
                label, (unsigned long long)ino, got, want_joined);
        failures++;
    }
}

int main(void)
{
    /* ino 2 → …/0000/0000/0000/0000/0002 */
    expect_path("ino2", 2, "0000/0000/0000/0000/0002");
    expect_seg("ino2 d0", 2, 0, "0002");

    /* 10000 → carry into seg[1] */
    expect_path("ino10000", 10000, "0000/0000/0000/0001/0000");

    /* 123456789012345 → spans several groups */
    expect_path("span", 123456789012345ULL, "0000/0123/4567/8901/2345");

    /* Full uint64 high-bit meta table ino must be unique (5 segments). */
    expect_path("meta", EFS_META_TABLE_INO, "0922/3372/0368/5477/5810");

    /* Client-style namespace bit (tag << 40) must not collapse to low ino. */
    efs_ino_t ns = ((efs_ino_t)1 << 40) | 7;
    expect_path("ns", ns, "0000/0001/0995/1162/7783");
    {
        char a[EFS_INO_PATH_SEGS][5], b[EFS_INO_PATH_SEGS][5];
        efs_ino_path_segments(7, a);
        efs_ino_path_segments(ns, b);
        if (strcmp(a[0], b[0]) == 0 && strcmp(a[1], b[1]) == 0 &&
            strcmp(a[2], b[2]) == 0 && strcmp(a[3], b[3]) == 0 &&
            strcmp(a[4], b[4]) == 0) {
            fprintf(stderr, "FAIL namespace ino collided with low ino 7\n");
            failures++;
        }
    }

    if (failures == 0) {
        printf("test_ino_path: OK\n");
        return 0;
    }
    printf("test_ino_path: %d failures\n", failures);
    return 1;
}
