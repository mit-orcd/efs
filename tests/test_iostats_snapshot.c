/* Exercise the real routine/full snapshot split without an RDMA device. */
#include "../src/server/server_internal.h"
#include "efs/rdma.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned scans;
void efs_rdma_totals(struct efs_rdma_totals *out)
{
    ++scans;
    *out = (struct efs_rdma_totals){7, 11, 13, 17, 19, 23};
}

int main(void)
{
    struct efs_msg_io_stats_reply r;
    efs_iostats_dump(0); /* even idle routine diagnostics must not scan */
    assert(scans == 0);
    for (int i = 0; i < EFS_IO_STATS_CLASSES; ++i) {
        efs_iostats_add(i, 100 + i, 20, 0);
        efs_iostats_add(i, 200 + i, 40, 1);
    }
    efs_iostats_dump(1);
    efs_iostats_dump(1);
    assert(scans == 0);
    memset(&r, 0xa5, sizeof(r));
    efs_iostats_snapshot(&r);
    assert(scans == 1);
    for (int i = 0; i < EFS_IO_STATS_CLASSES; ++i) {
        assert(r.cls[i].ops == 2 && r.cls[i].bytes == (uint64_t)(300 + 2*i));
        assert(r.cls[i].errors == 1 && r.cls[i].us_sum == 60);
        assert(r.cls[i].us_max == 40 && r.cls[i].p50_us == 40);
    }
    assert(r.rdma.live_conns == 7 && r.rdma.data_frames_tx == 11);
    assert(r.rdma.data_bytes_tx == 13 && r.rdma.data_frames_rx == 17);
    assert(r.rdma.data_bytes_rx == 19 && r.rdma.credit_stalls == 23);
    efs_iostats_dump(1);
    assert(scans == 1);
    puts("I/O snapshot: routine dumps never aggregate RDMA; management preserves every field PASS");
    return 0;
}
