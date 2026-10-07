#ifndef EFS_NETWORK_H
#define EFS_NETWORK_H

#include <stddef.h>
#include <stdint.h>

/* Thread-local absolute monotonic I/O budget; zero clears it. Shared with
 * client RPC scopes so partial frames cannot restart a relative timeout. */
void efs_net_set_deadline_ms(uint64_t deadline);
uint64_t efs_net_deadline_ms(void);
/* Clamp a relative wait; negative cap means unlimited. Zero means expired. */
int efs_net_remaining_ms(int cap_ms);

/* Create a TCP connection to host:port. Returns fd or -1 on error. */
int efs_connect_tcp(const char *host, uint16_t port);

/* Bind and listen on host:port. Returns fd or -1 on error. */
int efs_listen_tcp(const char *host, uint16_t port, int backlog);

/* Set receive timeout in milliseconds. */
int efs_set_recv_timeout(int fd, int ms);

/* Set send timeout in milliseconds. */
int efs_set_send_timeout(int fd, int ms);

/* SO_KEEPALIVE + short TCP_KEEPIDLE so a peer that FINs/vanishes is
 * reaped without a userspace recv timeout tearing down idle pool fds. */
int efs_tcp_keepalive(int fd);

/* Send exactly len bytes. Returns 0 on success, -1 on error. */

/* Receive exactly len bytes. Returns 0 on success, -1 on error/EOF. */
int efs_recv_all(int fd, void *buf, size_t len);

/* Transport handle: a TCP fd that may additionally carry an RDMA QP.
 * The fd stays open as the side-channel for oversized frames and as the
 * liveness/keepalive path even when kind == EFS_CONN_RDMA. */
#define EFS_CONN_TCP  0
#define EFS_CONN_RDMA 1

struct efs_conn {
    int kind;      /* EFS_CONN_TCP / EFS_CONN_RDMA */
    int is_server; /* responder: reply channel follows the request channel */
    int fd;
    int recv_chan; /* client: channel the in-flight reply arrives on;
                      server: channel the current request arrived on */
    struct efs_rdma_conn *rc;
    /* Kernel sockfs identity of `fd` at wrap. A pooled conn whose number
     * was close()'d and recycled still has dest_qpn for the old QP; the
     * new socket looks ESTABLISHED so a liveness poll cannot see it.
     * Destroy must not close `fd` when this no longer matches — that
     * would FIN the new owner's TCP and tear the peer QP down. */
    uint64_t fd_dev;
    uint64_t fd_ino;
    int fd_id_ok;
    /* Pool-owned conns compare this generation instead of fstat. The pool
     * is the only closer; a bump means the fd was dropped. */
    int use_gen;
    uint64_t gen_seen;
    uint64_t *gen_live;
    int64_t last_ok_ms;
    /* Reply wait bound for this conn, ms. 0 = EFS_IO_TIMEOUT_MS (30 s).
     * TCP conns carry it as SO_RCVTIMEO too; on an RDMA conn it bounds
     * efs_rdma_recv_wait, which used to be the 30 s pool default on every
     * conn (Sep 30 2026: a Raft peer lane sat 30 s on one lost RAFT_REPLY
     * and the unheard peer deposed the leader). */
    int recv_timeout_ms;
    /* Channel the last efs_conn_recv_msg* delivered on. A pipelined
     * sender that mixed an RDMA-sized message with a TCP-sized one gets
     * its replies on two channels and must count them per channel
     * (Sep 30 2026: the Raft peer lane between the two dual-group hosts
     * read TCP only after a big AppendEntries, left the RDMA reply in the
     * ring, and timed out every batch). */
    int last_recv_chan;
};

/* Take ownership of fd. */
struct efs_conn *efs_conn_wrap_tcp(int fd, int is_server);

/* Bound every reply wait on this conn (RDMA frame wait and the TCP
 * side-channel). ms <= 0 restores the EFS_IO_TIMEOUT_MS default. */
void efs_conn_set_recv_timeout(struct efs_conn *c, int ms);

/* 1 if this handle still owns fd. Pool conns compare a generation the
 * pool bumps on destroy; everyone else fstats. */
int efs_conn_fd_matches(const struct efs_conn *c);
/* Point a pool conn at its slot generation. slot must outlive the conn. */
void efs_conn_bind_gen(struct efs_conn *c, uint64_t *slot);
/* Successful send or recv. Checkouts skip the liveness probe for 1 s. */
void efs_conn_note_ok(struct efs_conn *c);

/* Destroy the QP (if any), close the fd when we still own it, free. */
void efs_conn_destroy(struct efs_conn *c);

#endif
