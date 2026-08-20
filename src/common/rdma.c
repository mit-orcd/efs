/* RDMA (verbs, RC QP) transport for efs.
 *
 * Two-sided SEND/RECV over an RC QP bootstrapped with EFS_MSG_RDMA_SETUP on
 * the existing TCP connection. Wire framing is identical to TCP
 * ([len:4][type:1][payload]); frames bigger than the pool buffer stay on the
 * TCP fd (side-channel). The request/reply discipline guarantees at most one
 * outstanding request per connection, so a tiny fixed recv pool suffices and
 * the peer's reply channel is deterministic: replies follow the request's
 * channel, oversized replies (GET_META) are pinned to TCP by the client.
 *
 * Recv waits spin briefly (RDMA RTT is a few us) and then block on the CQ
 * event channel — the direct analog of today's blocking recv().
 */
#include "efs/rdma.h"
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/common.h"

#include <infiniband/verbs.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define EFS_RDMA_NRECV_DEFAULT 4 /* posted recv buffers per QP */
#define EFS_RDMA_NSEND         2 /* pool send buffers per QP */
#define EFS_RDMA_SPIN_US       200
/* Floor for the adaptive recv-spin budget: low enough that idle/sparse conns
 * don't burn CPU, high enough to still catch a reply within ~2x the ~5 us HW
 * RTT (validated: a fixed 20 us spin cut server CPU from ~70% us to ~6% us
 * under a 9-client write load without costing throughput). */
#define EFS_RDMA_SPIN_MIN      16
#define EFS_RDMA_SEND_WAIT_US  (5 * 1000 * 1000) /* a stuck send = dead QP */

#define SEND_WRID_POOL   0x1000
#define SEND_WRID_INLINE 0x2000

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int efs_rdma_transport(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *v = getenv("EFS_TRANSPORT");
        if (v && strcmp(v, "tcp") == 0)
            mode = EFS_TRANSPORT_TCP;
        else if (v && strcmp(v, "rdma") == 0)
            mode = EFS_TRANSPORT_RDMA;
        else
            mode = EFS_TRANSPORT_AUTO;
    }
    return mode;
}

static int spin_us(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("EFS_RDMA_SPIN_US");
        v = (e && *e) ? atoi(e) : EFS_RDMA_SPIN_US;
        if (v < 0)
            v = 0;
    }
    return v;
}

static int nrecv_bufs(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("EFS_RDMA_BUFS");
        v = (e && *e) ? atoi(e) : EFS_RDMA_NRECV_DEFAULT;
        if (v < 2)
            v = 2;
        if (v > 64)
            v = 64;
    }
    return v;
}

/* ---------------- device / PD cache ---------------- */

struct efs_rdma_dev {
    char name[64];
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    uint8_t port;
    uint16_t lid;
    enum ibv_mtu mtu;
};

static struct efs_rdma_dev g_devs[4];
static int g_dev_count;
static pthread_mutex_t g_dev_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_live_conns;
static int g_spinning; /* conn threads currently in the recv-CQ spin loop */

int efs_rdma_live_conns(void)
{
    return __sync_fetch_and_add(&g_live_conns, 0);
}

/* Adaptive recv-spin budget, scaled by the number of conn threads currently
 * spinning (g_spinning). Idle conns block on the comp channel and don't burn
 * CPU, so the aggregate spin cost tracks ACTIVE spinners, not live conns (the
 * cluster keeps ~200 conns mounted even when one client does IO, so live-conn
 * count is the wrong signal). Few spinners (single active client) → full
 * budget for lowest latency; many spinners (many active clients) → decay
 * toward EFS_RDMA_SPIN_MIN, since throughput is then pipelined across conns
 * and the aggregate spin would otherwise saturate the CPU (a fixed 200 us
 * spin across ~70+ spinners burned ~60-70% of server CPU under a 9-client
 * write load). */
static int spin_budget_for_load(void)
{
    int spinning = __sync_fetch_and_add(&g_spinning, 0);
    int max = spin_us();
    if (spinning <= 16)
        return max;
    if (spinning >= 64)
        return EFS_RDMA_SPIN_MIN;
    return max - (max - EFS_RDMA_SPIN_MIN) * (spinning - 16) / (64 - 16);
}

static int rdma_port_no(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("EFS_RDMA_PORT");
        v = (e && *e) ? atoi(e) : 1;
        if (v < 1)
            v = 1;
    }
    return v;
}

/* Open + validate one device (active IB port, non-zero LID). RoCE/Ethernet
 * link layers are rejected: the address vector uses dlid only. */
static int dev_open(const char *name, struct efs_rdma_dev *out)
{
    struct ibv_device **list = ibv_get_device_list(NULL);
    if (!list)
        return -1;
    struct ibv_device *d = NULL;
    for (int i = 0; list[i]; i++) {
        if (strcmp(ibv_get_device_name(list[i]), name) == 0) {
            d = list[i];
            break;
        }
    }
    if (!d) {
        ibv_free_device_list(list);
        return -1;
    }
    struct ibv_context *ctx = ibv_open_device(d);
    ibv_free_device_list(list);
    if (!ctx)
        return -1;
    struct ibv_port_attr pa;
    if (ibv_query_port(ctx, rdma_port_no(), &pa) != 0 ||
        pa.state != IBV_PORT_ACTIVE || pa.lid == 0 ||
        pa.link_layer != IBV_LINK_LAYER_INFINIBAND) {
        ibv_close_device(ctx);
        return -1;
    }
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) {
        ibv_close_device(ctx);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    strncpy(out->name, name, sizeof(out->name) - 1);
    out->ctx = ctx;
    out->pd = pd;
    out->port = (uint8_t)rdma_port_no();
    out->lid = pa.lid;
    out->mtu = pa.active_mtu;
    return 0;
}

static struct efs_rdma_dev *dev_cached(const char *name)
{
    struct efs_rdma_dev *r = NULL;
    pthread_mutex_lock(&g_dev_lock);
    for (int i = 0; i < g_dev_count; i++) {
        if (strcmp(g_devs[i].name, name) == 0) {
            r = &g_devs[i];
            break;
        }
    }
    if (!r && g_dev_count < (int)(sizeof(g_devs) / sizeof(g_devs[0]))) {
        if (dev_open(name, &g_devs[g_dev_count]) == 0)
            r = &g_devs[g_dev_count++];
    }
    pthread_mutex_unlock(&g_dev_lock);
    return r;
}

/* First device with an active IB port (loopback conns: both ends are local,
 * any HCA loops a QP back to itself). */
static struct efs_rdma_dev *dev_probe_first(void)
{
    struct ibv_device **list = ibv_get_device_list(NULL);
    if (!list)
        return NULL;
    struct efs_rdma_dev *r = NULL;
    for (int i = 0; list[i]; i++) {
        const char *nm = ibv_get_device_name(list[i]);
        if (nm && (r = dev_cached(nm)) != NULL)
            break; /* first device that passes dev_open validation */
    }
    ibv_free_device_list(list);
    return r;
}

static int ifname_for_ip(uint32_t ip, char *ifname, size_t len)
{
    struct ifaddrs *ifas = NULL;
    if (getifaddrs(&ifas) != 0)
        return -1;
    int found = -1;
    for (struct ifaddrs *a = ifas; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET)
            continue;
        if (((struct sockaddr_in *)a->ifa_addr)->sin_addr.s_addr == ip) {
            strncpy(ifname, a->ifa_name, len - 1);
            ifname[len - 1] = '\0';
            found = 0;
            break;
        }
    }
    freeifaddrs(ifas);
    return found;
}

static int ibdev_for_ifname(const char *ifname, char *devname, size_t len)
{
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/device/infiniband", ifname);
    DIR *d = opendir(path);
    if (!d)
        return -1;
    struct dirent *de;
    int found = -1;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        strncpy(devname, de->d_name, len - 1);
        devname[len - 1] = '\0';
        found = 0;
        break;
    }
    closedir(d);
    return found;
}

/* Pick the HCA that carries this TCP conn's local address. */
static struct efs_rdma_dev *dev_for_fd(int fd)
{
    const char *env = getenv("EFS_RDMA_DEV");
    if (env && *env)
        return dev_cached(env);

    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) != 0)
        return dev_probe_first();
    if (ss.ss_family != AF_INET)
        return dev_probe_first(); /* unix socketpair / v6: any local HCA */
    struct sockaddr_in *sa = (struct sockaddr_in *)&ss;
    uint32_t ip = sa->sin_addr.s_addr;
    if ((ntohl(ip) & 0xFF000000u) == 0x7F000000u)
        return dev_probe_first(); /* loopback: same-host QP on any HCA */

    char ifname[IF_NAMESIZE];
    char devname[64];
    if (ifname_for_ip(ip, ifname, sizeof(ifname)) != 0)
        return NULL;
    if (ibdev_for_ifname(ifname, devname, sizeof(devname)) != 0)
        return NULL;
    return dev_cached(devname);
}

int efs_rdma_available(void)
{
    static int avail = -1;
    if (efs_rdma_transport() == EFS_TRANSPORT_TCP)
        return 0;
    if (avail < 0) {
        struct ibv_device **list = ibv_get_device_list(NULL);
        avail = (list && list[0]) ? 1 : 0;
        if (list)
            ibv_free_device_list(list);
    }
    return avail;
}

/* ---------------- connection ---------------- */

struct efs_rdma_conn {
    struct efs_rdma_dev *dev;
    struct ibv_qp *qp;
    struct ibv_cq *send_cq;
    struct ibv_cq *recv_cq;
    struct ibv_comp_channel *recv_chan;
    struct ibv_mr *mr;
    uint8_t *arena;
    int nrecv;
    uint32_t bufsz;
    uint32_t max_frame; /* min(local, remote) buffer size */
    uint32_t psn;
    uint8_t **recv_bufs;
    uint8_t **send_bufs;
    int send_busy[EFS_RDMA_NSEND];
    int send_rr;
    int reserved; /* send buf held by efs_rdma_send_buf, -1 when none */
    int broken;
    int armed;    /* recv CQ notification armed */
    int pend_valid; /* completion harvested by efs_rdma_reply_ready */
    int pend_buf;
    uint32_t pend_len;
    int cur_buf;  /* buffer owned by the caller between wait and repost */
    uint32_t cur_len;
    int counted;  /* counted in g_live_conns (set on successful handshake) */
};

static int post_recv(struct efs_rdma_conn *rc, int idx)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)rc->recv_bufs[idx],
        .length = rc->bufsz,
        .lkey = rc->mr->lkey,
    };
    struct ibv_recv_wr wr = {
        .wr_id = (uint64_t)idx,
        .num_sge = 1,
        .sg_list = &sge,
    };
    struct ibv_recv_wr *bad = NULL;
    return ibv_post_recv(rc->qp, &wr, &bad);
}

static struct efs_rdma_conn *conn_create(struct efs_rdma_dev *dev)
{
    struct efs_rdma_conn *rc = calloc(1, sizeof(*rc));
    if (!rc)
        return NULL;
    rc->dev = dev;
    rc->nrecv = nrecv_bufs();
    rc->bufsz = EFS_RDMA_BUFSZ;
    rc->reserved = -1;
    rc->cur_buf = -1;
    rc->psn = (uint32_t)(rand() & 0xFFFFFF);

    rc->recv_chan = ibv_create_comp_channel(dev->ctx);
    if (!rc->recv_chan)
        goto fail;
    /* Send completions are reaped on every send/recv op; the send CQ only
     * ever holds a handful of unreaped entries. */
    rc->send_cq = ibv_create_cq(dev->ctx, 64, NULL, NULL, 0);
    rc->recv_cq = ibv_create_cq(dev->ctx, rc->nrecv + 4, NULL,
                                rc->recv_chan, 0);
    if (!rc->send_cq || !rc->recv_cq)
        goto fail;

    size_t total = (size_t)(rc->nrecv + EFS_RDMA_NSEND) * rc->bufsz;
    if (posix_memalign((void **)&rc->arena, 4096, total) != 0)
        goto fail;
    rc->mr = ibv_reg_mr(dev->pd, rc->arena, total, IBV_ACCESS_LOCAL_WRITE);
    if (!rc->mr)
        goto fail;

    rc->recv_bufs = calloc(rc->nrecv, sizeof(uint8_t *));
    rc->send_bufs = calloc(EFS_RDMA_NSEND, sizeof(uint8_t *));
    if (!rc->recv_bufs || !rc->send_bufs)
        goto fail;
    for (int i = 0; i < rc->nrecv; i++)
        rc->recv_bufs[i] = rc->arena + (size_t)i * rc->bufsz;
    for (int i = 0; i < EFS_RDMA_NSEND; i++)
        rc->send_bufs[i] = rc->arena + (size_t)(rc->nrecv + i) * rc->bufsz;

    struct ibv_qp_init_attr ia;
    memset(&ia, 0, sizeof(ia));
    ia.send_cq = rc->send_cq;
    ia.recv_cq = rc->recv_cq;
    ia.cap.max_send_wr = 32;
    ia.cap.max_recv_wr = rc->nrecv + 4;
    ia.cap.max_send_sge = 1;
    ia.cap.max_recv_sge = 1;
    ia.cap.max_inline_data = EFS_RDMA_INLINE_MAX;
    ia.qp_type = IBV_QPT_RC;
    ia.sq_sig_all = 1;
    rc->qp = ibv_create_qp(dev->pd, &ia);
    if (!rc->qp)
        goto fail;

    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = dev->port;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE;
    if (ibv_modify_qp(rc->qp, &attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                      IBV_QP_ACCESS_FLAGS) != 0)
        goto fail;

    /* Recv buffers go up front: the peer may SEND as soon as it is RTS. */
    for (int i = 0; i < rc->nrecv; i++) {
        if (post_recv(rc, i) != 0)
            goto fail;
    }
    return rc;

fail:
    efs_rdma_conn_destroy(rc);
    return NULL;
}

static int qp_rtr(struct efs_rdma_conn *rc, uint32_t lid, uint32_t qpn,
                  uint32_t psn, enum ibv_mtu mtu)
{
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = mtu;
    attr.dest_qp_num = qpn;
    attr.rq_psn = psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.dlid = (uint16_t)lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.is_global = 0;
    attr.ah_attr.port_num = rc->dev->port;
    return ibv_modify_qp(rc->qp, &attr,
                         IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                         IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                         IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
}

static int qp_rts(struct efs_rdma_conn *rc)
{
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = rc->psn;
    attr.max_rd_atomic = 1;
    return ibv_modify_qp(rc->qp, &attr,
                         IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                         IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                         IBV_QP_MAX_QP_RD_ATOMIC);
}

static enum ibv_mtu mtu_min(enum ibv_mtu a, int b)
{
    return (enum ibv_mtu)((int)a < b ? (int)a : b);
}

int efs_rdma_client_upgrade(struct efs_conn *c)
{
    if (!efs_rdma_available())
        return -1;
    struct efs_rdma_dev *dev = dev_for_fd(c->fd);
    if (!dev)
        return -1;
    struct efs_rdma_conn *rc = conn_create(dev);
    if (!rc)
        return -1;

    struct efs_msg_rdma_setup req;
    memset(&req, 0, sizeof(req));
    req.lid = dev->lid;
    req.qpn = rc->qp->qp_num;
    req.psn = rc->psn;
    req.mtu = (uint32_t)dev->mtu;
    req.buf_size = rc->bufsz;

    uint8_t type = 0;
    void *payload = NULL;
    uint32_t payload_len = 0;
    struct efs_msg_rdma_setup_reply *rep = NULL;
    int ok = 0;
    if (efs_send_msg(c->fd, EFS_MSG_RDMA_SETUP, &req, sizeof(req)) == 0 &&
        efs_recv_msg(c->fd, &type, &payload, &payload_len) == 0 &&
        type == EFS_MSG_RDMA_SETUP_REPLY &&
        payload_len >= sizeof(struct efs_msg_rdma_setup_reply)) {
        rep = payload;
        if (rep->status == EFS_RDMA_STATUS_OK &&
            qp_rtr(rc, rep->lid, rep->qpn, rep->psn,
                   mtu_min(dev->mtu, (int)rep->mtu)) == 0 &&
            qp_rts(rc) == 0) {
            rc->max_frame = rep->buf_size < rc->bufsz ? rep->buf_size
                                                      : rc->bufsz;
            ok = 1;
        }
    }
    free(payload);
    if (!ok) {
        efs_rdma_conn_destroy(rc);
        return -1;
    }
    c->rc = rc;
    c->kind = EFS_CONN_RDMA;
    rc->counted = 1;
    __sync_fetch_and_add(&g_live_conns, 1);
    static int logged;
    if (!__sync_lock_test_and_set(&logged, 1))
        fprintf(stderr, "efs: RDMA transport up on %s port %u "
                "(lid %u, %u x %u KiB bufs/conn)\n",
                dev->name, dev->port, dev->lid, rc->nrecv + EFS_RDMA_NSEND,
                rc->bufsz / 1024);
    return 0;
}

int efs_rdma_server_accept(struct efs_conn *c, const void *setup_payload,
                           uint32_t payload_len, void *reply,
                           uint32_t *reply_len)
{
    struct efs_msg_rdma_setup_reply *rep = reply;
    memset(rep, 0, sizeof(*rep));
    *reply_len = sizeof(*rep);
    rep->status = EFS_RDMA_STATUS_UNSUPPORTED;

    if (payload_len < sizeof(struct efs_msg_rdma_setup) || c->rc)
        return -1;
    if (!efs_rdma_available())
        return -1;
    struct efs_rdma_dev *dev = dev_for_fd(c->fd);
    if (!dev)
        return -1;

    const struct efs_msg_rdma_setup *req = setup_payload;
    struct efs_rdma_conn *rc = conn_create(dev);
    if (!rc)
        return -1;
    if (qp_rtr(rc, req->lid, req->qpn, req->psn,
               mtu_min(dev->mtu, (int)req->mtu)) != 0 ||
        qp_rts(rc) != 0) {
        efs_rdma_conn_destroy(rc);
        return -1;
    }
    rc->max_frame = req->buf_size < rc->bufsz ? req->buf_size : rc->bufsz;

    rep->status = EFS_RDMA_STATUS_OK;
    rep->lid = dev->lid;
    rep->qpn = rc->qp->qp_num;
    rep->psn = rc->psn;
    rep->mtu = (uint32_t)dev->mtu;
    rep->buf_size = rc->bufsz;

    c->rc = rc;
    c->kind = EFS_CONN_RDMA;
    rc->counted = 1;
    __sync_fetch_and_add(&g_live_conns, 1);
    static int logged;
    if (!__sync_lock_test_and_set(&logged, 1))
        fprintf(stderr, "efsd: RDMA transport up on %s port %u (lid %u)\n",
                dev->name, dev->port, dev->lid);
    return 0;
}

void efs_rdma_conn_destroy(struct efs_rdma_conn *rc)
{
    if (!rc)
        return;
    if (rc->qp)
        ibv_destroy_qp(rc->qp);
    if (rc->send_cq)
        ibv_destroy_cq(rc->send_cq);
    if (rc->recv_cq)
        ibv_destroy_cq(rc->recv_cq);
    if (rc->recv_chan)
        ibv_destroy_comp_channel(rc->recv_chan);
    if (rc->mr)
        ibv_dereg_mr(rc->mr);
    int counted = rc->counted;
    free(rc->arena);
    free(rc->recv_bufs);
    free(rc->send_bufs);
    free(rc);
    if (counted)
        __sync_fetch_and_sub(&g_live_conns, 1);
}

uint32_t efs_rdma_max_frame(struct efs_rdma_conn *rc)
{
    return rc->max_frame;
}

/* ---------------- send path ---------------- */

static int reap_sends(struct efs_rdma_conn *rc)
{
    struct ibv_wc wc[8];
    for (;;) {
        int n = ibv_poll_cq(rc->send_cq, 8, wc);
        if (n < 0) {
            rc->broken = 1;
            return EFS_ERR_NET;
        }
        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                rc->broken = 1;
                return EFS_ERR_NET;
            }
            if ((wc[i].wr_id & 0xF000u) == SEND_WRID_POOL)
                rc->send_busy[wc[i].wr_id & 0xFF] = 0;
        }
        if (n < 8)
            break;
    }
    return EFS_OK;
}

/* Round-robin a pool send buffer, waiting (spin) if its previous send is
 * still in flight. A send that outlives EFS_RDMA_SEND_WAIT_US means the QP
 * is dead. */
static int send_buf_pick(struct efs_rdma_conn *rc)
{
    int idx = rc->send_rr;
    rc->send_rr = (rc->send_rr + 1) % EFS_RDMA_NSEND;
    if (reap_sends(rc) != 0)
        return -1;
    if (!rc->send_busy[idx])
        return idx;
    int64_t end = now_us() + EFS_RDMA_SEND_WAIT_US;
    while (rc->send_busy[idx]) {
        if (reap_sends(rc) != 0)
            return -1;
        if (!rc->send_busy[idx])
            break;
        if (now_us() > end) {
            rc->broken = 1;
            return -1;
        }
        sched_yield();
    }
    return idx;
}

static int post_send(struct efs_rdma_conn *rc, uint64_t wr_id,
                     const void *buf, uint32_t len, int inline_ok)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)buf,
        .length = len,
        .lkey = rc->mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id = wr_id,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
        .num_sge = 1,
        .sg_list = &sge,
    };
    if (inline_ok)
        wr.send_flags |= IBV_SEND_INLINE;
    struct ibv_send_wr *bad = NULL;
    if (ibv_post_send(rc->qp, &wr, &bad) != 0) {
        rc->broken = 1;
        return EFS_ERR_NET;
    }
    return EFS_OK;
}

int efs_rdma_send_frame(struct efs_rdma_conn *rc, uint8_t type,
                        const void *p1, uint32_t l1,
                        const void *p2, uint32_t l2)
{
    if (rc->broken)
        return EFS_ERR_NET;
    uint32_t payload = l1 + l2;
    uint32_t frame = 5 + payload;
    if (frame > rc->max_frame)
        return EFS_ERR_NET; /* caller must use the TCP side-channel */

    if (frame <= EFS_RDMA_INLINE_MAX) {
        uint8_t tmp[EFS_RDMA_INLINE_MAX];
        uint32_t nl = htonl(1 + payload);
        memcpy(tmp, &nl, 4);
        tmp[4] = type;
        if (l1)
            memcpy(tmp + 5, p1, l1);
        if (l2)
            memcpy(tmp + 5 + l1, p2, l2);
        if (post_send(rc, SEND_WRID_INLINE, tmp, frame, 1) != 0)
            return EFS_ERR_NET;
        return reap_sends(rc);
    }

    int idx = send_buf_pick(rc);
    if (idx < 0)
        return EFS_ERR_NET;
    uint8_t *b = rc->send_bufs[idx];
    uint32_t nl = htonl(1 + payload);
    memcpy(b, &nl, 4);
    b[4] = type;
    if (l1)
        memcpy(b + 5, p1, l1);
    if (l2)
        memcpy(b + 5 + l1, p2, l2);
    if (post_send(rc, SEND_WRID_POOL | (uint64_t)idx, b, frame, 0) != 0)
        return EFS_ERR_NET;
    rc->send_busy[idx] = 1;
    return EFS_OK;
}

void *efs_rdma_send_buf(struct efs_rdma_conn *rc, uint32_t payload_len)
{
    if (rc->broken)
        return NULL;
    uint32_t frame = 5 + payload_len;
    if (frame > rc->max_frame || frame <= EFS_RDMA_INLINE_MAX)
        return NULL;
    int idx = send_buf_pick(rc);
    if (idx < 0)
        return NULL;
    rc->reserved = idx;
    return rc->send_bufs[idx] + 5;
}

int efs_rdma_send_commit(struct efs_rdma_conn *rc, void *buf, uint8_t type,
                         uint32_t payload_len)
{
    int idx = rc->reserved;
    rc->reserved = -1;
    if (idx < 0 || buf != rc->send_bufs[idx] + 5 || rc->broken) {
        rc->broken = 1;
        return EFS_ERR_NET;
    }
    uint8_t *b = rc->send_bufs[idx];
    uint32_t nl = htonl(1 + payload_len);
    memcpy(b, &nl, 4);
    b[4] = type;
    if (post_send(rc, SEND_WRID_POOL | (uint64_t)idx, b, 5 + payload_len,
                  0) != 0)
        return EFS_ERR_NET;
    rc->send_busy[idx] = 1;
    return EFS_OK;
}

/* ---------------- recv path ---------------- */

/* Pair every retrieved channel event with an ack; drain is only ever called
 * when the CQ was armed and we harvested a CQE directly (the event for it
 * may or may not have been queued yet). */
static void drain_chan(struct efs_rdma_conn *rc)
{
    struct pollfd p = { .fd = rc->recv_chan->fd, .events = POLLIN };
    if (poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
        struct ibv_cq *cq = NULL;
        void *ctx = NULL;
        if (ibv_get_cq_event(rc->recv_chan, &cq, &ctx) == 0)
            ibv_ack_cq_events(cq, 1);
    }
    rc->armed = 0;
}

static int recv_wc_ok(struct efs_rdma_conn *rc, struct ibv_wc *wc)
{
    if (wc->status != IBV_WC_SUCCESS) {
        rc->broken = 1;
        return EFS_ERR_NET;
    }
    rc->cur_buf = (int)wc->wr_id;
    rc->cur_len = wc->byte_len;
    return EFS_OK;
}

int efs_rdma_recv_wait(struct efs_rdma_conn *rc, int timeout_ms)
{
    if (rc->broken)
        return EFS_ERR_NET;
    if (rc->pend_valid) {
        rc->pend_valid = 0;
        rc->cur_buf = rc->pend_buf;
        rc->cur_len = rc->pend_len;
        return EFS_OK;
    }
    int64_t spin_end = now_us() + spin_us();
    int64_t deadline = timeout_ms >= 0 ? now_ms() + timeout_ms : -1;
    /* now_us() is a vDSO clock_gettime (~20 ns); called per CQ poll it was
     * ~20% of efs-fuse CPU under the 9-client ecopy storm. Gate the clock to
     * every 64th poll — spin-exit granularity stays in the low µs. */
    uint32_t polls = 0;
    for (;;) {
        struct ibv_wc wc;
        int n = ibv_poll_cq(rc->recv_cq, 1, &wc);
        if (n < 0) {
            rc->broken = 1;
            return EFS_ERR_NET;
        }
        if (n > 0) {
            if (recv_wc_ok(rc, &wc) != 0)
                return EFS_ERR_NET;
            if (rc->armed)
                drain_chan(rc);
            return EFS_OK;
        }
        if (((++polls) & 63) != 0 || now_us() < spin_end)
            continue;
        if (!rc->armed) {
            if (ibv_req_notify_cq(rc->recv_cq, 0) != 0) {
                rc->broken = 1;
                return EFS_ERR_NET;
            }
            rc->armed = 1;
            continue; /* re-poll: catches anything queued before the arm */
        }
        int ms = -1;
        if (deadline >= 0) {
            int64_t left = deadline - now_ms();
            if (left <= 0)
                return EFS_ERR_NET;
            ms = left > INT32_MAX ? INT32_MAX : (int)left;
        }
        struct pollfd p = { .fd = rc->recv_chan->fd, .events = POLLIN };
        int pr = poll(&p, 1, ms);
        if (pr == 0)
            return EFS_ERR_NET; /* timeout: caller drops the conn */
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            rc->broken = 1;
            return EFS_ERR_NET;
        }
        struct ibv_cq *cq = NULL;
        void *ctx = NULL;
        if (ibv_get_cq_event(rc->recv_chan, &cq, &ctx) == 0)
            ibv_ack_cq_events(cq, 1);
        rc->armed = 0;
    }
}

void *efs_rdma_recv_frame(struct efs_rdma_conn *rc, uint32_t *frame_len)
{
    if (rc->cur_buf < 0)
        return NULL;
    if (frame_len)
        *frame_len = rc->cur_len;
    return rc->recv_bufs[rc->cur_buf];
}

int efs_rdma_recv_repost(struct efs_rdma_conn *rc)
{
    if (rc->cur_buf < 0)
        return EFS_OK;
    int idx = rc->cur_buf;
    rc->cur_buf = -1;
    if (post_recv(rc, idx) != 0) {
        rc->broken = 1;
        return EFS_ERR_NET;
    }
    return EFS_OK;
}

int efs_rdma_reply_ready(struct efs_rdma_conn *rc)
{
    if (rc->broken)
        return -1;
    if (rc->pend_valid)
        return 1;
    if (!rc->armed) {
        if (ibv_req_notify_cq(rc->recv_cq, 0) != 0) {
            rc->broken = 1;
            return -1;
        }
        rc->armed = 1;
    }
    /* Arm first (so a later completion still fires the channel event), then
     * spin-poll before reporting "not ready". Callers react to 0 by
     * sleep-polling the comp channel, whose interrupt-moderated wakeup cost
     * ~1.9 ms per request-reply exchange here — 4x slower than TCP and 380x
     * the 5 us hardware RTT. The spin catches replies within microseconds.
     *
     * The budget scales with the active-spinner count (spin_budget_for_load):
     * full for a single active client, decaying toward EFS_RDMA_SPIN_MIN under
     * many concurrent spinners where a fixed 200 us spin would saturate CPU. */
    __sync_fetch_and_add(&g_spinning, 1);
    int budget = spin_budget_for_load();
    int64_t spin_end = now_us() + budget;
    uint32_t polls = 0;
    int result = 0;
    for (;;) {
        struct ibv_wc wc;
        int n = ibv_poll_cq(rc->recv_cq, 1, &wc);
        if (n < 0) {
            rc->broken = 1;
            result = -1;
            break;
        }
        if (n > 0) {
            if (recv_wc_ok(rc, &wc) != 0) {
                result = -1;
                break;
            }
            rc->pend_valid = 1;
            rc->pend_buf = rc->cur_buf;
            rc->pend_len = rc->cur_len;
            rc->cur_buf = -1;
            drain_chan(rc);
            result = 1;
            break;
        }
        if (((++polls) & 63) == 0 && now_us() >= spin_end)
            break; /* result = 0 */
    }
    __sync_fetch_and_sub(&g_spinning, 1);
    return result;
}

int efs_rdma_reply_fd(struct efs_rdma_conn *rc)
{
    return rc->recv_chan->fd;
}
