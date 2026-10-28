/* RDMA (verbs, RC QP) transport for efs.
 *
 * Versioned setup over TCP, followed by complete messages segmented over
 * registered physical buffers. One owner services DATA/CREDIT and send CQEs;
 * successful upgrades never carry application data over the TCP descriptor.
 *
 * All conns share one recv CQ per device, harvested by a single poller
 * thread that fans completions out to per-conn SPSC rings (wr_id encodes
 * the conn's registry slot). The message path checks the ring (an atomic
 * load — no CQ lock) and then blocks on the conn's eventfd. This replaced
 * per-conn recv CQs, whose per-thread spin-polling (ibv_poll_cq lock churn)
 * was the top CPU consumer on both client and server under load.
 */
#include "efs/rdma.h"
#include "efs/rdma_message.h"
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/wire.h"
#include "efs/common.h"
#include "rdma_memcheck.h"

#include <infiniband/verbs.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <linux/tcp.h>
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

/* x86 PAUSE / aarch64 YIELD: hint that this is a spin, not a data dependency. */
static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

#define EFS_RDMA_NRECV_DEFAULT 6 /* posted recv buffers per QP; half the smaller
                                  pool carries data (efs_rmsg_credit_init): 3 */
/* One buffer per in-flight send on this QP. The write pipeline posts
 * EFS_WRITE_PIPELINE chunks; with 2 buffers the third send spun in
 * send_buf_pick (W14 step 5 / W15 step 4). */
#define EFS_RDMA_NSEND         EFS_WRITE_PIPELINE
#define EFS_RDMA_SEND_WAIT_US  (5 * 1000 * 1000) /* a stuck send = dead QP */
#define EFS_RDMA_CREDIT_SPIN_US 200 /* reap a busy control slot before deferring */

/* Recv SGEs are posted this far into each recv buffer so the fragment data
 * of a PUT_CHUNK (5-byte frame header + EFS_PUT_HEADER_LEN) lands exactly on
 * a 4096 boundary. The server's O_DIRECT fragment write can then go
 * DMA->disk with no bounce-buffer copy (that copy was ~11% of efsd CPU
 * under a 9-client write load). Only PUT data needs the alignment; other
 * payloads simply sit at an unaligned offset, which they never notice. */
#define EFS_RDMA_RECV_ALIGN \
    ((4096 - ((5 + EFS_PUT_HEADER_LEN) % 4096)) % 4096)
#define EFS_RDMA_RECV_STRIDE   4096 /* extra slack per recv buffer */

#define SEND_WRID_POOL   0x1000

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

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* RNR retry count for the send QP. 7 means retry forever; see qp_rts. */
#define RDMA_RNR_RETRY 7

int efs_rdma_transport(void)
{
    static int mode_s = -1; /* any thread may be first: atomic, local copy (TSan) */
    int mode = __atomic_load_n(&mode_s, __ATOMIC_ACQUIRE);
    if (mode < 0) {
        const char *v = getenv("EFS_TRANSPORT");
        if (v && strcmp(v, "tcp") == 0)
            mode = EFS_TRANSPORT_TCP;
        else if (v && strcmp(v, "rdma") == 0)
            mode = EFS_TRANSPORT_RDMA;
        else
            mode = EFS_TRANSPORT_AUTO;
        __atomic_store_n(&mode_s, mode, __ATOMIC_RELEASE);
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

static int nrecv_bufs(void)
{
    static int v_s=-1; /* any thread may be first: atomic, local copy (TSan) */
    int v=__atomic_load_n(&v_s,__ATOMIC_ACQUIRE);
    if (v<0) {
        const char *e=getenv("EFS_RDMA_BUFS"); char *end=NULL;
        long n=(e&&*e)?strtol(e,&end,10):EFS_RDMA_NRECV_DEFAULT;
        v=(n<2 || n>64 || (end&&*end))?0:(int)n;
        __atomic_store_n(&v_s,v,__ATOMIC_RELEASE);
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
    int roce, gid_index;
    union ibv_gid gid;
    /* Shared recv completion channel: every conn's QP posts recvs here and
     * ONE poller thread harvests all completions, pushing frames onto
     * per-conn SPSC rings. Replaces per-conn CQs, whose spin-polling by
     * every conn thread was the top CPU consumer on client AND server. */
    struct ibv_cq *recv_cq;
    /* A provider may copy inline CQE bytes into the posted receive buffer
     * inside poll_cq. Serialize polling with repost so CPU buffer accesses
     * before ownership returns to verbs precede that provider copy too. */
    pthread_mutex_t recv_handoff_mu;
    int recv_capacity, recv_reserved;
    struct ibv_comp_channel *recv_chan;
    pthread_t poller;
    int poller_started;
    pid_t poller_pid; /* K31: the process that owns the poller thread */
};

/* Syntax cap 128MiB covers the maximum 64MiB fragment plus all wire headers.
 * Raft snapshot transport emits bounded chunks, not a whole image. */
static struct efs_rmsg_budget g_assembly_budget = {512ull*1024*1024, 0};
static struct efs_rdma_dev g_devs[4];
static int g_dev_count;
static pthread_mutex_t g_dev_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_live_conns;

/* W39: zero-copy send regions. A client bufpool slab (32 MiB) is named
 * once; each HCA registers it on the first send out of it (ibv_reg_mr of
 * 32 MiB pins the pages, ~ms, once per slab per device). The table is
 * append-only: base/len are read without the lock, the per-device MR
 * pointer is published with a release store after registration. A send
 * whose payload is not in any region (TLS parity buffers, malloc'd
 * records) copies into the pool buffer as before. Direct fill is disabled
 * (efs_rdma_send_buf), so the table only records regions; the per-device
 * lookup was removed with it. */
#define ZC_MAX_REGIONS 128
struct zc_region {
    uint8_t *base;
    size_t len;
    struct ibv_mr *mr[4]; /* indexed like g_devs */
    int failed[4];        /* registration failed: copy path, logged once */
};
static struct zc_region g_zc[ZC_MAX_REGIONS];
static int g_zc_n;
static pthread_mutex_t g_zc_lock = PTHREAD_MUTEX_INITIALIZER;

int efs_rdma_zc_region_add(void *base, size_t len)
{
    if (!base || len == 0)
        return -1;
    pthread_mutex_lock(&g_zc_lock);
    if (g_zc_n >= ZC_MAX_REGIONS) {
        pthread_mutex_unlock(&g_zc_lock);
        return -1;
    }
    struct zc_region *r = &g_zc[g_zc_n];
    r->base = base;
    r->len = len;
    memset(r->mr, 0, sizeof(r->mr));
    memset(r->failed, 0, sizeof(r->failed));
    __atomic_store_n(&g_zc_n, g_zc_n + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_zc_lock);
    return 0;
}

/* Conn registry: recv WR wr_id = (gen << 32) | (reg_idx << 8) | buf_idx, so
 * the shared-CQ poller can map a completion back to its conn. The generation
 * tag is bumped each time a slot is freed: completions a dead QP already
 * generated live on in the SHARED CQ after ibv_destroy_qp (a per-conn CQ took
 * them to the grave), and without the tag the poller would inject them into
 * whoever reuses the slot. */
#define EFS_RDMA_MAX_CONNS  4096
#define EFS_RDMA_PEND_RING  128 /* >= max nrecv bufs per conn */
static struct efs_rdma_conn *g_conn_reg[EFS_RDMA_MAX_CONNS];
static uint32_t g_reg_gen[EFS_RDMA_MAX_CONNS];
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
/* Counters of connections that left the registry (under g_reg_lock), so
 * efs_rdma_totals counts every connection exactly once. */
static struct efs_rdma_totals g_retired;

int efs_rdma_live_conns(void)
{
    return __sync_fetch_and_add(&g_live_conns, 0);
}

static int rdma_port_no(void)
{
    static int v_s = -1; /* any thread may be first: atomic, local copy (TSan) */
    int v = __atomic_load_n(&v_s, __ATOMIC_ACQUIRE);
    if (v < 0) {
        const char *e = getenv("EFS_RDMA_PORT");
        v = (e && *e) ? atoi(e) : 1;
        if (v < 1)
            v = 1;
        __atomic_store_n(&v_s, v, __ATOMIC_RELEASE);
    }
    return v;
}

/* Native IB uses LID routing; RoCE uses an explicit nonzero local GID. */
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
        pa.state != IBV_PORT_ACTIVE ||
        (pa.link_layer != IBV_LINK_LAYER_INFINIBAND && pa.link_layer != IBV_LINK_LAYER_ETHERNET) ||
        (pa.link_layer == IBV_LINK_LAYER_INFINIBAND && pa.lid == 0)) {
        ibv_close_device(ctx);
        return -1;
    }
    int roce = pa.link_layer == IBV_LINK_LAYER_ETHERNET;
    int gid_index = -1;
    union ibv_gid gid = {0};
    if (roce) {
        const char *env = getenv("EFS_RDMA_GID_INDEX");
        int first = 0, last = pa.gid_tbl_len;
        if (env && *env) {
            char *end; long v = strtol(env, &end, 10);
            if (*end || v < 0 || v >= pa.gid_tbl_len) {
                ibv_close_device(ctx); return -1;
            }
            first = (int)v; last = first + 1;
        }
        for (int i = first; i < last; i++) {
            union ibv_gid candidate;
            if (ibv_query_gid(ctx, rdma_port_no(), i, &candidate)) continue;
            unsigned nonzero = 0;
            for (unsigned j = 0; j < sizeof(candidate.raw); j++) nonzero |= candidate.raw[j];
            if (nonzero) { gid = candidate; gid_index = i; break; }
        }
        if (gid_index < 0) { ibv_close_device(ctx); return -1; }
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
    out->roce = roce; out->gid_index = gid_index; out->gid = gid;
    pthread_mutex_init(&out->recv_handoff_mu, NULL);
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

/* Local IPv4 → HCA, resolved once. A client opens a conn per RPC slot (64
 * per node × 4 nodes) and every upgrade used to walk getifaddrs() + sysfs
 * + ibv_get_device_list(): 26 % of efs-fuse cycles under a 20 GiB dd, with
 * the rest of the client queued on the rtnl/osq lock behind it. The host's
 * interface → HCA mapping does not change while the process runs. */
#define IP_DEV_MAX 8
static struct {
    uint32_t ip; /* network order; 0 = empty */
    struct efs_rdma_dev *dev;
} g_ip_dev[IP_DEV_MAX];
static int g_ip_dev_count;

static struct efs_rdma_dev *ip_dev_lookup(uint32_t ip)
{
    struct efs_rdma_dev *r = NULL;
    pthread_mutex_lock(&g_dev_lock);
    for (int i = 0; i < g_ip_dev_count; i++) {
        if (g_ip_dev[i].ip == ip) {
            r = g_ip_dev[i].dev;
            break;
        }
    }
    pthread_mutex_unlock(&g_dev_lock);
    return r;
}

static void ip_dev_remember(uint32_t ip, struct efs_rdma_dev *dev)
{
    if (!dev)
        return; /* a failed probe is retried next time, not cached */
    pthread_mutex_lock(&g_dev_lock);
    int have = 0;
    for (int i = 0; i < g_ip_dev_count; i++)
        if (g_ip_dev[i].ip == ip)
            have = 1;
    if (!have && g_ip_dev_count < IP_DEV_MAX) {
        g_ip_dev[g_ip_dev_count].ip = ip;
        g_ip_dev[g_ip_dev_count].dev = dev;
        g_ip_dev_count++;
    }
    pthread_mutex_unlock(&g_dev_lock);
}

/* The uncached resolution: interface for the IP, HCA behind the interface. */
static struct efs_rdma_dev *dev_for_ip_resolve(uint32_t ip)
{
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
    struct efs_rdma_dev *r = ip_dev_lookup(ip);
    if (r)
        return r;
    /* One resolver at a time. A client's first PUT window opens its
     * whole conn pool at once; 256 concurrent getifaddrs() serialize on
     * the kernel's rtnl lock and the waiters spin (osq_lock 20 % of the
     * 20 GiB dd profile). The rest sleep here and take the cached answer. */
    static pthread_mutex_t resolve_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&resolve_mu);
    r = ip_dev_lookup(ip);
    if (!r) {
        r = dev_for_ip_resolve(ip);
        ip_dev_remember(ip, r);
    }
    pthread_mutex_unlock(&resolve_mu);
    return r;
}

/* NUMA node of the HCA that will carry traffic to host:port. The local
 * address is the one the kernel routes to the server (a connected UDP
 * socket sends nothing); EFS_RDMA_DEV overrides the interface walk.
 * Returns the node, or -1 (no IB device behind the route, sysfs says
 * -1, or TCP transport). Call before threads exist if the result sets
 * the process affinity. */
int efs_rdma_numa_node_for_host(const char *host, uint16_t port,
                                char *devname, size_t devlen)
{
    if (efs_rdma_transport() == EFS_TRANSPORT_TCP)
        return -1;
    char dev[64] = {0};
    const char *env = getenv("EFS_RDMA_DEV");
    if (env && *env) {
        strncpy(dev, env, sizeof(dev) - 1);
    } else {
        struct addrinfo hints, *ai = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        char pstr[8];
        snprintf(pstr, sizeof(pstr), "%u", (unsigned)port);
        if (getaddrinfo(host, pstr, &hints, &ai) != 0 || !ai)
            return -1;
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        uint32_t ip = 0;
        if (s >= 0 && connect(s, ai->ai_addr, ai->ai_addrlen) == 0) {
            struct sockaddr_in la;
            socklen_t ll = sizeof(la);
            if (getsockname(s, (struct sockaddr *)&la, &ll) == 0)
                ip = la.sin_addr.s_addr;
        }
        if (s >= 0)
            close(s);
        freeaddrinfo(ai);
        if (!ip || (ntohl(ip) & 0xFF000000u) == 0x7F000000u)
            return -1;
        char ifname[IF_NAMESIZE];
        if (ifname_for_ip(ip, ifname, sizeof(ifname)) != 0 ||
            ibdev_for_ifname(ifname, dev, sizeof(dev)) != 0)
            return -1;
    }
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/infiniband/%s/device/numa_node",
             dev);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int node = -1;
    if (fscanf(f, "%d", &node) != 1)
        node = -1;
    fclose(f);
    if (node >= 0 && devname && devlen) {
        strncpy(devname, dev, devlen - 1);
        devname[devlen - 1] = '\0';
    }
    return node;
}

/* The device a connection would use when no route picks one: EFS_RDMA_DEV
 * when set (it must open), else the first usable HCA; NULL when none. */
static struct efs_rdma_dev *dev_default(void)
{
    const char *env = getenv("EFS_RDMA_DEV");
    return env && *env ? dev_cached(env) : dev_probe_first();
}

int efs_rdma_available(void)
{
    static int avail_s = -1; /* any thread may be first: atomic, local copy (TSan) */
    if (efs_rdma_transport() == EFS_TRANSPORT_TCP)
        return 0;
    int avail = __atomic_load_n(&avail_s, __ATOMIC_ACQUIRE);
    if (avail < 0) {
        avail = (dev_default() != NULL) ? 1 : 0;
        __atomic_store_n(&avail_s, avail, __ATOMIC_RELEASE);
    }
    return avail;
}

/* A8 (D42): resolve EFS_TRANSPORT once at efsd/efs-fuse start. auto (the
 * default; also unset or empty), tcp or rdma. An unknown value, or rdma
 * without a usable RDMA device, refuses to start: an explicit rdma never
 * falls back to TCP. auto may choose TCP; the line names what is used. */
int efs_transport_startup(const char *prog)
{
    const char *v = getenv("EFS_TRANSPORT");
    if (v && *v && strcmp(v, "auto") && strcmp(v, "tcp") && strcmp(v, "rdma")) {
        fprintf(stderr, "%s: EFS_TRANSPORT=%s is not auto, tcp or rdma; refusing to start\n",
                prog, v);
        return EFS_ERR_INVAL;
    }
    int mode = efs_rdma_transport();
    if (mode == EFS_TRANSPORT_TCP) {
        fprintf(stderr, "%s: transport tcp (EFS_TRANSPORT=tcp)\n", prog);
        return EFS_OK;
    }
    struct efs_rdma_dev *d = efs_rdma_available() ? dev_default() : NULL;
    const char *dev = getenv("EFS_RDMA_DEV");
    if (mode == EFS_TRANSPORT_RDMA && !d) {
        fprintf(stderr, "%s: EFS_TRANSPORT=rdma but no usable RDMA device%s%s; refusing to "
                "start (unset it or use auto to allow TCP)\n", prog,
                dev && *dev ? " named by EFS_RDMA_DEV=" : "", dev && *dev ? dev : "");
        return EFS_ERR_UNSUPPORTED;
    }
    if (d)
        fprintf(stderr, "%s: transport %s: rdma on %s (TCP carries setup)\n", prog,
                mode == EFS_TRANSPORT_RDMA ? "rdma" : "auto", d->name);
    else
        fprintf(stderr, "%s: transport auto: tcp (no usable RDMA device%s%s)\n", prog,
                dev && *dev ? " named by EFS_RDMA_DEV=" : "", dev && *dev ? dev : "");
    return EFS_OK;
}

/* ---------------- connection ---------------- */

struct efs_rdma_conn {
    struct efs_rmsg_rx messages;
    struct efs_rmsg_credits credits;
    uint64_t tx_id, reposted_data, advertised_data;
    unsigned peer_outstanding;
    int64_t assembly_end_us;
    uint64_t data_frames, data_bytes, app_tx, app_rx, credit_stalls;
    uint64_t rx_frames,rx_bytes,logical_tx,logical_rx,ctrl_tx,ctrl_rx,tcp_app_rx;
    uint64_t bootstrap_tx,bootstrap_rx,assembly_peak;
    int tcp_counted;
    int cq_reserved;
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
    struct ibv_mr *send_mrs[EFS_RDMA_NSEND];
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
    int efd_armed; /* W47: a waiter is in (or entering) poll() on efd */
    /* SPSC pend ring: the shared-CQ poller is the only producer; the conn's
     * current owner thread is the only consumer. Capacity covers every
     * posted recv buffer, so it can never overflow in correct operation. */
    uint8_t pr_buf[EFS_RDMA_PEND_RING];
    uint32_t pr_len[EFS_RDMA_PEND_RING];
    int pr_head; /* poller-written */
    int pr_tail; /* consumer-written */
    int cur_buf;  /* buffer owned by message progress until repost */
    uint32_t cur_len;
    int counted;  /* counted in g_live_conns (set on successful handshake) */
    int tcp_fd;   /* control-channel socket, borrowed from efs_conn: polled
                   * alongside efd so a dead peer (FIN/RST) wakes the wait
                   * even though the QP itself never errors when idle */
    uint32_t max_inline; /* actual QP inline cap (may be < EFS_RDMA_INLINE_MAX) */
    int zc_ok;           /* QP accepts 2 SGEs: W39 zero-copy sends allowed */
};

/* Message counters have one writer, the checked-out owner; efs_rdma_totals
 * reads them from another thread, so updates are single-writer atomic
 * stores (no read-modify-write contention on the data path). */
#define OWNER_ADD(field, n) \
    __atomic_store_n(&(field), (field) + (uint64_t)(n), __ATOMIC_RELAXED)
static void totals_add(struct efs_rdma_totals *t, struct efs_rdma_conn *rc)
{
    t->data_frames_tx += __atomic_load_n(&rc->data_frames, __ATOMIC_RELAXED);
    t->data_bytes_tx += __atomic_load_n(&rc->data_bytes, __ATOMIC_RELAXED);
    t->data_frames_rx += __atomic_load_n(&rc->rx_frames, __ATOMIC_RELAXED);
    t->data_bytes_rx += __atomic_load_n(&rc->rx_bytes, __ATOMIC_RELAXED);
    t->credit_stalls += __atomic_load_n(&rc->credit_stalls, __ATOMIC_RELAXED);
}

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
    pthread_mutex_lock(&rc->dev->recv_handoff_mu);
    rdma_memcheck_post_recv((void *)(uintptr_t)sge.addr, sge.length);
    int result = ibv_post_recv(rc->qp, &wr, &bad);
    pthread_mutex_unlock(&rc->dev->recv_handoff_mu);
    return result;
}

/* Nonblocking CQ access only: never hold the handoff lock while waiting
 * for an event, harvesting the registry, decoding, or sending a reply. */
static int recv_poll(struct efs_rdma_dev *dev, int count, struct ibv_wc *wcs)
{
    pthread_mutex_lock(&dev->recv_handoff_mu);
    int result = ibv_poll_cq(dev->recv_cq, count, wcs);
    pthread_mutex_unlock(&dev->recv_handoff_mu);
    return result;
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
    /* W47: write the eventfd only when a waiter is (about to be) blocked
     * in poll() on it. The ring is the source of truth; the fd is a wake.
     * Dekker pairing with efd_arm(): the head store above and this
     * efd_armed load are both seq_cst, as are the waiter's armed store
     * and its ring re-check, so at least one side sees the other — the
     * waiter self-signals if it finds the ring non-empty after arming. A
     * still-checking waiter (message progress) is unarmed and costs the
     * poller nothing. An error
     * (buf < 0) always writes: the broken flag has no ring entry. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if ((empty_before && __atomic_load_n(&rc->efd_armed, __ATOMIC_SEQ_CST)) ||
        buf < 0) {
        uint64_t one = 1;
        if (write(rc->efd, &one, sizeof(one)) < 0 && errno != EAGAIN)
            rc->broken = 1;
    }
}

/* Waiter side of W47: announce that the next step is poll() on efd. Must
 * be followed by the poll without an intervening pop; a ring entry that
 * landed before the announcement is turned into a level-triggered wake
 * here so the poll returns at once. */
static void efd_arm(struct efs_rdma_conn *rc)
{
    __atomic_store_n(&rc->efd_armed, 1, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&rc->pr_head, __ATOMIC_SEQ_CST) != rc->pr_tail ||
        rc->broken) {
        uint64_t one = 1;
        if (write(rc->efd, &one, sizeof(one)) < 0 && errno != EAGAIN)
            rc->broken = 1;
    }
}

static inline void efd_disarm(struct efs_rdma_conn *rc)
{
    __atomic_store_n(&rc->efd_armed, 0, __ATOMIC_RELAXED);
}

static void harvest_recv_wcs(struct ibv_wc *wcs, int n)
{
    if (efs_rdma_trace_on()) {
        static int nh;
        int k = __sync_fetch_and_add(&nh, 1);
        if (k < 6)
            fprintf(stderr, "rdma-trace: harvest n=%d status0=%d len0=%u\n",
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
                if (buf >= 0 && buf < rc->nrecv)
                    rdma_memcheck_recv_complete(rc->recv_bufs[buf] +
                                               EFS_RDMA_RECV_ALIGN,
                                               rc->bufsz, wcs[i].byte_len);
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
 * completion channel and blocks on it (1 s pulse). The old live-QP path
 * paused and sched_yield'd forever: during posix jobs=1 that thread
 * was 64% of efs-fuse and 31% of efsd, and the yield handed the core
 * away for a timeslice so the reply sat until the poller was
 * rescheduled. Do not sleep 100 ms after an event — that missed the
 * next completion and stalled mkdir. Heartbeats are 50 ms and the
 * election timeout is 500 ms; the wait is event-driven, so its length
 * cannot start an election.
 * Arm, drain the CQ, then wait. Ack only after poll() reports the
 * channel is readable — acking first disarms the notify. */
static void *recv_poller(void *arg)
{
    struct efs_rdma_dev *dev = arg;
    struct ibv_wc wcs[32];
    unsigned empty = 0;
    for (;;) {
        int n = recv_poll(dev, 32, wcs);
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
            cpu_relax();
            continue;
        }
        empty = 0;
        if (ibv_req_notify_cq(dev->recv_cq, 0) != 0) {
            usleep(1000);
            continue;
        }
        n = recv_poll(dev, 32, wcs);
        if (n < 0) {
            usleep(1000);
            continue;
        }
        if (n > 0) {
            harvest_recv_wcs(wcs, n);
            continue;
        }
        {
            /* The CQ is armed and was drained after arming, so the next
             * completion fires the channel; nothing is waiting on this
             * thread's clock. A 1 ms timeout here was a 1 kHz timer on
             * every idle process (Sep 29: 932 wakeups/s per idle client,
             * recv_poller 34% of its samples, 14% of an idle efsd's, and
             * 158 MB of strace per client in 35 min). One second is a
             * pulse for a missed notify, not a poll interval. */
            struct pollfd p = { .fd = dev->recv_chan->fd, .events = POLLIN };
            int pr = poll(&p, 1, 1000);
            if (pr > 0 && (p.revents & POLLIN))
                ack_cq_events(dev);
        }
    }
    return NULL;
}

/* Lazily create the shared recv CQ + poller for a device (first conn). */
static int dev_shared_cq_start(struct efs_rdma_dev *dev)
{
    if (dev->poller_started) {
        /* K31: fork() keeps only the calling thread. A device whose poller
         * was started in another process has no poller here: completions
         * would land in the CQ and never reach a connection (requests sent,
         * replies silently lost). Refuse instead of hanging. */
        if (dev->poller_pid != getpid()) {
            static int warned;
            if (!__sync_fetch_and_add(&warned, 1))
                fprintf(stderr, "efs: RDMA device %s was initialized in pid %d and is "
                        "used in pid %d after fork(); its completion poller did not "
                        "survive the fork; refusing RDMA in this process\n",
                        dev->ctx && dev->ctx->device ? ibv_get_device_name(dev->ctx->device) : "?",
                        (int)dev->poller_pid, (int)getpid());
            return -1;
        }
        return 0;
    }
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
    struct ibv_device_attr caps;
    if (ibv_query_device(dev->ctx, &caps) || caps.max_cqe < 2) goto cq_fail;
    int capacity = caps.max_cqe < (1 << 16) ? caps.max_cqe : (1 << 16);
    dev->recv_cq = ibv_create_cq(dev->ctx, capacity, NULL, dev->recv_chan, 0);
    if (!dev->recv_cq) goto cq_fail;
    dev->recv_capacity = dev->recv_cq->cqe / 2; /* reserve teardown headroom */
    cq_unmoderate(dev->recv_cq);
    if (pthread_create(&dev->poller, NULL, recv_poller, dev) != 0) {
        ibv_destroy_cq(dev->recv_cq); dev->recv_cq = NULL; goto cq_fail;
    }
    pthread_detach(dev->poller);
    dev->poller_pid = getpid();
    dev->poller_started = 1;
    return 0;
cq_fail:
    ibv_destroy_comp_channel(dev->recv_chan); dev->recv_chan = NULL;
    return -1;
}

static struct efs_rdma_conn *conn_create(struct efs_rdma_dev *dev)
{
    struct efs_rdma_conn *rc = calloc(1, sizeof(*rc));
    if (!rc)
        return NULL;
    rc->dev = dev;
    rc->nrecv = nrecv_bufs();
    if (!rc->nrecv) { free(rc); return NULL; }
    rc->bufsz = EFS_RDMA_BUFSZ;
    rc->reserved = -1;
    rc->cur_buf = -1;
    rc->reg_idx = -1;
    rc->efd = -1;
    rc->tcp_fd = -1;
    rc->psn = (uint32_t)(rand() & 0xFFFFFF);

    pthread_mutex_lock(&g_dev_lock);
    int cq_rc = dev_shared_cq_start(dev);
    if (!cq_rc && dev->recv_reserved + rc->nrecv <= dev->recv_capacity) {
        dev->recv_reserved += rc->nrecv; rc->cq_reserved = rc->nrecv;
    } else cq_rc = -1;
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
    size_t recv_total = (size_t)rc->nrecv * recv_stride;
    size_t total = recv_total + (size_t)EFS_RDMA_NSEND * rc->bufsz;
    if (posix_memalign((void **)&rc->arena, 4096, total) != 0)
        goto fail;
    rc->mr = ibv_reg_mr(dev->pd, rc->arena, recv_total, IBV_ACCESS_LOCAL_WRITE);
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
    ia.cap.max_send_sge = 2; /* W39: header + region payload */
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
    rc->zc_ok = ia.cap.max_send_sge >= 2;

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
                  uint32_t psn, enum ibv_mtu mtu, const uint8_t *gid)
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
    attr.ah_attr.is_global = rc->dev->roce;
    if (rc->dev->roce) {
        if (!gid) return -1;
        unsigned nonzero = 0;
        for (int i = 0; i < 16; i++) nonzero |= gid[i];
        if (!nonzero) return -1;
        memcpy(attr.ah_attr.grh.dgid.raw, gid, 16);
        attr.ah_attr.grh.sgid_index = rc->dev->gid_index;
        attr.ah_attr.grh.hop_limit = 64;
    }
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
    /* rnr_retry 7 means retry FOREVER. With a bounded RQ that can turn
     * "responder momentarily out of recv buffers" into a send that never
     * completes and never errors. A finite count would yield
     * IBV_WC_RNR_RETRY_EXC_ERR, a real CQE the send path already handles;
     * the shipped value stays 7 (open A8 question, see the A6 inventory). */
    attr.rnr_retry = RDMA_RNR_RETRY;
    attr.sq_psn = rc->psn;
    attr.max_rd_atomic = 1;
    return ibv_modify_qp(rc->qp, &attr,
                         IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                         IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                         IBV_QP_MAX_QP_RD_ATOMIC);
}

static void setup_local(struct efs_rdma_conn *rc, struct efs_rmsg_setup *s)
{
    memset(s,0,sizeof(*s)); s->capabilities=EFS_RMSG_CAP_SEGMENTED;
    s->physical_cap=rc->bufsz; s->logical_cap=EFS_RMSG_LOGICAL_MAX;
    s->receives=rc->nrecv; s->outstanding=EFS_RMSG_QUEUE_MAX;
    s->address_version=rc->dev->roce ? 1 : 0; s->lid=rc->dev->lid;
    s->qpn=rc->qp->qp_num; s->psn=rc->psn; s->mtu=rc->dev->mtu;
    if (rc->dev->roce) memcpy(s->gid,rc->dev->gid.raw,16);
}
static int setup_peer(struct efs_rdma_conn *rc, const struct efs_rmsg_setup *peer)
{
    struct efs_rmsg_setup local, n;
    setup_local(rc,&local);
    if (efs_rmsg_negotiate(&n,&local,peer) ||
        efs_rmsg_credit_init(&rc->credits,peer->receives,local.receives) ||
        efs_rmsg_rx_init(&rc->messages,&g_assembly_budget,256ull*1024*1024,
                         n.physical_cap,n.logical_cap,n.outstanding)) return -1;
    rc->max_frame=n.physical_cap; rc->peer_outstanding=n.outstanding;
    if (qp_rtr(rc,peer->lid,peer->qpn,peer->psn,(int)n.mtu,
               rc->dev->roce ? peer->gid : NULL) || qp_rts(rc)) return -1;
    return 0;
}
static int tcp_counts(int fd,uint64_t *tx,uint64_t *rx,int *peer_fin)
{
    struct tcp_info info;memset(&info,0,sizeof(info));socklen_t n=sizeof(info);
    if (getsockopt(fd,IPPROTO_TCP,TCP_INFO,&info,&n) ||
        n<offsetof(struct tcp_info,tcpi_bytes_sent)+sizeof(info.tcpi_bytes_sent)) return -1;
    *tx=info.tcpi_bytes_sent;*rx=info.tcpi_bytes_received;
    *peer_fin=info.tcpi_state==8; /* Linux TCP_CLOSE_WAIT: received FIN counts one octet */
    return 0;
}
static void setup_publish(struct efs_conn *c, struct efs_rdma_conn *rc)
{
    c->rc=rc; c->kind=EFS_CONN_RDMA; rc->tcp_fd=c->fd; rc->counted=1;
    fd_link_now(c->fd,rc->tcp_link,sizeof(rc->tcp_link));
    int fin=0;
    rc->tcp_counted=!tcp_counts(c->fd,&rc->bootstrap_tx,&rc->bootstrap_rx,&fin);
    if (rc->tcp_counted && c->is_server) rc->bootstrap_tx+=5+EFS_RMSG_SETUP_BYTES;
    __sync_fetch_and_add(&g_live_conns,1);
    fprintf(stderr,"efs: RDMA transport up segmented-v1 on %s qpn=%u physical=%u logical=%u peer-receives=%u messages=%u\n",
            rc->dev->name,rc->qp->qp_num,rc->max_frame,rc->messages.logical_cap,
            rc->credits.capacity+1,rc->peer_outstanding);
}
int efs_rdma_client_upgrade(struct efs_conn *c)
{
    if (!efs_rdma_available()) return -1;
    struct efs_rdma_dev *dev=dev_for_fd(c->fd);
    if (!dev) return -1;
    struct efs_rdma_conn *rc=conn_create(dev);
    if (!rc) return -1;
    struct efs_rmsg_setup local,peer; uint8_t req[EFS_RMSG_SETUP_BYTES],type=0;
    void *payload=NULL; uint32_t len=0;
    setup_local(rc,&local);
    int ok=!efs_rmsg_setup_encode(req,&local) &&
        !efs_send_msg(c->fd,EFS_MSG_RDMA_SETUP,req,sizeof(req)) &&
        !efs_recv_msg(c->fd,&type,&payload,&len) &&
        type==EFS_MSG_RDMA_SETUP_REPLY && !efs_rmsg_setup_decode(&peer,payload,len) &&
        !setup_peer(rc,&peer);
    free(payload);
    if (!ok) {
        fprintf(stderr,"efs: segmented RDMA setup failed or incompatible peer\n");
        efs_rdma_conn_destroy(rc); return -1;
    }
    setup_publish(c,rc); return 0;
}
int efs_rdma_server_accept(struct efs_conn *c, const void *payload,
                           uint32_t len, void *reply, uint32_t *reply_len)
{
    /* Failure reply is deliberately not a valid setup record. Bootstrap
     * callers use the raw TCP send; no application wrapper after publish. */
    ((uint8_t *)reply)[0]=EFS_RDMA_STATUS_UNSUPPORTED; *reply_len=1;
    struct efs_rmsg_setup peer,local;
    if (c->rc || efs_rmsg_setup_decode(&peer,payload,len) || !efs_rdma_available()) return -1;
    struct efs_rdma_dev *dev=dev_for_fd(c->fd);
    if (!dev) return -1;
    struct efs_rdma_conn *rc=conn_create(dev);
    if (!rc) return -1;
    if (setup_peer(rc,&peer)) { efs_rdma_conn_destroy(rc);return -1; }
    setup_local(rc,&local);
    if (efs_rmsg_setup_encode(reply,&local)) { efs_rdma_conn_destroy(rc);return -1; }
    *reply_len=EFS_RMSG_SETUP_BYTES; setup_publish(c,rc);return 0;
}

void efs_rdma_conn_destroy(struct efs_rdma_conn *rc)
{
    if (!rc)
        return;
    uint32_t dying_qpn=rc->qp?rc->qp->qp_num:0;
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
            totals_add(&g_retired, rc);
        }
        pthread_mutex_unlock(&g_reg_lock);
    }
    if (rc->send_cq)
        ibv_destroy_cq(rc->send_cq);
    if (rc->efd >= 0)
        close(rc->efd);
    for (int i = 0; i < EFS_RDMA_NSEND; i++) {
        if (rc->send_mrs[i])
            ibv_dereg_mr(rc->send_mrs[i]);
    }
    if (rc->mr)
        ibv_dereg_mr(rc->mr);
    if (rc->cq_reserved) {
        pthread_mutex_lock(&g_dev_lock);
        rc->dev->recv_reserved -= rc->cq_reserved;
        pthread_mutex_unlock(&g_dev_lock);
    }
    if (rc->messages.global) efs_rmsg_rx_destroy(&rc->messages);
    if (rc->counted) {
        uint64_t tx=0,rx=0;int fin=0;
        int measured=rc->tcp_counted&&!tcp_counts(rc->tcp_fd,&tx,&rx,&fin);
        uint64_t extra_tx=measured&&tx>rc->bootstrap_tx?tx-rc->bootstrap_tx:0;
        uint64_t extra_rx=measured&&rx>rc->bootstrap_rx+(unsigned)fin?rx-rc->bootstrap_rx-(unsigned)fin:0;
        fprintf(stderr,"efs: RDMA counters qpn=%u fd=%d data_tx=%llu/%llu data_rx=%llu/%llu app_tx=%llu/%llu app_rx=%llu/%llu controls=%llu/%llu peak=%llu stalls=%llu tcp_measured=%d bootstrap=%llu/%llu post_setup_tcp=%llu/%llu violations=%llu\n",
            dying_qpn,rc->tcp_fd,(unsigned long long)rc->data_frames,(unsigned long long)rc->data_bytes,
            (unsigned long long)rc->rx_frames,(unsigned long long)rc->rx_bytes,
            (unsigned long long)rc->app_tx,(unsigned long long)rc->logical_tx,
            (unsigned long long)rc->app_rx,(unsigned long long)rc->logical_rx,
            (unsigned long long)rc->ctrl_tx,(unsigned long long)rc->ctrl_rx,
            (unsigned long long)rc->assembly_peak,(unsigned long long)rc->credit_stalls,
            measured,(unsigned long long)rc->bootstrap_tx,(unsigned long long)rc->bootstrap_rx,
            (unsigned long long)extra_tx,(unsigned long long)extra_rx,(unsigned long long)rc->tcp_app_rx);
    }
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
    return rc->messages.logical_cap;
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
/* Register only slots actually needed by this connection. Registering all
 * 32 send slots at handshake pinned ~3 MiB even for idle metadata RPCs.
 * Keep the full pipeline capacity, but reuse idle registered slots first. */
static int send_buf_prepare(struct efs_rdma_conn *rc, int idx)
{
    if (!rc->send_mrs[idx]) {
        rc->send_mrs[idx] = ibv_reg_mr(rc->dev->pd, rc->send_bufs[idx],
                                      rc->bufsz, IBV_ACCESS_LOCAL_WRITE);
        if (!rc->send_mrs[idx])
            return -1;
    }
    return idx;
}

static int send_buf_pick(struct efs_rdma_conn *rc)
{
    if (reap_sends(rc)) return -1;
    for (int i=0;i<EFS_RDMA_NSEND-1;i++) {
        int idx=(rc->send_rr+i)%(EFS_RDMA_NSEND-1);
        if (!rc->send_busy[idx] && rc->send_mrs[idx]) {
            rc->send_rr=(idx+1)%(EFS_RDMA_NSEND-1); return idx;
        }
    }
    for (int i=0;i<EFS_RDMA_NSEND-1;i++) {
        int idx=(rc->send_rr+i)%(EFS_RDMA_NSEND-1);
        if (!rc->send_busy[idx]) {
            if (send_buf_prepare(rc,idx)<0) return -1;
            rc->send_rr=(idx+1)%(EFS_RDMA_NSEND-1); return idx;
        }
    }
    return -2; /* owner must progress receives as well as send CQ */
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

static void send_fail_dump(struct efs_rdma_conn *rc, uint8_t type,
                           const char *why)
{
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
            "efs: RDMA send type=%u FAILED (%s) qpn=%u state=%d "
            "dest_qpn=%u dlid=%u rnr_retry=%u retry_cnt=%u timeout=%u "
            "busy=%d broken=%d qrc=%d\n",
            type, why, rc->qp ? rc->qp->qp_num : 0,
            qrc == 0 ? (int)a.qp_state : -1, qrc == 0 ? a.dest_qp_num : 0,
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
                ? "(send CQ drained; failure may be credit/peer/protocol wait)"
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
    rdma_memcheck_send(buf, len);
    struct ibv_sge sge = {
        .addr = (uintptr_t)buf,
        .length = len,
        .lkey = rc->send_mrs[wr_id & 0xffu]->lkey,
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

static int message_progress(struct efs_rdma_conn *rc, unsigned frames);
static int message_pause(struct efs_rdma_conn *rc, int64_t end);
static int send_credit(struct efs_rdma_conn *rc)
{
    int idx=EFS_RDMA_NSEND-1;
    if (rc->advertised_data==rc->reposted_data) return 0;
    /* The control slot's previous CREDIT completes within microseconds.
     * Deferring the new one left only the 1 ms wait timeout to send it, and
     * a peer out of credits slept with us: request/reply round trips of
     * 30 us became 1-2 ms steps (orcd-mini Raft AppendEntries, Oct 16). */
    if (rc->send_busy[idx]) {
        int64_t end=now_us()+EFS_RDMA_CREDIT_SPIN_US;
        while (rc->send_busy[idx]) {
            if (reap_sends(rc)) return -1;
            if (!rc->send_busy[idx] || now_us()>=end) break;
            cpu_relax();
        }
        if (rc->send_busy[idx]) return 0; /* the timed retry remains */
    }
    if (send_buf_prepare(rc,idx)<0) return -1;
    struct efs_rmsg_header h={EFS_RMSG_CREDIT,0,rc->reposted_data,0,0,0};
    if (efs_rmsg_encode(rc->send_bufs[idx],&h) ||
        post_send(rc,SEND_WRID_POOL|idx,rc->send_bufs[idx],EFS_RMSG_ENVELOPE,0)) return -1;
    rc->send_busy[idx]=1;rc->send_posted_us[idx]=now_us();
    rc->advertised_data=rc->reposted_data;rc->ctrl_tx++;return 0;
}
int efs_rdma_send_quiesce(struct efs_rdma_conn *rc)
{
    int ms=efs_net_remaining_ms(EFS_RDMA_SEND_WAIT_US/1000);
    int64_t end=now_us()+(int64_t)ms*1000;
    while (!rc->broken && rc->n_reaped<rc->n_posted) {
        if (message_progress(rc,rc->nrecv)<0) return -1;
        if (rc->n_reaped<rc->n_posted && message_pause(rc,end)<0) return -1;
    }
    return rc->broken ? -1 : 0;
}
/* Copy only the current segment into a registered pool slot. Source p1/p2
 * may be reused on return, even if the last SEND CQE has not arrived. */
int efs_rdma_send_frame(struct efs_rdma_conn *rc, uint8_t type,
                        const void *p1, uint32_t l1,const void *p2,uint32_t l2)
{
    if (rc->broken || l1>UINT32_MAX-l2 || l1+l2>rc->messages.logical_cap-5 ||
        (l1&&!p1) || (l2&&!p2) || rc->tx_id==UINT64_MAX) goto fail;
    uint32_t total=5+l1+l2;uint8_t header[5];
    if (efs_wire_frame_header(type,l1+l2,header)) goto fail;
    int ms=efs_net_remaining_ms(EFS_RDMA_SEND_WAIT_US/1000);
    int64_t end=now_us()+(int64_t)ms*1000;
    uint64_t id=++rc->tx_id;
    for (uint32_t off=0;off<total;) {
        int idx;
        for (;;) {
            if (message_progress(rc,rc->nrecv)<0 || now_us()>=end) goto fail;
            idx=send_buf_pick(rc);if (idx==-1) goto fail;
            if (idx>=0 && rc->credits.sent-rc->credits.returned<rc->credits.capacity) break;
            OWNER_ADD(rc->credit_stalls, 1);
            if (message_pause(rc,end)<0) goto fail;
        }
        uint32_t n=total-off;if (n>rc->max_frame-EFS_RMSG_ENVELOPE) n=rc->max_frame-EFS_RMSG_ENVELOPE;
        struct efs_rmsg_header h={EFS_RMSG_DATA,off+n==total?EFS_RMSG_FINAL:0,id,total,off,n};
        uint8_t *b=rc->send_bufs[idx];if (efs_rmsg_encode(b,&h)) goto fail;
        const void *parts[3]={header,p1,p2};uint32_t lens[3]={5,l1,l2};
        uint32_t pos=off,left=n,dst=EFS_RMSG_ENVELOPE;
        for (unsigned i=0;i<3 && left;i++) {
            if (pos>=lens[i]) {pos-=lens[i];continue;}
            uint32_t take=lens[i]-pos;if (take>left) take=left;
            memcpy(b+dst,(const uint8_t *)parts[i]+pos,take);
            dst+=take;left-=take;pos=0;
        }
        if (left || efs_rmsg_credit_take(&rc->credits)!=1 ||
            post_send(rc,SEND_WRID_POOL|idx,b,n+EFS_RMSG_ENVELOPE,n+EFS_RMSG_ENVELOPE<=rc->max_inline)) goto fail;
        rc->send_busy[idx]=1;rc->send_posted_us[idx]=now_us();
        OWNER_ADD(rc->data_frames, 1);OWNER_ADD(rc->data_bytes, n+EFS_RMSG_ENVELOPE);off+=n;
    }
    rc->app_tx++;rc->logical_tx+=total;return EFS_OK;
fail:
    rc->broken=1;send_fail_dump(rc,type,"segmented send failed");return EFS_ERR_NET;
}
/* Initially disable direct-fill/W39 sources: every segmented send is copied
 * so TLS/client-slab lifetimes cannot outlive the operation. */
void *efs_rdma_send_buf(struct efs_rdma_conn *rc,uint32_t n)
{ (void)rc;(void)n;return NULL; }
int efs_rdma_send_commit(struct efs_rdma_conn *rc,void *buf,uint8_t type,uint32_t n)
{ (void)buf;(void)type;(void)n;rc->broken=1;return EFS_ERR_NET; }

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

/* Only the checked-out connection owner enters this progress routine.
 * Poller publishes physical CQEs under g_reg_lock, never assembles or waits. */
static int message_progress(struct efs_rdma_conn *rc,unsigned frames)
{
    if (rc->broken || reap_or_dead(rc)) return -1;
    if (rc->assembly_end_us && now_us()>=rc->assembly_end_us) goto fail;
    for (unsigned i=0;i<frames && pend_pop(rc);i++) {
        uint32_t len=rc->cur_len;uint8_t *p=rc->recv_bufs[rc->cur_buf]+EFS_RDMA_RECV_ALIGN;
        struct efs_rmsg_header h;
        if (len>rc->max_frame || efs_rmsg_decode(&h,p,len)) goto fail;
        if (h.kind==EFS_RMSG_CREDIT) {
            if (efs_rmsg_credit_return(&rc->credits,h.id)) goto fail;
            rc->ctrl_rx++;
        } else {
            if (!rc->messages.active) rc->assembly_end_us=now_us()+(int64_t)EFS_IO_TIMEOUT_MS*1000;
            int got=efs_rmsg_rx_feed(&rc->messages,p,len);
            if (got<0) goto fail;
            OWNER_ADD(rc->rx_frames, 1);OWNER_ADD(rc->rx_bytes, len);
            if (rc->messages.used>rc->assembly_peak) rc->assembly_peak=rc->messages.used;
            if (got) {rc->assembly_end_us=0;rc->app_rx++;rc->logical_rx+=h.total;}
        }
        if (efs_rdma_recv_repost(rc)) goto fail;
        if (h.kind==EFS_RMSG_DATA) {
            if (rc->reposted_data==UINT64_MAX) goto fail;
            rc->reposted_data++;
        }
        if (send_credit(rc)) goto fail;
    }
    if (send_credit(rc)) goto fail;
    return 0;
fail:
    rc->broken=1;return -1;
}
static int message_pause(struct efs_rdma_conn *rc,int64_t end)
{
    if (rc->broken || now_us()>=end) {rc->broken=1;return -1;}
    struct pollfd pf[2]={{rc->efd,POLLIN,0},{rc->tcp_fd,POLLIN,0}};
    efd_arm(rc);
    int n=poll(pf,rc->tcp_fd>=0?2:1,1);efd_disarm(rc);
    if (n<0 && errno!=EINTR) {rc->broken=1;return -1;}
    if (rc->tcp_fd>=0 && pf[1].revents) {
        char byte;ssize_t got=recv(rc->tcp_fd,&byte,1,MSG_PEEK|MSG_DONTWAIT);
        if (got>=0 || (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)) {
            if (got>0) rc->tcp_app_rx++;
            rc->broken=1;return -1; /* TCP app bytes and EOF both retire */
        }
    }
    if (__atomic_load_n(&rc->pr_head,__ATOMIC_ACQUIRE)==rc->pr_tail) pend_drain_efd(rc);
    return 0;
}
int efs_rdma_message_ready(struct efs_rdma_conn *rc)
{
    efd_disarm(rc);
    if (message_progress(rc,1)<0) return -1;
    /* CREDIT-only and consumed-frame wakes must not leave a permanently ready
     * eventfd. The caller arms/rechecks the ring before its next poll, so a
     * concurrent push racing this drain is still observed without a lost wake. */
    if (__atomic_load_n(&rc->pr_head,__ATOMIC_ACQUIRE)==rc->pr_tail)
        pend_drain_efd(rc);
    return rc->broken ? -1 : efs_rmsg_rx_peek(&rc->messages)!=NULL;
}
int efs_rdma_message_wait(struct efs_rdma_conn *rc,int timeout_ms)
{
    int64_t end=timeout_ms<0?INT64_MAX:now_us()+(int64_t)timeout_ms*1000;
    while (!efs_rmsg_rx_peek(&rc->messages)) {
        if (message_progress(rc,rc->nrecv)<0) return EFS_ERR_NET;
        if (!efs_rmsg_rx_peek(&rc->messages) && message_pause(rc,end)<0)
            return EFS_ERR_NET;
    }
    return rc->broken?EFS_ERR_NET:EFS_OK;
}
void *efs_rdma_message_frame(struct efs_rdma_conn *rc,uint32_t *len)
{
    const struct efs_rmsg_view *v=efs_rmsg_rx_peek(&rc->messages);
    if (!v) return NULL;
    if (len) *len=v->len;
    return v->frame;
}
void efs_rdma_message_release(struct efs_rdma_conn *rc)
{ efs_rmsg_rx_release(&rc->messages); }
void *efs_rdma_message_take(struct efs_rdma_conn *rc)
{
    struct efs_rmsg_rx *r=&rc->messages;
    if (!r->count) return NULL;
    struct efs_rmsg_view *v=&r->queue[r->head];uint8_t *p=v->frame;
    memmove(p,p+5,v->len-5);
    r->used-=v->len;__atomic_fetch_sub(&r->global->used,v->len,__ATOMIC_RELAXED);
    memset(v,0,sizeof(*v));r->head=(r->head+1)%r->capacity;r->count--;
    return p; /* normal free() ownership; no second full allocation */
}
uint64_t efs_rdma_assembly_bytes(void)
{ return __atomic_load_n(&g_assembly_budget.used,__ATOMIC_RELAXED); }
void efs_rdma_tcp_violation(struct efs_rdma_conn *rc)
{ rc->tcp_app_rx++;rc->broken=1; }
void efs_rdma_totals(struct efs_rdma_totals *out)
{
    pthread_mutex_lock(&g_reg_lock);
    *out = g_retired;
    for (int i = 0; i < EFS_RDMA_MAX_CONNS; i++)
        if (g_conn_reg[i])
            totals_add(out, g_conn_reg[i]);
    pthread_mutex_unlock(&g_reg_lock);
    out->live_conns = (uint64_t)efs_rdma_live_conns();
}
void efs_rdma_message_stats(struct efs_rdma_conn *rc,struct efs_rdma_message_stats *o)
{
    *o=(struct efs_rdma_message_stats){rc->data_frames,rc->data_bytes,rc->rx_frames,rc->rx_bytes,
        rc->app_tx,rc->app_rx,rc->logical_tx,rc->logical_rx,rc->ctrl_tx,rc->ctrl_rx,
        rc->credit_stalls,rc->tcp_app_rx,rc->messages.used,
        __atomic_load_n(&g_assembly_budget.used,__ATOMIC_RELAXED)};
}
int efs_rdma_message_poll_ms(struct efs_rdma_conn *rc)
{
    if (rc->reposted_data!=rc->advertised_data) return 1;
    if (rc->assembly_end_us) {
        int64_t left=(rc->assembly_end_us-now_us()+999)/1000;
        return left<1?0:left>100?100:(int)left;
    }
    return -1; /* idle connections still sleep until CQ or TCP lifetime event */
}
unsigned efs_rdma_message_capacity(struct efs_rdma_conn *rc)
{ return rc->peer_outstanding; }

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

/* The caller is about to poll() this fd (conn_wait_request, the PUT reply
 * poll in write.c): arm it. A reply that landed between the caller's
 * ready check and this call is self-signalled by efd_arm so the poll does
 * not sleep on a ring that is already non-empty. */
int efs_rdma_reply_fd(struct efs_rdma_conn *rc)
{
    efd_arm(rc);
    return rc->efd;
}
