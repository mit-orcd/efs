#ifndef EFS_RDMA_H
#define EFS_RDMA_H

#include <stdint.h>
#include <stddef.h>

/* Two-sided RDMA (RC QP, SEND/RECV) transport for efs, bootstrapped over the
 * existing TCP connection. Identical wire framing (len + type + payload);
 * frames larger than the pool buffer stay on the TCP side-channel. */

struct efs_conn;         /* network.h */
struct efs_rdma_conn;    /* opaque, rdma.c */

/* EFS_TRANSPORT=auto|rdma|tcp (default auto: try RDMA, fall back to TCP).
 * "rdma" is strict: the client fails the connect when the upgrade fails. */
#define EFS_TRANSPORT_AUTO 0
#define EFS_TRANSPORT_RDMA 1
#define EFS_TRANSPORT_TCP  2

/* Pool buffer size: covers the largest chunk frame (PUT_CHUNK wire size is
 * 5 + ~56 hdr + 64 KiB) with headroom. Bigger frames use the TCP fd. */
#define EFS_RDMA_BUFSZ (72 * 1024)

/* Frames up to this size are sent IBV_SEND_INLINE (no pool buffer). */
#define EFS_RDMA_INLINE_MAX 256

int efs_rdma_transport(void);       /* EFS_TRANSPORT_* */
int efs_rdma_available(void);       /* transport != tcp and a usable IB dev */

/* Client: upgrade a fresh TCP conn to RDMA (SETUP handshake over TCP).
 * Returns 0 with conn->rc live, or -1 (conn stays usable as TCP). */
int efs_rdma_client_upgrade(struct efs_conn *c);

/* Server: handle an EFS_MSG_RDMA_SETUP payload received on conn. Fills
 * reply/reply_len (caller sends it over TCP). On success conn->rc is live
 * and recv buffers are posted before the reply goes out. */
int efs_rdma_server_accept(struct efs_conn *c, const void *setup_payload,
                           uint32_t payload_len, void *reply,
                           uint32_t *reply_len);

void efs_rdma_conn_destroy(struct efs_rdma_conn *rc);

/* ---- data path (used by the protocol.c dispatchers and handler.c) ---- */

/* Send a full frame (len+type+p1+p2). Inline when small, else a pool send
 * buffer (payload is copied in). Returns EFS_OK or EFS_ERR_NET. */
int efs_rdma_send_frame(struct efs_rdma_conn *rc, uint8_t type,
                        const void *p1, uint32_t l1,
                        const void *p2, uint32_t l2);

/* Zero-copy send for big replies: reserve a pool send buffer, fill the
 * payload area directly, then commit. Reserve returns NULL when no buffer
 * is available for that size (caller falls back to efs_conn_send_msg). */
void *efs_rdma_send_buf(struct efs_rdma_conn *rc, uint32_t payload_len);
int efs_rdma_send_commit(struct efs_rdma_conn *rc, void *buf, uint8_t type,
                         uint32_t payload_len);

/* Wait for a received frame (spin briefly, then block on the CQ channel).
 * On success the frame is at efs_rdma_recv_frame() until
 * efs_rdma_recv_repost(). timeout_ms < 0 waits forever. */
int efs_rdma_recv_wait(struct efs_rdma_conn *rc, int timeout_ms);
void *efs_rdma_recv_frame(struct efs_rdma_conn *rc, uint32_t *frame_len);
int efs_rdma_recv_repost(struct efs_rdma_conn *rc);

/* Arm the recv CQ and check for an already-queued completion. Returns 1 when
 * a frame is ready (recv_wait returns immediately), 0 when not (poll
 * efs_rdma_reply_fd()), -1 on error. */
int efs_rdma_reply_ready(struct efs_rdma_conn *rc);
int efs_rdma_reply_fd(struct efs_rdma_conn *rc);

/* Largest frame this conn carries over RDMA (min of both ends' buffers). */
uint32_t efs_rdma_max_frame(struct efs_rdma_conn *rc);

/* Number of live RDMA conns in this process (tests/validation). */
int efs_rdma_live_conns(void);

#endif
