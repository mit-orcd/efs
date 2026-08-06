#include "efs/protocol.h"
#include "efs/network.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int efs_send_msg(int fd, uint8_t type, const void *payload, uint32_t payload_len)
{
    uint32_t len = htonl(1 + payload_len);
    if (efs_send_all(fd, &len, sizeof(len)) != 0)
        return EFS_ERR_NET;
    if (efs_send_all(fd, &type, 1) != 0)
        return EFS_ERR_NET;
    if (payload_len > 0 && efs_send_all(fd, payload, payload_len) != 0)
        return EFS_ERR_NET;
    return EFS_OK;
}

int efs_recv_msg(int fd, uint8_t *type, void **payload, uint32_t *payload_len)
{
    uint32_t len;
    if (efs_recv_all(fd, &len, sizeof(len)) != 0)
        return EFS_ERR_NET;
    len = ntohl(len);
    if (len == 0 || len > 16 * 1024 * 1024)
        return EFS_ERR_PROTO;

    uint8_t *buf = malloc(len);
    if (!buf)
        return EFS_ERR_NOMEM;

    if (efs_recv_all(fd, buf, len) != 0) {
        free(buf);
        return EFS_ERR_NET;
    }

    *type = buf[0];
    if (payload_len) {
        *payload_len = len - 1;
    }
    if (payload) {
        *payload = len > 1 ? malloc(len - 1) : NULL;
        if (len > 1) {
            memcpy(*payload, buf + 1, len - 1);
        }
    }
    free(buf);
    return EFS_OK;
}
