/* D39 C1: configured cluster membership (cluster.conf) and its digest.
 * See include/efs/cluster_conf.h for the file contract. */
#include "efs/cluster_conf.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- SHA-256 (FIPS 180-4), small and dependency-free ---- */
static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(uint32_t st[8], const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = st[0]; b = st[1]; c = st[2]; d = st[3]; e = st[4]; f = st[5]; g = st[6]; h = st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

void efs_sha256(const void *data, size_t len, uint8_t out[32])
{
    uint32_t st[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const uint8_t *p = data;
    size_t left = len;
    uint8_t tail[128];
    while (left >= 64) {
        sha_block(st, p);
        p += 64;
        left -= 64;
    }
    memset(tail, 0, sizeof(tail));
    memcpy(tail, p, left);
    tail[left] = 0x80;
    size_t tl = left + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha_block(st, tail);
    if (tl == 128)
        sha_block(st, tail + 64);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(st[i] >> 24);
        out[4 * i + 1] = (uint8_t)(st[i] >> 16);
        out[4 * i + 2] = (uint8_t)(st[i] >> 8);
        out[4 * i + 3] = (uint8_t)st[i];
    }
}

void efs_cluster_digest_short(const uint8_t digest[EFS_CLUSTER_DIGEST_LEN], char out[13])
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        out[2 * i] = hx[digest[i] >> 4];
        out[2 * i + 1] = hx[digest[i] & 15];
    }
    out[12] = '\0';
}

/* ---- parsing ---- */
static void conf_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || !errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* Canonical decimal: digits only, no sign, no leading zero unless "0". */
static int canonical_decimal(const char *s, size_t n, uint32_t max, uint32_t *out)
{
    uint64_t v = 0;
    if (!n || n > 10 || (n > 1 && s[0] == '0'))
        return -1;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    if (v > max)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

static int addr_canonical_n(const char *s, size_t n, uint32_t *ip)
{
    uint32_t octet[4], v = 0;
    size_t start = 0;
    int part = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || s[i] == '.') {
            if (part > 3 || canonical_decimal(s + start, i - start, 255, &octet[part]))
                return -1;
            part++;
            start = i + 1;
        }
    }
    if (part != 4)
        return -1;
    for (int i = 0; i < 4; i++)
        v = v << 8 | octet[i];
    if (v == 0 || v == 0xffffffffu)
        return -1; /* wildcard, broadcast */
    if (ip)
        *ip = htonl(v);
    return 0;
}

int efs_cluster_addr_canonical(const char *s, uint32_t *ip)
{
    if (!s)
        return EFS_ERR_INVAL;
    return addr_canonical_n(s, strlen(s), ip) ? EFS_ERR_INVAL : EFS_OK;
}

int efs_cluster_conf_parse(const char *text, size_t len, struct efs_cluster_conf *out,
                           char *err, size_t errlen)
{
    struct efs_cluster_conf c;
    uint32_t count = 0;
    size_t pos = 0;
    unsigned line = 0;

    if (!text || !out) {
        conf_err(err, errlen, "no configuration");
        return EFS_ERR_INVAL;
    }
    memset(&c, 0, sizeof(c));
    while (pos < len) {
        size_t end = pos;
        while (end < len && text[end] != '\n')
            end++;
        line++;
        /* Fields of this line, comment stripped. */
        const char *f[3];
        size_t fl[3];
        int nf = 0;
        size_t i = pos;
        while (i < end && text[i] != '#') {
            if (text[i] == ' ' || text[i] == '\t' || text[i] == '\r') {
                i++;
                continue;
            }
            if (text[i] == '\0') {
                conf_err(err, errlen, "line %u: NUL byte", line);
                return EFS_ERR_INVAL;
            }
            size_t s = i;
            while (i < end && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '#')
                i++;
            if (nf == 2) {
                conf_err(err, errlen, "line %u: extra field (want `<id> <ipv4>:<port>`)", line);
                return EFS_ERR_INVAL;
            }
            f[nf] = text + s;
            fl[nf] = i - s;
            nf++;
        }
        pos = end + 1;
        if (nf == 0)
            continue;
        if (nf != 2) {
            conf_err(err, errlen, "line %u: missing endpoint (want `<id> <ipv4>:<port>`)", line);
            return EFS_ERR_INVAL;
        }
        uint32_t id = 0, port = 0, ip = 0;
        if (canonical_decimal(f[0], fl[0], EFS_CLUSTER_MAX_MEMBERS, &id) || id == 0) {
            conf_err(err, errlen, "line %u: member id `%.*s` is not a canonical decimal 1..%d",
                     line, (int)fl[0], f[0], EFS_CLUSTER_MAX_MEMBERS);
            return EFS_ERR_INVAL;
        }
        const char *colon = memchr(f[1], ':', fl[1]);
        if (!colon) {
            conf_err(err, errlen, "line %u: endpoint `%.*s` has no port", line, (int)fl[1], f[1]);
            return EFS_ERR_INVAL;
        }
        size_t al = (size_t)(colon - f[1]);
        if (addr_canonical_n(f[1], al, &ip)) {
            conf_err(err, errlen, "line %u: `%.*s` is not a canonical member IPv4 address "
                     "(no host names, no wildcard or broadcast)", line, (int)al, f[1]);
            return EFS_ERR_INVAL;
        }
        if (canonical_decimal(colon + 1, fl[1] - al - 1, 65535, &port) || port == 0) {
            conf_err(err, errlen, "line %u: port `%.*s` is not a canonical decimal 1..65535",
                     line, (int)(fl[1] - al - 1), colon + 1);
            return EFS_ERR_INVAL;
        }
        if (c.m[id - 1].id) {
            conf_err(err, errlen, "line %u: duplicate member id %u", line, id);
            return EFS_ERR_INVAL;
        }
        for (uint32_t k = 0; k < EFS_CLUSTER_MAX_MEMBERS; k++) {
            if (c.m[k].id && c.m[k].port == port && !strncmp(c.m[k].addr, f[1], al) &&
                c.m[k].addr[al] == '\0') {
                conf_err(err, errlen, "line %u: duplicate endpoint %.*s (member %u)", line,
                         (int)fl[1], f[1], c.m[k].id);
                return EFS_ERR_INVAL;
            }
        }
        c.m[id - 1].id = id;
        memcpy(c.m[id - 1].addr, f[1], al);
        c.m[id - 1].addr[al] = '\0';
        c.m[id - 1].port = (uint16_t)port;
        count++;
    }
    if (count < EFS_CLUSTER_MIN_MEMBERS || count > EFS_CLUSTER_MAX_MEMBERS) {
        conf_err(err, errlen, "%u members; a cluster has %d..%d", count,
                 EFS_CLUSTER_MIN_MEMBERS, EFS_CLUSTER_MAX_MEMBERS);
        return EFS_ERR_INVAL;
    }
    for (uint32_t k = 0; k < count; k++) {
        if (!c.m[k].id) {
            conf_err(err, errlen, "member ids must be exactly 1..%u; %u is missing", count, k + 1);
            return EFS_ERR_INVAL;
        }
    }
    c.n = count;
    {
        char canon[EFS_CLUSTER_MAX_MEMBERS * 32 + 32];
        size_t cl = efs_cluster_conf_canonical(&c, canon, sizeof(canon));
        if (!cl) {
            conf_err(err, errlen, "canonical form overflow");
            return EFS_ERR_INVAL;
        }
        efs_sha256(canon, cl, c.digest);
    }
    *out = c;
    return EFS_OK;
}

size_t efs_cluster_conf_canonical(const struct efs_cluster_conf *c, char *buf, size_t cap)
{
    size_t used = 0;
    int w = snprintf(buf, cap, "%s", EFS_CLUSTER_CANONICAL_HEADER);
    if (w < 0 || (size_t)w >= cap)
        return 0;
    used = (size_t)w;
    for (uint32_t i = 0; i < c->n; i++) {
        w = snprintf(buf + used, cap - used, "%u %s:%u\n", c->m[i].id, c->m[i].addr,
                     (unsigned)c->m[i].port);
        if (w < 0 || (size_t)w >= cap - used)
            return 0;
        used += (size_t)w;
    }
    return used;
}

int efs_cluster_conf_load(const char *path, struct efs_cluster_conf *out, char *err, size_t errlen)
{
    char buf[EFS_CLUSTER_CONF_MAX_BYTES + 1];
    size_t got = 0;
    int fd = path ? open(path, O_RDONLY | O_CLOEXEC) : -1;
    if (fd < 0) {
        conf_err(err, errlen, "%s: %s", path ? path : "(none)", strerror(path ? errno : EINVAL));
        return EFS_ERR_INVAL;
    }
    for (;;) {
        ssize_t n = read(fd, buf + got, sizeof(buf) - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            conf_err(err, errlen, "%s: %s", path, strerror(errno));
            close(fd);
            return EFS_ERR_INVAL;
        }
        if (n == 0)
            break;
        got += (size_t)n;
        if (got > EFS_CLUSTER_CONF_MAX_BYTES) {
            conf_err(err, errlen, "%s: larger than %d bytes", path, EFS_CLUSTER_CONF_MAX_BYTES);
            close(fd);
            return EFS_ERR_INVAL;
        }
    }
    close(fd);
    char inner[256];
    if (efs_cluster_conf_parse(buf, got, out, inner, sizeof(inner)) != EFS_OK) {
        conf_err(err, errlen, "%s: %s", path, inner);
        return EFS_ERR_INVAL;
    }
    return EFS_OK;
}

uint32_t efs_cluster_conf_member_at(const struct efs_cluster_conf *c, const char *addr,
                                    uint16_t port)
{
    if (!c || !addr || efs_cluster_addr_canonical(addr, NULL) != EFS_OK)
        return 0;
    for (uint32_t i = 0; i < c->n; i++)
        if (c->m[i].port == port && !strcmp(c->m[i].addr, addr))
            return c->m[i].id;
    return 0;
}

static struct efs_cluster_conf g_conf;
static int g_conf_set;

const struct efs_cluster_conf *efs_cluster_conf_current(void)
{
    return __atomic_load_n(&g_conf_set, __ATOMIC_ACQUIRE) ? &g_conf : NULL;
}

void efs_cluster_conf_set_current(const struct efs_cluster_conf *c)
{
    if (!c) {
        __atomic_store_n(&g_conf_set, 0, __ATOMIC_RELEASE);
        return;
    }
    g_conf = *c;
    __atomic_store_n(&g_conf_set, 1, __ATOMIC_RELEASE);
}
