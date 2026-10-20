/* W125 packed segment store on a storage node.
 *
 * A packed segment is one stripe of standard fragments owned by its
 * publication group, never by an inode, so its fragment files live in their
 * own tree, not under any inode directory (W124 inode reclaim never reaches
 * them):
 *   <root>/data/segments/g<group>/<id & 0xfff : 3 hex>/<id:016llx>.<gen:llx>.<fi>
 * The root is chosen by segment id, so a read needs no probing.
 *
 * File format: the fragment payload (geometry/2 bytes, a 4 KiB multiple)
 * followed by a 4 KiB tail: checksum:32, payload length:4, "EFSSEG1\0".
 * Written once with O_EXCL (O_DIRECT when the node uses direct I/O), then
 * fsynced with its directory before the PUT is acknowledged: the packer's
 * switch requires the copy durable on every member.
 *
 * A PUT is admitted only for an admitted, ACTIVE segment object whose
 * placement names this node for that fragment with exactly that checksum,
 * read linearizably from the group (server_raft_host_pack_object). The
 * segment's guard orders a PUT against reclaim acceptance: acceptance
 * installs the segment's fence under the write guard, and a PUT checks the
 * fence under the read guard, so no PUT lands after acceptance. */
#include "server_internal.h"
#include "efs/checksum.h"
#include "efs/pack_mapping.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SG_TAIL 4096u
#define SG_MAGIC "EFSSEG1"
#define SG_GUARDS 256u

static pthread_rwlock_t sg_guard[SG_GUARDS];
static pthread_once_t sg_guard_once = PTHREAD_ONCE_INIT;
static void sg_guard_init(void)
{
    for (unsigned i = 0; i < SG_GUARDS; i++) pthread_rwlock_init(&sg_guard[i], NULL);
}
pthread_rwlock_t *server_segment_guard(uint32_t group, uint64_t id)
{
    pthread_once(&sg_guard_once, sg_guard_init);
    return &sg_guard[(id * 0x9E3779B97F4A7C15ull ^ group) % SG_GUARDS];
}

static void sg_put32(uint8_t *p, uint32_t v) { for (int i = 3; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; } }
static uint32_t sg_get32(const uint8_t *p) { uint32_t v = 0; for (int i = 0; i < 4; i++) v = (v << 8) | p[i]; return v; }
static uint64_t sg_get64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v; }

/* Builds without the raft host (storage unit tests) have no authority:
 * every PUT is refused and no segment is ever acceptable for reclaim. A
 * test that includes this file may supply its own oracle instead. */
#ifndef EFS_SEGMENT_TEST_AUTHORITY
__attribute__((weak)) int server_raft_host_pack_object(uint32_t group, uint64_t id, uint64_t gen,
                                                       struct efs_pack_object *out)
{
    (void)group; (void)id; (void)gen; (void)out;
    return EFS_ERR_NOT_PRIMARY;
}
#endif

static uint32_t sg_root(const struct efsd_server *s, uint64_t id)
{
    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;
    return (uint32_t)(id % n);
}
static void sg_dir(const struct efsd_server *s, uint32_t group, uint64_t id, char *out, size_t n)
{
    snprintf(out, n, "%s/data/segments/g%u/%03llx", s->storage_paths[sg_root(s, id)], group,
             (unsigned long long)(id & 0xfffu));
}
void server_segment_path(const struct efsd_server *s, uint32_t group, uint64_t id, uint64_t gen,
                         uint32_t fi, char *out, size_t n)
{
    char dir[EFS_MAX_PATH + 64];
    sg_dir(s, group, id, dir, sizeof(dir));
    snprintf(out, n, "%s/%016llx.%llx.%u", dir, (unsigned long long)id, (unsigned long long)gen, fi);
}

static int sg_sync_path(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return EFS_ERR_IO;
    int rc = fsync(fd) ? EFS_ERR_IO : EFS_OK;
    close(fd);
    return rc;
}
/* mkdir -p for the segment directory; every created level's parent is
 * synced so the new name survives a crash. */
static int sg_mkdirs(const char *dir)
{
    char p[EFS_MAX_PATH + 64];
    size_t n = strlen(dir);
    if (n >= sizeof(p)) return EFS_ERR_INVAL;
    memcpy(p, dir, n + 1);
    for (char *c = p + 1; ; c++) {
        if (*c != '/' && *c) continue;
        char save = *c;
        *c = 0;
        if (mkdir(p, 0755) == 0) {
            char *slash = strrchr(p, '/');
            if (slash && slash != p) {
                *slash = 0;
                int rc = sg_sync_path(p);
                *slash = '/';
                if (rc != EFS_OK) return rc;
            }
        } else if (errno != EEXIST) {
            return EFS_ERR_IO;
        }
        *c = save;
        if (!save) break;
    }
    return EFS_OK;
}

static int sg_read_tail(int fd, uint32_t len, uint8_t sum[EFS_HASH_SIZE])
{
    uint8_t t[64];
    if (pread(fd, t, sizeof(t), len) != (ssize_t)sizeof(t)) return EFS_ERR_IO;
    if (memcmp(t + 36, SG_MAGIC, 8) || sg_get32(t + 32) != len) return EFS_ERR_CHECKSUM;
    memcpy(sum, t, EFS_HASH_SIZE);
    return EFS_OK;
}

/* Write and make durable one fragment. EEXIST with the same checksum (a
 * retried PUT) is success; a different file under the name is refused. */
static int sg_write(struct efsd_server *s, const char *path, const uint8_t *data, uint32_t len,
                     const uint8_t sum[EFS_HASH_SIZE], int *created)
{
    char dir[EFS_MAX_PATH + 64];
    *created = 0;
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return EFS_ERR_INVAL;
    *slash = 0;
    int rc = sg_mkdirs(dir);
    if (rc != EFS_OK) return rc;
    uint8_t *buf = NULL;
    size_t total = (size_t)len + SG_TAIL;
    if (posix_memalign((void **)&buf, 4096, total)) return EFS_ERR_NOMEM;
    memcpy(buf, data, len);
    memset(buf + len, 0, SG_TAIL);
    memcpy(buf + len, sum, EFS_HASH_SIZE);
    sg_put32(buf + len + 32, len);
    memcpy(buf + len + 36, SG_MAGIC, 8);
    int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
    if (s->direct_io) flags |= O_DIRECT;
    int fd = open(path, flags, 0644);
    if (fd < 0 && errno == EINVAL && (flags & O_DIRECT)) fd = open(path, flags & ~O_DIRECT, 0644);
    if (fd < 0) {
        free(buf);
        if (errno != EEXIST) return EFS_ERR_IO;
        uint8_t have[EFS_HASH_SIZE];
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return EFS_ERR_IO;
        rc = sg_read_tail(fd, len, have);
        close(fd);
        if (rc != EFS_OK) return rc;
        return memcmp(have, sum, EFS_HASH_SIZE) ? EFS_ERR_EXIST : EFS_OK;
    }
    *created = 1;
    size_t done = 0;
    while (done < total) {
        ssize_t w = pwrite(fd, buf + done, total - done, (off_t)done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { rc = EFS_ERR_IO; break; }
        done += (size_t)w;
    }
    if (rc == EFS_OK && fsync(fd)) rc = EFS_ERR_IO;
    close(fd);
    free(buf);
    if (rc == EFS_OK) rc = sg_sync_path(dir);
    if (rc != EFS_OK) unlink(path);
    return rc;
}

/* Segment reserve above the quota: 1/64 of it, at least 16 MiB (a pass of
 * concurrent segment jobs) and at most 256 MiB. */
static uint64_t sg_quota_reserve(uint64_t quota)
{
    uint64_t r = quota / 64;
    if (r < (16ull << 20)) r = 16ull << 20;
    if (r > (256ull << 20)) r = 256ull << 20;
    return r;
}
int server_segment_put(struct efsd_server *s, const uint8_t *body, uint32_t bytes)
{
    if (!s || !body || bytes < EFS_SEGMENT_PUT_HEADER) return EFS_ERR_INVAL;
    uint32_t group = body[0], fi = body[1], geometry = sg_get32(body + 4);
    uint64_t id = sg_get64(body + 8), gen = sg_get64(body + 16);
    const uint8_t *sum = body + 24, *data = body + EFS_SEGMENT_PUT_HEADER;
    uint32_t len = geometry / 2;
    if (!efs_pack_group_anchor(group) || fi >= EFS_NUM_FRAGMENTS || !id || !gen ||
        geometry < EFS_SEGMENT_ALIGNMENT || geometry > EFS_SEGMENT_MAX_SIZE ||
        geometry % EFS_SEGMENT_ALIGNMENT || bytes - EFS_SEGMENT_PUT_HEADER != len)
        return EFS_ERR_INVAL;
    uint8_t got[EFS_HASH_SIZE];
    efs_hash(data, len, got);
    if (memcmp(got, sum, EFS_HASH_SIZE)) return EFS_ERR_CHECKSUM;
    /* Linearizable authority: the object must be admitted, still ACTIVE,
     * with records left to switch, and place this exact fragment here. */
    struct efs_pack_object *o = malloc(sizeof(*o));
    if (!o) return EFS_ERR_NOMEM;
    int rc = server_raft_host_pack_object(group, id, gen, o);
    if (rc == EFS_ERR_NOT_FOUND) rc = EFS_ERR_STALE;
    if (rc == EFS_OK) {
        struct efs_pack_ledger_usage u;
        efs_pack_ledger_usage(&o->ledger, &u);
        if (o->ledger.phase != EFS_PACK_ACTIVE || !u.pending) rc = EFS_ERR_STALE;
        else if (o->ledger.geometry != geometry || o->nodes[fi] != s->id ||
                 memcmp(o->checksums[fi], sum, EFS_HASH_SIZE)) rc = EFS_ERR_INVAL;
    }
    free(o);
    if (rc != EFS_OK) return rc;
    char path[EFS_MAX_PATH + 128];
    server_segment_path(s, group, id, gen, fi, path, sizeof(path));
    pthread_rwlock_t *g = server_segment_guard(group, id);
    pthread_rwlock_rdlock(g);
    if (server_reclaim_fenced(EFS_RECLAIM_SEGMENT_EID(group), id)) {
        pthread_rwlock_unlock(g);
        return EFS_ERR_STALE;
    }
    /* Charge the payload like a fragment before writing it. A segment may
     * use a reserve above the quota: it replaces up to 512 per-file
     * fragments, so packing frees far more than it borrows, but at a full
     * quota it could otherwise never write the segment that frees space
     * (2026-10-20: unpacked files filled the quota, every PUT_SEGMENT was
     * refused and the node stayed full). Client fragments keep the quota. */
    pthread_mutex_lock(&s->lock);
    struct efs_node *ln = server_local_node(s);
    uint64_t limit = s->quota ? s->quota + sg_quota_reserve(s->quota) : 0;
    if (!ln || len > UINT64_MAX - ln->used ||
        (limit && (ln->used > limit || len > limit - ln->used)) ||
        server_phys_admit_locked(s, path, len)) {
        pthread_mutex_unlock(&s->lock);
        pthread_rwlock_unlock(g);
        return EFS_ERR_QUOTA;
    }
    ln->used += len;
    pthread_mutex_unlock(&s->lock);
    int created = 0;
    rc = sg_write(s, path, data, len, sum, &created);
    if (rc != EFS_OK || !created) {
        pthread_mutex_lock(&s->lock);
        ln = server_local_node(s);
        if (ln && ln->used >= len) ln->used -= len;
        pthread_mutex_unlock(&s->lock);
    }
    server_usage_mark_dirty(s);
    pthread_rwlock_unlock(g);
    return rc;
}

int server_segment_read(struct efsd_server *s, uint32_t group, uint64_t id, uint64_t gen,
                        uint32_t fi, uint32_t len, uint8_t *out, uint8_t sum[EFS_HASH_SIZE])
{
    char path[EFS_MAX_PATH + 128];
    server_segment_path(s, group, id, gen, fi, path, sizeof(path));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? EFS_ERR_NOT_FOUND : EFS_ERR_IO;
    int rc = sg_read_tail(fd, len, sum);
    if (rc == EFS_OK) {
        size_t done = 0;
        while (done < len) {
            ssize_t r = pread(fd, out + done, len - done, (off_t)done);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { rc = EFS_ERR_IO; break; }
            done += (size_t)r;
        }
    }
    close(fd);
    if (rc == EFS_OK) {
        uint8_t got[EFS_HASH_SIZE];
        efs_hash(out, len, got);
        if (memcmp(got, sum, EFS_HASH_SIZE)) rc = EFS_ERR_CHECKSUM;
    }
    return rc;
}

int server_segment_get(struct efsd_server *s, const uint8_t *req, uint32_t bytes,
                       uint8_t **reply, uint32_t *reply_len)
{
    *reply = NULL; *reply_len = 0;
    if (!s || !req || bytes != EFS_SEGMENT_GET_HEADER) return EFS_ERR_INVAL;
    uint32_t group = req[0], fi = req[1], geometry = sg_get32(req + 4);
    uint64_t id = sg_get64(req + 8), gen = sg_get64(req + 16);
    if (!efs_pack_group_anchor(group) || fi >= EFS_NUM_FRAGMENTS || !id || !gen ||
        geometry < EFS_SEGMENT_ALIGNMENT || geometry > EFS_SEGMENT_MAX_SIZE ||
        geometry % EFS_SEGMENT_ALIGNMENT) return EFS_ERR_INVAL;
    uint32_t len = geometry / 2;
    uint8_t *r = malloc(EFS_SEGMENT_GET_REPLY + len);
    if (!r) return EFS_ERR_NOMEM;
    memset(r, 0, EFS_SEGMENT_GET_REPLY);
    int rc = server_segment_read(s, group, id, gen, fi, len, r + EFS_SEGMENT_GET_REPLY, r + 8);
    sg_put32(r, (uint32_t)rc);
    sg_put32(r + 4, rc == EFS_OK ? len : 0);
    *reply = r;
    *reply_len = EFS_SEGMENT_GET_REPLY + (rc == EFS_OK ? len : 0);
    return EFS_OK;
}

/* Payload bytes of this node's fragments of a segment (reclaim credit). */
uint64_t server_segment_payload(struct efsd_server *s, uint32_t group, uint64_t id, uint64_t gen)
{
    uint64_t total = 0;
    for (uint32_t fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        char path[EFS_MAX_PATH + 128];
        struct stat st;
        server_segment_path(s, group, id, gen, fi, path, sizeof(path));
        if (!stat(path, &st) && (uint64_t)st.st_size > SG_TAIL)
            total += (uint64_t)st.st_size - SG_TAIL;
    }
    return total;
}

/* Remove this node's fragments of a fenced segment; *removed gets the
 * payload unlinked. Idempotent: absent files are already gone. */
int server_segment_delete(struct efsd_server *s, uint32_t group, uint64_t id, uint64_t gen,
                          uint64_t *removed)
{
    char dir[EFS_MAX_PATH + 64];
    int rc = EFS_OK, any = 0;
    sg_dir(s, group, id, dir, sizeof(dir));
    for (uint32_t fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        char path[EFS_MAX_PATH + 128];
        struct stat st;
        server_segment_path(s, group, id, gen, fi, path, sizeof(path));
        if (stat(path, &st)) { if (errno != ENOENT) rc = EFS_ERR_IO; continue; }
        if (unlink(path) && errno != ENOENT) { rc = EFS_ERR_IO; continue; }
        any = 1;
        if (removed && (uint64_t)st.st_size > SG_TAIL) *removed += (uint64_t)st.st_size - SG_TAIL;
    }
    if (any && rc == EFS_OK && sg_sync_path(dir) != EFS_OK) rc = EFS_ERR_IO;
    return rc;
}

/* Visit every segment fragment file on every root: (group, id, gen). */
int server_segment_scan(struct efsd_server *s,
                        void (*fn)(void *, uint32_t, uint64_t, uint64_t), void *arg)
{
    uint32_t nroot = s->storage_path_count ? s->storage_path_count : 1;
    for (uint32_t r = 0; r < nroot; r++) {
        char top[EFS_MAX_PATH + 32];
        snprintf(top, sizeof(top), "%s/data/segments", s->storage_paths[r]);
        DIR *gd = opendir(top);
        if (!gd) continue;
        struct dirent *ge;
        while ((ge = readdir(gd))) {
            unsigned group;
            if (sscanf(ge->d_name, "g%u", &group) != 1) continue;
            char gpath[EFS_MAX_PATH + 64];
            snprintf(gpath, sizeof(gpath), "%s/%s", top, ge->d_name);
            DIR *bd = opendir(gpath);
            if (!bd) continue;
            struct dirent *be;
            while ((be = readdir(bd))) {
                if (be->d_name[0] == '.') continue;
                char bpath[EFS_MAX_PATH + 96];
                snprintf(bpath, sizeof(bpath), "%s/%s", gpath, be->d_name);
                DIR *fd = opendir(bpath);
                if (!fd) continue;
                struct dirent *fe;
                while ((fe = readdir(fd))) {
                    unsigned long long id, gen;
                    unsigned fi;
                    if (sscanf(fe->d_name, "%16llx.%llx.%u", &id, &gen, &fi) == 3 &&
                        fi < EFS_NUM_FRAGMENTS && id && gen)
                        fn(arg, group, id, gen);
                }
                closedir(fd);
            }
            closedir(bd);
        }
        closedir(gd);
    }
    return EFS_OK;
}
