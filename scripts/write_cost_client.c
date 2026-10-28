/* Measurement-only direct C API workload, not an alternative product client.
 * Same file-per-request layout and close-equivalent REPORT as the FUSE case.
 * Uses the synchronous C write API, not the FUSE dcache acceptance pipeline.
 * Build via write_cost_profile.py against this checkout's normal libefs.a. */
#include "../src/client/client_internal.h"
#include "../src/common/parse_host_port.h"
#include "efs/kv_key.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct worker {
    unsigned id, writers;
    uint64_t files;
    size_t size;
    int rc;
};

static void *run(void *arg)
{
    struct worker *w = arg;
    char name[64];
    efs_ino_t dir = 0;
    snprintf(name, sizeof(name), "cost-w%u", w->id);
    w->rc = efs_client_create_result(EFS_ROOT_INO, name, S_IFDIR | 0755,
                                    getuid(), getgid(), 0, &dir);
    if (w->rc != EFS_OK) return NULL;
    char *buf = malloc(w->size);
    if (!buf) { w->rc = EFS_ERR_NOMEM; return NULL; }
    memset(buf, 'A', w->size);
    for (uint64_t i = w->id; i < w->files && w->rc == EFS_OK; i += w->writers) {
        efs_ino_t ino = 0;
        snprintf(name, sizeof(name), "f%llu", (unsigned long long)i);
        w->rc = efs_client_create_result(dir, name, S_IFREG | 0644,
                                         getuid(), getgid(), 0, &ino);
        if (w->rc == EFS_OK)
            w->rc = efs_client_write(ino, 0, w->size, buf);
        if (w->rc == EFS_OK)
            w->rc = efs_client_report_dirty_ino(ino, 1);
        if (w->rc != EFS_OK)
            fprintf(stderr, "direct worker=%u file=%llu rc=%d\n", w->id,
                    (unsigned long long)i, w->rc);
    }
    free(buf);
    return NULL;
}

static uint64_t number(const char *s)
{
    char *end;
    errno = 0;
    unsigned long long n = strtoull(s, &end, 10);
    if (errno || *end || !*s || *s == '-' || !n) {
        fprintf(stderr, "invalid positive integer: %s\n", s);
        exit(2);
    }
    return n;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s SEED TOTAL_BYTES IO_BYTES WRITERS\n", argv[0]);
        return 2;
    }
    uint64_t total = number(argv[2]), size = number(argv[3]), nw = number(argv[4]);
    if (total % size || size > (16u << 20) || nw > 8 || total / size < nw)
        return 2;
    char host[64]; uint16_t port;
    if (efs_parse_host_port(argv[1], host, sizeof(host), &port)) return 2;
    const char *nodes[EFS_MAX_NODES] = {argv[1]};
    pthread_mutex_init(&g_client.lock, NULL);
    efs_client_init_nodes(&g_client, nodes, 1);
    strcpy(g_client.export_name, "default");
    g_client.export_id = 1;
    efs_export_init(&g_client.export, 1, "default");
    g_client.export.root.shard_bits = EFS_KV_SHARD_BITS;
    g_client.export.root.shard_count = 1u << EFS_KV_SHARD_BITS;
    g_client.export.chunk_size = EFS_DEFAULT_CHUNK_SIZE;
    efs_client_setup_mount_token();
    if (efs_client_discover_nodes(&g_client, host, port) ||
        efs_client_view_mount() != EFS_OK ||
        efs_client_session_start() != EFS_OK) return 1;
    efs_dcache_init();
    efs_client_enable_meta_batch(4096);
    pthread_t threads[8];
    struct worker workers[8];
    unsigned started = 0;
    int rc = 0;
    for (unsigned i = 0; i < nw; i++) {
        workers[i] = (struct worker){i, nw, total / size, size, 0};
        if (pthread_create(&threads[i], NULL, run, &workers[i])) { rc = 1; break; }
        started++;
    }
    for (unsigned i = 0; i < started; i++) {
        if (pthread_join(threads[i], NULL) || workers[i].rc != EFS_OK) rc = 1;
    }
    if (efs_dcache_flush_all() != EFS_OK || efs_client_report_dirty(1) != EFS_OK)
        rc = 1;
    if (efs_client_shutdown() != EFS_OK) rc = 1;
    printf("direct files=%llu bytes=%llu writers=%llu rc=%d\n",
           (unsigned long long)(total / size), (unsigned long long)total,
           (unsigned long long)nw, rc);
    return rc;
}
