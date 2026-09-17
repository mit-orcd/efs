/* Open an existing LSM dir and compact. Used to repro a live kv tree. */
#include "efs/kv_lsm.h"
#include "efs/common.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    struct efs_kv_lsm_cfg cfg;
    struct efs_kv *kv;
    uint32_t l0 = 0, l1 = 0;
    int rc;

    if (argc < 2) {
        fprintf(stderr, "usage: kv_compact_dir <dir>\n");
        return 2;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.sync_mode = EFS_KV_LSM_NOSYNC;
    kv = efs_kv_lsm_open(argv[1], &cfg);
    if (!kv) {
        fprintf(stderr, "open failed %s\n", argv[1]);
        return 1;
    }
    if (efs_kv_lsm_seg_count(kv, &l0, &l1) != EFS_OK)
        fprintf(stderr, "count failed\n");
    else
        fprintf(stderr, "before l0=%u l1=%u\n", l0, l1);
    rc = efs_kv_lsm_compact(kv);
    fprintf(stderr, "compact rc=%d\n", rc);
    if (efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK)
        fprintf(stderr, "after l0=%u l1=%u\n", l0, l1);
    efs_kv_lsm_close(kv);
    return rc != EFS_OK;
}
