/* K88 Task DK driver (run by tests/test_k88_chunk_hints_live.py): the
 * slot-less library write path (client_write_run, as efs-bench) on real
 * daemons. Two chunks of one file each get a half image committed by one
 * sync REPORT; the second halves make full images with base 0, so the next
 * REPORT publishes only if each chunk kept its own CAS hint (cbase). With one
 * hint per inode the first chunk goes STALE twice and `report-stuck` drops it
 * as a writeback error. Usage: test_k88_chunk_hints HOST:PORT NAME */
#include "../common/parse_host_port.h"
#include "client_internal.h"
#include "efs/common.h"
#include "efs/kv_key.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HALF (EFS_DEFAULT_CHUNK_SIZE / 2)

/* Same client start as efs-bench's published_client_start. */
static int client_start(const char *seed)
{
    char host[64]; uint16_t port;
    if (efs_parse_host_port(seed, host, sizeof(host), &port)) return EFS_ERR_INVAL;
    const char *nodes[EFS_MAX_NODES] = {seed};
    memset(&g_client, 0, sizeof(g_client));
    pthread_mutex_init(&g_client.lock, NULL);
    efs_client_init_nodes(&g_client, nodes, 1);
    snprintf(g_client.export_name, sizeof(g_client.export_name), "%s", "default");
    g_client.export_id = 1;
    efs_export_init(&g_client.export, 1, "default");
    g_client.export.root.shard_bits = EFS_KV_SHARD_BITS;
    g_client.export.root.shard_count = 1u << EFS_KV_SHARD_BITS;
    g_client.export.chunk_size = EFS_DEFAULT_CHUNK_SIZE;
    efs_client_setup_mount_token();
    int rc = efs_client_discover_nodes(&g_client, host, port);
    if (rc == EFS_OK) rc = efs_client_view_mount();
    if (rc == EFS_OK) rc = efs_client_session_start();
    if (rc != EFS_OK) return rc;
    efs_dcache_init();
    efs_client_enable_meta_batch(4096);
    return efs_client_view_start();
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s HOST:PORT NAME\n", argv[0]); return 2; }
    int rc = client_start(argv[1]);
    if (rc != EFS_OK) { fprintf(stderr, "K88 driver: start rc=%d\n", rc); return 1; }
    static char buf[4][HALF], back[2 * EFS_DEFAULT_CHUNK_SIZE];
    for (int i = 0; i < 4; i++) memset(buf[i], 'a' + i, HALF);
    efs_ino_t ino = 0;
    rc = efs_client_create_result(EFS_ROOT_INO, argv[2], S_IFREG | 0644, getuid(), getgid(), 0, &ino);
    struct efs_wb_description *wb = rc == EFS_OK ? efs_wb_description_open(ino) : NULL;
    if (!wb) { fprintf(stderr, "K88 driver: create rc=%d\n", rc); return 1; }
    /* Round 1: the first half of chunks 0 and 1, both committed together. */
    int w1 = efs_client_write(ino, 0, HALF, buf[0]);
    if (w1 == EFS_OK) w1 = efs_client_write(ino, EFS_DEFAULT_CHUNK_SIZE, HALF, buf[1]);
    int r1 = w1 == EFS_OK ? efs_client_report_dirty_ino(ino, 1) : w1;
    /* Round 2: the second halves; each chunk is a new full image at base 0. */
    int w2 = efs_client_write(ino, HALF, HALF, buf[2]);
    if (w2 == EFS_OK) w2 = efs_client_write(ino, EFS_DEFAULT_CHUNK_SIZE + HALF, HALF, buf[3]);
    int r2 = w2 == EFS_OK ? efs_client_report_dirty_ino(ino, 1) : w2;
    int wbe = efs_wb_description_sync(wb, 0);
    size_t got = 0;
    int rr = efs_client_read(ino, 0, sizeof(back), back, &got);
    int exact = rr == EFS_OK && got == sizeof(back) &&
                !memcmp(back, buf[0], HALF) && !memcmp(back + HALF, buf[2], HALF) &&
                !memcmp(back + 2 * HALF, buf[1], HALF) && !memcmp(back + 3 * HALF, buf[3], HALF);
    printf("K88 driver ino=%llu round1=%d round2=%d writeback=%d read=%d exact=%d\n",
           (unsigned long long)ino, r1, r2, wbe, rr, exact);
    efs_wb_description_close(wb);
    efs_client_view_stop();
    int sd = efs_client_shutdown();
    return r1 == EFS_OK && r2 == EFS_OK && !wbe && exact && sd == EFS_OK ? 0 : 1;
}
