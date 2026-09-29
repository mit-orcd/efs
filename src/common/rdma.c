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
 * All conns share one recv CQ per device, harvested by a single poller
 * thread that fans completions out to per-conn SPSC rings (wr_id encodes
 * the conn's registry slot). Recv waits spin briefly on the ring (an atomic
 * load — no CQ lock) and then block on the conn's eventfd. This replaced
 * per-conn recv CQs, whose per-thread spin-polling (ibv_poll_cq lock churn)
 * was the top CPU consumer on both client and server under load.
 */
#include "efs/rdma.h"
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/wire.h"
#include "efs/common.h"

#include <infiniband/verbs.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define EFS_RDMA_NRECV_DEFAULT 4 /* posted recv buffers per QP */
/* One buffer per in-flight send on this QP. The write pipeline posts
 * EFS_WRITE_PIPELINE chunks; with 2 buffers the third send spun in
 * send_buf_pick (W14 step 5 / W15 step 4). */
#define EFS_RDMA_NSEND         EFS_WRITE_PIPELINE
#define EFS_RDMA_SPIN_US       200
/* Floor for the adaptive recv-spin budget: low enough that idle/sparse conns
 * don't burn CPU, high enough to still catch a reply within ~2x the ~5 us HW
 * RTT (validated: a fixed 20 us spin cut server CPU from ~70% us to ~6% us
 * under a 9-client write load without costing throughput). */
#define EFS_RDMA_SPIN_MIN      16
#define EFS_RDMA_SEND_WAIT_US  (5 * 1000 * 1000) /* a stuck send = dead QP */

/* Recv SGEs are posted this far into each recv buffer so the fragment data
 * of a PUT_CHUNK (5-byte frame header + efs_msg_put_chunk) lands exactly on
 * a 4096 boundary. The server's O_DIRECT fragment write can then go
 * DMA->disk with no bounce-buffer copy (that copy was ~11% of efsd CPU
 * under a 9-client write load). Only PUT data needs the alignment; other
 * payloads simply sit at an unaligned offset, which they never notice. */
#define EFS_RDMA_RECV_ALIGN \
    ((4096 - ((5 + sizeof(struct efs_msg_put_chunk)) % 4096)) % 4096)
#define EFS_RDMA_RECV_STRIDE   4096 /* extra slack per recv buffer */

#define SEND_WRID_POOL   0x1000
#define SEND_WRID_INLINE 0x2000

/* What a file descriptor currently points at, e.g. "socket:[12345]". */
static void fd_link_now(int fd, char *out, size_t n)
{
    out[0] = '\0';
    if (fd < 0)
        return;
    char p[64];
    snprintf(p, sizeof(p), "/proc/self/fd/%d", fd);
    ssize_t r = readlink(p, out, n - 1);
    out[r > 0 ? r : 0] = '\0';
}

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

static int rdma_first_log(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("EFS_RDMA_FIRST") ? 1 : 0;
    return v;
}

/* RNR retry count for the send QP. 7 means retry forever; see qp_rts.
 * The default keeps the shipped behaviour; the knob exists so a hang can be
 * turned into a loud error completion. Setting it finite is what disproved
 * the "first inode LOOKUP is stuck in an RNR retry loop" hypothesis. */
static int rdma_rnr_retry(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("EFS_RDMA_RNR_RETRY");
        v = (e && *e) ? atoi(e) : 7;
        if (v < 0)
            v = 0;
        if (v > 7)
            v = 7;
    }
    return v;
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

/* mlx5 completion moderation holds a CQE back for cq_period even when the
 * CQ is polled. A raft AppendEntries round trip was ~15us on TCP and
 * ~380us on RDMA (private 3-node, Sep 27); the send-CQE wait was most of
 * that. Ask for a completion per CQE with no timer. */
static void cq_unmoderate(struct ibv_cq *cq)
{
    struct ibv_modify_cq_attr a;
    memset(&a, 0, sizeof(a));
    a.attr_mask = IBV_CQ_ATTR_MODERATE;
    a.moderate.cq_count = 1;
    a.moderate.cq_period = 0;
    if (ibv_modify_cq(cq, &a) != 0) {
        static int logged;
        if (!__sync_lock_test_and_set(&logged, 1))
            fprintf(stderr, "efs: CQ moderation off failed errno=%d\n",
                    errno);
    }
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
    /* Shared recv completion channel: every conn's QP posts recvs here and
     * ONE poller thread harvests all completions, pushing frames onto
     * per-conn SPSC rings. Replaces per-conn CQs, whose spin-polling by
     * every conn thread was the top CPU consumer on client AND server. */
    struct ibv_cq *recv_cq;
    struct ibv_comp_channel *recv_chan;
    pthread_t poller;
    int poller_started;
};

static struct efs_rdma_dev g_devs[4];
static int g_dev_count;
static pthread_mutex_t g_dev_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_live_conns;
static int g_spinning; /* conn threads currently in the recv-ring spin wait */

/* Conn registry: recv WR wr_id = (gen << 32) | (reg_idx << 8) | buf_idx, so
 * the shared-CQ poller can map a completion back to its conn. The generation
 * tag is bumped each time a slot is freed: completions a dead QP already
 * generated live on in the SHARED CQ after ibv_destroy_qp (a per-conn CQ took
 * them to the grave), and without the tag the poller would inject them into
 * whoever reuses the slot. */
#define EFS_RDMA_MAX_CONNS  4096
#define EFS_RDMA_PEND_RING  64 /* >= max nrecv bufs per conn */
static struct efs_rdma_conn *g_conn_reg[EFS_RDMA_MAX_CONNS];
static uint32_t g_reg_gen[EFS_RDMA_MAX_CONNS];
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;

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
    if (ifname_for_ip(ip, ifname, sizeof(ifname)) == 0 &&
        ibdev_for_ifname(ifname, devname, sizeof(devname)) == 0) {
        struct efs_rdma_dev *r = dev_cached(devname);
        if (r)
            return r;
        /* TCP NIC mapped to a RoCE/down port (lid=0). Fall through to a
         * native-IB HCA on the same host — fcstor Ethernet is mlx5_0,
         * IPoIB data is mlx5_2. */
    }
    return dev_probe_first();
}

int efs_rdma_available(void)
{
    static int avail = -1;
    if (efs_rdma_transport() == EFS_TRANSPORT_TCP)
        return 0;
    if (avail < 0)
        avail = (dev_probe_first() != NULL) ? 1 : 0;
    return avail;
}

/* ---------------- connection ---------------- */

struct efs_rdma_conn {
    struct efs_rdma_dev *dev;
    struct ibv_qp *qp;
    struct ibv_cq *send_cq;
    struct ibv_mr *mr;
    uint8_t *arena;
    int nrecv;
    uint32_t bufsz;
    uint32_t max_frame; /* min(local, remote) buffer size */
    uint32_t psn;
    uint8_t **recv_bufs;
    uint8_t **send_bufs;
    int send_busy[EFS_RDMA_NSEND];
    int64_t send_posted_us[EFS_RDMA_NSEND]; /* when send_busy was set */
    uint64_t n_posted; /* sends posted / send CQEs reaped on this conn; a */
    uint64_t n_reaped; /* reaped==posted at a "no CQE" timeout means the   */
                       /* completion arrived and the flag was missed.      */
    int send_rr;
    int reserved; /* send buf held by efs_rdma_send_buf, -1 when none */
    int broken;
    /* /proc/self/fd link of tcp_fd at upgrade time. The peer tears down its
     * QP when this socket hits EOF, so if the link changed (or vanished) the
     * socket was closed behind this conn's back and the fd number reused. */
    char tcp_link[64];
    int reg_idx;  /* conn registry slot ((wr_id >> 8) & 0xFFFFFF), -1 until registered */
    uint32_t reg_gen; /* registry slot generation (wr_id >> 32) */
    int efd;      /* eventfd: poller signals when the pend ring empties->fills */
    /* SPSC pend ring: the shared-CQ poller is the only producer; the conn's
     * current owner thread is the only consumer. Capacity covers every
     * posted recv buffer, so it can never overflow in correct operation. */
    uint8_t pr_buf[EFS_RDMA_PEND_RING];
    uint32_t pr_len[EFS_RDMA_PEND_RING];
    int pr_head; /* poller-written */
    int pr_tail; /* consumer-written */
    int cur_buf;  /* buffer owned by the caller between wait and repost */
    uint32_t cur_len;
    int counted;  /* counted in g_live_conns (set on successful handshake) */
    int tcp_fd;   /* control-channel socket, borrowed from efs_conn: polled
                   * alongside efd so a dead peer (FIN/RST) wakes the wait
                   * even though the QP itself never errors when idle */
    uint32_t max_inline; /* actual QP inline cap (may be < EFS_RDMA_INLINE_MAX) */
};

static int post_recv(struct efs_rdma_conn *rc, int idx)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)rc->recv_bufs[idx] + EFS_RDMA_RECV_ALIGN,
        .length = rc->bufsz,
        .lkey = rc->mr->lkey,
    };
    struct ibv_recv_wr wr = {
        .wr_id = ((uint64_t)rc->reg_gen << 32) |
                 ((uint64_t)(uint32_t)rc->reg_idx << 8) | (uint64_t)idx,
        .num_sge = 1,
        .sg_list = &sge,
    };
    struct ibv_recv_wr *bad = NULL;
    return ibv_post_recv(rc->qp, &wr, &bad);
}

/* Push one harvested recv completion onto the conn's pend ring. Called only
 * by the device's shared-CQ poller. */
static void pend_push(struct efs_rdma_conn *rc, int buf, uint32_t len)
{
    int head = rc->pr_head;
    int next = (head + 1) & (EFS_RDMA_PEND_RING - 1);
    if (next == __atomic_load_n(&rc->pr_tail, __ATOMIC_ACQUIRE)) {
        rc->broken = 1; /* ring overflow: protocol/bug — kill the conn */
        buf = -1;
    }
    if (buf >= 0) {
        rc->pr_buf[head] = (uint8_t)buf;
        rc->pr_len[head] = len;
        __atomic_store_n(&rc->pr_head, next, __ATOMIC_RELEASE);
    }
    int empty_before = (head == __atomic_load_n(&rc->pr_tail,
                                                __ATOMIC_ACQUIRE));
    if (empty_before || buf < 0) {
        uint64_t one = 1;
        if (write(rc->efd, &one, sizeof(one)) < 0 && errno != EAGAIN)
            rc->broken = 1;
    }
}

static void harvest_recv_wcs(struct ibv_wc *wcs, int n)
{
    if (rdma_first_log()) {
        static int nh;
        int k = __sync_fetch_and_add(&nh, 1);
        if (k < 6)
            fprintf(stderr, "rdma-first: harvest n=%d status0=%d len0=%u\n",
                    n, n > 0 ? (int)wcs[0].status : -1,
                    n > 0 ? wcs[0].byte_len : 0);
    }
    for (int i = 0; i < n; i++) {
        uint32_t gen = (uint32_t)(wcs[i].wr_id >> 32);
        uint32_t reg = ((uint32_t)(wcs[i].wr_id >> 8)) & 0xFFFFFF;
        int buf = (int)(wcs[i].wr_id & 0xFF);
        if (reg >= EFS_RDMA_MAX_CONNS)
            continue;
        pthread_mutex_lock(&g_reg_lock);
        struct efs_rdma_conn *rc = g_conn_reg[reg];
        if (rc && rc->reg_gen != gen)
            rc = NULL; /* stale completion from a destroyed QP */
        if (rc) {
            if (wcs[i].status != IBV_WC_SUCCESS) {
                /* A failed RECV takes the whole RC QP to ERR, which also
                 * kills any send in flight on it. Silent before: the sender
                 * then looked like an unexplained hang. */
                static int nrerr;
                int k = __sync_fetch_and_add(&nrerr, 1);
                if (k < 10 || (k % 100) == 0)
                    fprintf(stderr,
                            "efs: RDMA recv CQE error status=%d (%s) qpn=%u "
                            "byte_len=%u\n",
                            (int)wcs[i].status,
                            ibv_wc_status_str(wcs[i].status),
                            rc->qp ? rc->qp->qp_num : 0, wcs[i].byte_len);
                rc->broken = 1;
                uint64_t one = 1;
                if (write(rc->efd, &one, sizeof(one)) < 0 && errno != EAGAIN)
                    rc->broken = 1;
            } else {
                pend_push(rc, buf, wcs[i].byte_len);
            }
        }
        pthread_mutex_unlock(&g_reg_lock);
    }
}

/* Drain the completion channel. The fd is O_NONBLOCK so this cannot hang
 * the poller (ibv_get_cq_event is otherwise blocking). Every req_notify
 * that fires leaves an event that MUST be acked; skipping the ack (the
 * old race-drain `continue`) left the next SEND without a notify. */
static void ack_cq_events(struct efs_rdma_dev *dev)
{
    struct ibv_cq *cq = NULL;
    void *ctx = NULL;
    while (ibv_get_cq_event(dev->recv_chan, &cq, &ctx) == 0)
        ibv_ack_cq_events(cq, 1);
}

/* Shared-CQ poller. A few empty polls spin; then the thread arms the
 * completion channel and waits at most 1 ms. The old live-QP path
 * paused and sched_yield'd forever: during posix jobs=1 that thread
 * was 64% of efs-fuse and 31% of efsd, and the yield handed the core
 * away for a timeslice so the reply sat until the poller was
 * rescheduled. Do not sleep 100 ms after an event — that missed the
 * next completion and stalled mkdir. Heartbeats are 50 ms and the
 * election timeout is 500 ms, so a 1 ms wait cannot start an election.
 * Arm, drain the CQ, then wait. Ack only after poll() reports the
 * channel is readable — acking first disarms the notify. */
static void *recv_poller(void *arg)
{
    struct efs_rdma_dev *dev = arg;
    struct ibv_wc wcs[32];
    unsigned empty = 0;
    for (;;) {
        int n = ibv_poll_cq(dev->recv_cq, 32, wcs);
        if (n < 0) {
            usleep(1000);
            continue;
        }
        if (n > 0) {
            harvest_recv_wcs(wcs, n);
            empty = 0;
            continue;
        }
        /* A few empty polls catch a completion already in the CQ.
         * 128 of them were 16% of efs-fuse during posix jobs=1; the
         * channel wait below wakes the thread for the next one.
         * Acking an event before that wait disarms the notify, and
         * the next completion is then invisible until poll() times
         * out — that added about a millisecond to every RPC. */
        if (__sync_fetch_and_add(&g_live_conns, 0) > 0 && ++empty < 32) {
            __asm__ volatile("pause" ::: "memory");
            continue;
        }
        empty = 0;
        if (ibv_req_notify_cq(dev->recv_cq, 0) != 0) {
            usleep(1000);
            continue;
        }
        n = ibv_poll_cq(dev->recv_cq, 32, wcs);
        if (n < 0) {
            usleep(1000);
            continue;
        }
        if (n > 0) {
            harvest_recv_wcs(wcs, n);
            continue;
        }
        {
            struct pollfd p = { .fd = dev->recv_chan->fd, .events = POLLIN };
            int pr = poll(&p, 1, 1);
            if (pr > 0 && (p.revents & POLLIN))
                ack_cq_events(dev);
        }
    }
    return NULL;
}

/* Lazily create the shared recv CQ + poller for a device (first conn). */
static int dev_shared_cq_start(struct efs_rdma_dev *dev)
{
    if (dev->poller_started)
        return 0;
    dev->recv_chan = ibv_create_comp_channel(dev->ctx);
    if (!dev->recv_chan)
        return -1;
    {
        int fl = fcntl(dev->recv_chan->fd, F_GETFL, 0);
        if (fl >= 0)
            (void)fcntl(dev->recv_chan->fd, F_SETFL, fl | O_NONBLOCK);
    }
    /* Sized for every conn's posted recvs with headroom; CQ entries are
     * cheap driver-side. */
    dev->recv_cq = ibv_create_cq(dev->ctx, 1 << 16, NULL, dev->recv_chan, 0);
    if (!dev->recv_cq)
        return -1;
    cq_unmoderate(dev->recv_cq);
    if (pthread_create(&dev->poller, NULL, recv_poller, dev) != 0)
        return -1;
    pthread_detach(dev->poller);
    dev->poller_started = 1;
    return 0;
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
    rc->reg_idx = -1;
    rc->efd = -1;
    rc->tcp_fd = -1;
    rc->psn = (uint32_t)(rand() & 0xFFFFFF);

    pthread_mutex_lock(&g_dev_lock);
    int cq_rc = dev_shared_cq_start(dev);
    pthread_mutex_unlock(&g_dev_lock);
    if (cq_rc != 0)
        goto fail;

    /* Registry slot must be live before any recv is posted: the wr_id
     * encodes it, and the poller may harvest as soon as the QP is RTS. */
    pthread_mutex_lock(&g_reg_lock);
    for (int i = 0; i < EFS_RDMA_MAX_CONNS; i++) {
        if (!g_conn_reg[i]) {
            rc->reg_idx = i;
            break;
        }
    }
    if (rc->reg_idx >= 0) {
        rc->reg_gen = g_reg_gen[rc->reg_idx];
        g_conn_reg[rc->reg_idx] = rc;
    }
    pthread_mutex_unlock(&g_reg_lock);
    if (rc->reg_idx < 0)
        goto fail;

    rc->efd = eventfd(0, EFD_NONBLOCK);
    if (rc->efd < 0)
        goto fail;
    /* Send completions are reaped on every send/recv op; the send CQ only
     * ever holds a handful of unreaped entries. */
    rc->send_cq = ibv_create_cq(dev->ctx, EFS_RDMA_NSEND * 2, NULL, NULL, 0);
    if (!rc->send_cq)
        goto fail;
    cq_unmoderate(rc->send_cq);

    size_t recv_stride = rc->bufsz + EFS_RDMA_RECV_STRIDE;
    size_t total = (size_t)rc->nrecv * recv_stride +
                   (size_t)EFS_RDMA_NSEND * rc->bufsz;
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
        rc->recv_bufs[i] = rc->arena + (size_t)i * recv_stride;
    for (int i = 0; i < EFS_RDMA_NSEND; i++)
        rc->send_bufs[i] = rc->arena + (size_t)rc->nrecv * recv_stride +
                           (size_t)i * rc->bufsz;

    struct ibv_qp_init_attr ia;
    memset(&ia, 0, sizeof(ia));
    ia.send_cq = rc->send_cq;
    ia.recv_cq = dev->recv_cq;
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
    /* Driver may reduce the requested inline cap. Stack-buffer INLINE
     * larger than this fails post_send (small LOOKUP after upgrade). */
    rc->max_inline = ia.cap.max_inline_data;

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
    /* rnr_retry 7 means retry FOREVER. With a bounded RQ that turns
     * "responder momentarily out of recv buffers" into a send that never
     * completes and never errors -- the wedge this exists to prevent. A
     * finite count yields IBV_WC_RNR_RETRY_EXC_ERR, a real CQE the send
     * path already handles. */
    attr.rnr_retry = (uint8_t)rdma_rnr_retry();
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
    rc->tcp_fd = c->fd;
    rc->counted = 1;
    fd_link_now(rc->tcp_fd, rc->tcp_link, sizeof(rc->tcp_link));
    __sync_fetch_and_add(&g_live_conns, 1);
    static int logged;
    if (!__sync_lock_test_and_set(&logged, 1))
        fprintf(stderr, "efs: RDMA transport up on %s port %u "
                "(lid %u, %u x %u KiB bufs/conn)\n",
                dev->name, dev->port, dev->lid, rc->nrecv + EFS_RDMA_NSEND,
                rc->bufsz / 1024);
    if (rdma_first_log()) {
        struct sockaddr_in la;
        socklen_t ll = sizeof(la);
        unsigned lport = 0;
        if (getsockname(c->fd, (struct sockaddr *)&la, &ll) == 0)
            lport = ntohs(la.sin_port);
        fprintf(stderr,
                "rdma-first: upgrade ok fd=%d qpn=%u dest_qpn=%u local_port=%u "
                "lid=%u max_frame=%u\n",
                c->fd, rc->qp->qp_num, rep->qpn, lport, dev->lid,
                rc->max_frame);
    }
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
    rc->tcp_fd = c->fd;
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
    /* Destroy the QP first so no new recv completions can arrive, then
     * clear the registry slot under the lock: the poller holds g_reg_lock
     * while pushing, so after this it can never touch rc. Bumping the slot
     * generation makes already-queued completions from the dead QP
     * recognizable as stale (they outlive the QP in the shared CQ). */
    if (rc->qp)
        ibv_destroy_qp(rc->qp);
    if (rc->reg_idx >= 0) {
        pthread_mutex_lock(&g_reg_lock);
        if (g_conn_reg[rc->reg_idx] == rc) {
            g_conn_reg[rc->reg_idx] = NULL;
            g_reg_gen[rc->reg_idx]++;
        }
        pthread_mutex_unlock(&g_reg_lock);
    }
    if (rc->send_cq)
        ibv_destroy_cq(rc->send_cq);
    if (rc->efd >= 0)
        close(rc->efd);
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

uint32_t efs_rdma_qpn(struct efs_rdma_conn *rc)
{
    return (rc && rc->qp) ? rc->qp->qp_num : 0;
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
                static int nerr;
                int e = __sync_fetch_and_add(&nerr, 1);
                if (e < 10 || (e % 100) == 0)
                    fprintf(stderr,
                            "efs: RDMA send CQE error status=%d (%s) qpn=%u\n",
                            (int)wc[i].status,
                            ibv_wc_status_str(wc[i].status),
                            rc->qp ? rc->qp->qp_num : 0);
                rc->broken = 1;
                return EFS_ERR_NET;
            }
            rc->n_reaped++;
            if ((wc[i].wr_id & 0xF000u) == SEND_WRID_POOL) {
                int si = (int)(wc[i].wr_id & 0xFF);
                if (si >= 0 && si < EFS_RDMA_NSEND)
                    rc->send_busy[si] = 0;
            }
        }
        if (n < 8)
            break;
    }
    return EFS_OK;
}

/* A posted send with no CQE after EFS_RDMA_SEND_WAIT_US is a dead QP.
 * The reply wait uses this so a failed SEND becomes EIO there, without
 * efs_rdma_send_frame waiting for its own CQE. */
static int sends_overdue(struct efs_rdma_conn *rc)
{
    int i;
    int64_t now = now_us();

    for (i = 0; i < EFS_RDMA_NSEND; i++) {
        if (!rc->send_busy[i] || rc->send_posted_us[i] == 0)
            continue;
        if (now - rc->send_posted_us[i] > EFS_RDMA_SEND_WAIT_US)
            return 1;
    }
    return 0;
}

static int reap_or_dead(struct efs_rdma_conn *rc)
{
    if (reap_sends(rc) != 0)
        return -1;
    if (sends_overdue(rc)) {
        rc->broken = 1;
        return -1;
    }
    return 0;
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
    /* The CQE is a few microseconds behind post_send. sched_yield on the
     * first miss hands the core away for the rest of a timeslice (~100us
     * here, and longer when another thread is runnable). Spin first. */
    int64_t end = now_us() + EFS_RDMA_SEND_WAIT_US;
    int64_t spin_end = now_us() + 200;
    while (rc->send_busy[idx]) {
        if (reap_sends(rc) != 0)
            return -1;
        if (!rc->send_busy[idx])
            break;
        if (now_us() > end) {
            rc->broken = 1;
            return -1;
        }
        if (now_us() >= spin_end)
            sched_yield();
    }
    return idx;
}

/* Describe a send failure. A send that neither completes nor errors leaves
 * the QP perfectly healthy with the WR parked in the SQ (an RNR NAK being
 * retried forever), which is indistinguishable from every other failure
 * without this. Three of the four failure paths below used to return
 * EFS_ERR_NET silently, so a looping client printed nothing at all. */
/* Read one IB port counter. Used to attribute a send failure to "the HCA
 * transmitted and got no ack" vs "the WR never went out at all", which no
 * amount of CQ inspection can distinguish. */
static uint64_t ib_port_counter(const char *dev, unsigned port,
                                const char *name)
{
    char p[256];
    snprintf(p, sizeof(p), "/sys/class/infiniband/%s/ports/%u/counters/%s",
             dev, port, name);
    FILE *f = fopen(p, "r");
    if (!f)
        return 0;
    unsigned long long v = 0;
    if (fscanf(f, "%llu", &v) != 1)
        v = 0;
    fclose(f);
    return (uint64_t)v;
}

/* Identity of the conn at post time. If any of this differs at failure time,
 * the conn was torn down (and its slot reused) under an active sender, and
 * the completion we are waiting for died with the per-conn send CQ. */
struct send_ident {
    struct ibv_qp *qp;
    struct ibv_cq *cq;
    uint32_t qpn;
    uint32_t gen;
    int st;
    uint64_t xmit_pkts;
};

static void send_ident_take(struct efs_rdma_conn *rc, struct send_ident *id)
{
    id->qp = rc->qp;
    id->cq = rc->send_cq;
    id->qpn = rc->qp ? rc->qp->qp_num : 0;
    id->gen = rc->reg_gen;
    id->st = -2;
    /* Port-counter and ibv_query_qp stay off this path. Both are kernel
     * round trips; doing them before every SEND made a same-host raft
     * AppendEntries ~380us against ~15us on TCP (Sep 27). The failure
     * dump reads them only after a send has already failed. */
    id->xmit_pkts = 0;
}

static void send_fail_dump2(struct efs_rdma_conn *rc, uint8_t type,
                            const char *why, const struct send_ident *id);

static void send_fail_dump(struct efs_rdma_conn *rc, uint8_t type,
                           const char *why)
{
    send_fail_dump2(rc, type, why, NULL);
}

static void send_fail_dump2(struct efs_rdma_conn *rc, uint8_t type,
                            const char *why, const struct send_ident *id)
{
    int st_pre = id ? id->st : -2;
    static int n;
    int i = __sync_fetch_and_add(&n, 1);
    if (i >= 10 && (i % 100) != 0)
        return;
    struct ibv_qp_attr a;
    struct ibv_qp_init_attr ia;
    memset(&a, 0, sizeof(a));
    memset(&ia, 0, sizeof(ia));
    int qrc = -1;
    if (rc->qp)
        qrc = ibv_query_qp(rc->qp, &a,
                           IBV_QP_STATE | IBV_QP_AV | IBV_QP_DEST_QPN |
                           IBV_QP_RNR_RETRY | IBV_QP_TIMEOUT |
                           IBV_QP_RETRY_CNT, &ia);
    int busy = 0;
    for (int i = 0; i < EFS_RDMA_NSEND; i++)
        busy += rc->send_busy[i] ? 1 : 0;
    fprintf(stderr,
            "efs: RDMA send type=%u FAILED (%s) qpn=%u state=%d st_pre=%d "
            "dest_qpn=%u dlid=%u rnr_retry=%u retry_cnt=%u timeout=%u "
            "busy=%d broken=%d qrc=%d\n",
            type, why, rc->qp ? rc->qp->qp_num : 0,
            qrc == 0 ? (int)a.qp_state : -1, st_pre, qrc == 0 ? a.dest_qp_num : 0,
            qrc == 0 ? a.ah_attr.dlid : 0, qrc == 0 ? a.rnr_retry : 0,
            qrc == 0 ? a.retry_cnt : 0, qrc == 0 ? a.timeout : 0,
            busy, rc->broken, qrc);
    if (rc->qp)
        fprintf(stderr,
                "efs: RDMA qp obj: state=%d qp->send_cq=%p rc->send_cq=%p %s "
                "qp->recv_cq=%p dev->recv_cq=%p handle=%u\n",
                (int)rc->qp->state, (void *)rc->qp->send_cq,
                (void *)rc->send_cq,
                rc->qp->send_cq == rc->send_cq ? "(match)"
                                               : "*** CQ MISMATCH ***",
                (void *)rc->qp->recv_cq,
                (void *)(rc->dev ? rc->dev->recv_cq : NULL), rc->qp->handle);
    if (id)
        fprintf(stderr,
                "efs: RDMA send identity now/at-post: qp=%p/%p cq=%p/%p "
                "qpn=%u/%u reg_gen=%u/%u %s\n",
                (void *)rc->qp, (void *)id->qp, (void *)rc->send_cq,
                (void *)id->cq, rc->qp ? rc->qp->qp_num : 0, id->qpn,
                rc->reg_gen, id->gen,
                (rc->qp != id->qp || rc->send_cq != id->cq ||
                 rc->reg_gen != id->gen)
                    ? "*** CONN REPLACED UNDER SENDER ***"
                    : "(same conn)");
    char now_link[64];
    fd_link_now(rc->tcp_fd, now_link, sizeof(now_link));
    fprintf(stderr, "efs: RDMA tcp_fd=%d link now=\"%s\" at-upgrade=\"%s\" %s\n",
            rc->tcp_fd, now_link, rc->tcp_link,
            strcmp(now_link, rc->tcp_link) == 0
                ? "(same socket)"
                : "*** SOCKET CLOSED/REUSED BEHIND THIS CONN ***");
    fprintf(stderr, "efs: RDMA conn sends posted=%llu reaped=%llu %s\n",
            (unsigned long long)rc->n_posted, (unsigned long long)rc->n_reaped,
            rc->n_reaped >= rc->n_posted
                ? "*** ALL COMPLETIONS ARRIVED: flag was missed ***"
                : "(a completion is genuinely outstanding)");
    if (rc->dev)
        fprintf(stderr, "efs: RDMA port_xmit_packets now = %llu\n",
                (unsigned long long)ib_port_counter(rc->dev->name,
                                                    rc->dev->port,
                                                    "port_xmit_packets"));
    /* Nothing else in the process reads the async queue, so whatever took the
     * QP out of RTS is still sitting in it. Non-blocking drain. */
    if (rc->dev && rc->dev->ctx) {
        int afd = rc->dev->ctx->async_fd;
        int fl = fcntl(afd, F_GETFL);
        if (fl >= 0)
            fcntl(afd, F_SETFL, fl | O_NONBLOCK);
        struct ibv_async_event ev;
        int k = 0;
        while (k++ < 8 && ibv_get_async_event(rc->dev->ctx, &ev) == 0) {
            fprintf(stderr, "efs: RDMA async event=%d (%s)\n",
                    (int)ev.event_type, ibv_event_type_str(ev.event_type));
            ibv_ack_async_event(&ev);
        }
        if (fl >= 0)
            fcntl(afd, F_SETFL, fl);
    }
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
    rc->n_posted++;
    return EFS_OK;
}

int efs_rdma_send_frame(struct efs_rdma_conn *rc, uint8_t type,
                        const void *p1, uint32_t l1,
                        const void *p2, uint32_t l2)
{
    if (rc->broken) {
        send_fail_dump(rc, type, "conn already broken");
        return EFS_ERR_NET;
    }
    uint32_t payload = l1 + l2;
    uint32_t frame = 5 + payload;
    if (frame > rc->max_frame)
        return EFS_ERR_NET; /* caller must use the TCP side-channel */

    /* Always copy into a registered send buffer. INLINE from a stack
     * temporary is rejected on some mlx5 FW; INLINE from the MR is
     * fine when the frame fits the QP's actual cap. */
    int idx = send_buf_pick(rc);
    if (idx < 0) {
        send_fail_dump(rc, type, "send_buf_pick: prior send never completed");
        return EFS_ERR_NET;
    }
    uint8_t *b = rc->send_bufs[idx];
    uint32_t encoded = 0;
    if (efs_wire_frame_encode(type, p1, l1, p2, l2, b, rc->max_frame,
                              &encoded) != EFS_OK)
        return EFS_ERR_NET;
    if (encoded != frame)
        return EFS_ERR_NET;
    int inline_ok = (frame <= rc->max_inline);
    if (rdma_first_log()) {
        static int nsend;
        int n = __sync_fetch_and_add(&nsend, 1);
        if (n < 6)
            fprintf(stderr, "rdma-first: send_frame post type=%u frame=%u inline=%d\n",
                    type, frame, inline_ok);
    }
    struct send_ident ident;
    send_ident_take(rc, &ident);
    if (post_send(rc, SEND_WRID_POOL | (uint64_t)idx, b, frame, inline_ok) != 0) {
        send_fail_dump2(rc, type, "ibv_post_send", &ident);
        return EFS_ERR_NET;
    }
    rc->send_busy[idx] = 1;
    rc->send_posted_us[idx] = now_us();
    /* W15.4: do not wait for this send's CQE. send_buf_pick reaps on the
     * next send; the reply wait (efs_rdma_recv_wait / reply_ready*) reaps
     * before it blocks and turns a send-CQE error, or a send still busy
     * after EFS_RDMA_SEND_WAIT_US, into EFS_ERR_NET. Waiting here was one
     * HCA round trip per fragment. */
    (void)ident;
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
    rc->send_posted_us[idx] = now_us();
    return EFS_OK;
}

/* ---------------- recv path ---------------- */

/* Pop one pended frame (poller-produced) into cur_buf/cur_len. */
static int pend_pop(struct efs_rdma_conn *rc)
{
    int tail = rc->pr_tail;
    if (tail == __atomic_load_n(&rc->pr_head, __ATOMIC_ACQUIRE))
        return 0;
    rc->cur_buf = rc->pr_buf[tail];
    rc->cur_len = rc->pr_len[tail];
    __atomic_store_n(&rc->pr_tail, (tail + 1) & (EFS_RDMA_PEND_RING - 1),
                     __ATOMIC_RELEASE);
    return 1;
}

/* Ring empty → drain the eventfd so the next push re-signals it. Must be
 * called with the ring already observed empty; a concurrent push either
 * lands before this read (drained here, but the ring check that follows
 * every wait iteration sees it) or after (counter stays > 0 → poll fires). */
static void pend_drain_efd(struct efs_rdma_conn *rc)
{
    uint64_t tmp;
    if (read(rc->efd, &tmp, sizeof(tmp)) < 0 && errno != EAGAIN)
        rc->broken = 1;
}

int efs_rdma_recv_wait(struct efs_rdma_conn *rc, int timeout_ms)
{
    if (rdma_first_log()) {
        static int nwait;
        int n = __sync_fetch_and_add(&nwait, 1);
        if (n < 6)
            fprintf(stderr, "rdma-first: recv_wait start timeout_ms=%d\n",
                    timeout_ms);
    }
    int64_t deadline = timeout_ms >= 0 ? now_ms() + timeout_ms : -1;
    for (;;) {
        if (pend_pop(rc)) {
            /* Keep efd == "ring non-empty": a pop that empties the ring
             * clears the stale signal so other pollers of the fd
             * (conn_wait_request) don't spuriously route back here. The
             * ring stays the source of truth; a push racing the drain
             * re-signals and is seen by the next ring check. */
            if (__atomic_load_n(&rc->pr_head, __ATOMIC_ACQUIRE) == rc->pr_tail)
                pend_drain_efd(rc);
            return EFS_OK;
        }
        if (rc->broken)
            return EFS_ERR_NET;
        if (reap_or_dead(rc) != 0)
            return EFS_ERR_NET;
        pend_drain_efd(rc);
        if (pend_pop(rc))
            return EFS_OK;
        if (rc->broken)
            return EFS_ERR_NET;
        /* A few pauses catch a completion that landed while the send
         * was returning. clock_gettime on a 200us spin was the top of
         * efs-fuse during posix jobs=1 (~20%, all of it this wait):
         * metadata replies wait on a Raft commit, so the spin always
         * ran out and then poll() woke on the eventfd anyway. */
        {
            uint32_t polls;

            for (polls = 0; polls < 64; polls++) {
                if (__atomic_load_n(&rc->pr_head, __ATOMIC_ACQUIRE) !=
                    rc->pr_tail)
                    break;
                if (rc->broken)
                    return EFS_ERR_NET;
                __asm__ volatile("pause" ::: "memory");
            }
        }
        if (pend_pop(rc))
            return EFS_OK;
        if (rc->broken)
            return EFS_ERR_NET;
        int ms = -1;
        if (deadline >= 0) {
            int64_t left = deadline - now_ms();
            if (left <= 0)
                return EFS_ERR_NET;
            ms = left > INT32_MAX ? INT32_MAX : (int)left;
        }
        /* Poll the eventfd AND the control-channel socket. An idle RC QP
         * never errors when the peer vanishes (no CM to drive a state
         * transition), so without the socket a dead peer would strand the
         * waiter here forever — the conn-thread leak this fixed piled up
         * thousands of stuck server threads. GET_META still rides TCP, so
         * POLLIN is a side-channel frame (return AGAIN so the server wait
         * loop handles it), not peer death. */
        struct pollfd pf[2] = {
            { .fd = rc->efd, .events = POLLIN },
            { .fd = rc->tcp_fd, .events = POLLIN },
        };
        nfds_t nf = rc->tcp_fd >= 0 ? 2 : 1;
        int pr = poll(pf, nf, ms);
        if (pr == 0)
            return EFS_ERR_NET; /* timeout: caller drops the conn */
        if (pr < 0 && errno != EINTR) {
            rc->broken = 1;
            return EFS_ERR_NET;
        }
        if (rc->tcp_fd >= 0) {
            short ev = pf[1].revents;
            if (ev & (POLLERR | POLLNVAL)) {
                rc->broken = 1;
                return EFS_ERR_NET;
            }
            if (ev & POLLIN) {
                /* poll() reports POLLIN for a real side-channel byte and
                 * also spuriously after the RDMA upgrade. A peek that
                 * finds nothing must not abort the RDMA wait: the client
                 * treated that as a dead conn and slept 50 ms before
                 * retrying a mkdir that had not failed. */
                char peek;
                ssize_t n = recv(rc->tcp_fd, &peek, 1,
                                 MSG_PEEK | MSG_DONTWAIT);
                if (n > 0)
                    return EFS_ERR_AGAIN;
                if (n == 0) {
                    rc->broken = 1;
                    return EFS_ERR_NET;
                }
            }
            if (ev & POLLHUP) {
                rc->broken = 1;
                return EFS_ERR_NET;
            }
        }
    }
}

void *efs_rdma_recv_frame(struct efs_rdma_conn *rc, uint32_t *frame_len)
{
    if (rc->cur_buf < 0)
        return NULL;
    if (frame_len)
        *frame_len = rc->cur_len;
    return rc->recv_bufs[rc->cur_buf] + EFS_RDMA_RECV_ALIGN;
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

/* Check whether the poller has pended a reply frame. The full variant spins
 * briefly on the ring (adaptive budget); the quick variant is a single
 * atomic check. Neither touches ibverbs — the CQ lock churn that dominated
 * both client and server CPU under load is gone from the wait path. */
int efs_rdma_reply_ready(struct efs_rdma_conn *rc)
{
    if (rc->broken || reap_or_dead(rc) != 0)
        return -1;
    if (__atomic_load_n(&rc->pr_head, __ATOMIC_ACQUIRE) != rc->pr_tail)
        return 1;
    __sync_fetch_and_add(&g_spinning, 1);
    int budget = spin_budget_for_load();
    int64_t spin_end = now_us() + budget;
    uint32_t polls = 0;
    int result = 0;
    for (;;) {
        if (__atomic_load_n(&rc->pr_head, __ATOMIC_ACQUIRE) != rc->pr_tail) {
            result = 1;
            break;
        }
        if (rc->broken) {
            result = -1;
            break;
        }
        if (((++polls) & 63) == 0 && now_us() >= spin_end)
            break;
    }
    __sync_fetch_and_sub(&g_spinning, 1);
    return result;
}

int efs_rdma_reply_ready_quick(struct efs_rdma_conn *rc)
{
    if (rc->broken || reap_or_dead(rc) != 0)
        return -1;
    return __atomic_load_n(&rc->pr_head, __ATOMIC_ACQUIRE) != rc->pr_tail;
}

/* Fixed-budget variant for the PUT reply wait. A reply is behind a disk
 * write, so a clock_gettime spin runs to its end on every chunk. 64
 * pauses measured ~200 µs in efs_rdma_recv_wait; 16 pauses is the ~50 µs
 * cap. budget_us scales within that cap. The caller then blocks on the
 * CQ event fd. */
int efs_rdma_reply_ready_us(struct efs_rdma_conn *rc, int budget_us)
{
    uint32_t pauses, n = 16;

    if (rc->broken || reap_or_dead(rc) != 0)
        return -1;
    if (__atomic_load_n(&rc->pr_head, __ATOMIC_ACQUIRE) != rc->pr_tail)
        return 1;
    if (budget_us <= 0)
        n = 1;
    else if (budget_us < 50)
        n = (uint32_t)((budget_us * 16 + 49) / 50);
    if (n < 1)
        n = 1;
    if (n > 16)
        n = 16;
    for (pauses = 0; pauses < n; pauses++) {
        if (__atomic_load_n(&rc->pr_head, __ATOMIC_ACQUIRE) != rc->pr_tail)
            return 1;
        if (rc->broken)
            return -1;
        __asm__ volatile("pause" ::: "memory");
    }
    return 0;
}

int efs_rdma_reply_fd(struct efs_rdma_conn *rc)
{
    return rc->efd;
}
