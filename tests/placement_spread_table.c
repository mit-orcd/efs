/* Placement spread table (K33 increment 2 finding): per-node fragment
 * load for member counts 3..16 under realistic inode patterns. Prints, per
 * pattern and member count, the lightest and heaviest node as a share of
 * the mean (1.00 = fair) and how many nodes got nothing.
 *
 * Reproduce (after and before the fix):
 *   cc -O2 -Iinclude tests/placement_spread_table.c src/common/placement.c -o /tmp/spread
 *   git show 3e01ba95c:src/common/placement.c > /tmp/placement_before.c
 *   cc -O2 -DBEFORE -Iinclude tests/placement_spread_table.c /tmp/placement_before.c \
 *      -o /tmp/spread_before
 * BEFORE uses the spare rule of the base write.c (first live unused member
 * in membership order); otherwise efs_place_spare. Not part of make test:
 * tests/test_placement.c asserts the bounds. */
#include "efs/placement.h"
#include <stdio.h>
#include <string.h>

#define FILES 8192u

static void spare(const struct efs_node *m, uint32_t n, efs_node_id_t down,
                  efs_ino_t ino, uint32_t ci, efs_node_id_t f[EFS_NUM_FRAGMENTS])
{
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (f[i] != down)
            continue;
#ifdef BEFORE
        (void)ino; (void)ci;
        for (uint32_t k = 0; k < n; k++) {
            efs_node_id_t id = m[k].id;
            if (id == down || id == f[0] || id == f[1] || id == f[2])
                continue;
            f[i] = id;
            break;
        }
#else
        efs_node_id_t live[EFS_MAX_NODES];
        uint32_t nl = 0;
        for (uint32_t k = 0; k < n; k++)
            if (m[k].id != down)
                live[nl++] = m[k].id;
        f[i] = efs_place_spare(live, nl, ino, ci, i, f);
#endif
    }
}

/* Pattern p, item k -> (ino, chunks). */
static efs_ino_t pattern(int p, uint32_t k, uint32_t *chunks)
{
    *chunks = 1;
    switch (p) {
    case 0: *chunks = 2; return 4096ull * (k + 1) + 7;   /* one shard, bits 12, 256 KiB */
    case 1: return 8ull * (k + 1) + 3;                   /* one shard, bits 3 */
    case 2: return 4096ull * (k / 64 + 1) + k % 64;      /* across 64 shards */
    case 3: return (k + 2);                              /* sequential */
    case 4: return (0x1234ull << 40) | (4096ull * k);    /* high prefix */
    case 5: *chunks = 512; return 4096ull * (k + 1) + 7; /* 16 large files */
    default: return 0;
    }
}

static const char *names[] = {
    "shard step 4096, 2 chunks", "shard step 8, 1 chunk", "64 shards, 1 chunk",
    "sequential, 1 chunk", "prefix<<40, step 4096", "16 files x 512 chunks",
    "packed seg, us clock", "packed seg, ns clock", "spare, node 1 down",
};
#define NPAT 9

static void load(int p, uint32_t n, uint32_t from, uint32_t to,
                 uint64_t c[EFS_MAX_NODES + 1])
{
    struct efs_node m[EFS_MAX_NODES];
    efs_node_id_t f[EFS_NUM_FRAGMENTS];

    memset(m, 0, sizeof(m));
    memset(c, 0, sizeof(uint64_t) * (EFS_MAX_NODES + 1));
    for (uint32_t i = 0; i < n; i++)
        m[i].id = i + 1;
    for (uint32_t k = from; k < to; k++) {
        if (p == 6 || p == 7) {
            /* packer.c pk_segment_id: now_ns << 8 | self (self = 1). */
            uint64_t ns = p == 6 ? 1700000000000000000ull + 1000ull * 37 * k
                                 : 1700000000000000000ull + 4099ull * k;
            if (efs_pack_destinations(m, n, 1, ns << 8 | 1, f) != EFS_OK)
                return;
            for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) c[f[i]]++;
            continue;
        }
        uint32_t chunks;
        efs_ino_t ino = pattern(p == 8 ? 0 : p, k, &chunks);
        for (uint32_t ci = 0; ci < chunks; ci++) {
            efs_place_fragments(m, n, ino, ci, f);
            if (p == 8) {
                if (f[0] != 1 && f[1] != 1 && f[2] != 1)
                    continue;
                efs_node_id_t was[EFS_NUM_FRAGMENTS];
                memcpy(was, f, sizeof(was));
                spare(m, n, 1, ino, ci, f);
                /* Count only the spares: where node 1's share went. */
                for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
                    if (was[i] == 1 && f[i] != 1) c[f[i]]++;
                continue;
            }
            for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) c[f[i]]++;
        }
    }
}

/* Short runs: windows of `w` consecutive items (files or segments); how
 * many leave some member with no fragment at all. */
static void windows(uint32_t w)
{
    uint64_t c[EFS_MAX_NODES + 1];

    printf("\n%u-item windows (of %u) in which some member gets no fragment\n| N |", w, FILES / w);
    for (int p = 0; p < NPAT; p++)
        if (p != 5 && p != 8) printf(" %s |", names[p]);
    printf("\n|---:|");
    for (int p = 0; p < NPAT; p++)
        if (p != 5 && p != 8) printf("---:|");
    printf("\n");
    for (uint32_t n = 3; n <= EFS_MAX_NODES; n++) {
        printf("| %u |", n);
        for (int p = 0; p < NPAT; p++) {
            uint32_t bad = 0;
            if (p == 5 || p == 8)
                continue;
            for (uint32_t s = 0; s < FILES; s += w) {
                int idle = 0;
                load(p, n, s, s + w, c);
                for (uint32_t i = 1; i <= n; i++) idle |= !c[i];
                bad += idle;
            }
            printf(" %u |", bad);
        }
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    uint64_t c[EFS_MAX_NODES + 1];

    /* The finding: 8 members, 24 new files of 256 KiB in one shard. */
    {
        struct efs_node m[8];
        efs_node_id_t f[EFS_NUM_FRAGMENTS];
        memset(m, 0, sizeof(m));
        memset(c, 0, sizeof(c));
        for (uint32_t i = 0; i < 8; i++) m[i].id = i + 1;
        for (uint32_t k = 0; k < 24; k++)
            for (uint32_t ci = 0; ci < 2; ci++) {
                efs_place_fragments(m, 8, 4096ull * (k + 1) + 7, ci, f);
                for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) c[f[i]]++;
            }
        printf("8 members, 24 files x 2 chunks, ino 4096k+7: fragments per node\n|");
        for (int i = 1; i <= 8; i++) printf(" n%d |", i);
        printf("\n|");
        for (int i = 1; i <= 8; i++) printf("---:|");
        printf("\n|");
        for (int i = 1; i <= 8; i++) printf(" %llu |", (unsigned long long)c[i]);
        printf("\n\n");
    }

    printf("min/mean - max/mean per node (zero-load nodes in brackets)\n| N |");
    for (int p = 0; p < NPAT; p++) printf(" %s |", names[p]);
    printf("\n|---:|");
    for (int p = 0; p < NPAT; p++) printf("---|");
    printf("\n");
    for (uint32_t n = 3; n <= EFS_MAX_NODES; n++) {
        printf("| %u |", n);
        for (int p = 0; p < NPAT; p++) {
            uint64_t sum = 0, lo = UINT64_MAX, hi = 0;
            uint32_t zero = 0, first = p == 8 ? 2 : 1, cnt = 0;
            load(p, n, 0, p == 5 ? 16 : FILES, c);
            for (uint32_t i = first; i <= n; i++) {
                sum += c[i];
                cnt++;
                lo = c[i] < lo ? c[i] : lo;
                hi = c[i] > hi ? c[i] : hi;
                zero += !c[i];
            }
            if (p == 8 && n == 3) {
                printf(" - |");
                continue;
            }
            double mean = (double)sum / cnt;
            printf(" %.2f-%.2f", mean ? lo / mean : 0, mean ? hi / mean : 0);
            if (zero) printf(" [%u]", zero);
            printf(" |");
        }
        printf("\n");
    }
    windows(64);
    windows(16);
    (void)argc; (void)argv;
    return 0;
}
