#ifndef EFS_TCP_COUNTS_H
#define EFS_TCP_COUNTS_H
#include <errno.h>
#include <linux/tcp.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

/* TCP_INFO receive sequence accounting includes FIN. A TCP_CLOSE state can
 * also follow RST, so state alone cannot authorize subtracting an octet.
 * Linux TCP_CM_INQ includes FIN while FIONREAD excludes it. A one-byte
 * MSG_PEEK obtains that hint without consuming application data.
 * OOBINLINE makes both queue counts include urgent payload too.
 * Bracket the queue samples with unchanged cumulative byte counters: an
 * arrival between syscalls must never masquerade as FIN. Missing/unstable
 * observations are unmeasured, not a successful zero-application-TCP audit. */
static int efs_tcp_payload_counts(int fd, uint64_t *tx, uint64_t *rx,
                                  unsigned *state, unsigned *fin)
{
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_OOBINLINE, &one, sizeof(one)) ||
        setsockopt(fd, IPPROTO_TCP, TCP_INQ, &one, sizeof(one))) return -1;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        struct tcp_info before, after;
        socklen_t n = sizeof(before);
        memset(&before, 0, sizeof(before));
        if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &before, &n) ||
            n < offsetof(struct tcp_info, tcpi_bytes_sent) + sizeof(before.tcpi_bytes_sent)) return -1;
        union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
        memset(&control, 0, sizeof(control));
        char byte;
        struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
        struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                              .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
        ssize_t peeked = recvmsg(fd, &msg, MSG_PEEK | MSG_DONTWAIT);
        int no_hint = peeked < 0;
        if (no_hint && errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNRESET) return -1;
        if (msg.msg_flags & MSG_CTRUNC) return -1;
        int hint = -1, queued = -1;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
            if (c->cmsg_level == IPPROTO_TCP && c->cmsg_type == TCP_CM_INQ &&
                c->cmsg_len == CMSG_LEN(sizeof(hint))) memcpy(&hint, CMSG_DATA(c), sizeof(hint));
        if (ioctl(fd, FIONREAD, &queued) || queued < 0) return -1;
        /* EAGAIN/reset provides no FIN certificate. Keep every raw receive
         * byte; reset alone never earns a one-octet discount. */
        if (no_hint) hint = queued;
        if (hint < 0) return -1;
        n = sizeof(after); memset(&after, 0, sizeof(after));
        if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &after, &n) ||
            n < offsetof(struct tcp_info, tcpi_bytes_sent) + sizeof(after.tcpi_bytes_sent)) return -1;
        if (before.tcpi_bytes_received != after.tcpi_bytes_received ||
            before.tcpi_bytes_sent != after.tcpi_bytes_sent) continue;
        if (hint != queued && (uint64_t)hint != (uint64_t)queued + 1) continue;
        *fin = hint != queued;
        if (after.tcpi_bytes_received < *fin) return -1;
        *state = after.tcpi_state;
        *tx = after.tcpi_bytes_sent;
        *rx = after.tcpi_bytes_received - *fin;
        return 0;
    }
    return -1;
}
#endif
