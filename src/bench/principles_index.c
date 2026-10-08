/* W95 experimental immutable packed bodies + atomic manifest publication.
 * This models a protocol, not the production storage engine. */
#include "principles_index.h"
#include "efs/checksum.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#define PI_HEADER 48u
#define PI_ENTRY 80u
#define PI_MAX_CAPACITY 65536u
#define PI_MAX_PAYLOAD 1048576u
struct pi_entry {
    uint64_t gen, segment, offset;
    uint32_t len, state;
    uint8_t digest[32];
};
struct principles_index {
    int dir, lock, readonly, failed;
    uint32_t capacity;
    uint64_t quota, sequence, next, syncs, block, body_written, index_written;
    uint8_t uuid[16];
    struct pi_entry *entries;
    principles_index_hook hook;
    void *hook_ctx;
};
static void putbe(uint8_t *p, uint64_t v, unsigned n)
{
    while (n) {
        p[--n] = (uint8_t)v;
        v >>= 8;
    }
}
static uint64_t getbe(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    while (n--)
        v = (v << 8) | *p++;
    return v;
}
static int io_all(int fd, void *buf, size_t n, off_t off, int reading)
{
    size_t done = 0;
    while (done < n) {
        ssize_t k = reading ? pread(fd, (uint8_t *)buf + done, n - done, off + (off_t)done)
                            : pwrite(fd, (uint8_t *)buf + done, n - done, off + (off_t)done);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0) {
            if (!k)
                errno = EIO;
            return -1;
        }
        done += (size_t)k;
    }
    return 0;
}
static int sync_file(int fd)
{
    int rc;
    do {
        rc = fsync(fd);
    } while (rc && errno == EINTR);
    return rc;
}
static int sync_model(struct principles_index *m, int fd)
{
    int rc = sync_file(fd);
    if (!rc)
        m->syncs++;
    return rc;
}
static void hook(struct principles_index *m, enum principles_index_stage stage)
{
    if (m->hook)
        m->hook(m->hook_ctx, stage);
}
static int segname(char name[32], uint64_t id)
{
    return snprintf(name, 32, "seg-%016llx", (unsigned long long)id) == 20 ? 0 : -1;
}
static int segment_id(const char *name, uint64_t *id)
{
    if (strlen(name) != 20 || strncmp(name, "seg-", 4))
        return 0;
    for (unsigned i = 4; i < 20; i++)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f')))
            return 0;
    char *end;
    errno = 0;
    *id = strtoull(name + 4, &end, 16);
    return !errno && !*end && *id;
}
/* readdir's EOF and failure both return NULL. Never admit or collect on an
 * incomplete inventory. Preserve the failure across closedir. */
static struct dirent *scan_next(DIR *dir, int *error)
{
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry && errno)
        *error = errno;
    return entry;
}
static int present(struct principles_index *m, uint64_t id)
{
    for (uint32_t i = 0; i < m->capacity; i++)
        if (m->entries[i].state == 1 && m->entries[i].segment == id)
            return 1;
    return 0;
}
static int snapshot(struct principles_index *m, const struct pi_entry *entries, uint64_t seq,
                    uint64_t next, uint8_t **out, size_t *len)
{
    *len = PI_HEADER + (size_t)m->capacity * PI_ENTRY + 32;
    uint8_t *b = calloc(1, *len);
    if (!b)
        return -1;
    memcpy(b, "EFSIDX01", 8);
    putbe(b + 8, m->capacity, 4);
    memcpy(b + 16, m->uuid, 16);
    putbe(b + 32, seq, 8);
    putbe(b + 40, next, 8);
    for (uint32_t i = 0; i < m->capacity; i++) {
        const struct pi_entry *e = &entries[i];
        uint8_t *p = b + PI_HEADER + (size_t)i * PI_ENTRY;
        putbe(p, e->gen, 8);
        putbe(p + 8, e->segment, 8);
        putbe(p + 16, e->offset, 8);
        putbe(p + 24, e->len, 4);
        putbe(p + 28, e->state, 4);
        memcpy(p + 32, e->digest, 32);
    }
    efs_hash(b, *len - 32, b + *len - 32);
    *out = b;
    return 0;
}
static int read_entry(struct principles_index *m, uint32_t id, void *out)
{
    struct pi_entry *e = &m->entries[id];
    char name[32];
    segname(name, e->segment);
    int fd = openat(m->dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    uint8_t head[24], *record = NULL;
    int rc = -1;
    size_t n = (size_t)e->len + 52;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || e->offset < 24 ||
        e->offset > (uint64_t)st.st_size || n > (uint64_t)st.st_size - e->offset)
        goto done;
    if (io_all(fd, head, sizeof(head), 0, 1) || memcmp(head, "EFSSEG01", 8) ||
        memcmp(head + 8, m->uuid, 16))
        goto done;
    record = malloc(n);
    if (!record)
        goto done;
    if (io_all(fd, record, n, (off_t)e->offset, 1))
        goto done;
    if (memcmp(record, "IP01", 4) || getbe(record + 4, 4) != id || getbe(record + 8, 8) != e->gen ||
        getbe(record + 16, 4) != e->len)
        goto done;
    uint8_t digest[32];
    efs_hash(record, n - 32, digest);
    if (memcmp(digest, e->digest, 32) || memcmp(digest, record + n - 32, 32))
        goto done;
    if (out)
        memcpy(out, record + 20, e->len);
    rc = 0;
done: {
    int saved = rc ? EIO : errno;
    free(record);
    close(fd);
    errno = saved;
}
    return rc;
}
int principles_index_stats(struct principles_index *m, struct principles_index_stats *out)
{
    if (!m || !out) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->syncs = m->syncs;
    out->body_written_bytes = m->body_written;
    out->index_written_bytes = m->index_written;
    uint64_t live_records = 0, live_headers = 0;
    for (uint32_t i = 0; i < m->capacity; i++)
        if (m->entries[i].state == 1) {
            out->live_bytes += m->entries[i].len;
            live_records += 52 + m->entries[i].len;
        }
    int scan = openat(m->dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (scan < 0)
        return -1;
    DIR *d = fdopendir(scan);
    if (!d) {
        close(scan);
        return -1;
    }
    struct dirent *de;
    int rc = 0;
    int scan_error = 0;
    while ((de = scan_next(d, &scan_error))) {
        uint64_t id = 0;
        int seg = segment_id(de->d_name, &id);
        if (!seg && strcmp(de->d_name, ".index-manifest") && strcmp(de->d_name, ".index-next"))
            continue;
        struct stat st;
        if (fstatat(m->dir, de->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) {
            rc = -1;
            errno = EIO;
            break;
        }
        out->allocated_bytes += (uint64_t)st.st_blocks * 512;
        if (seg) {
            out->segment_bytes += (uint64_t)st.st_size;
            if (present(m, id))
                live_headers += 24;
        } else
            out->index_bytes += (uint64_t)st.st_size;
    }
    if (!rc && scan_error) {
        errno = scan_error;
        rc = -1;
    }
    if (!rc) {
        if (out->segment_bytes < live_records + live_headers) {
            errno = EIO;
            rc = -1;
        } else
            out->garbage_bytes = out->segment_bytes - live_records - live_headers;
    }
    int saved = errno;
    closedir(d);
    errno = saved;
    return rc;
}
static int publish_manifest(struct principles_index *m, const struct pi_entry *entries,
                            uint64_t seq, uint64_t next)
{
    uint8_t *b = NULL;
    size_t n = 0;
    if (snapshot(m, entries, seq, next, &b, &n))
        return -1;
    int fd =
        openat(m->dir, ".index-next", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        free(b);
        return -1;
    }
    int rc = io_all(fd, b, n, 0, 0);
    if (!rc) {
        m->index_written += n;
        hook(m, PI_INDEX_WRITE);
    }
    if (!rc)
        rc = sync_model(m, fd);
    if (!rc)
        hook(m, PI_INDEX_SYNC);
    int saved = errno;
    if (close(fd) && !rc) {
        rc = -1;
        saved = errno;
    }
    free(b);
    errno = saved;
    if (rc)
        return -1;
    struct principles_index_stats allocation;
    if (principles_index_stats(m, &allocation))
        return -1;
    if (allocation.allocated_bytes > m->quota) {
        errno = ENOSPC;
        return -1;
    }
    if (renameat(m->dir, ".index-next", m->dir, ".index-manifest"))
        return -1;
    hook(m, PI_RENAME);
    if (sync_model(m, m->dir))
        return -1;
    hook(m, PI_PUBLICATION);
    return 0;
}
struct principles_index *principles_index_open(const char *root, uint32_t cap, uint64_t quota,
                                               int create, int readonly)
{
    if (!root || !cap || cap > PI_MAX_CAPACITY || !quota || (create && readonly)) {
        errno = EINVAL;
        return NULL;
    }
    uint64_t initial = PI_HEADER + (uint64_t)cap * PI_ENTRY + 32;
    if (quota < ((initial + 4095) / 4096) * 4096) {
        errno = ENOSPC;
        return NULL;
    }
    struct principles_index *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->dir = m->lock = -1;
    m->capacity = cap;
    m->quota = quota;
    m->readonly = readonly;
    m->next = 1;
    m->entries = calloc(cap, sizeof(*m->entries));
    if (!m->entries)
        goto bad;
    m->dir = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (m->dir < 0)
        goto bad;
    struct statvfs geometry;
    if (fstatvfs(m->dir, &geometry))
        goto bad;
    m->block = geometry.f_frsize > 4096 ? geometry.f_frsize : 4096;
    if (m->block > (1u << 20)) {
        errno = EINVAL;
        goto bad;
    }
    if (quota < ((initial + m->block - 1) / m->block) * m->block) {
        errno = ENOSPC;
        goto bad;
    }
    if (create) {
        int scan = openat(m->dir, ".", O_RDONLY | O_DIRECTORY);
        DIR *d = scan < 0 ? NULL : fdopendir(scan);
        if (!d) {
            if (scan >= 0)
                close(scan);
            goto bad;
        }
        struct dirent *de;
        int empty = 1, scan_error = 0;
        while ((de = scan_next(d, &scan_error)))
            if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
                empty = 0;
        closedir(d);
        if (scan_error) {
            errno = scan_error;
            goto bad;
        }
        if (!empty) {
            errno = EEXIST;
            goto bad;
        }
    }
    m->lock = openat(m->dir, ".index-lock",
                     (readonly ? O_RDONLY : O_RDWR) | (create ? O_CREAT | O_EXCL : 0) | O_NOFOLLOW |
                         O_CLOEXEC,
                     0600);
    if (m->lock < 0 || flock(m->lock, (readonly ? LOCK_SH : LOCK_EX) | LOCK_NB))
        goto bad;
    if (create) {
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            goto bad;
        ssize_t got = read(fd, m->uuid, 16);
        close(fd);
        if (got != 16) {
            errno = EIO;
            goto bad;
        }
        if (publish_manifest(m, m->entries, 0, 1))
            goto bad;
        return m;
    }
    int fd = openat(m->dir, ".index-manifest", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        goto bad;
    size_t n = PI_HEADER + (size_t)cap * PI_ENTRY + 32;
    uint8_t *b = malloc(n);
    struct stat st;
    int rc = b ? fstat(fd, &st) : -1;
    if (!rc && (!S_ISREG(st.st_mode) || st.st_size != (off_t)n)) {
        errno = EIO;
        rc = -1;
    }
    if (!rc)
        rc = io_all(fd, b, n, 0, 1);
    int saved = errno;
    close(fd);
    errno = saved;
    if (rc) {
        free(b);
        goto bad;
    }
    uint8_t digest[32];
    efs_hash(b, n - 32, digest);
    if (memcmp(b, "EFSIDX01", 8) || getbe(b + 8, 4) != cap || getbe(b + 12, 4) ||
        memcmp(digest, b + n - 32, 32)) {
        free(b);
        errno = EIO;
        goto bad;
    }
    memcpy(m->uuid, b + 16, 16);
    m->sequence = getbe(b + 32, 8);
    m->next = getbe(b + 40, 8);
    if (!m->next) {
        free(b);
        errno = EIO;
        goto bad;
    }
    for (uint32_t i = 0; i < cap; i++) {
        uint8_t *p = b + PI_HEADER + (size_t)i * PI_ENTRY;
        struct pi_entry *e = &m->entries[i];
        e->gen = getbe(p, 8);
        e->segment = getbe(p + 8, 8);
        e->offset = getbe(p + 16, 8);
        e->len = (uint32_t)getbe(p + 24, 4);
        e->state = (uint32_t)getbe(p + 28, 4);
        memcpy(e->digest, p + 32, 32);
        uint8_t zero[16] = {0};
        int invalid = memcmp(p + 64, zero, 16) != 0 || e->state > 2;
        if (!e->state)
            invalid |= e->gen || e->segment || e->offset || e->len ||
                       memcmp(e->digest, (uint8_t[32]){0}, 32);
        else if (e->state == 2)
            invalid |= !e->gen || e->segment || e->offset || e->len ||
                       memcmp(e->digest, (uint8_t[32]){0}, 32);
        else
            invalid |= !e->gen || !e->segment || e->segment >= m->next || !e->len ||
                       e->len > PI_MAX_PAYLOAD;
        if (invalid) {
            free(b);
            errno = EIO;
            goto bad;
        }
    }
    free(b);
    for (uint32_t i = 0; i < cap; i++)
        if (m->entries[i].state == 1 && read_entry(m, i, NULL))
            goto bad;
    /* Unpublished segment names are never reused after a cold reopen. */
    int scan = openat(m->dir, ".", O_RDONLY | O_DIRECTORY);
    DIR *d = scan < 0 ? NULL : fdopendir(scan);
    if (!d) {
        if (scan >= 0)
            close(scan);
        goto bad;
    }
    struct dirent *de;
    int scan_error = 0;
    while ((de = scan_next(d, &scan_error))) {
        uint64_t id;
        if (segment_id(de->d_name, &id) && id >= m->next) {
            if (id == UINT64_MAX) {
                closedir(d);
                errno = EOVERFLOW;
                goto bad;
            }
            m->next = id + 1;
        }
    }
    closedir(d);
    if (scan_error) {
        errno = scan_error;
        goto bad;
    }
    if (!readonly) {
        /* A process crash can expose a valid rename that had not reached its
         * directory barrier. Stabilize the recovered authority before GC can
         * delete bodies referenced by the previous durable manifest. */
        int stable = openat(m->dir, ".index-manifest", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (stable < 0)
            goto bad;
        int err = sync_model(m, stable), saved = errno;
        if (close(stable) && !err) {
            err = -1;
            saved = errno;
        }
        errno = saved;
        if (err || sync_model(m, m->dir))
            goto bad;
    }
    return m;
bad: {
    int saved = errno;
    principles_index_close(m);
    errno = saved;
}
    return NULL;
}
void principles_index_set_hook(struct principles_index *m, principles_index_hook fn, void *ctx)
{
    if (m) {
        m->hook = fn;
        m->hook_ctx = ctx;
    }
}
int principles_index_get(struct principles_index *m, uint32_t id, void *out, uint32_t *len,
                         uint64_t *gen)
{
    if (!m || id >= m->capacity || !len || !gen || m->failed) {
        errno = EINVAL;
        return -1;
    }
    struct pi_entry *e = &m->entries[id];
    *gen = e->gen;
    if (e->state != 1) {
        errno = ENOENT;
        return -1;
    }
    if (*len < e->len || !out) {
        *len = e->len;
        errno = ENOBUFS;
        return -1;
    }
    if (read_entry(m, id, out))
        return -1;
    *len = e->len;
    return 0;
}
int principles_index_publish(struct principles_index *m, const struct principles_change *changes,
                             uint32_t count)
{
    if (!m || !changes || !count || count > m->capacity || m->readonly || m->failed) {
        errno = EINVAL;
        return -1;
    }
    if (m->sequence == UINT64_MAX || m->next == UINT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    uint64_t body = 24;
    unsigned puts = 0;
    for (uint32_t i = 0; i < count; i++) {
        const struct principles_change *c = &changes[i];
        if (c->object >= m->capacity || c->len > PI_MAX_PAYLOAD || !!c->data != !!c->len) {
            errno = EINVAL;
            return -1;
        }
        for (uint32_t j = 0; j < i; j++)
            if (changes[j].object == c->object) {
                errno = EINVAL;
                return -1;
            }
        if (m->entries[c->object].gen != c->expected_generation) {
            errno = ESTALE;
            return -1;
        }
        if (c->expected_generation == UINT64_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
        if (!c->data && m->entries[c->object].state != 1) {
            errno = ENOENT;
            return -1;
        }
        if (c->data) {
            body += 52 + c->len;
            puts++;
        }
    }
    struct principles_index_stats st;
    if (principles_index_stats(m, &st))
        return -1;
    uint64_t index = PI_HEADER + (uint64_t)m->capacity * PI_ENTRY + 32;
    uint64_t extra = ((index + m->block - 1) / m->block) * m->block +
                     (puts ? ((body + m->block - 1) / m->block) * m->block : 0);
    if (st.allocated_bytes > m->quota || extra > m->quota - st.allocated_bytes) {
        errno = ENOSPC;
        return -1;
    }
    struct pi_entry *next = malloc((size_t)m->capacity * sizeof(*next));
    if (!next)
        return -1;
    memcpy(next, m->entries, (size_t)m->capacity * sizeof(*next));
    int fd = -1, rc = -1;
    uint64_t offset = 24;
    char name[32];
    segname(name, m->next);
    if (puts) {
        fd = openat(m->dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0)
            goto failed;
        uint8_t head[24];
        memcpy(head, "EFSSEG01", 8);
        memcpy(head + 8, m->uuid, 16);
        if (io_all(fd, head, 24, 0, 0))
            goto failed;
        m->body_written += 24;
    }
    for (uint32_t i = 0; i < count; i++) {
        const struct principles_change *c = &changes[i];
        struct pi_entry *e = &next[c->object];
        memset(e, 0, sizeof(*e));
        e->gen = c->expected_generation + 1;
        e->state = c->data ? 1 : 2;
        if (!c->data)
            continue;
        size_t n = (size_t)c->len + 52;
        uint8_t *b = malloc(n);
        if (!b)
            goto failed;
        memcpy(b, "IP01", 4);
        putbe(b + 4, c->object, 4);
        putbe(b + 8, e->gen, 8);
        putbe(b + 16, c->len, 4);
        memcpy(b + 20, c->data, c->len);
        efs_hash(b, n - 32, b + n - 32);
        e->segment = m->next;
        e->offset = offset;
        e->len = c->len;
        memcpy(e->digest, b + n - 32, 32);
        int err = io_all(fd, b, n, (off_t)offset, 0);
        free(b);
        if (err)
            goto failed;
        m->body_written += n;
        offset += n;
    }
    hook(m, PI_BODY_WRITE);
    if (fd >= 0 && sync_model(m, fd))
        goto failed;
    hook(m, PI_BODY_SYNC);
    if (fd >= 0) {
        if (close(fd)) {
            fd = -1;
            goto failed;
        }
        fd = -1;
    }
    if (sync_model(m, m->dir))
        goto failed;
    hook(m, PI_BODY_DIR);
    if (publish_manifest(m, next, m->sequence + 1, m->next + (puts ? 1 : 0)))
        goto failed;
    free(m->entries);
    m->entries = next;
    m->sequence++;
    if (puts)
        m->next++;
    return 0;
failed: {
    int saved = errno;
    if (fd >= 0)
        close(fd);
    free(next);
    m->failed = 1;
    errno = saved;
}
    return rc;
}
int principles_index_collect(struct principles_index *m, uint64_t *removed)
{
    if (!m || !removed || m->readonly || m->failed) {
        errno = EINVAL;
        return -1;
    }
    *removed = 0;
    int scan = openat(m->dir, ".", O_RDONLY | O_DIRECTORY);
    if (scan < 0)
        return -1;
    DIR *d = fdopendir(scan);
    if (!d) {
        close(scan);
        return -1;
    }
    struct dirent *de;
    int rc = 0;
    struct stat temp;
    if (!fstatat(m->dir, ".index-next", &temp, AT_SYMLINK_NOFOLLOW)) {
        uint8_t head[32];
        int fd = openat(m->dir, ".index-next", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        int bad = fd < 0 || !S_ISREG(temp.st_mode) || io_all(fd, head, sizeof(head), 0, 1) ||
                  memcmp(head, "EFSIDX01", 8) || memcmp(head + 16, m->uuid, 16);
        if (fd >= 0)
            close(fd);
        if (bad || unlinkat(m->dir, ".index-next", 0)) {
            errno = EIO;
            rc = -1;
        } else
            *removed += (uint64_t)temp.st_blocks * 512;
    } else if (errno != ENOENT)
        rc = -1;
    int scan_error = 0;
    while (!rc && (de = scan_next(d, &scan_error))) {
        uint64_t id;
        if (!segment_id(de->d_name, &id) || present(m, id))
            continue;
        int fd = openat(m->dir, de->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        struct stat st;
        uint8_t head[24];
        if (fd < 0) {
            rc = -1;
            break;
        }
        int bad = fstat(fd, &st) || !S_ISREG(st.st_mode) || io_all(fd, head, 24, 0, 1) ||
                  memcmp(head, "EFSSEG01", 8) || memcmp(head + 8, m->uuid, 16);
        close(fd);
        if (bad) {
            errno = EIO;
            rc = -1;
            break;
        }
        if (unlinkat(m->dir, de->d_name, 0)) {
            rc = -1;
            break;
        }
        *removed += (uint64_t)st.st_blocks * 512;
        hook(m, PI_GC);
    }
    if (!rc && scan_error) {
        errno = scan_error;
        rc = -1;
    }
    int saved = errno;
    closedir(d);
    if (!rc)
        rc = sync_model(m, m->dir);
    if (rc)
        m->failed = 1;
    else
        errno = saved;
    return rc;
}
void principles_index_close(struct principles_index *m)
{
    if (!m)
        return;
    if (m->lock >= 0)
        close(m->lock);
    if (m->dir >= 0)
        close(m->dir);
    free(m->entries);
    free(m);
}
