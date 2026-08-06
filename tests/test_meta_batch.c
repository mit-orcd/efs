#include "efs/metadata.h"
#include "client_internal.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void)
{
    memset(&g_client, 0, sizeof(g_client));
    for (int i = 0; i < EFS_MAX_NODES; i++)
        g_client.conn_fd[i] = -1;
    pthread_mutex_init(&g_client.lock, NULL);
    efs_export_init(&g_client.export, 1, "t");
    g_client.ino_namespace = 1ULL << 40;
    g_client.ino_counter = 1;
    efs_client_enable_meta_batch(4096);

    efs_ino_t dir = efs_client_create(EFS_ROOT_INO, "tree", S_IFDIR | 0755, 0, 0);
    printf("dir=%llu\n", (unsigned long long)dir);
    assert(dir != 0);

    for (int i = 0; i < 5000; i++) {
        char name[32];
        snprintf(name, sizeof(name), "f%06d", i);
        efs_ino_t ino = efs_client_create(dir, name, S_IFREG | 0644, 0, 0);
        if (ino == 0) {
            printf("create failed at %d\n", i);
            return 1;
        }
        if ((i + 1) % 1000 == 0)
            printf("created %d dirty_inos=%llu\n", i + 1,
                   (unsigned long long)g_client.dirty_ino_count);
    }
    printf("forcing flush...\n");
    int rc = efs_client_note_meta_change(1);
    printf("flush_rc=%d (may be no_quorum without servers)\n", rc);
    printf("OK\n");
    return 0;
}
