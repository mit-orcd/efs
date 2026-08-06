#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "server_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int server_send_metadata_to(struct efsd_server *s, struct efs_export *ex,
                              const char *host, uint16_t port)
{
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return -1;

    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    char *buf = NULL;
    size_t len = 0;
    pthread_mutex_lock(&s->lock);
    efs_export_serialize(ex, &buf, &len);
    pthread_mutex_unlock(&s->lock);

    if (!buf) {
        close(fd);
        return -1;
    }

    int rc = efs_send_msg(fd, EFS_MSG_PUT_META, buf, (uint32_t)len);
    free(buf);
    if (rc != 0) {
        close(fd);
        return -1;
    }

    uint8_t type;
    void *payload = NULL;
    uint32_t payload_len = 0;
    rc = efs_recv_msg(fd, &type, &payload, &payload_len);
    free(payload);
    close(fd);
    return (rc == 0 && type == EFS_MSG_PUT_META_REPLY) ? 0 : -1;
}

int server_replicate_metadata(struct efsd_server *s, struct efs_export *ex)
{
    pthread_mutex_lock(&s->lock);
    struct efs_node nodes[EFS_MAX_NODES];
    uint32_t node_count = s->node_count;
    memcpy(nodes, s->nodes, sizeof(nodes));
    pthread_mutex_unlock(&s->lock);

    int acks = 0;
    for (uint32_t i = 0; i < node_count; i++) {
        if (nodes[i].id == s->id)
            continue;
        if (server_send_metadata_to(s, ex, nodes[i].addr, nodes[i].port) == 0)
            acks++;
    }
    return acks;
}

int server_fetch_metadata_from(struct efsd_server *s, const char *host, uint16_t port)
{
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return -1;

    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    if (efs_send_msg(fd, EFS_MSG_GET_META, NULL, 0) != 0) {
        close(fd);
        return -1;
    }

    uint8_t type;
    void *payload = NULL;
    uint32_t payload_len = 0;
    if (efs_recv_msg(fd, &type, &payload, &payload_len) != 0 ||
        type != EFS_MSG_GET_META_REPLY || payload_len == 0) {
        close(fd);
        return -1;
    }

    pthread_mutex_lock(&s->lock);
    if (s->export_count > 0) {
        efs_export_deserialize(&s->exports[0], payload, payload_len);
        server_save_export(s, &s->exports[0]);
    }
    pthread_mutex_unlock(&s->lock);

    free(payload);
    close(fd);
    return 0;
}
