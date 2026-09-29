#ifndef EFS_NETWORK_H
#define EFS_NETWORK_H

#include <stddef.h>
#include <stdint.h>

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
};

/* Take ownership of fd. */
struct efs_conn *efs_conn_wrap_tcp(int fd, int is_server);

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
