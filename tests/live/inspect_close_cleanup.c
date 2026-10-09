/* Offline inspection of an OWNED, STOPPED replay fixture only. Opening an
 * LSM can repair its WAL tail; never point this at a running or installed store.
 * Build: cc -Iinclude -Isrc/common tests/live/inspect_close_cleanup.c libefs.a
 *        -lpthread -lm -ldl -libverbs -o /tmp/inspect-close-cleanup
 */
#include "efs/kv_lsm.h"
#include "efs/kv_key.h"
#include "efs/meta_apply.h"
#include "efs/session.h"
#include <stdio.h>
#include <stdlib.h>

static int count(void *ctx, const uint8_t *key, uint32_t kl,
                 const uint8_t *value, uint32_t vl)
{
    (void)value; (void)vl;
    if (kl >= 3 && key[2] == EFS_KV_KIND_CLOSE_CLEANUP)
        ++*(unsigned *)ctx;
    return 0;
}
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    struct efs_kv *kv = efs_kv_lsm_open(argv[1], NULL);
    if (!kv) return 3;
    unsigned markers = 0; uint32_t nopen = 0;
    uint64_t watermark = 0, frontier = 0;
    int scan = efs_kv_scan_from(kv, NULL, 0, NULL, 0, count, &markers);
    int rc = efs_meta_apply_append_state(kv, strtoull(argv[2], NULL, 0),
                                         &watermark, &frontier, &nopen);
    struct efs_meta_row row;
    int row_rc = efs_meta_apply_get_inode(kv, strtoull(argv[2], NULL, 0), &row);
    int held = row_rc == EFS_OK ? efs_lease_any(kv, row.ino, row.generation) : -1;
    printf("held=%d markers=%u scan_rc=%d append_rc=%d nopen=%u watermark=%llu frontier=%llu\n",
           held, markers, scan, rc, nopen, (unsigned long long)watermark,
           (unsigned long long)frontier);
    efs_kv_lsm_close(kv);
    return scan < 0 || markers || rc != EFS_OK || held < 0 || (!held && nopen) ? 1 : 0;
}
