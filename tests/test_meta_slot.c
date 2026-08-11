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

    if (failures == 0) {
        printf("test_meta_slot: OK\n");
        return 0;
    }
    printf("test_meta_slot: %d failures\n", failures);
    return 1;
}
