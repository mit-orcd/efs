/* K33/W157: local-subnet discovery. See discovery.h. */
#include "discovery.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const uint8_t disc_magic[8] = {'E', 'F', 'S', 'D', 'I', 'S', 'C', 1};
#define DISC_PROBE 1u
#define DISC_ANSWER 2u

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static void put64(uint8_t *p, uint64_t v) { efs_store_be32(p, (uint32_t)(v >> 32)); efs_store_be32(p + 4, (uint32_t)v); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static uint64_t get64(const uint8_t *p) { return (uint64_t)efs_load_be32(p) << 32 | efs_load_be32(p + 4); }

void efs_disc_probe_encode(uint8_t out[EFS_DISC_PROBE_LEN], uint64_t nonce)
{
    memcpy(out, disc_magic, 8);
    out[8] = DISC_PROBE;
    put64(out + 9, nonce);
}

int efs_disc_probe_decode(const uint8_t *in, size_t len, uint64_t *nonce)
{
    if (!in || !nonce || len != EFS_DISC_PROBE_LEN || memcmp(in, disc_magic, 8) || in[8] != DISC_PROBE)
        return -1;
    *nonce = get64(in + 9);
    return 0;
}

static int printable(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] < 0x21 || s[i] > 0x7e)
            return 0;
    return 1;
}

size_t efs_disc_answer_encode(uint8_t *out, size_t cap, uint64_t nonce,
                              const struct efs_disc_answer *a)
{
    size_t alen = strnlen(a->addr, sizeof(a->addr));
    size_t blen = strnlen(a->build_id, sizeof(a->build_id));
    size_t len = 8 + 1 + 8 + 8 + 4 + 1 + 2 + 1 + alen + 1 + blen;
    if (!a->cluster || !a->port || !alen || alen > 63 || blen >= sizeof(a->build_id) ||
        len > cap)
        return 0;
    memcpy(out, disc_magic, 8);
    out[8] = DISC_ANSWER;
    put64(out + 9, nonce);
    put64(out + 17, a->cluster);
    efs_store_be32(out + 25, a->count);
    out[29] = a->formed ? 1 : 0;
    put16(out + 30, a->port);
    out[32] = (uint8_t)alen;
    memcpy(out + 33, a->addr, alen);
    out[33 + alen] = (uint8_t)blen;
    memcpy(out + 34 + alen, a->build_id, blen);
    return len;
}

int efs_disc_answer_decode(const uint8_t *in, size_t len, uint64_t nonce,
                           struct efs_disc_answer *a)
{
    if (!in || !a || len < 34 || memcmp(in, disc_magic, 8) || in[8] != DISC_ANSWER || get64(in + 9) != nonce)
        return -1;
    size_t alen = in[32];
    if (!alen || alen > 63 || len < 34 + alen)
        return -1;
    size_t blen = in[33 + alen];
    if (blen >= EFS_BUILD_ID_LEN || len != 34 + alen + blen || in[29] > 1)
        return -1;
    struct efs_disc_answer decoded = {0};
    struct efs_disc_answer *dst = &decoded;
    dst->cluster = get64(in + 17);
    dst->count = efs_load_be32(in + 25);
    dst->formed = in[29];
    dst->port = get16(in + 30);
    memcpy(dst->addr, in + 33, alen);
    memcpy(dst->build_id, in + 34 + alen, blen);
    if (!dst->cluster || !dst->port || !printable(dst->addr, alen) || !printable(dst->build_id, blen))
        return -1;
    *a = decoded;
    return 0;
}

uint32_t efs_disc_broadcast(uint32_t addr, uint32_t mask)
{
    return addr | ~mask;
}

int efs_disc_targets(const char *iface, struct efs_disc_target *t, int max, char *names,
                     size_t cap)
{
    struct ifaddrs *ifs = NULL;
    size_t nl = 0;
    int n = 0;

    if (names && cap)
        names[0] = '\0';
    if (getifaddrs(&ifs) != 0)
        return 0;
    for (struct ifaddrs *i = ifs; i && n < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask ||
            !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_POINTOPOINT))
            continue;
        uint32_t a = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        uint32_t m = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
        struct in_addr ia = {.s_addr = htonl(a)}, ib = {.s_addr = htonl(efs_disc_broadcast(a, m))};
        char as[16], bs[16];
        inet_ntop(AF_INET, &ia, as, sizeof(as));
        inet_ntop(AF_INET, &ib, bs, sizeof(bs));
        if (iface && *iface && strcmp(iface, i->ifa_name) && strcmp(iface, as))
            continue;
        snprintf(t[n].ifname, sizeof(t[n].ifname), "%s", i->ifa_name);
        snprintf(t[n].addr, sizeof(t[n].addr), "%s", as);
        snprintf(t[n].bcast, sizeof(t[n].bcast), "%s", bs);
        if (names && nl < cap)
            nl += (size_t)snprintf(names + nl, cap - nl, "%s%s %s (broadcast %s)", n ? ", " : "",
                                   i->ifa_name, as, bs);
        n++;
    }
    freeifaddrs(ifs);
    return n;
}

static uint64_t disc_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int efs_disc_probe(const struct efs_disc_target *t, int n, uint16_t port, int wait_ms,
                   struct efs_disc_answer *out, int max)
{
    uint8_t probe[EFS_DISC_PROBE_LEN], buf[512];
    uint64_t nonce = 0;
    int got = 0, sent = 0, one = 1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    int rfd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (rfd >= 0) {
        if (read(rfd, &nonce, sizeof(nonce)) != (ssize_t)sizeof(nonce))
            nonce = 0;
        close(rfd);
    }
    nonce ^= disc_now_ms() ^ ((uint64_t)getpid() << 40);
    efs_disc_probe_encode(probe, nonce);
    uint64_t end = disc_now_ms() + (uint64_t)wait_ms, next = 0;
    for (int round = 0;;) {
        uint64_t now = disc_now_ms();
        if (now >= end)
            break;
        /* Three probes over the window: a datagram may be lost. */
        if (round < 3 && now >= next) {
            for (int i = 0; i < n; i++) {
                struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port)};
                if (inet_pton(AF_INET, t[i].bcast, &sa.sin_addr) != 1)
                    continue;
                if (sendto(fd, probe, sizeof(probe), 0, (struct sockaddr *)&sa, sizeof(sa)) ==
                    (ssize_t)sizeof(probe))
                    sent++;
            }
            round++;
            next = now + (uint64_t)wait_ms / 3;
        }
        struct pollfd p = {.fd = fd, .events = POLLIN};
        uint64_t until = round < 3 && next < end ? next : end;
        int timeout = until > now ? (int)(until - now) : 0;
        if (poll(&p, 1, timeout) <= 0)
            continue;
        ssize_t r = recv(fd, buf, sizeof(buf), 0);
        struct efs_disc_answer a;
        if (r <= 0 || efs_disc_answer_decode(buf, (size_t)r, nonce, &a) != 0)
            continue;
        int dup = 0;
        for (int i = 0; i < got; i++)
            dup |= out[i].port == a.port && !strcmp(out[i].addr, a.addr);
        if (!dup && got < max)
            out[got++] = a;
    }
    close(fd);
    return sent ? got : -1;
}

struct disc_responder {
    int fd;
    int (*info)(struct efs_disc_answer *a);
};

static void *disc_responder_thread(void *arg)
{
    struct disc_responder *r = arg;
    uint8_t buf[512], ans[EFS_DISC_ANSWER_MAX];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(r->fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            break;
        uint64_t nonce;
        struct efs_disc_answer a;
        memset(&a, 0, sizeof(a));
        if (efs_disc_probe_decode(buf, (size_t)n, &nonce) != 0 || r->info(&a) != 0)
            continue;
        size_t len = efs_disc_answer_encode(ans, sizeof(ans), nonce, &a);
        if (len)
            (void)sendto(r->fd, ans, len, 0, (struct sockaddr *)&from, fl);
    }
    return NULL;
}

int efs_disc_responder_start(uint16_t port, int (*info)(struct efs_disc_answer *a))
{
    int one = 1;
    struct disc_responder *r = calloc(1, sizeof(*r));
    if (!r)
        return -1;
    r->info = info;
    r->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (r->fd < 0) {
        free(r);
        return -1;
    }
    /* Several nodes on one host share the port; every one of them receives
     * a broadcast probe. */
    (void)setsockopt(r->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    (void)setsockopt(r->fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    (void)setsockopt(r->fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(INADDR_ANY)};
    pthread_t tid;
    if (bind(r->fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        pthread_create(&tid, NULL, disc_responder_thread, r) != 0) {
        int e = errno;
        close(r->fd);
        free(r);
        errno = e;
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

int efs_disc_local_addr(const char *host, uint16_t port, char *out, size_t cap)
{
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port)}, me;
    socklen_t ml = sizeof(me);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1)
        return -1;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
                     getsockname(fd, (struct sockaddr *)&me, &ml) == 0 &&
                     inet_ntop(AF_INET, &me.sin_addr, out, (socklen_t)cap)
                 ? 0 : -1;
    close(fd);
    return rc;
}
