#ifndef EFS_RDMA_H
#define EFS_RDMA_H

#include <stdint.h>
#include <stddef.h>

/* Two-sided RDMA (RC QP, SEND/RECV) transport for efs, bootstrapped over the
 * existing TCP connection. Identical wire framing (len + type + payload);
 * all upgraded application messages use bounded segmentation. */

struct efs_conn;         /* network.h */
struct efs_rdma_conn;    /* opaque, rdma.c */

/* EFS_TRANSPORT=auto|rdma|tcp (default auto: try RDMA, fall back to TCP).
 * "rdma" is strict: the client fails the connect when the upgrade fails. */
#define EFS_TRANSPORT_AUTO 0
#define EFS_TRANSPORT_RDMA 1
#define EFS_TRANSPORT_TCP  2

/* Physical registered frame capacity; DATA usable bytes subtract the
 * segmentation envelope. Logical messages are negotiated separately. */
#define EFS_RDMA_BUFSZ (84 * 1024)

/* Frames up to this size are sent IBV_SEND_INLINE (no pool buffer). */
#define EFS_RDMA_INLINE_MAX 256

int efs_rdma_transport(void);       /* EFS_TRANSPORT_* */
int efs_rdma_available(void);       /* transport != tcp and a usable IB dev */

/* NUMA node of the HCA on the route to host:port (EFS_RDMA_DEV wins),
 * -1 when unknown or TCP. devname (optional) receives the HCA name. */
int efs_rdma_numa_node_for_host(const char *host, uint16_t port,
                                char *devname, size_t devlen);

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

/* Send one logical frame as copied segments, preserving the original I/O
 * budget through credit/send-slot waits. Source parts may be reused on return.
 * Failure poisons the connection; there is no application TCP fallback. */
int efs_rdma_send_frame(struct efs_rdma_conn *rc, uint8_t type,
                        const void *p1, uint32_t l1,
                        const void *p2, uint32_t l2);

#define EFS_RDMA_ZC_MIN 4096u

/* Retained client-slab registration API; segmented sends currently copy. */
int efs_rdma_zc_region_add(void *base, size_t len);

/* Wait (bounded by the send deadline) until every send posted on rc has
 * completed. Returns 0, or -1 when the conn is broken. */
int efs_rdma_send_quiesce(struct efs_rdma_conn *rc);

/* Counters for raft-obs / fuse stats: zero-copy frames vs copied frames
 * that were eligible by size. */
void efs_rdma_zc_stats(uint64_t *zc, uint64_t *copied);

/* Direct-fill fast path is currently disabled (returns NULL); generic reply
 * fallback segments over RDMA and copies before TLS reply storage is reused. */
void *efs_rdma_send_buf(struct efs_rdma_conn *rc, uint32_t payload_len);
int efs_rdma_send_commit(struct efs_rdma_conn *rc, void *buf, uint8_t type,
                         uint32_t payload_len);

/* Repost the receive buffer the message path consumed. */
int efs_rdma_recv_repost(struct efs_rdma_conn *rc);
/* Arm the CQ event fd and return it for poll(); a frame pended before the
 * arm self-signals, so the poll does not sleep on a non-empty ring. */
int efs_rdma_reply_fd(struct efs_rdma_conn *rc);

struct efs_rdma_message_stats {
    uint64_t data_frames_tx, data_bytes_tx, data_frames_rx, data_bytes_rx;
    uint64_t app_tx, app_rx, logical_bytes_tx, logical_bytes_rx;
    uint64_t controls_tx, controls_rx, credit_stalls, tcp_app_rx;
    uint64_t assembly_bytes, global_assembly_bytes;
};
void efs_rdma_message_stats(struct efs_rdma_conn *rc,struct efs_rdma_message_stats *out);
uint64_t efs_rdma_assembly_bytes(void);
void efs_rdma_tcp_violation(struct efs_rdma_conn *rc);

/* Complete application messages; borrowed frame until release or take.
 * One checked-out owner must serialize every send/progress/receive/teardown. */
int efs_rdma_message_ready(struct efs_rdma_conn *rc);
int efs_rdma_message_wait(struct efs_rdma_conn *rc,int timeout_ms);
void *efs_rdma_message_frame(struct efs_rdma_conn *rc,uint32_t *len);
void efs_rdma_message_release(struct efs_rdma_conn *rc);
void *efs_rdma_message_take(struct efs_rdma_conn *rc);
unsigned efs_rdma_message_capacity(struct efs_rdma_conn *rc);
int efs_rdma_message_poll_ms(struct efs_rdma_conn *rc);

/* Negotiated maximum logical frame, including five-byte application header. */
uint32_t efs_rdma_max_frame(struct efs_rdma_conn *rc);

/* Local QP number, for diagnostics: a peer that keeps sending to a QP this
 * side already destroyed gets no error anywhere, so teardown logs must name
 * the QP to be correlatable with the sender's dest_qpn. */
uint32_t efs_rdma_qpn(struct efs_rdma_conn *rc);

/* Number of live RDMA conns in this process (tests/validation). */
int efs_rdma_live_conns(void);

#endif
