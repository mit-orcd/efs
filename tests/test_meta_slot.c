#include "efs/common.h"
#include <stdio.h>
#include <stdint.h>
#include <limits.h>

static int failures = 0;

static void expect_u32(const char *label, uint32_t got, uint32_t want)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %u want %u\n", label, got, want);
        failures++;
    }
}

int main(void)
{
    expect_u32("gen0 page0", efs_meta_page_chunk_index(0, 0), 0);
    expect_u32("gen0 page7", efs_meta_page_chunk_index(0, 7), 7);
    expect_u32("gen1 page0", efs_meta_page_chunk_index(1, 0), EFS_META_SLOT_STRIDE);
    expect_u32("gen1 page3", efs_meta_page_chunk_index(1, 3),
               EFS_META_SLOT_STRIDE + 3);
    expect_u32("gen2 page0", efs_meta_page_chunk_index(2, 0), 0);
    expect_u32("gen3 page1", efs_meta_page_chunk_index(3, 1),
               EFS_META_SLOT_STRIDE + 1);

    /* Alternating slots never collide for the same page. */
    for (uint32_t p = 0; p < 16; p++) {
        uint32_t a = efs_meta_page_chunk_index(10, p);
        uint32_t b = efs_meta_page_chunk_index(11, p);
        if (a == b) {
            fprintf(stderr, "FAIL slot collide page=%u\n", p);
            failures++;
        }
    }

    expect_u32("oob", efs_meta_page_chunk_index(1, EFS_META_MAX_PAGES), UINT32_MAX);
    expect_u32("lastok", efs_meta_page_chunk_index(0, EFS_META_MAX_PAGES - 1),
               EFS_META_MAX_PAGES - 1);

    /* v4 vs v5 chunk-region bases differ; inode pages at even gen match. */
    expect_u32("v4 chunk0",
               efs_meta_assembled_page_ci(0, 4, 4772, 1266, 4772),
               EFS_META_V4_CHUNK_BASE);
    expect_u32("v5 chunk0",
               efs_meta_assembled_page_ci(0, 5, 4772, 1266, 4772),
               EFS_META_CHUNK_PAGE_BASE);
    expect_u32("v4 ino0", efs_meta_assembled_page_ci(0, 4, 4772, 1266, 0), 0);
    expect_u32("v5 ino0", efs_meta_assembled_page_ci(0, 5, 4772, 1266, 0), 0);

    uint32_t cis[3];
    int nc = efs_meta_page_ci_candidates(0, 5, 4772, 1266, 4772, cis);
    if (nc < 2 || cis[0] != EFS_META_CHUNK_PAGE_BASE ||
        cis[1] != EFS_META_V4_CHUNK_BASE) {
        fprintf(stderr, "FAIL v5-labeled candidates: n=%d ci0=%u ci1=%u\n",
                nc, cis[0], cis[1]);
        failures++;
    }
    expect_u32("layout v4",
               efs_meta_page_ci_layout(0, 4772, 1266, 4772,
                                       EFS_META_V4_CHUNK_BASE),
               4);
    expect_u32("layout v5",
               efs_meta_page_ci_layout(0, 4772, 1266, 4772,
                                       EFS_META_CHUNK_PAGE_BASE),
               5);
    expect_u32("layout ambiguous ino",
               efs_meta_page_ci_layout(0, 4772, 1266, 0, 0), 0);

    if (failures == 0) {
        printf("test_meta_slot: OK\n");
        return 0;
    }
    printf("test_meta_slot: %d failures\n", failures);
    return 1;
}
