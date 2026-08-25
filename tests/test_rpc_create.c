/* Phase 2a milestone harness: exercise the server-side RPC mutation path
 * (efs_client_rpc_create / _lookup / _unlink) directly, without FUSE. The
 * server-side meta-flush thread is what makes these mutations durable; this
 * harness just drives the RPCs so the milestone script can create a file,
 * restart the metadata primary, then confirm the file is still resolvable. */
#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int parse_host_port(const char *s, char *host, size_t host_len,
                           uint16_t *port)
{
    const char *colon = strrchr(s, ':');
    if (!colon || colon == s)
        return -1;
    size_t n = (size_t)(colon - s);
    if (n >= host_len)
        return -1;
    memcpy(host, s, n);
    host[n] = '\0';
    int p = atoi(colon + 1);
    if (p <= 0 || p > 65535)
        return -1;
    *port = (uint16_t)p;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <create|lookup|unlink> <seed host:port> <name> "
                "[export_id]\n", argv[0]);
        return 2;
    }
    const char *op = argv[1];
    const char *seed = argv[2];
    const char *name = argv[3];
    efs_export_id_t export_id = (argc > 4) ? (efs_export_id_t)atoi(argv[4]) : 1;

    char host[64];
    uint16_t port = 0;
    if (parse_host_port(seed, host, sizeof(host), &port) != 0) {
        fprintf(stderr, "invalid seed: %s\n", seed);
        return 2;
    }

    memset(&g_client, 0, sizeof(g_client));
    pthread_mutex_init(&g_client.lock, NULL);
    if (efs_client_discover_nodes(&g_client, host, port) != 0) {
        fprintf(stderr, "discover failed from %s:%u\n", host, port);
        return 1;
    }

    if (strcmp(op, "create") == 0) {
        efs_ino_t ino = 0;
        int rc = efs_client_rpc_create(export_id, EFS_ROOT_INO, name, 0100644,
                                       getuid(), getgid(), 0, &ino, NULL);
        if (rc != EFS_OK) {
            fprintf(stderr, "rpc_create failed rc=%d\n", rc);
            return 1;
        }
        printf("created ino=%llu\n", (unsigned long long)ino);
        return 0;
    } else if (strcmp(op, "lookup") == 0) {
        struct efs_inode inode;
        memset(&inode, 0, sizeof(inode));
        int rc = efs_client_rpc_lookup(export_id, EFS_ROOT_INO, name, &inode);
        if (rc != EFS_OK) {
            fprintf(stderr, "rpc_lookup failed rc=%d\n", rc);
            return 1;
        }
        printf("lookup ok ino=%llu size=%llu mode=%o name=%s\n",
               (unsigned long long)inode.ino,
               (unsigned long long)inode.size, inode.mode, inode.name);
        return 0;
    } else if (strcmp(op, "unlink") == 0) {
        int rc = efs_client_rpc_unlink(export_id, EFS_ROOT_INO, name, 0);
        if (rc != EFS_OK) {
            fprintf(stderr, "rpc_unlink failed rc=%d\n", rc);
            return 1;
        }
        printf("unlinked\n");
        return 0;
    }

    fprintf(stderr, "unknown op: %s\n", op);
    return 2;
}
