/* D39 C2/C3: store ownership, the durable boot counter and the persisted
 * member identity. See member_store.h. */
#include "member_store.h"
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
static const uint8_t ident_magic[8] = {'E', 'F', 'S', 'I', 'D', 'N', 'T', '1'};
#define BOOT_VERSION 1u
#define IDENT_VERSION 1u

static void ms_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || !errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) << 32 | get32(p + 4);
}
static uint64_t check64(const uint8_t *p, size_t n)
{
    uint8_t d[32];
    efs_sha256(p, n, d);
    return get64(d);
}

void member_store_boot_encode(uint64_t counter, uint8_t out[EFS_BOOT_FILE_LEN])
{
    memset(out, 0, EFS_BOOT_FILE_LEN);
    memcpy(out, boot_magic, 8);
    put32(out + 8, BOOT_VERSION);
    put64(out + 16, counter);
    put64(out + 24, check64(out, 24));
}

int member_store_boot_decode(const uint8_t *in, size_t len, uint64_t *counter)
{
    if (len != EFS_BOOT_FILE_LEN)
        return EFS_ERR_DECODE;
    if (memcmp(in, boot_magic, 8) || get32(in + 8) != BOOT_VERSION || get32(in + 12))
        return EFS_ERR_UNSUPPORTED;
    if (get64(in + 24) != check64(in, 24))
        return EFS_ERR_CHECKSUM;
    *counter = get64(in + 16);
    return EFS_OK;
}

const char *member_store_state_name(uint32_t state)
{
    return state == EFS_MEMBER_UNFORMED ? "unformed" : state == EFS_MEMBER_FORMING ? "forming"
           : state == EFS_MEMBER_FORMED ? "formed" : "invalid";
}

static void ident_encode(const struct member_store *ms, uint8_t out[EFS_IDENTITY_FILE_LEN])
{
    memset(out, 0, EFS_IDENTITY_FILE_LEN);
    memcpy(out, ident_magic, 8);
    put32(out + 8, IDENT_VERSION);
    put32(out + 12, ms->member_id);
    put32(out + 16, ms->state);
    put32(out + 20, ms->pair_known ? 1u : 0u);
    put64(out + 24, ms->pair_known ? ms->salt : 0);
    memcpy(out + 32, ms->digest, EFS_CLUSTER_DIGEST_LEN);
    put64(out + 64, check64(out, 64));
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

static int path_exists(const char *dir, const char *name)
{
    char p[4200];
    struct stat st;
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    return lstat(p, &st) == 0;
}

void member_store_close(struct member_store *ms)
{
    if (ms && ms->lockfd >= 0) {
        close(ms->lockfd); /* releases the flock */
        ms->lockfd = -1;
    }
}

int member_store_open(struct member_store *ms, const char *dir, uint32_t member_id,
                      const uint8_t digest[EFS_CLUSTER_DIGEST_LEN], char *err, size_t errlen)
{
    char p[4200], want[13], have[13];
    uint8_t buf[EFS_IDENTITY_FILE_LEN + 1];
    size_t got = 0;
    int rc;

    memset(ms, 0, sizeof(*ms));
    ms->lockfd = -1;
    if (!dir || strlen(dir) >= sizeof(ms->dir)) {
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
    snprintf(p, sizeof(p), "%s/identity.bin", dir);
    rc = read_small(p, buf, EFS_IDENTITY_FILE_LEN, &got);
    if (rc == EFS_ERR_NOT_FOUND) {
        if (path_exists(dir, "boot.bin")) {
            ms_err(err, errlen, "store %s has a boot counter but was never initialized "
                   "(interrupted initialization); format it fresh", dir);
            member_store_close(ms);
            return EFS_ERR_UNSUPPORTED;
        }
        if (path_exists(dir, "kv") || path_exists(dir, "log")) {
            ms_err(err, errlen, "store %s holds Raft/KV data without a member identity "
                   "(another build's format); format it fresh", dir);
            member_store_close(ms);
            return EFS_ERR_UNSUPPORTED;
        }
        uint8_t b[EFS_BOOT_FILE_LEN];
        member_store_boot_encode(0, b);
        ms->member_id = member_id;
        memcpy(ms->digest, digest, EFS_CLUSTER_DIGEST_LEN);
        ms->state = EFS_MEMBER_UNFORMED;
        uint8_t id[EFS_IDENTITY_FILE_LEN];
        ident_encode(ms, id);
        if (write_replace(dir, "boot.bin", b, sizeof(b)) != EFS_OK ||
            write_replace(dir, "identity.bin", id, sizeof(id)) != EFS_OK) {
            ms_err(err, errlen, "store %s: cannot persist its initialization: %s", dir,
                   strerror(errno));
            member_store_close(ms);
            return EFS_ERR_IO;
        }
        ms->created = 1;
        return EFS_OK;
    }
    if (rc != EFS_OK || got != EFS_IDENTITY_FILE_LEN || memcmp(buf, ident_magic, 8) ||
        get32(buf + 8) != IDENT_VERSION || get64(buf + 64) != check64(buf, 64) ||
        get32(buf + 16) > EFS_MEMBER_FORMED || get32(buf + 20) > 1) {
        ms_err(err, errlen, "store %s: identity.bin is unreadable, short, corrupt or of "
               "another version", dir);
        member_store_close(ms);
        return EFS_ERR_UNSUPPORTED;
    }
    ms->member_id = get32(buf + 12);
    ms->state = get32(buf + 16);
    ms->pair_known = (int)get32(buf + 20);
    ms->salt = get64(buf + 24);
    memcpy(ms->digest, buf + 32, EFS_CLUSTER_DIGEST_LEN);
    if ((ms->state == EFS_MEMBER_UNFORMED) == !!ms->pair_known) {
        ms_err(err, errlen, "store %s: identity state %s disagrees with its pair", dir,
               member_store_state_name(ms->state));
        member_store_close(ms);
        return EFS_ERR_UNSUPPORTED;
    }
    if (ms->member_id != member_id ||
        memcmp(ms->digest, digest, EFS_CLUSTER_DIGEST_LEN)) {
        efs_cluster_digest_short(digest, want);
        efs_cluster_digest_short(ms->digest, have);
        ms_err(err, errlen, "store %s belongs to member %u of configuration %s; this daemon "
               "is member %u of configuration %s; format it fresh or use the matching "
               "configuration", dir, ms->member_id, have, member_id, want);
        member_store_close(ms);
        return EFS_ERR_FENCED;
    }
    return EFS_OK;
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

static int ident_write(struct member_store *ms, char *err, size_t errlen)
{
    uint8_t id[EFS_IDENTITY_FILE_LEN];
    ident_encode(ms, id);
    if (write_replace(ms->dir, "identity.bin", id, sizeof(id)) != EFS_OK) {
        ms_err(err, errlen, "store %s: cannot persist identity: %s", ms->dir, strerror(errno));
        return EFS_ERR_IO;
    }
    return EFS_OK;
}

int member_store_learn_pair(struct member_store *ms, uint64_t salt, char *err, size_t errlen)
{
    if (ms->pair_known) {
        if (ms->salt == salt)
            return EFS_OK;
        ms_err(err, errlen, "store %s: pair salt %llu conflicts with the persisted pair "
               "salt %llu", ms->dir, (unsigned long long)salt, (unsigned long long)ms->salt);
        return EFS_ERR_FENCED;
    }
    struct member_store next = *ms;
    next.pair_known = 1;
    next.salt = salt;
    next.state = EFS_MEMBER_FORMING;
    int rc = ident_write(&next, err, errlen);
    if (rc == EFS_OK) {
        ms->pair_known = 1;
        ms->salt = salt;
        ms->state = EFS_MEMBER_FORMING;
    }
    return rc;
}

int member_store_mark_formed(struct member_store *ms, char *err, size_t errlen)
{
    if (ms->state == EFS_MEMBER_FORMED)
        return EFS_OK;
    if (!ms->pair_known) {
        ms_err(err, errlen, "store %s: formed without a pair", ms->dir);
        return EFS_ERR_INVAL;
    }
    struct member_store next = *ms;
    next.state = EFS_MEMBER_FORMED;
    int rc = ident_write(&next, err, errlen);
    if (rc == EFS_OK)
        ms->state = EFS_MEMBER_FORMED;
    return rc;
}
