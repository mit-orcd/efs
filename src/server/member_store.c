/* Store ownership, the durable boot counter and the persisted member
 * identity with the committed membership. See member_store.h. */
#include "member_store.h"
#include "efs/checksum.h"
#include "efs/common.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint8_t boot_magic[8] = {'E', 'F', 'S', 'B', 'O', 'O', 'T', 0};
static const uint8_t ident_magic[8] = {'E', 'F', 'S', 'I', 'D', 'N', 'T', 0};
#define BOOT_VERSION 1u
#define IDENT_VERSION 3u
#define IDENT_SUM_AT (EFS_IDENTITY_FILE_LEN - 32u)

static void ms_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || !errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}


static void put64(uint8_t *p, uint64_t v)
{
    efs_store_be32(p, (uint32_t)(v >> 32));
    efs_store_be32(p + 4, (uint32_t)v);
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)efs_load_be32(p) << 32 | efs_load_be32(p + 4);
}
static uint64_t check64(const uint8_t *p, size_t n)
{
    uint8_t d[EFS_HASH_SIZE];
    efs_hash(p, n, d);
    return get64(d);
}

void member_store_boot_encode(uint64_t counter, uint8_t out[EFS_BOOT_FILE_LEN])
{
    memset(out, 0, EFS_BOOT_FILE_LEN);
    memcpy(out, boot_magic, 8);
    efs_store_be32(out + 8, BOOT_VERSION);
    put64(out + 16, counter);
    put64(out + 24, check64(out, 24));
}

int member_store_boot_decode(const uint8_t *in, size_t len, uint64_t *counter)
{
    if (!in || !counter || len != EFS_BOOT_FILE_LEN)
        return EFS_ERR_DECODE;
    if (memcmp(in, boot_magic, 8) || efs_load_be32(in + 8) != BOOT_VERSION || efs_load_be32(in + 12))
        return EFS_ERR_UNSUPPORTED;
    if (get64(in + 24) != check64(in, 24))
        return EFS_ERR_CHECKSUM;
    *counter = get64(in + 16);
    return EFS_OK;
}

const char *member_store_state_name(uint32_t state)
{
    return state == EFS_MEMBER_JOINING ? "joining" : state == EFS_MEMBER_FORMED ? "formed"
           : state == EFS_MEMBER_SEED ? "seed" : state == EFS_MEMBER_PROMISED ? "promised"
           : "invalid";
}

/* [magic:8][version:4][member:4][state:4][reclen:4][token:8]
 * [cluster:8][port:2][alen:1][addr:63][pad:6][record:MAX][sum:32]. */
#define IDENT_REC_AT EFS_IDENTITY_REC_AT
int member_store_identity_encode(const struct member_store *ms, uint8_t out[EFS_IDENTITY_FILE_LEN])
{
    size_t rlen = 0, alen = strlen(ms->addr);
    memset(out, 0, EFS_IDENTITY_FILE_LEN);
    memcpy(out, ident_magic, 8);
    efs_store_be32(out + 8, IDENT_VERSION);
    efs_store_be32(out + 12, ms->member_id);
    efs_store_be32(out + 16, ms->state);
    put64(out + 24, ms->token);
    put64(out + 32, ms->cluster);
    out[40] = (uint8_t)(ms->port >> 8);
    out[41] = (uint8_t)ms->port;
    if (alen > EFS_MEMBER_ADDR_MAX)
        return EFS_ERR_INVAL;
    out[42] = (uint8_t)alen;
    memcpy(out + 43, ms->addr, alen);
    if ((ms->state == EFS_MEMBER_FORMED || ms->state == EFS_MEMBER_PROMISED) &&
        efs_membership_encode(&ms->members, out + IDENT_REC_AT, EFS_MEMBERSHIP_MAX_LEN,
                              &rlen) != EFS_OK)
        return EFS_ERR_INVAL;
    efs_store_be32(out + 20, (uint32_t)rlen);
    efs_hash(out, IDENT_SUM_AT, out + IDENT_SUM_AT);
    return EFS_OK;
}

int member_store_identity_decode(const uint8_t *buf, size_t got,
                                  struct member_store *ms, char *err, size_t errlen)
{
    uint8_t sum[EFS_HASH_SIZE];
    if (buf && got == EFS_IDENTITY_FILE_LEN)
        efs_hash(buf, IDENT_SUM_AT, sum);
    if (!buf || !ms || got != EFS_IDENTITY_FILE_LEN || memcmp(buf, ident_magic, 8) ||
        efs_load_be32(buf + 8) != IDENT_VERSION || memcmp(sum, buf + IDENT_SUM_AT, EFS_HASH_SIZE) ||
        efs_load_be32(buf + 16) > EFS_MEMBER_PROMISED || efs_load_be32(buf + 20) > EFS_MEMBERSHIP_MAX_LEN ||
        buf[42] > EFS_MEMBER_ADDR_MAX) {
        ms_err(err, errlen, "store %s: identity.bin is unreadable, short, corrupt or of "
               "another version", "record");
        return EFS_ERR_UNSUPPORTED;
    }
    struct member_store decoded = *ms;
    struct member_store *out = ms;
    ms = &decoded;
    ms->member_id = efs_load_be32(buf + 12);
    ms->state = efs_load_be32(buf + 16);
    ms->token = get64(buf + 24);
    ms->cluster = get64(buf + 32);
    ms->port = (uint16_t)(buf[40] << 8 | buf[41]);
    memcpy(ms->addr, buf + 43, buf[42]);
    ms->addr[buf[42]] = '\0';
    for (unsigned i = 0; i < buf[42]; i++)
        if (buf[43+i] <= 0x20 || buf[43+i] >= 0x7f) {
            ms_err(err, errlen, "identity.bin: invalid endpoint");
            return EFS_ERR_UNSUPPORTED;
        }
    uint32_t rlen = efs_load_be32(buf + 20);
    const struct efs_member *self = NULL;
    int has_rec = ms->state == EFS_MEMBER_FORMED || ms->state == EFS_MEMBER_PROMISED;
    if (rlen && efs_membership_decode(buf + IDENT_REC_AT, rlen, &ms->members) == EFS_OK)
        self = efs_membership_find(&ms->members, ms->member_id);
    int ok = ms->token && has_rec == (rlen != 0);
    if (ms->state == EFS_MEMBER_JOINING)
        ok = ok && !ms->member_id && !ms->cluster && !ms->port && !ms->addr[0];
    else if (ms->state == EFS_MEMBER_SEED)
        ok = ok && ms->member_id == 1 && ms->cluster && ms->port && ms->addr[0];
    else
        ok = ok && self && self->token == ms->token && self->port == ms->port &&
             !strcmp(self->addr, ms->addr) && ms->members.cluster == ms->cluster &&
             (ms->state == EFS_MEMBER_FORMED || ms->members.count == ms->members.formed);
    if (!ok) {
        ms_err(err, errlen, "store %s: identity.bin state %s disagrees with its node id, "
               "endpoint or membership record", "record", member_store_state_name(ms->state));
        return EFS_ERR_UNSUPPORTED;
    }
    *out = decoded;
    return EFS_OK;
}

/* Read a whole small file; EFS_ERR_NOT_FOUND when absent. */
static int read_small(const char *path, uint8_t *buf, size_t cap, size_t *got)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return errno == ENOENT ? EFS_ERR_NOT_FOUND : EFS_ERR_IO;
    size_t n = 0;
    for (;;) {
        ssize_t r = read(fd, buf + n, cap - n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            close(fd);
            return EFS_ERR_IO;
        }
        if (r == 0)
            break;
        n += (size_t)r;
        if (n == cap) {
            uint8_t extra;
            r = read(fd, &extra, 1);
            if (r != 0) {
                close(fd);
                *got = cap + 1; /* too long */
                return EFS_OK;
            }
            break;
        }
    }
    close(fd);
    *got = n;
    return EFS_OK;
}

static int write_replace(const char *dir, const char *name, const uint8_t *buf, size_t len)
{
    char tmp[4200], path[4200];
    snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", dir, name);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0)
        return EFS_ERR_IO;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            close(fd);
            unlink(tmp);
            return EFS_ERR_IO;
        }
        off += (size_t)w;
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return EFS_ERR_IO;
    }
    close(fd);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return EFS_ERR_IO;
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0)
        return EFS_ERR_IO;
    int rc = fsync(dfd) == 0 ? EFS_OK : EFS_ERR_IO;
    close(dfd);
    return rc;
}

/* 64 random bits from the kernel, never 0. */
static int mint_token(uint64_t *tok)
{
    uint8_t b[8];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return EFS_ERR_IO;
    do {
        size_t n = 0;
        while (n < sizeof(b)) {
            ssize_t r = read(fd, b + n, sizeof(b) - n);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0) {
                close(fd);
                return EFS_ERR_IO;
            }
            n += (size_t)r;
        }
        *tok = get64(b);
    } while (!*tok);
    close(fd);
    return EFS_OK;
}

static int path_exists(const char *dir, const char *name)
{
    char p[4200];
    struct stat st;
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    return lstat(p, &st) == 0;
}

#if EFS_FAULTS
void (*member_store_fault_exit)(const char *point);
#endif

void member_store_close(struct member_store *ms)
{
    if (ms && ms->lockfd >= 0) {
        close(ms->lockfd); /* releases the flock */
        ms->lockfd = -1;
    }
}

int member_store_open(struct member_store *ms, const char *dir, int seed, const char *addr,
                      uint16_t port, char *err, size_t errlen)
{
    char p[4200];
    uint8_t buf[EFS_IDENTITY_FILE_LEN + 1];
    size_t got = 0;
    int rc;

    memset(ms, 0, sizeof(*ms));
    ms->lockfd = -1;
    if (!dir || strlen(dir) >= sizeof(ms->dir) - 32) {
        ms_err(err, errlen, "store path too long");
        return EFS_ERR_INVAL;
    }
    snprintf(ms->dir, sizeof(ms->dir), "%s", dir);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        ms_err(err, errlen, "mkdir %s: %s", dir, strerror(errno));
        return EFS_ERR_IO;
    }
    snprintf(p, sizeof(p), "%s/owner.lock", dir);
    ms->lockfd = open(p, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (ms->lockfd < 0) {
        ms_err(err, errlen, "%s: %s", p, strerror(errno));
        return EFS_ERR_IO;
    }
    if (flock(ms->lockfd, LOCK_EX | LOCK_NB) != 0) {
        int e = errno;
        member_store_close(ms);
        if (e == EWOULDBLOCK) {
            ms_err(err, errlen, "store %s is owned by another running process", dir);
            return EFS_ERR_BUSY;
        }
        ms_err(err, errlen, "lock %s: %s", p, strerror(e));
        return EFS_ERR_IO;
    }
    /* Which stores start, which are refused (K33; identity.bin absent):
     *   nothing          initialize (below).
     *   boot.bin at 0    initialization was interrupted between boot.bin
     *                    and identity.bin: initialize again (a new token).
     *                    Counter 0 means member_store_boot never ran, so
     *                    open never returned and the token minted then
     *                    never left the process: no seed, joiner or record
     *                    can know this store.
     *   boot.bin above 0, unreadable, or Raft/KV data
     *                    refused: the store started before and lost its
     *                    identity (or is another build's).
     * identity.bin present: every state is opened as recorded; what the
     * cluster thinks of it is settled by the HELLO, not here:
     *   joining   retries the join with its token (the seed's record
     *             names it if the addition committed, else it is added
     *             now at count + 1);
     *   seed, promised, formed  start as that node id at that endpoint;
     *             a cluster whose record holds this id or endpoint under
     *             another token refuses it (wiped or swapped store). */
    snprintf(p, sizeof(p), "%s/identity.bin", dir);
    rc = read_small(p, buf, EFS_IDENTITY_FILE_LEN, &got);
    if (rc == EFS_ERR_NOT_FOUND) {
        if (path_exists(dir, "boot.bin")) {
            uint8_t b[EFS_BOOT_FILE_LEN + 1];
            uint64_t counter = 1;
            size_t bgot = 0;
            snprintf(p, sizeof(p), "%s/boot.bin", dir);
            if (read_small(p, b, EFS_BOOT_FILE_LEN, &bgot) != EFS_OK ||
                member_store_boot_decode(b, bgot, &counter) != EFS_OK || counter) {
                ms_err(err, errlen, "store %s has a boot counter but no member identity (it "
                       "started before and lost identity.bin, or its boot counter is "
                       "unreadable); format it fresh", dir);
                member_store_close(ms);
                return EFS_ERR_UNSUPPORTED;
            }
        }
        if (path_exists(dir, "kv") || path_exists(dir, "log")) {
            ms_err(err, errlen, "store %s holds Raft/KV data without a member identity "
                   "(another build's format); format it fresh", dir);
            member_store_close(ms);
            return EFS_ERR_UNSUPPORTED;
        }
        if (seed && (!addr || !*addr || strlen(addr) > EFS_MEMBER_ADDR_MAX || !port)) {
            ms_err(err, errlen, "store %s: a new cluster needs this node's ADDR:PORT", dir);
            member_store_close(ms);
            return EFS_ERR_INVAL;
        }
        uint8_t b[EFS_BOOT_FILE_LEN], id[EFS_IDENTITY_FILE_LEN];
        member_store_boot_encode(0, b);
        ms->state = seed ? EFS_MEMBER_SEED : EFS_MEMBER_JOINING;
        if (mint_token(&ms->token) != EFS_OK || (seed && mint_token(&ms->cluster) != EFS_OK)) {
            ms_err(err, errlen, "store %s: cannot read /dev/urandom for its token", dir);
            member_store_close(ms);
            return EFS_ERR_IO;
        }
        if (seed) {
            ms->member_id = 1;
            snprintf(ms->addr, sizeof(ms->addr), "%s", addr);
            ms->port = port;
        }
        rc = member_store_identity_encode(ms, id) == EFS_OK &&
             write_replace(dir, "boot.bin", b, sizeof(b)) == EFS_OK;
#if EFS_FAULTS
        /* K33 test hook: boot.bin at 0 written, identity.bin not yet. */
        if (rc && member_store_fault_exit)
            member_store_fault_exit("before-identity");
#endif
        if (!rc || write_replace(dir, "identity.bin", id, sizeof(id)) != EFS_OK) {
            ms_err(err, errlen, "store %s: cannot persist its initialization: %s", dir,
                   strerror(errno));
            member_store_close(ms);
            return EFS_ERR_IO;
        }
        ms->created = 1;
        return EFS_OK;
    }
    rc = member_store_identity_decode(buf, got, ms, err, errlen);
    if (rc != EFS_OK)
        member_store_close(ms);
    return rc;
}

int member_store_boot(struct member_store *ms, uint64_t fresh, uint64_t *incarnation,
                      char *err, size_t errlen)
{
    char p[4200];
    uint8_t buf[EFS_BOOT_FILE_LEN + 1];
    size_t got = 0;
    uint64_t saved = 0, next;

    snprintf(p, sizeof(p), "%s/boot.bin", ms->dir);
    int rc = read_small(p, buf, EFS_BOOT_FILE_LEN, &got);
    if (rc == EFS_ERR_NOT_FOUND) {
        ms_err(err, errlen, "store %s: boot counter is missing", ms->dir);
        return EFS_ERR_NOT_FOUND;
    }
    if (rc != EFS_OK) {
        ms_err(err, errlen, "store %s: boot counter unreadable: %s", ms->dir, strerror(errno));
        return rc;
    }
    rc = member_store_boot_decode(buf, got, &saved);
    if (rc != EFS_OK) {
        ms_err(err, errlen, "store %s: boot counter is %s", ms->dir,
               rc == EFS_ERR_DECODE ? "short or long" :
               rc == EFS_ERR_UNSUPPORTED ? "of another encoding or version" : "corrupt");
        return rc;
    }
    if (saved == UINT64_MAX) {
        ms_err(err, errlen, "store %s: boot counter exhausted", ms->dir);
        return EFS_ERR_OVERFLOW;
    }
    next = fresh > saved ? fresh : saved + 1;
    member_store_boot_encode(next, buf);
    if (write_replace(ms->dir, "boot.bin", buf, EFS_BOOT_FILE_LEN) != EFS_OK) {
        ms_err(err, errlen, "store %s: cannot persist the boot counter: %s", ms->dir,
               strerror(errno));
        return EFS_ERR_IO;
    }
    *incarnation = next;
    return EFS_OK;
}

/* Write `next` as this store's identity, then adopt it. */
static int ms_persist(struct member_store *ms, const struct member_store *next, char *err,
                      size_t errlen)
{
    uint8_t id[EFS_IDENTITY_FILE_LEN];
    if (member_store_identity_encode(next, id) != EFS_OK ||
        write_replace(ms->dir, "identity.bin", id, sizeof(id)) != EFS_OK) {
        ms_err(err, errlen, "store %s: cannot persist identity: %s", ms->dir, strerror(errno));
        return EFS_ERR_IO;
    }
    int lockfd = ms->lockfd, created = ms->created;
    *ms = *next;
    ms->lockfd = lockfd;
    ms->created = created;
    return EFS_OK;
}

/* The record's entry for this store (by token, then checked against the
 * node id and endpoint it already has), or NULL with err set. */
static const struct efs_member *ms_self(const struct member_store *ms,
                                        const struct efs_membership *m, char *err,
                                        size_t errlen)
{
    const struct efs_member *self = NULL;
    for (uint32_t i = 0; i < m->count; i++)
        if (m->m[i].token == ms->token)
            self = &m->m[i];
    if (ms->state != EFS_MEMBER_JOINING) {
        const struct efs_member *at = efs_membership_find(m, ms->member_id);
        if (!self && at) {
            ms_err(err, errlen, "store %s: the committed member %u holds store token %016llx, "
                   "this store is %016llx (the store was replaced; a member's store cannot be "
                   "swapped)", ms->dir, ms->member_id, (unsigned long long)at->token,
                   (unsigned long long)ms->token);
            return NULL;
        }
        if (self && (self->id != ms->member_id || self->port != ms->port ||
                     strcmp(self->addr, ms->addr))) {
            ms_err(err, errlen, "store %s: the record holds this store as node %u at %s:%u; "
                   "it is node %u at %s:%u", ms->dir, self->id, self->addr, self->port,
                   ms->member_id, ms->addr, ms->port);
            return NULL;
        }
    }
    if (!self)
        ms_err(err, errlen, "store %s: node %u (store %016llx) is not in the membership of %u "
               "members", ms->dir, ms->member_id, (unsigned long long)ms->token, m->count);
    return self;
}

int member_store_promise(struct member_store *ms, const struct efs_membership *proposal,
                         char *err, size_t errlen)
{
    const struct efs_member *self;
    if (!efs_membership_valid(proposal) || proposal->count != proposal->formed) {
        ms_err(err, errlen, "store %s: invalid formation proposal", ms->dir);
        return EFS_ERR_INVAL;
    }
    if (ms->state == EFS_MEMBER_PROMISED || ms->state == EFS_MEMBER_FORMED) {
        if (ms->state == EFS_MEMBER_PROMISED ? efs_membership_equal(&ms->members, proposal)
                                            : efs_membership_extends(&ms->members, proposal))
            return EFS_OK;
        ms_err(err, errlen, "store %s: it promised (or formed) %u members of cluster %016llx; "
               "a proposal of %u members of cluster %016llx is refused", ms->dir,
               ms->members.formed, (unsigned long long)ms->cluster, proposal->count,
               (unsigned long long)proposal->cluster);
        return EFS_ERR_FENCED;
    }
    if (ms->state == EFS_MEMBER_SEED && proposal->cluster != ms->cluster) {
        ms_err(err, errlen, "store %s: the seed of cluster %016llx cannot promise cluster "
               "%016llx", ms->dir, (unsigned long long)ms->cluster,
               (unsigned long long)proposal->cluster);
        return EFS_ERR_FENCED;
    }
    self = ms_self(ms, proposal, err, errlen);
    if (!self)
        return EFS_ERR_FENCED;
    struct member_store next = *ms;
    next.state = EFS_MEMBER_PROMISED;
    next.member_id = self->id;
    next.cluster = proposal->cluster;
    next.port = self->port;
    snprintf(next.addr, sizeof(next.addr), "%s", self->addr);
    next.members = *proposal;
    return ms_persist(ms, &next, err, errlen);
}

int member_store_learn(struct member_store *ms, const struct efs_membership *m,
                       char *err, size_t errlen)
{
    if (!efs_membership_valid(m)) {
        ms_err(err, errlen, "store %s: invalid membership record", ms->dir);
        return EFS_ERR_INVAL;
    }
    if (ms->state != EFS_MEMBER_JOINING && m->cluster != ms->cluster) {
        ms_err(err, errlen, "store %s belongs to cluster %016llx; a record of cluster %016llx "
               "is refused", ms->dir, (unsigned long long)ms->cluster,
               (unsigned long long)m->cluster);
        return EFS_ERR_FENCED;
    }
    const struct efs_member *self = ms_self(ms, m, err, errlen);
    if (!self)
        return EFS_ERR_FENCED;
    if (ms->state == EFS_MEMBER_FORMED || ms->state == EFS_MEMBER_PROMISED) {
        if (ms->state == EFS_MEMBER_FORMED && efs_membership_extends(&ms->members, m))
            return EFS_OK; /* the same or an earlier revision */
        if (!efs_membership_extends(m, &ms->members)) {
            ms_err(err, errlen, "store %s: committed membership of %u members (cluster %016llx) "
                   "conflicts with the persisted one of %u members (cluster %016llx)", ms->dir,
                   m->count, (unsigned long long)m->cluster, ms->members.count,
                   (unsigned long long)ms->members.cluster);
            return EFS_ERR_FENCED;
        }
    }
    struct member_store next = *ms;
    next.state = EFS_MEMBER_FORMED;
    next.member_id = self->id;
    next.cluster = m->cluster;
    next.port = self->port;
    snprintf(next.addr, sizeof(next.addr), "%s", self->addr);
    next.members = *m;
    return ms_persist(ms, &next, err, errlen);
}
