#include "efs/protocol.h"
#include "efs/network.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>

static int send_iov(int fd, struct iovec *iov, int niov)
{
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

int efs_send_msg_parts(int fd, uint8_t type,
                       const void *part1, uint32_t part1_len,
                       const void *part2, uint32_t part2_len)
{
    uint32_t payload_len = part1_len + part2_len;
    uint32_t len = htonl(1 + payload_len);
    struct iovec iov[4];
    int niov = 2;
    iov[0].iov_base = &len;
    iov[0].iov_len = sizeof(len);
    iov[1].iov_base = &type;
    iov[1].iov_len = 1;
    if (part1_len > 0 && part1) {
        iov[niov].iov_base = (void *)part1;
        iov[niov].iov_len = part1_len;
        niov++;
    }
    if (part2_len > 0 && part2) {
        iov[niov].iov_base = (void *)part2;
        iov[niov].iov_len = part2_len;
        niov++;
    }
    return send_iov(fd, iov, niov);
}

int efs_send_msg(int fd, uint8_t type, const void *payload, uint32_t payload_len)
{
    return efs_send_msg_parts(fd, type, payload, payload_len, NULL, 0);
}

int efs_recv_u8_reply(int fd, uint8_t *type, uint8_t *status)
{
    uint32_t len;
    if (efs_recv_all(fd, &len, sizeof(len)) != 0)
        return EFS_ERR_NET;
    len = ntohl(len);
    if (len != 2)
        return EFS_ERR_PROTO;
    uint8_t buf[2];
    if (efs_recv_all(fd, buf, 2) != 0)
        return EFS_ERR_NET;
    if (type)
        *type = buf[0];
    if (status)
        *status = buf[1];
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

    uint8_t t;
    if (efs_recv_all(fd, &t, 1) != 0)
        return EFS_ERR_NET;
    *type = t;

    uint32_t plen = len - 1;
    if (payload_len)
        *payload_len = plen;

    if (plen == 0) {
        if (payload)
            *payload = NULL;
        return EFS_OK;
    }

    if (!payload) {
        /* Drain unread payload. */
        uint8_t sink[4096];
        uint32_t left = plen;
        while (left) {
            uint32_t n = left > sizeof(sink) ? (uint32_t)sizeof(sink) : left;
            if (efs_recv_all(fd, sink, n) != 0)
                return EFS_ERR_NET;
            left -= n;
        }
        return EFS_OK;
    }

    /* Payload lands at malloc base — no 64 KiB memmove to strip the type. */
    uint8_t *buf = malloc(plen);
    if (!buf)
        return EFS_ERR_NOMEM;
    if (efs_recv_all(fd, buf, plen) != 0) {
        free(buf);
        return EFS_ERR_NET;
    }
    *payload = buf;
    return EFS_OK;
}
