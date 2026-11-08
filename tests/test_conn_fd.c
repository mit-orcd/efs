/* Pool fd-identity: a conn must not outlive the socket it wrapped.
 *
 * The AUTO-RDMA first-inode hang was a pooled efs_conn whose TCP fd number
 * was close()'d and recycled while the object (and dest_qpn) stayed in the
 * pool. Destroy must not close a recycled number — that FINs the new
 * owner's TCP and the peer tears down the QP the next send uses. */
#include "efs/network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                    msg);                                             \
            exit(1);                                                  \
        }                                                             \
    } while (0)

int main(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    struct efs_conn *c = efs_conn_wrap_tcp(sv[0], 0);
    CHECK(c != NULL, "wrap");
    CHECK(c->fd == sv[0], "fd");
    CHECK(efs_conn_fd_matches(c), "identity at wrap");

    /* Close behind the handle. The number is often reused immediately. */
    close(sv[0]);
    CHECK(!efs_conn_fd_matches(c), "closed fd must not match");

    int peer[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, peer) == 0, "recycle pair");
    CHECK(!efs_conn_fd_matches(c), "new sockets must not match wrap-time");

    char one = 1;
    CHECK(write(peer[0], &one, 1) == 1, "write before destroy");
    efs_conn_destroy(c);
    /* If destroy closed a recycled number that equalled c->fd, this write
     * (or the read) fails. The pair must still be a live socket. */
    char two = 2;
    CHECK(write(peer[0], &two, 1) == 1, "destroy must not close recycled fd");
    char got[2];
    CHECK(read(peer[1], got, 2) == 2, "peer still receives");
    CHECK(got[0] == 1 && got[1] == 2, "bytes");
    close(peer[0]);
    close(peer[1]);
    close(sv[1]);

    /* Owned fd: destroy does close it. */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair 2");
    c = efs_conn_wrap_tcp(sv[0], 0);
    CHECK(c && efs_conn_fd_matches(c), "wrap 2");
    efs_conn_destroy(c);
    CHECK(write(sv[0], &one, 1) < 0 && errno == EBADF, "owned fd closed");
    close(sv[1]);

    printf("test_conn_fd: OK\n");
    return 0;
}
