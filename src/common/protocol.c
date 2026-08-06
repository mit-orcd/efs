#include "efs/protocol.h"
#include "efs/network.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>

int efs_send_msg(int fd, uint8_t type, const void *payload, uint32_t payload_len)
{
    uint32_t len = htonl(1 + payload_len);
    struct iovec iov[3];
    int niov = 2;
    iov[0].iov_base = &len;
    iov[0].iov_len = sizeof(len);
    iov[1].iov_base = &type;
    iov[1].iov_len = 1;
    if (payload_len > 0 && payload) {
        iov[2].iov_base = (void *)payload;
        iov[2].iov_len = payload_len;
        niov = 3;
    }

    /* Single writev for header+payload when the kernel accepts it all;
     * fall back to advancing through partial sends. */
    int i = 0;
    while (i < niov) {
        ssize_t n = writev(fd, &iov[i], niov - i);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return EFS_ERR_NET;
        }
        if (n == 0)
            return EFS_ERR_NET;
        size_t left = (size_t)n;
        while (i < niov && left >= iov[i].iov_len) {
            left -= iov[i].iov_len;
            i++;
        }
        if (i < niov && left > 0) {
            iov[i].iov_base = (uint8_t *)iov[i].iov_base + left;
            iov[i].iov_len -= left;
        }
    }
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
    if (payload_len)
        *payload_len = len - 1;

    if (!payload) {
        free(buf);
        return EFS_OK;
    }

    /* Reuse the receive buffer: shift payload over the type byte so callers
     * free one allocation instead of alloc+copy+free. */
    if (len == 1) {
        *payload = NULL;
        free(buf);
        return EFS_OK;
    }
    memmove(buf, buf + 1, len - 1);
    *payload = buf;
    return EFS_OK;
}
