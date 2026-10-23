#include "server_internal.h"
#include "storage_roots.h"
#ifndef EFS_BENCH_BUILD
#include "directory_sync_group.h"
#endif
#include "efs/checksum.h"
#include "efs/frag_record.h"
#include "efs/store.h"
#include "efs/kv_key.h"
#include "efs/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/uio.h>
#include <sys/vfs.h>

/* writev until every iovec is consumed. O_DIRECT needs each remaining
 * iovec base/len aligned; a short write that is not a 4 KiB multiple
 * leaves an unaligned remainder and the next writev fails with EINVAL,
 * which the caller reports as EIO. */
static int writev_all(int fd, struct iovec *iov, int n)
{
    while (n > 0) {
        ssize_t w = writev(fd, iov, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            return -1;
        }
        size_t left = (size_t)w;
        while (n > 0 && left >= iov[0].iov_len) {
            left -= iov[0].iov_len;
            iov++;
            n--;
        }
        if (n > 0 && left > 0) {
            iov[0].iov_base = (uint8_t *)iov[0].iov_base + left;
            iov[0].iov_len -= left;
        }
    }
    return 0;
}

/* mkdir -p, leaf first. `path` is writable and is restored before return.
 * The top-down walk did mkdir("/data1"), mkdir("/data1/06"), ... on every
 * new fragment directory: 9 304 mkdir for 2 360 fragment writes in one
 * 120 s window, 8 176 of them EEXIST (Sep 29, fcstor003). An existing
 * tree now costs one mkdir; only a missing parent recurses. */
static void make_dir_up(char *path)
{
    char *slash;

    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return;
    if (errno != ENOENT)
        return;
    slash = strrchr(path, '/');
    if (!slash || slash == path)
        return;
    *slash = '\0';
    make_dir_up(path);
    *slash = '/';
    (void)mkdir(path, 0755);
}

static void make_dir(const char *path)
{
    size_t n = strlen(path) + 1;
    char *tmp = malloc(n);
    if (!tmp)
        return;
    memcpy(tmp, path, n);
    make_dir_up(tmp);
    free(tmp);
}

static void make_dir_for_file(const char *path)
{
    /* Skip full mkdir walk when the parent dir matches the last one we
     * successfully created on this thread (sequential chunk PUTs share
     * chunk>>10 dirs). */
    static __thread char last_parent[8192];
    const char *slash = strrchr(path, '/');
    size_t plen = 0;
    if (slash && slash > path)
        plen = (size_t)(slash - path);
    if (plen > 0 && plen < sizeof(last_parent) && last_parent[0] &&
        strncmp(last_parent, path, plen) == 0 && last_parent[plen] == '\0')
        return;

    size_t n = strlen(path) + 1;
    char *tmp = malloc(n);
    if (!tmp)
        return;
    memcpy(tmp, path, n);

    char *last_slash = strrchr(tmp, '/');
    if (last_slash) {
        *last_slash = '\0';
        make_dir(tmp);
        if (plen > 0 && plen < sizeof(last_parent)) {
            memcpy(last_parent, path, plen);
            last_parent[plen] = '\0';
        }
    }
    free(tmp);
}

struct efs_export *server_get_export(struct efsd_server *s, efs_export_id_t id)
{
    for (uint32_t i = 0; i < s->export_count; i++) {
        if (s->exports[i].id == id)
            return &s->exports[i];
    }
    return NULL;
}

int server_export_index_locked(struct efsd_server *s, struct efs_export *ex)
{
    if (!ex)
        return -1;
    ptrdiff_t idx = ex - s->exports;
    if (idx < 0 || (uint32_t)idx >= s->export_count)
        return -1;
    return (int)idx;
}

struct efs_export *server_export_acquire_locked(struct efsd_server *s,
                                                efs_export_id_t id)
{
    struct efs_export *ex = server_get_export(s, id);
    int idx = server_export_index_locked(s, ex);
    if (idx < 0 || s->export_destroying[idx])
        return NULL;
    s->export_inflight[idx]++;
    return ex;
}

struct efs_export *server_export_acquire_or_create_locked(struct efsd_server *s,
                                                          efs_export_id_t id)
{
    struct efs_export *ex = server_export_acquire_locked(s, id);
    int nidx;

    if (ex)
        return ex;
    if (!id || s->export_count >= EFS_MAX_EXPORTS)
        return NULL;
    ex = &s->exports[s->export_count++];
    efs_export_init(ex, id, "pending");
    ex->id = id;
    nidx = server_export_index_locked(s, ex);
    if (nidx >= 0)
        s->export_inflight[nidx]++;
    return ex;
}

void server_export_put(struct efsd_server *s, struct efs_export *ex)
{
    if (!ex)
        return;
    pthread_mutex_lock(&s->lock);
    int idx = server_export_index_locked(s, ex);
    if (idx >= 0 && s->export_inflight[idx] > 0) {
        if (--s->export_inflight[idx] == 0)
            pthread_cond_broadcast(&s->export_idle_cv);
    }
    pthread_mutex_unlock(&s->lock);
}

#define EFS_USAGE_MAGIC   "EFSU"
#define EFS_USAGE_VERSION 2

static void usage_path(struct efsd_server *s, char *path, size_t path_len)
{
    const char *root = (s->storage_path_count > 0) ? s->storage_paths[0]
                                                   : s->storage_path;
    snprintf(path, path_len, "%s/meta/usage.bin", root);
}


void server_usage_mark_dirty(struct efsd_server *s)
{
    if (s)
        __atomic_store_n(&s->usage_dirty, 1, __ATOMIC_RELEASE);
}

void server_usage_flush_dirty(struct efsd_server *s)
{
    if (!s || !__atomic_exchange_n(&s->usage_dirty, 0, __ATOMIC_ACQ_REL))
        return;
    server_usage_save(s);
}

void server_usage_save(struct efsd_server *s)
{
    /* Serialize disk I/O: many writers used to race on usage.bin.tmp, causing
     * rename ENOENT storms (one thread renames the shared .tmp out from under
     * another). Unique tmp names + mutex make persistence safe. */
    static pthread_mutex_t usage_io_mu = PTHREAD_MUTEX_INITIALIZER;
    if (!s)
        return;
    uint64_t used = 0;
    pthread_mutex_lock(&s->lock);
    struct efs_node *local = server_local_node(s);
    if (local)
        used = local->used;
    __atomic_store_n(&s->usage_dirty, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&s->lock);

    /* Heap paths: heartbeat flushes usage and must stay stack-light (ASAN). */
    char *path = malloc(8192);
    char *tmp = malloc(8192);
    if (!path || !tmp) {
        free(path);
        free(tmp);
        server_usage_mark_dirty(s);
        return;
    }
    usage_path(s, path, 8192);

    pthread_mutex_lock(&usage_io_mu);
    make_dir_for_file(path);

    snprintf(tmp, 8192, "%s.tmp.%d.%lu", path, (int)getpid(),
             (unsigned long)pthread_self());
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "Could not save usage to %s: %s\n", tmp, strerror(errno));
        pthread_mutex_unlock(&usage_io_mu);
        free(path);
        free(tmp);
        server_usage_mark_dirty(s);
        return;
    }
    uint32_t version = EFS_USAGE_VERSION;
    if (fwrite(EFS_USAGE_MAGIC, 4, 1, f) != 1 ||
        fwrite(&version, sizeof(version), 1, f) != 1 ||
        fwrite(&used, sizeof(used), 1, f) != 1) {
        fprintf(stderr, "Could not write usage to %s: %s\n", tmp, strerror(errno));
        fclose(f);
        unlink(tmp);
        pthread_mutex_unlock(&usage_io_mu);
        free(path);
        free(tmp);
        server_usage_mark_dirty(s);
        return;
    }
    fflush(f);
    fclose(f);
    if (rename(tmp, path) != 0) {
        /* Parent may have been wiped mid-flight; recreate and retry once. */
        make_dir_for_file(path);
        if (rename(tmp, path) != 0) {
            fprintf(stderr, "Could not rename usage file to %s: %s\n",
                    path, strerror(errno));
            unlink(tmp);
            server_usage_mark_dirty(s);
        }
    }
    pthread_mutex_unlock(&usage_io_mu);
    free(path);
    free(tmp);
}


/* One-pass path append helpers. The snprintf chain they replace (7+ calls
 * per fragment path, 2+ paths per PUT) was ~13% of efsd CPU under the
 * 9-client ImageNet small-file storm. */
static inline char *path_append_str(char *p, const char *s)
{
    size_t n = strlen(s);
    memcpy(p, s, n);
    return p + n;
}

static inline char *path_append_u64(char *p, uint64_t v)
{
    char tmp[20];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    return p;
}

/* Sharded: .../exports/{id}/{d4}/{d3}/{d2}/{d1}/{d0}/{chunk>>10}
 * seg[0] is the least-significant group (rightmost path component). */
static int format_ino_chunk_dir(char *path, size_t path_len, const char *root,
                                efs_export_id_t export_id, efs_ino_t ino,
                                uint32_t chunk_index)
{
    char seg[EFS_INO_PATH_SEGS][5];
    efs_ino_path_segments(ino, seg);
    size_t root_len = strlen(root);
    /* root + literal + id + 5×("/"+4seg) + "/"+ci — bail to snprintf if the
     * caller's buffer is unexpectedly tight (never with the 8 KiB stacks). */
    if (root_len + 96 > path_len)
        return snprintf(path, path_len,
                        "%s/data/exports/%u/%s/%s/%s/%s/%s/%u", root,
                        export_id, seg[4], seg[3], seg[2], seg[1], seg[0],
                        chunk_index >> 10);
    char *p = path;
    p = path_append_str(p, root);
    p = path_append_str(p, "/data/exports/");
    p = path_append_u64(p, export_id);
    for (int i = EFS_INO_PATH_SEGS - 1; i >= 0; i--) {
        *p++ = '/';
        memcpy(p, seg[i], 4);
        p += 4;
    }
    *p++ = '/';
    p = path_append_u64(p, chunk_index >> 10);
    *p = '\0';
    return (int)(p - path);
}

/* Append "/{ci}.{fi}.{gen}" onto a dir built by format_ino_chunk_dir.
 * The wire and store entry points refuse generation 0. */
__thread uint64_t efs_tls_chunk_gen;

static inline char *path_append_frag(char *p, uint32_t chunk_index,
                                     uint32_t fragment_index)
{
    *p++ = '/';
    p = path_append_u64(p, chunk_index);
    *p++ = '.';
    p = path_append_u64(p, fragment_index);
    *p++ = '.';
    p = path_append_u64(p, efs_tls_chunk_gen);
    *p = '\0';
    return p;
}

/* The writer pool picks the root (pick_write_path) before every write;
 * UINT32_MAX = no valid root was picked, and the write is refused. */
static uint32_t write_root_index(const struct efsd_server *s)
{
    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;
    if (efs_tls_write_root >= 0 && (uint32_t)efs_tls_write_root < n)
        return (uint32_t)efs_tls_write_root;
    return UINT32_MAX;
}

static void fragment_path_at(struct efsd_server *s, uint32_t root_idx,
                             struct efs_export *ex, efs_ino_t ino,
                             uint32_t chunk_index, uint32_t fragment_index,
                             char *path, size_t path_len)
{
    int n = format_ino_chunk_dir(path, path_len, s->storage_paths[root_idx],
                                 ex->id, ino, chunk_index);
    if (n > 0 && (size_t)n + 48 <= path_len)
        path_append_frag(path + n, chunk_index, fragment_index);
}

int server_find_fragment_root(struct efsd_server *s, struct efs_export *ex,
                              efs_ino_t ino, uint32_t chunk_index,
                              uint32_t fragment_index)
{
    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;
    char path[8192];
    uint32_t ri;
    int hint = efs_tls_path_hint;

    /* First write of this fragment generation. The name cannot collide,
     * so the access() walk is pure overhead. The writer picks the
     * least-queued root when this returns "not found". */
    if (hint == EFS_PATH_HINT_SKIP)
        return -1;

    /* Do not fall back to gen=0 when the caller asked for a candidate.
     * A per-fragment leftover hit mixed with gen-N siblings decodes as
     * EFS_ERR_DECODE (W1 two-client RMW).
     * Called without the writer-pool lock. Holding that lock across
     * these walks serialized every PUT. A thread-local directory fd
     * removed the walk from the profile and slowed the 9-client dd.
     * W14.4: a hint from the previous PUT of this (ino, ci) is one
     * access(). The other roots are walked only when that misses. */
    if (hint >= 0 && (uint32_t)hint < n) {
        fragment_path_at(s, (uint32_t)hint, ex, ino, chunk_index,
                         fragment_index, path, sizeof(path));
        if (access(path, F_OK) == 0)
            return hint;
    }
    for (ri = 0; ri < n; ri++) {
        if ((int)ri == hint)
            continue;
        fragment_path_at(s, ri, ex, ino, chunk_index, fragment_index, path,
                         sizeof(path));
        if (access(path, F_OK) == 0)
            return (int)ri;
    }
    return -1;
}

static void frag_loc_note(efs_export_id_t export_id, efs_ino_t ino,
                          uint32_t chunk_index, uint32_t fragment_index,
                          uint64_t gen, uint32_t root);

int server_fragment_path(struct efsd_server *s, struct efs_export *ex,
                         efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                         char *path, size_t path_len)
{
    uint32_t root = write_root_index(s);

    if (root == UINT32_MAX) {
        fprintf(stderr, "store: fragment write without a picked storage root refused\n");
        return EFS_ERR_INVAL;
    }
    fragment_path_at(s, root, ex, ino, chunk_index, fragment_index, path,
                     path_len);
    /* The GET that follows (verify-after-write, a peer's read-modify-
     * write) finds the root here instead of probing them in order. */
    frag_loc_note(ex->id, ino, chunk_index, fragment_index,
                  efs_tls_chunk_gen, root);
    return 0;
}

/* Unlink a fragment from every storage root. A leftover .sum from
 * before the tail format is removed too; new files have no sidecar. */
int server_unlink_fragment_files(struct efsd_server *s, struct efs_export *ex,
                                  efs_ino_t ino, uint32_t chunk_index,
                                  uint32_t fragment_index)
{
    char path[8192];
    char sum_path[8200];
    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;

    for (uint32_t ri = 0; ri < n; ri++) {
        fragment_path_at(s, ri, ex, ino, chunk_index, fragment_index, path,
                         sizeof(path));
        unlink(path);
        snprintf(sum_path, sizeof(sum_path), "%s.sum", path);
        unlink(sum_path);
    }
    return EFS_OK;
}

void server_gc_status(struct efsd_server *s, struct efs_msg_gc_status_reply *out)
{
    memset(out, 0, sizeof(*out));
    out->version = 5;
    out->node_id = s->id;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#define GC_LOAD(f) __atomic_load_n(&s->gc_##f, __ATOMIC_RELAXED)
/* A concurrent update can be newer than this snapshot's clock sample. */
#define GC_AGE(t) ((t) ? (now >= (t) ? (now - (t)) / 1000 : 0) : UINT64_MAX)
    out->stage = GC_LOAD(stage);
    out->group = GC_LOAD(group);
    out->sampled_mask = GC_LOAD(sampled_mask);
    out->passes = GC_LOAD(passes);
    uint64_t start = GC_LOAD(pass_start_us), done = GC_LOAD(last_pass_us),
             del = GC_LOAD(last_delete_us);
    out->pass_elapsed_ms = start ? GC_AGE(start) : 0;
    out->last_pass_age_ms = GC_AGE(done);
    out->last_delete_age_ms = GC_AGE(del);
    out->removed_fragments = GC_LOAD(removed_fragments);
    out->removed_file_bytes = GC_LOAD(removed_file_bytes);
    out->removed_payload_bytes = GC_LOAD(removed_payload_bytes);
    out->delete_errors = GC_LOAD(delete_errors);
    out->mismatches = GC_LOAD(mismatches);
    out->sweep_errors = GC_LOAD(sweep_errors);
    out->reap_errors = GC_LOAD(reap_errors);
    out->ack_errors = GC_LOAD(ack_errors);
    out->scan_errors = GC_LOAD(scan_errors);
    out->missing_exports = GC_LOAD(missing_exports);
    for (unsigned i = 0; i < 2; i++) {
        out->pending[i] = __atomic_load_n(&s->gc_pending[i], __ATOMIC_RELAXED);
        out->orphan_seen[i] = __atomic_load_n(&s->gc_orphan_seen[i], __ATOMIC_RELAXED);
        out->reap_seen[i] = __atomic_load_n(&s->gc_reap_seen[i], __ATOMIC_RELAXED);
        out->trunc_pending[i] = __atomic_load_n(&s->gc_trunc_seen[i], __ATOMIC_RELAXED);
        out->reap_capped[i] = __atomic_load_n(&s->gc_reap_capped[i], __ATOMIC_RELAXED);
        out->first_reap[i] = __atomic_load_n(&s->gc_first_reap[i], __ATOMIC_RELAXED);
        uint64_t first = __atomic_load_n(&s->gc_first_seen_us[i], __ATOMIC_RELAXED);
        out->first_seen_age_ms[i] = GC_AGE(first);
    }
    for (unsigned i = 0; i < EFS_GC_IO_CLASSES; i++) {
#define GC_IO_LOAD(f) out->io[i].f = __atomic_load_n(&s->gc_io[i].f, __ATOMIC_RELAXED)
        GC_IO_LOAD(ops); GC_IO_LOAD(bytes); GC_IO_LOAD(errors); GC_IO_LOAD(us_sum);
#undef GC_IO_LOAD
    }
    pthread_mutex_lock(&s->lock);
    out->exports = s->export_count;
    struct efs_node *local = server_local_node(s);
    out->data_usage_bytes = local ? local->used : UINT64_MAX;
    pthread_mutex_unlock(&s->lock);
#undef GC_AGE
#undef GC_LOAD
    server_raft_host_expiry_status(&out->expiry);
    server_reclaim_status(s, &out->reclaim);
}

/* Measure completed GC syscalls, including failed attempts. This deliberately
 * does not claim physical device traffic, metadata WAL I/O or fence-page I/O. */
static uint64_t gc_clock_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static void gc_io_add(struct efsd_server *s, unsigned cls, uint64_t start,
                      uint64_t bytes, int error)
{
    __atomic_fetch_add(&s->gc_io[cls].bytes, bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->gc_io[cls].us_sum, gc_clock_us() - start, __ATOMIC_RELAXED);
    if (error) __atomic_fetch_add(&s->gc_io[cls].errors, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->gc_io[cls].ops, 1, __ATOMIC_RELAXED);
}
static int gc_sync_directory(struct efsd_server *s, int fd)
{
    uint64_t start = gc_clock_us();
    int rc;
    do { rc = fsync(fd); } while (rc < 0 && errno == EINTR);
    int saved_errno = errno;
    gc_io_add(s, 2, start, 0, rc < 0);
    errno = saved_errno;
    return rc;
}
static int gc_unlink_file(struct efsd_server *s, const char *path)
{
    uint64_t start = gc_clock_us();
    int rc = unlink(path), saved_errno = errno;
    /* An already absent object is an idempotent success, not an I/O error. */
    gc_io_add(s, 1, start, 0, rc < 0 && saved_errno != ENOENT);
    errno = saved_errno;
    return rc;
}

/* PUT and checksum-test/unlink share an object lock: an immutable
 * generation avoids replacement, and the lock serializes a retried PUT with
 * the checksum test and unlink of the same object. */
#define FRAG_LOCKS 4096
static pthread_mutex_t frag_locks[FRAG_LOCKS];
static pthread_rwlock_t inode_gc_locks[FRAG_LOCKS];
static uint64_t inode_gc_epoch;
static pthread_mutex_t death_page_mu[64];
static pthread_once_t frag_locks_once = PTHREAD_ONCE_INIT;
static void frag_locks_init(void)
{
    for (unsigned i = 0; i < 64; i++)
        pthread_mutex_init(&death_page_mu[i], NULL);
    for (unsigned i = 0; i < FRAG_LOCKS; i++) {
        pthread_mutex_init(&frag_locks[i], NULL);
        pthread_rwlock_init(&inode_gc_locks[i], NULL);
    }
}
static pthread_mutex_t *fragment_mutex(efs_export_id_t eid, efs_ino_t ino, uint32_t ci,
                                       uint32_t fi)
{
    pthread_once(&frag_locks_once, frag_locks_init);
    uint64_t h = ino * 11400714819323198485ull;
    h ^= (uint64_t)ci * 0x9e3779b9u;
    h ^= ((uint64_t)eid << 32) ^ fi;
    return &frag_locks[(h ^ (h >> 32)) & (FRAG_LOCKS - 1)];
}

/* GC/restart accounting must decode the stored format, not today's mount
 * mode or transient export geometry. PUT bodies are multiples of 4 KiB. */
static int fragment_payload_bytes(uint64_t bytes, uint64_t *payload)
{
    if (bytes >= EFS_FRAG_RECORD_BYTES &&
        (bytes - EFS_FRAG_RECORD_BYTES) % 4096 == 0) {
        *payload = bytes - EFS_FRAG_RECORD_BYTES;
        return EFS_OK;
    }
    if (bytes >= 4096 && bytes % 4096 == 0) {
        *payload = bytes - 4096;
        return EFS_OK;
    }
    return EFS_ERR_PROTO;
}

/* A GC ACK must not outlive the directory operation it retires. Repeat the
 * parent sync for an already-absent file after a failed sync/crash retry. */
static int fragment_parent_sync(struct efsd_server *s, const char *path)
{
    char parent[8192];
    size_t len = strlen(path);
    if (len >= sizeof(parent))
        return EFS_ERR_IO;
    memcpy(parent, path, len + 1);
    char *slash = strrchr(parent, '/');
    if (!slash)
        return EFS_ERR_INVAL;
    *slash = '\0';
    /* A retry may find both the file and its former directory absent.
     * Sync the nearest surviving ancestor before acknowledging removal. */
    int fd;
    for (;;) {
        fd = open(parent, O_RDONLY | O_DIRECTORY);
        if (fd >= 0) break;
        if (errno != ENOENT) return EFS_ERR_IO;
        slash = strrchr(parent, '/');
        if (!slash) return EFS_ERR_IO;
        if (slash == parent) {
            if (!parent[1]) return EFS_ERR_IO;
            parent[1] = '\0';
        } else *slash = '\0';
    }
    int rc;
    rc = gc_sync_directory(s, fd);
    int cr = close(fd);
    return rc == 0 && cr == 0 ? EFS_OK : EFS_ERR_IO;
}

/* I25: the identity a chunk fragment file under this name must carry.
 * The generation is the caller's efs_tls_chunk_gen (part of the name). */
static void fragment_ident(const struct efs_export *ex, efs_ino_t ino,
                           uint32_t chunk_index, uint32_t fragment_index,
                           uint32_t payload_len, struct efs_frag_ident *id)
{
    memset(id, 0, sizeof(*id));
    id->kind = EFS_FRAG_KIND_CHUNK;
    id->space = ex->id;
    id->object = ino;
    id->generation = efs_tls_chunk_gen;
    id->chunk_index = chunk_index;
    id->fragment_index = fragment_index;
    id->payload_len = payload_len;
    id->coding_profile = EFS_FRAG_PROFILE_K2F1;
}

/* Every refused fragment is logged (bounded: the first 256, then every
 * 1024th), naming the path, the refusal and the first mismatching field. */
static uint64_t fragment_refusals;
uint64_t server_fragment_refusals(void)
{
    return __atomic_load_n(&fragment_refusals, __ATOMIC_RELAXED);
}
static void fragment_refused(const char *op, const char *path, int rc,
                             const char *field, const struct efs_frag_ident *id)
{
    uint64_t n = __atomic_add_fetch(&fragment_refusals, 1, __ATOMIC_RELAXED);
    if (n > 256 && n % 1024)
        return;
    fprintf(stderr, "store: I25 %s refused fragment %s: %s%s%s "
            "(expected export=%u ino=%llu ci=%u fi=%u gen=%llu len=%u; refusals=%llu)\n",
            op, path, efs_strerror(rc), field ? " field=" : "", field ? field : "",
            id->space, (unsigned long long)id->object, id->chunk_index,
            id->fragment_index, (unsigned long long)id->generation,
            id->payload_len, (unsigned long long)n);
}

/* Only a proven checksum mismatch is ackable. Read/open/stat/unlink failures
 * leave the durable GC record for retry. Count only files actually unlinked. */
int server_delete_fragment_if_sum(struct efsd_server *s, struct efs_export *ex,
                                  efs_ino_t ino, uint32_t chunk_index,
                                  uint32_t fragment_index,
                                  const uint8_t expect_sum[EFS_HASH_SIZE])
{
    if (!s || !ex || !expect_sum)
        return EFS_ERR_INVAL;
    char path[8192];
    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;
    int live = 0, rc = EFS_OK;
    pthread_mutex_t *mu = fragment_mutex(ex->id, ino, chunk_index, fragment_index);
    pthread_mutex_lock(mu);
    for (uint32_t ri = 0; ri < n; ri++) {
        struct stat st;
        uint8_t rec[EFS_FRAG_RECORD_BYTES], got[EFS_HASH_SIZE];
        uint64_t payload;
        fragment_path_at(s, ri, ex, ino, chunk_index, fragment_index, path,
                         sizeof(path));
        /* A small buffered record read works for both stored tail layouts. */
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            if (errno == ENOENT) {
                rc = fragment_parent_sync(s, path);
                if (rc == EFS_OK)
                    continue;
            }
            rc = EFS_ERR_IO;
            break;
        }
        int sr;
        do {
            sr = fstat(fd, &st);
        } while (sr < 0 && errno == EINTR);
        if (sr < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            fragment_payload_bytes((uint64_t)st.st_size, &payload) != EFS_OK) {
            close(fd);
            rc = EFS_ERR_IO;
            break;
        }
        uint64_t read_start = gc_clock_us();
        ssize_t rn;
        do {
            rn = pread(fd, rec, sizeof(rec), (off_t)payload);
        } while (rn < 0 && errno == EINTR);
        int read_errno = errno;
        gc_io_add(s, 0, read_start, rn > 0 ? (uint64_t)rn : 0,
                  rn != (ssize_t)sizeof(rec));
        errno = read_errno;
        int cr = close(fd); /* never retry close on a potentially released fd */
        if (rn != (ssize_t)sizeof(rec) || cr != 0) {
            rc = EFS_ERR_IO;
            break;
        }
        /* I25: GC acts only on a file proven to be this exact object. An
         * unknown format, a rotten record or another identity under this
         * name is neither deleted nor acknowledged: the record stays for an
         * operator, audibly. */
        struct efs_frag_ident want;
        const char *field = NULL;
        if (payload > UINT32_MAX) {
            rc = EFS_ERR_FRAG_FORMAT;
            break;
        }
        fragment_ident(ex, ino, chunk_index, fragment_index, (uint32_t)payload,
                       &want);
        int vr = efs_frag_record_verify(rec, sizeof(rec), &want, got, &field);
        if (vr != EFS_OK) {
            fragment_refused("gc", path, vr, field, &want);
            rc = vr;
            break;
        }
        if (memcmp(got, expect_sum, sizeof(got)) != 0) {
            live = 1;
            __atomic_fetch_add(&s->gc_mismatches, 1, __ATOMIC_RELAXED);
            continue;
        }
        if (gc_unlink_file(s, path) != 0) {
            if (errno == ENOENT) {
                rc = fragment_parent_sync(s, path);
                if (rc == EFS_OK)
                    continue;
            }
            rc = EFS_ERR_IO;
            break;
        }
        __atomic_fetch_add(&s->gc_removed_fragments, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s->gc_removed_file_bytes, (uint64_t)st.st_size,
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&s->gc_removed_payload_bytes, payload, __ATOMIC_RELAXED);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        __atomic_store_n(&s->gc_last_delete_us,
                         (uint64_t)now.tv_sec * 1000000u + now.tv_nsec / 1000,
                         __ATOMIC_RELAXED);
        if (!efs_ino_is_meta_table(ino) && payload) {
            pthread_mutex_lock(&s->lock);
            struct efs_node *ln = server_local_node(s);
            if (ln)
                ln->used = ln->used >= payload ? ln->used - payload : 0;
            pthread_mutex_unlock(&s->lock);
            server_usage_mark_dirty(s);
        }
        rc = fragment_parent_sync(s, path);
        if (rc != EFS_OK)
            break;
    }
    pthread_mutex_unlock(mu);
    if (rc != EFS_OK) {
        __atomic_fetch_add(&s->gc_delete_errors, 1, __ATOMIC_RELAXED);
        return rc;
    }
    return live ? EFS_ERR_EXIST : EFS_OK;
}

/* Current FileIDs never reuse an inode number. Persist a death fence before
 * orphan cleanup so a delayed PUT cannot recreate files after collection.
 * Keep these certificates: pruning them requires a durable admission floor. */
static pthread_rwlock_t *inode_gc_lock(efs_export_id_t eid, efs_ino_t ino)
{
    pthread_once(&frag_locks_once, frag_locks_init);
    /* Low inode bits identify a shard. Mixing the allocation counter prevents
     * collection of one file from exclusively locking its whole directory. */
    return &inode_gc_locks[efs_ino_lock_slot(ino ^ ((uint64_t)eid << 32),
                                           FRAG_LOCKS)];
}
/* PUT and GC paths for the exact inode/export use this same guard. Fragment
 * reads do not take it: an unlink under an open reader is harmless, but any
 * future in-place reuse of a fragment file must retire readers first. */
static pthread_rwlock_t *server_inode_gc_lock(struct efsd_server *s,
                                            efs_export_id_t eid,efs_ino_t ino)
{
    (void)s;
    return inode_gc_lock(eid,ino);
}
pthread_rwlock_t *server_inode_guard(efs_export_id_t eid, efs_ino_t ino)
{
    return inode_gc_lock(eid, ino);
}

#define DEATH_PAGE_BYTES 2048u
#define DEATH_PAGE_BITS (DEATH_PAGE_BYTES * 8u)
static void inode_death_path(struct efsd_server *s, efs_export_id_t eid, efs_ino_t ino,
                             char path[8192])
{
    uint64_t ordinal = ino >> EFS_KV_SHARD_BITS;
    snprintf(path, 8192, "%s/meta/gc-dead/%u/%u.%llu", s->storage_paths[0], eid,
             (unsigned)(ino & EFS_KV_SHARD_MASK),
             (unsigned long long)(ordinal / DEATH_PAGE_BITS));
}
/* Compact immutable pages, atomically replaced on update. A checksum error
 * fails admission closed; no per-deleted-file inode or unbounded RAM index. */
static int death_page_read(const char *path, uint8_t page[DEATH_PAGE_BYTES])
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) {
            memset(page, 0, DEATH_PAGE_BYTES);
            return EFS_OK;
        }
        return EFS_ERR_IO;
    }
    uint8_t sum[EFS_HASH_SIZE], got[EFS_HASH_SIZE];
    struct stat st;
    int rc = EFS_OK;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        st.st_size != DEATH_PAGE_BYTES + EFS_HASH_SIZE)
        rc = EFS_ERR_IO;
    ssize_t n;
    do {
        n = pread(fd, page, DEATH_PAGE_BYTES, 0);
    } while (n < 0 && errno == EINTR);
    if (n != DEATH_PAGE_BYTES)
        rc = EFS_ERR_IO;
    do {
        n = pread(fd, sum, sizeof(sum), DEATH_PAGE_BYTES);
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(sum))
        rc = EFS_ERR_IO;
    if (close(fd))
        rc = EFS_ERR_IO;
    if (rc == EFS_OK) {
        efs_hash(page, DEATH_PAGE_BYTES, got);
        if (memcmp(sum, got, sizeof(sum)))
            rc = EFS_ERR_IO;
    }
    return rc;
}
void server_inode_gc_epoch_bump(void)
{
    __atomic_fetch_add(&inode_gc_epoch, 1, __ATOMIC_RELEASE);
}
/* 1 dead, 0 live, -1 unknown (refuse). W124: a journalled fence counts as
 * dead before its bitmap page is checkpointed, and a root whose reclaim
 * journal failed verification refuses every data write until recovery. */
static int inode_is_dead(struct efsd_server *s, efs_export_id_t eid, efs_ino_t ino)
{
    /* Metadata tables are never reclaimed through the journal. */
    if (__atomic_load_n(&s->reclaim_fail_closed, __ATOMIC_ACQUIRE) && !efs_ino_is_meta_table(ino))
        return -1;
    if (server_reclaim_fenced(eid, ino))
        return 1;
    static __thread struct efsd_server *last_server;
    static __thread uint64_t last_ino, last_epoch;
    static __thread uint32_t last_eid;
    static __thread int last_dead, valid;
    uint64_t epoch = __atomic_load_n(&inode_gc_epoch, __ATOMIC_ACQUIRE);
    if (valid && s == last_server && ino == last_ino && eid == last_eid &&
        epoch == last_epoch)
        return last_dead;
    char path[8192];
    uint8_t page[DEATH_PAGE_BYTES];
    inode_death_path(s, eid, ino, path);
    int rc = death_page_read(path, page);
    if (rc != EFS_OK)
        return -1;
    unsigned bit = (unsigned)((ino >> EFS_KV_SHARD_BITS) % DEATH_PAGE_BITS);
    int dead = (page[bit / 8] >> (bit % 8)) & 1;
    last_server = s;
    last_ino = ino;
    last_eid = eid;
    last_epoch = epoch;
    last_dead = dead;
    valid = 1;
    return dead;
}
/* W124: set while deleting an inode whose bytes were already credited back
 * to quota at acceptance. Its unlinks only count removed payload here; the
 * reclaim deleter settles quota once the whole inode is gone. */
static __thread uint64_t *t_gc_counted;
/* W124: set while the reclaim deleter runs. Directory syncs are skipped; the
 * journal makes the unlinks durable with server_gc_sync_roots before it
 * retires their records, and replays the deletion after a crash. */
static __thread int t_gc_defer_sync;
static void gc_account_removed(struct efsd_server *s,uint64_t count,
                               uint64_t payload,uint64_t stored_bytes)
{
    if(!count)return;
    pthread_mutex_lock(&s->lock);
    if (t_gc_counted) {
        *t_gc_counted += payload;
    } else {
        struct efs_node *ln=server_local_node(s);
        if(ln)ln->used=ln->used>=payload?ln->used-payload:0;
    }
    pthread_mutex_unlock(&s->lock);
    server_usage_mark_dirty(s);
    __atomic_fetch_add(&s->gc_removed_fragments,count,__ATOMIC_RELAXED);
    __atomic_fetch_add(&s->gc_removed_file_bytes,stored_bytes,__ATOMIC_RELAXED);
    __atomic_fetch_add(&s->gc_removed_payload_bytes,payload,__ATOMIC_RELAXED);
    __atomic_store_n(&s->gc_last_delete_us,gc_clock_us(),__ATOMIC_RELAXED);
}
/* Bounded physical orphan sweep, called only after Raft-confirmed lane
 * sweeps. These bodies include losing CAS attempts absent from chunk rows. */
static int gc_dead_directory(struct efsd_server *s, const char *path, unsigned *left, unsigned depth)
{
    (void)depth;
    int dfd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dfd < 0)
        return errno == ENOENT ? (t_gc_defer_sync ? EFS_OK : fragment_parent_sync(s, path)) : EFS_ERR_IO;
    DIR *dir = fdopendir(dfd);
    if (!dir) { close(dfd); return EFS_ERR_IO; }
    int rc = EFS_OK;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dir);
        if (!de) {
            if (errno)
                rc = EFS_ERR_IO;
            break;
        }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char child[8192];
        if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >=
            (int)sizeof(child)) {
            rc = EFS_ERR_IO;
            break;
        }
        struct stat st;
        if (lstat(child, &st)) {
            if (errno == ENOENT)
                continue;
            rc = EFS_ERR_IO;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (depth >= 1) { rc = EFS_ERR_IO; break; }
            if (!*left) { rc = EFS_ERR_BUSY; break; }
            (*left)--;
            rc = gc_dead_directory(s, child, left, depth + 1);
            if (rc != EFS_OK)
                break;
        } else if (S_ISREG(st.st_mode)) {
            if (!*left) {
                rc = EFS_ERR_BUSY;
                break;
            }
            uint64_t payload = 0, stored_bytes = (uint64_t)st.st_size;
            if (gc_unlink_file(s, child)) {
                if (errno == ENOENT)
                    continue;
                rc = EFS_ERR_IO;
                break;
            }
            (*left)--;
            if (fragment_payload_bytes((uint64_t)st.st_size, &payload) != EFS_OK)
                payload = (uint64_t)st.st_size;
            gc_account_removed(s,1,payload,stored_bytes);
        } else {
            rc = EFS_ERR_IO;
            break;
        } /* Never follow links or unknown objects. */
    }
    /* Sync even a partial pass; no successful RPC may retire an undurable
     * unlink. An absent-file retry also syncs the containing directory. */
    int fd = dirfd(dir), sr;
    sr = t_gc_defer_sync ? 0 : gc_sync_directory(s, fd);
    if (sr < 0)
        rc = EFS_ERR_IO;
    if (closedir(dir))
        rc = EFS_ERR_IO;
    if (rc == EFS_OK) {
        if (rmdir(path) && errno != ENOENT)
            rc = EFS_ERR_IO;
        if (rc == EFS_OK && !t_gc_defer_sync)
            rc = fragment_parent_sync(s, path);
    }
    return rc;
}
/* The five inode path segments are scaffolding, not user directories.
 * Remove empty shared ancestors only; another live inode makes rmdir stop. */
static int gc_dead_ancestors(struct efsd_server *s, const char *inode_path)
{
    char path[8192];
    if (strlen(inode_path) >= sizeof(path)) return EFS_ERR_IO;
    strcpy(path, inode_path);
    for (unsigned level = 1; level < EFS_INO_PATH_SEGS; level++) {
        char *slash = strrchr(path, '/');
        if (!slash) return EFS_ERR_IO;
        *slash = '\0';
        if (rmdir(path) && errno != ENOENT) {
            if (errno == ENOTEMPTY || errno == EEXIST) return EFS_OK;
            return EFS_ERR_IO;
        }
        int rc = t_gc_defer_sync ? EFS_OK : fragment_parent_sync(s, path);
        if (rc != EFS_OK) return rc;
    }
    return EFS_OK;
}

/* W124: write every death bit of `inos` with one checksummed page update per
 * touched page (temp file, fsync, rename, parent syncs), not one per inode. */
struct death_ref { uint32_t shard; uint64_t page; efs_ino_t ino; };
static int death_ref_cmp(const void *a, const void *b)
{
    const struct death_ref *x = a, *y = b;
    if (x->shard != y->shard) return x->shard < y->shard ? -1 : 1;
    if (x->page != y->page) return x->page < y->page ? -1 : 1;
    return (x->ino > y->ino) - (x->ino < y->ino);
}
/* All changed pages are written as temp files, made durable with one syncfs,
 * renamed into place, and the renames made durable with a second syncfs:
 * two filesystem syncs per checkpoint however many pages it touches (orcd-mini
 * spreads inodes over ~4096 shard pages). Only the reclaim checkpoint thread
 * writes pages; death_page_mu still serialises each page's read-modify-write. */
struct death_pending { char *tmp, *path; };
static int death_sync_meta(struct efsd_server *s)
{
    int fd = open(s->storage_paths[0], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return EFS_ERR_IO;
    int rc = syncfs(fd);
    close(fd);
    return rc ? EFS_ERR_IO : EFS_OK;
}
int server_gc_death_set_many(struct efsd_server *s, efs_export_id_t eid,
                             const efs_ino_t *inos, uint32_t n)
{
    if (!s || !eid || (n && !inos)) return EFS_ERR_INVAL;
    if (!n) return EFS_OK;
    pthread_once(&frag_locks_once, frag_locks_init);
    struct death_ref *refs = malloc((size_t)n * sizeof(*refs));
    struct death_pending *pend = calloc(n, sizeof(*pend));
    if (!refs || !pend) { free(refs); free(pend); return EFS_ERR_NOMEM; }
    for (uint32_t i = 0; i < n; i++) {
        refs[i].shard = (uint32_t)(inos[i] & EFS_KV_SHARD_MASK);
        refs[i].page = (inos[i] >> EFS_KV_SHARD_BITS) / DEATH_PAGE_BITS;
        refs[i].ino = inos[i];
    }
    qsort(refs, n, sizeof(*refs), death_ref_cmp);
    int rc = EFS_OK;
    uint32_t np = 0;
    for (uint32_t i = 0; rc == EFS_OK && i < n;) {
        uint32_t j = i;
        while (j < n && refs[j].shard == refs[i].shard && refs[j].page == refs[i].page) j++;
        pthread_mutex_t *mu = &death_page_mu[refs[i].shard % 64];
        pthread_mutex_lock(mu);
        char path[8192];
        uint8_t page[DEATH_PAGE_BYTES];
        inode_death_path(s, eid, refs[i].ino, path);
        rc = death_page_read(path, page);
        int changed = 0;
        for (uint32_t k = i; rc == EFS_OK && k < j; k++) {
            unsigned bit = (unsigned)((refs[k].ino >> EFS_KV_SHARD_BITS) % DEATH_PAGE_BITS);
            if (!(page[bit / 8] & (1u << (bit % 8)))) {
                page[bit / 8] |= (uint8_t)(1u << (bit % 8));
                changed = 1;
            }
        }
        if (rc == EFS_OK && changed) {
            char tmp[8192];
            uint8_t bytes[DEATH_PAGE_BYTES + EFS_HASH_SIZE];
            memcpy(bytes, page, DEATH_PAGE_BYTES);
            efs_hash(page, DEATH_PAGE_BYTES, bytes + DEATH_PAGE_BYTES);
            make_dir_for_file(path);
            int fd = snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path) < (int)sizeof(tmp) ? mkstemp(tmp) : -1;
            if (fd < 0) rc = EFS_ERR_IO;
            else {
                size_t off = 0;
                while (rc == EFS_OK && off < sizeof(bytes)) {
                    ssize_t w = write(fd, bytes + off, sizeof(bytes) - off);
                    if (w < 0 && errno == EINTR) continue;
                    if (w <= 0) rc = EFS_ERR_IO; else off += (size_t)w;
                }
                if (close(fd)) rc = EFS_ERR_IO;
                pend[np].tmp = strdup(tmp);
                pend[np].path = strdup(path);
                if (!pend[np].tmp || !pend[np].path) rc = EFS_ERR_NOMEM;
                np++;
                if (rc != EFS_OK) unlink(tmp);
            }
        }
        pthread_mutex_unlock(mu);
        i = j;
    }
    /* Page bodies durable, then every rename, then the renames durable. An
     * unchanged page needs no rename: the final sync still covers a rename a
     * failed earlier checkpoint left undurable. */
    if (rc == EFS_OK) rc = death_sync_meta(s);
    for (uint32_t k = 0; k < np; k++) {
        if (rc == EFS_OK && rename(pend[k].tmp, pend[k].path)) rc = EFS_ERR_IO;
        if (rc != EFS_OK && pend[k].tmp) unlink(pend[k].tmp);
    }
    if (rc == EFS_OK) rc = death_sync_meta(s);
    __atomic_fetch_add(&inode_gc_epoch, 1, __ATOMIC_RELEASE);
    for (uint32_t k = 0; k < np; k++) { free(pend[k].tmp); free(pend[k].path); }
    free(pend);
    free(refs);
    return rc;
}

/* W124: payload bytes this node stores for one inode (all roots). */
static int inode_dir_payload(const char *path, uint64_t *payload, unsigned depth)
{
    DIR *dir = opendir(path);
    if (!dir)
        return errno == ENOENT ? EFS_OK : EFS_ERR_IO;
    int rc = EFS_OK;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dir);
        if (!de) { if (errno) rc = EFS_ERR_IO; break; }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char child[8192];
        if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >= (int)sizeof(child)) {
            rc = EFS_ERR_IO; break;
        }
        struct stat st;
        if (lstat(child, &st)) { if (errno == ENOENT) continue; rc = EFS_ERR_IO; break; }
        if (S_ISDIR(st.st_mode)) {
            if (depth >= 1) { rc = EFS_ERR_IO; break; }
            rc = inode_dir_payload(child, payload, depth + 1);
            if (rc != EFS_OK) break;
        } else if (S_ISREG(st.st_mode)) {
            uint64_t p = 0;
            if (fragment_payload_bytes((uint64_t)st.st_size, &p) != EFS_OK)
                p = (uint64_t)st.st_size;
            *payload += p;
        }
    }
    closedir(dir);
    return rc;
}
int server_gc_inode_payload(struct efsd_server *s, efs_export_id_t eid, efs_ino_t ino,
                            uint64_t *payload)
{
    if (!s || !eid || !payload) return EFS_ERR_INVAL;
    *payload = 0;
    for (uint32_t r = 0; r < (s->storage_path_count ? s->storage_path_count : 1); r++) {
        char path[8192];
        format_ino_chunk_dir(path, sizeof(path), s->storage_paths[r], eid, ino, 0);
        *strrchr(path, '/') = '\0';
        int rc = inode_dir_payload(path, payload, 0);
        if (rc != EFS_OK) return rc;
    }
    return EFS_OK;
}

/* W124: durably delete one fenced inode's fragments on every root, then its
 * empty scaffolding. Runs without the inode guard: the fence (journalled
 * before acceptance) already refuses every later PUT, and a PUT that held the
 * guard before the fence finished first. Idempotent. */
int server_gc_inode_delete(struct efsd_server *s, efs_export_id_t eid, efs_ino_t ino,
                           uint64_t *counted, int durable)
{
    if (!s || !eid || ino <= EFS_ROOT_INO || efs_ino_is_meta_table(ino)) return EFS_ERR_INVAL;
    int rc = EFS_OK;
    t_gc_counted = counted;
    t_gc_defer_sync = !durable;
    for (uint32_t r = 0;
         rc == EFS_OK && r < (s->storage_path_count ? s->storage_path_count : 1); r++) {
        char path[8192];
        format_ino_chunk_dir(path, sizeof(path), s->storage_paths[r], eid, ino, 0);
        *strrchr(path, '/') = '\0';
        do {
            unsigned left = 4096;
            rc = gc_dead_directory(s, path, &left, 0);
        } while (rc == EFS_ERR_BUSY);
        if (rc == EFS_OK) rc = gc_dead_ancestors(s, path);
    }
    t_gc_counted = NULL;
    t_gc_defer_sync = 0;
    if (rc != EFS_OK)
        __atomic_fetch_add(&s->gc_delete_errors, 1, __ATOMIC_RELAXED);
    return rc;
}

/* W124: make every unlink and rmdir on the storage roots durable, one
 * syncfs per distinct filesystem. */
int server_gc_superseded_delete(struct efsd_server *s, efs_export_id_t eid, efs_ino_t ino,
                                uint32_t ci, uint32_t fi, uint64_t gen,
                                const uint8_t sum[EFS_HASH_SIZE])
{
    if (!s || !eid || ino <= EFS_ROOT_INO || efs_ino_is_meta_table(ino) || !gen ||
        fi >= EFS_NUM_FRAGMENTS || !sum) return EFS_ERR_INVAL;
    struct efs_export ex;
    memset(&ex, 0, sizeof(ex));
    ex.id = eid;
    uint64_t saved = efs_tls_chunk_gen;
    efs_tls_chunk_gen = gen;
    int rc = server_delete_fragment_if_sum(s, &ex, ino, ci, fi, sum);
    efs_tls_chunk_gen = saved;
    if (rc == EFS_ERR_EXIST) rc = EFS_OK; /* reused slot: the dead generation is gone */
    if (rc != EFS_OK) return rc;
    /* A packed or otherwise emptied inode keeps no directories. Under the
     * inode's write guard a PUT cannot be between its mkdir and create. */
    pthread_rwlock_t *g = server_inode_guard(eid, ino);
    pthread_rwlock_wrlock(g);
    for (uint32_t r = 0; rc == EFS_OK && r < (s->storage_path_count ? s->storage_path_count : 1); r++) {
        char path[8192];
        format_ino_chunk_dir(path, sizeof(path), s->storage_paths[r], eid, ino, ci);
        if (rmdir(path)) {
            if (errno == ENOENT) continue;
            if (errno == ENOTEMPTY || errno == EEXIST) continue;
            rc = EFS_ERR_IO;
            break;
        }
        char *slash = strrchr(path, '/');
        if (!slash) { rc = EFS_ERR_IO; break; }
        *slash = '\0';
        if (rmdir(path)) {
            if (errno == ENOTEMPTY || errno == EEXIST || errno == ENOENT) {
                rc = fragment_parent_sync(s, path);
                continue;
            }
            rc = EFS_ERR_IO;
            break;
        }
        rc = fragment_parent_sync(s, path);
        if (rc == EFS_OK) rc = gc_dead_ancestors(s, path);
    }
    pthread_rwlock_unlock(g);
    return rc;
}

int server_gc_sync_roots(struct efsd_server *s)
{
    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;
    dev_t seen[EFS_MAX_STORAGE_PATHS];
    uint32_t nseen = 0;
    for (uint32_t r = 0; r < n; r++) {
        struct stat st;
        if (stat(s->storage_paths[r], &st)) return EFS_ERR_IO;
        uint32_t j;
        for (j = 0; j < nseen && seen[j] != st.st_dev; j++) {}
        if (j < nseen) continue;
        seen[nseen++] = st.st_dev;
        int fd = open(s->storage_paths[r], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) return EFS_ERR_IO;
        uint64_t start = gc_clock_us();
        int rc = syncfs(fd);
        gc_io_add(s, 2, start, 0, rc < 0);
        close(fd);
        if (rc) return EFS_ERR_IO;
    }
    return EFS_OK;
}

/* direct: caller decides per-ino — data fragments only. Metadata pages
 * stay buffered. rec, when non-NULL, receives the I25 fragment record
 * stored at offset want_len (the start of a 4 KiB O_DIRECT tail page, or
 * EFS_FRAG_RECORD_BYTES buffered bytes); *rec_ok says whether all of its
 * bytes were present. The caller verifies it (efs_frag_verify). */
static int read_file_bytes(const char *path,
                           uint8_t *buf, uint32_t want_len, uint32_t *got,
                           int direct, uint8_t *rec, int *rec_ok)
{
    int flags = O_RDONLY;
    if (direct)
        flags |= O_DIRECT;
    int fd = open(path, flags);
    if (fd < 0)
        return EFS_ERR_NOT_FOUND;

    ssize_t n;
    if (direct) {
        uint8_t *aligned = NULL;
        uint32_t alloc = (want_len + 4095u) & ~4095u;
        if (alloc < 4096)
            alloc = 4096;
        if (posix_memalign((void **)&aligned, 4096, alloc) != 0) {
            close(fd);
            return EFS_ERR_NOMEM;
        }
        n = read(fd, aligned, alloc);
        if (n > 0) {
            uint32_t copy = (uint32_t)n;
            if (copy > want_len)
                copy = want_len;
            memcpy(buf, aligned, copy);
            *got = copy;
        } else {
            *got = 0;
        }
        free(aligned);
    } else {
        n = read(fd, buf, want_len);
        *got = (n > 0) ? (uint32_t)n : 0;
    }
    if (rec && rec_ok && n > 0) {
        *rec_ok = 0;
        if (direct) {
            uint8_t *tail = NULL;
            if (posix_memalign((void **)&tail, 4096, 4096) == 0) {
                ssize_t rn = pread(fd, tail, 4096, (off_t)want_len);
                /* Buffered writers persist only the record, whereas direct
                 * writers pad it to an aligned page. An aligned O_DIRECT
                 * read may return a short final page at EOF. Both layouts
                 * carry the same complete record; require its bytes. */
                if (rn >= (ssize_t)EFS_FRAG_RECORD_BYTES) {
                    memcpy(rec, tail, EFS_FRAG_RECORD_BYTES);
                    *rec_ok = 1;
                }
                free(tail);
            }
        } else {
            uint8_t tmp[EFS_FRAG_RECORD_BYTES];
            ssize_t rn = pread(fd, tmp, EFS_FRAG_RECORD_BYTES, (off_t)want_len);
            if (rn == (ssize_t)EFS_FRAG_RECORD_BYTES) {
                memcpy(rec, tmp, EFS_FRAG_RECORD_BYTES);
                *rec_ok = 1;
            }
        }
    }
    close(fd);
    if (n < 0)
        return EFS_ERR_IO;
    return EFS_OK;
}

struct put_sync_root;
struct shard_io_arg {
    int sync_write;
    struct efsd_server *s;
    char path[8192];
    uint8_t *buf;
    uint32_t len;
    int is_write;
    int direct; /* data fragments only; meta stays buffered */
    int no_create; /* write only if the file already exists (overwrite probe) */
    int excl;      /* O_CREAT|O_EXCL: EEXIST -> EFS_ERR_EXIST, no second open */
    const uint8_t *rec; /* EFS_FRAG_RECORD_BYTES I25 record after the payload */
    int result;
    /* Data fragments are acknowledged after their write and made durable
     * by the root's background barrier (put_sync_root). */
    struct put_sync_root *sync_root;
};

/* Preserve buffered extents for same-size rewrites, while retaining the exact payload
 * and checksum-tail length for shrinking writes. Never acknowledge failure
 * to establish that length. This does not add or remove a durability barrier. */
static int finish_shard_write(int fd, size_t bytes, int sync_write, int direct)
{
    struct stat st;
    int rc;
    do {
        rc = fstat(fd, &st);
    } while (rc < 0 && errno == EINTR);
    /* A completed write already extends the file to its new end. Avoid
     * another inode-size transaction unless an older, longer tail remains. */
    int resized = rc == 0 && st.st_size != (off_t)bytes;
    if (resized) {
        do {
            rc = ftruncate(fd, (off_t)bytes);
        } while (rc < 0 && errno == EINTR);
    }
    /* O_SYNC persists direct writes; only a later length change needs
     * another barrier. Buffered benchmark semantics retain their barrier. */
    if (rc == 0 && sync_write && (!direct || resized)) {
        do { rc = fsync(fd); } while (rc < 0 && errno == EINTR);
    }
    /* Close may report delayed storage errors even after a successful
     * write. Never retry close: the descriptor may already be released. */
    int close_rc = close(fd);
    if (rc == 0)
        rc = close_rc;
    return rc == 0 ? EFS_OK : EFS_ERR_IO;
}

#ifndef EFS_BENCH_BUILD
static struct directory_sync_group fragment_directory_syncs = DIRECTORY_SYNC_GROUP_INIT;
static int fragment_directory_barrier(void *context, int fd)
{
    (void)context;
    int rc;
    do { rc=fsync(fd); } while (rc<0 && errno==EINTR);
    return rc<0 ? EFS_ERR_IO : EFS_OK;
}
static int fragment_directory_group_barrier(void *context, int fd)
{
    (void)context;
    return directory_sync_group_run(&fragment_directory_syncs, fd,
                                    fragment_directory_barrier, NULL);
}
/* Startup/attach seals each canonical root's ancestry before publishing its
 * initialized target. Runtime PUT fences its containing directory every time;
 * pinned inode proofs may elide only previously sealed, unchanged ancestry.
 * Unknown/uninitialized roots retain the full ancestry barrier. */
static int sync_fragment_parents(struct efsd_server *s, const char *path)
{
    char parent[8192], root[EFS_MAX_PATH] = {0};
    size_t n = strlen(path);
    if (!n || n >= sizeof(parent)) return EFS_ERR_INVAL;
    if (s) {
        size_t best = 0;
        pthread_mutex_lock(&s->lock);
        for (uint32_t i = 0; i < s->storage_path_count; ++i) {
            const char *candidate = s->storage_paths[i];
            size_t length = strlen(candidate);
            if (!s->storage_roots[i] || !length || length <= best ||
                length >= n || strncmp(path, candidate, length) ||
                (length != 1 && path[length] != '/')) continue;
            memcpy(root, candidate, length + 1);
            best = length;
        }
        pthread_mutex_unlock(&s->lock);
    }
    /* Fragments use the same bounded pinned-inode ancestry proof as metadata
     * fragments. This never skips the changed containing directory or caches
     * a failed barrier. Root attach/close owns
     * descriptor lifetime; fallback below covers uninitialized/unknown roots. */
    if (root[0]) return server_storage_sync_parents_with(s, path,
                                      fragment_directory_group_barrier, NULL);
    memcpy(parent, path, n + 1);
    char *slash = strrchr(parent, '/');
    if (!slash) strcpy(parent, ".");
    else if (slash == parent) parent[1] = '\0';
    else *slash = '\0';
    for (;;) {
        int fd, rc;
        do { fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC); }
        while (fd < 0 && errno == EINTR);
        if (fd < 0) return EFS_ERR_IO;
        do { rc = fsync(fd); } while (rc < 0 && errno == EINTR);
        int closed = close(fd);
        if (rc < 0 || closed < 0) return EFS_ERR_IO;
        if (!strcmp(parent, root) || !strcmp(parent, "/") ||
            !strcmp(parent, ".")) break;
        slash = strrchr(parent, '/');
        if (!slash) strcpy(parent, ".");
        else if (slash == parent) parent[1] = '\0';
        else *slash = '\0';
    }
    return EFS_OK;
}

#define XFS_SUPER_MAGIC_ 0x58465342

/* Index of the attached storage root containing path (longest prefix), or
 * -1. root receives its path. */
static int fragment_root_lookup(struct efsd_server *s, const char *path,
                                char root[EFS_MAX_PATH])
{
    size_t n = strlen(path), best = 0;
    int idx = -1;
    pthread_mutex_lock(&s->lock);
    for (uint32_t i = 0; i < s->storage_path_count && i < EFS_MAX_STORAGE_PATHS; ++i) {
        const char *candidate = s->storage_paths[i];
        size_t length = strlen(candidate);
        if (!s->storage_roots[i] || !length || length <= best ||
            length >= n || strncmp(path, candidate, length) ||
            (length != 1 && path[length] != '/')) continue;
        memcpy(root, candidate, length + 1);
        best = length;
        idx = (int)i;
    }
    pthread_mutex_unlock(&s->lock);
    return idx;
}

/* ---- Asynchronous PUT acknowledgement ----------------------------------
 * The only data-fragment PUT mode: NFS unstable/commit
 * (docs/status/intel-async-durability-20261015.md). A fragment is ACKed
 * after its write and close; it joins its root's open epoch. A per-root
 * syncer runs one barrier when the epoch is EFS_PUT_SYNC_MS old (default
 * 200, 1..10000), holds EFS_PUT_SYNC_BYTES (default 256 MiB), or a
 * DATA_COMMIT (client fsync) asks; PUTs wait above twice that byte bound.
 * Smaller values make durability more aggressive.
 *
 * Barrier: syncfs(root) (writeback and journal commit), then the epoch
 * record <root>/.efs-durable-epoch rewritten and fdatasynced. That
 * fdatasync of rewritten data issues a device cache flush even when the
 * journal had nothing to write, which also covers O_DIRECT overwrites
 * that changed no metadata. The record names the last durable (boot,
 * seq) and is written only after the fragments it covers are durable.
 * A failed barrier is sticky until restart: COMMIT and fsync return EIO;
 * PUTs continue. Shutdown runs a final barrier and marks the record
 * clean; an unclean restart logs the exposure and verifies the checksum
 * tails of data fragments modified since the last durable time (report
 * only: parity reads and scrub repair a bad fragment). Metadata-table
 * fragments keep their per-file barriers. */
#define PUT_SYNC_RECORD ".efs-durable-epoch"
#define PUT_SYNC_RECORD_LEN 192
struct put_sync_root {
    char root[EFS_MAX_PATH];
    int fd, record_fd;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread, scanner;
    int thread_started, scanner_started;
    uint32_t boot, root_count;
    uint64_t verifier; /* retained only across a proven all-root clean stop */
    uint64_t seq, durable_seq;            /* open epoch, last durable epoch */
    uint64_t cur_bytes, cur_frags, cur_first_ns;
    uint64_t inflight_bytes, inflight_frags, inflight_first_ns;
    int want_now, stop, error;
    int recovery_uncertain; /* persists until verified recovery or fresh format */
    uint64_t acked_bytes, synced_bytes, barriers, failures;
    uint64_t last_barrier_ns, last_barrier_us;
    uint32_t prev_clean;
    uint64_t prev_durable_ns;
    uint64_t scanned, bad;
    int scan_done;
    /* The record as found at attach, restored if startup is abandoned
     * before this boot could acknowledge any PUT (see put_sync_abandon). */
    uint8_t prev_record[PUT_SYNC_RECORD_LEN];
    int prev_existed, prev_record_ok;
};
static struct put_sync_root put_sync_roots[EFS_MAX_STORAGE_PATHS];
static int put_sync_state[EFS_MAX_STORAGE_PATHS]; /* 0 unset, 1 async, -1 not async */
static int put_sync_serving, put_sync_stopped; /* under put_sync_mu */
static pthread_mutex_t put_sync_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t put_sync_window_ms = 200, put_sync_window_bytes = 256ull << 20;

static uint64_t put_sync_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t put_sync_wall_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void put_sync_config(void)
{
    static int done;
    if (!done) {
        done = 1;
        const char *w = getenv("EFS_PUT_SYNC_MS");
        if (w && *w) {
            unsigned long v = strtoul(w, NULL, 10);
            if (v >= 1 && v <= 10000) put_sync_window_ms = v;
        }
        const char *b = getenv("EFS_PUT_SYNC_BYTES");
        if (b && *b) {
            unsigned long long v = strtoull(b, NULL, 10);
            if (v >= (1ull << 20) && v <= (16ull << 30)) put_sync_window_bytes = v;
        }
    }
}

/* V2 is a fixed, checksummed record. Clean-restart reuse must not trust a
 * torn combination of old clean state and a new verifier. A malformed record
 * fails closed; the v1 reader is migration-only and cannot authorize reuse. */
struct put_sync_record {
    uint32_t boot, clean, roots;
    uint64_t seq, time_ns, verifier;
};
static int put_sync_record_read(int fd, struct put_sync_record *out)
{
    uint8_t rec[PUT_SYNC_RECORD_LEN + 1]={0};
    ssize_t n;
    do { n=pread(fd,rec,PUT_SYNC_RECORD_LEN+1,0); } while(n<0 && errno==EINTR);
    if(n!=PUT_SYNC_RECORD_LEN)return EFS_ERR_IO;
    memset(out,0,sizeof(*out));
    if(!memcmp(rec,"EFSDUR02",8)) {
        if(efs_put_epoch_read(rec+184)!=efs_crc32(rec,184))return EFS_ERR_IO;
        uint64_t boot=efs_put_epoch_read(rec+8),clean=efs_put_epoch_read(rec+16);
        uint64_t roots=efs_put_epoch_read(rec+48);
        if(!boot || boot>=UINT32_MAX || clean>1 || !roots || roots>EFS_MAX_STORAGE_PATHS ||
           !efs_put_epoch_read(rec+40))return EFS_ERR_IO;
        out->boot=(uint32_t)boot;out->clean=(uint32_t)clean;out->roots=(uint32_t)roots;
        out->seq=efs_put_epoch_read(rec+24);out->time_ns=efs_put_epoch_read(rec+32);
        out->verifier=efs_put_epoch_read(rec+40);
        return EFS_OK;
    }
    unsigned boot=0,clean=0;unsigned long long seq=0,ns=0;int used=0;
    if(sscanf((char *)rec,"efs-durable-epoch v1 boot=%u seq=%llu time_ns=%llu clean=%u%n",
              &boot,&seq,&ns,&clean,&used)!=4 || !boot || boot>=UINT32_MAX || clean>1)
        return EFS_ERR_IO;
    for(int i=used;i<PUT_SYNC_RECORD_LEN;i++)
        if(rec[i]!=' ' && rec[i]!='\n')return EFS_ERR_IO;
    out->boot=boot;out->clean=clean;out->seq=seq;out->time_ns=ns;
    return EFS_OK;
}

static int put_sync_record_write(struct put_sync_root *r, uint64_t seq, int clean)
{
    uint8_t rec[PUT_SYNC_RECORD_LEN]={0};
    if(!r->verifier || !r->root_count)return EFS_ERR_IO;
    memcpy(rec,"EFSDUR02",8);
    efs_put_epoch_write(rec+8,r->boot);efs_put_epoch_write(rec+16,clean);
    efs_put_epoch_write(rec+24,seq);efs_put_epoch_write(rec+32,put_sync_wall_ns());
    efs_put_epoch_write(rec+40,r->verifier);efs_put_epoch_write(rec+48,r->root_count);
    efs_put_epoch_write(rec+184,efs_crc32(rec,184));
    ssize_t w;
    do { w = pwrite(r->record_fd, rec, sizeof(rec), 0); } while (w < 0 && errno == EINTR);
    if (w != (ssize_t)sizeof(rec)) return EFS_ERR_IO;
    int rc;
    do { rc = fdatasync(r->record_fd); } while (rc < 0 && errno == EINTR);
    return rc ? EFS_ERR_IO : EFS_OK;
}

/* Inspect ALL roots before marking any of them running. Reuse is safe only
 * when every previous root was clean, agrees on the verifier, and the root
 * count is unchanged. Missing/replaced/removed/legacy/unclean roots rotate it.
 * No retained process verifier is a claim that uncertain recovery succeeded. */
static int put_sync_reuse_epoch(struct efsd_server *s,uint64_t *epoch)
{
    uint64_t saved=0;int reusable=s->storage_path_count>0;
    *epoch=0;
    if(s->storage_path_count>EFS_MAX_STORAGE_PATHS)return EFS_ERR_INVAL;
    for(uint32_t i=0;i<s->storage_path_count;i++) {
        char path[EFS_MAX_PATH+32];
        snprintf(path,sizeof(path),"%s/%s",s->storage_paths[i],PUT_SYNC_RECORD);
        int fd=open(path,O_RDONLY|O_CLOEXEC);
        if(fd<0){if(errno==ENOENT){reusable=0;continue;}return EFS_ERR_IO;}
        struct put_sync_record rec;int rc=put_sync_record_read(fd,&rec);close(fd);
        if(rc)return rc;
        if(!rec.clean || !rec.verifier || rec.roots!=s->storage_path_count)reusable=0;
        if(saved && saved!=rec.verifier)reusable=0;
        if(!saved)saved=rec.verifier;
    }
    if(reusable)*epoch=saved;
    return EFS_OK;
}

/* One barrier for everything acknowledged before it started. Called by the
 * syncer only, with mu held; drops mu during I/O. */
static void put_sync_barrier_locked(struct put_sync_root *r)
{
    uint64_t target = r->seq;
    r->inflight_bytes = r->cur_bytes;
    r->inflight_frags = r->cur_frags;
    r->inflight_first_ns = r->cur_first_ns;
    r->cur_bytes = r->cur_frags = r->cur_first_ns = 0;
    r->seq++;
    r->want_now = 0;
    pthread_mutex_unlock(&r->mu);
    uint64_t t0 = put_sync_now_ns();
    int rc = syncfs(r->fd) == 0 ? EFS_OK : EFS_ERR_IO;
    if (rc == EFS_OK)
        rc = put_sync_record_write(r, target, 0);
    uint64_t t1 = put_sync_now_ns();
    pthread_mutex_lock(&r->mu);
    r->barriers++;
    r->last_barrier_ns = t1;
    r->last_barrier_us = (t1 - t0) / 1000;
    if (rc == EFS_OK) {
        r->durable_seq = target;
        r->synced_bytes += r->inflight_bytes;
    } else {
        /* Durability of these writes is unknown: keep counting them as
         * unsynced and stop acknowledging asynchronously. */
        r->failures++;
        if (!r->error)
            fprintf(stderr, "store: put sync root=%s barrier failed seq=%llu; "
                    "COMMIT and fsync return EIO until restart\n",
                    r->root, (unsigned long long)target);
        r->error = 1;
        r->cur_bytes += r->inflight_bytes;
        r->cur_frags += r->inflight_frags;
        if (r->inflight_first_ns && (!r->cur_first_ns || r->inflight_first_ns < r->cur_first_ns))
            r->cur_first_ns = r->inflight_first_ns;
    }
    r->inflight_bytes = r->inflight_frags = r->inflight_first_ns = 0;
    pthread_cond_broadcast(&r->cv);
}

static void *put_sync_thread(void *arg)
{
    struct put_sync_root *r = arg;
    pthread_mutex_lock(&r->mu);
    for (;;) {
        uint64_t now = put_sync_now_ns();
        int due = r->cur_frags &&
                  (r->want_now || r->cur_bytes >= put_sync_window_bytes ||
                   now - r->cur_first_ns >= put_sync_window_ms * 1000000ull);
        if (due) {
            put_sync_barrier_locked(r);
            continue;
        }
        if (r->want_now && !r->cur_frags) {
            r->want_now = 0; /* nothing open; waiters are already covered */
            pthread_cond_broadcast(&r->cv);
        }
        if (r->stop)
            break;
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        uint64_t wait_ns = r->cur_frags
            ? put_sync_window_ms * 1000000ull - (now - r->cur_first_ns)
            : 1000000000ull;
        until.tv_sec += (time_t)(wait_ns / 1000000000ull);
        until.tv_nsec += (long)(wait_ns % 1000000000ull);
        if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&r->cv, &r->mu, &until);
    }
    pthread_mutex_unlock(&r->mu);
    return NULL;
}

/* Count an acknowledged-but-unsynced fragment. Waits only while the root
 * holds more than twice the byte window (bounded exposure). */
static void put_sync_note(struct put_sync_root *r, uint64_t bytes)
{
    pthread_mutex_lock(&r->mu);
    if (!r->cur_frags)
        r->cur_first_ns = put_sync_now_ns();
    r->cur_bytes += bytes;
    r->cur_frags++;
    r->acked_bytes += bytes;
    if (r->cur_bytes >= put_sync_window_bytes)
        pthread_cond_broadcast(&r->cv);
    while (r->cur_bytes + r->inflight_bytes > 2 * put_sync_window_bytes &&
           !r->stop && !(r->error && !r->inflight_frags))
        pthread_cond_wait(&r->cv, &r->mu);
    pthread_mutex_unlock(&r->mu);
}

/* Wait until everything acknowledged before this call is durable. */
static int put_sync_commit_root(struct put_sync_root *r)
{
    pthread_mutex_lock(&r->mu);
    uint64_t want = r->cur_frags ? r->seq
                  : r->inflight_frags ? r->seq - 1 : r->durable_seq;
    if (r->cur_frags) {
        r->want_now = 1;
        pthread_cond_broadcast(&r->cv);
    }
    while (r->durable_seq < want && !r->error && !r->stop)
        pthread_cond_wait(&r->cv, &r->mu);
    int rc = r->durable_seq >= want && !r->error && !r->recovery_uncertain ? EFS_OK : EFS_ERR_IO;
    pthread_mutex_unlock(&r->mu);
    return rc;
}

/* Report-only verification after an unclean stop: data fragments modified
 * since the last durable barrier (minus a margin) must still carry a valid
 * checksum tail. A missing or torn fragment is served from parity on read
 * and rebuilt by scrub; this pass bounds and reports the exposure. */
static void put_sync_scan_dir(struct put_sync_root *r, const char *dir,
                              uint64_t since_ns, int depth)
{
    DIR *d = opendir(dir);
    if (!d || depth > 32) { if (d) closedir(d); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char path[8192];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            put_sync_scan_dir(r, path, since_ns, depth + 1);
            continue;
        }
        if (!S_ISREG(st.st_mode))
            continue;
        uint64_t mt = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
        if (mt < since_ns)
            continue;
        uint64_t payload = 0;
        int ok = 0;
        if (fragment_payload_bytes((uint64_t)st.st_size, &payload) == EFS_OK &&
            payload <= (64ull << 20)) {
            uint8_t *buf = malloc((size_t)st.st_size);
            int fd = buf ? open(path, O_RDONLY | O_CLOEXEC) : -1;
            if (fd >= 0) {
                ssize_t got = pread(fd, buf, (size_t)st.st_size, 0);
                close(fd);
                uint8_t sum[EFS_HASH_SIZE], want[EFS_HASH_SIZE];
                struct efs_frag_ident id;
                /* Self-consistency of the I25 record (the identity is the
                 * name; GET checks it against the request) plus payload. */
                if (got == (ssize_t)st.st_size &&
                    efs_frag_record_parse(buf + payload,
                                          (size_t)st.st_size - (size_t)payload,
                                          &id, want) == EFS_OK &&
                    id.payload_len == payload) {
                    efs_hash(buf, (size_t)payload, sum);
                    ok = !memcmp(sum, want, EFS_HASH_SIZE);
                }
            }
            free(buf);
        }
        pthread_mutex_lock(&r->mu);
        r->scanned++;
        if (!ok) {
            r->bad++;
            if (r->bad <= 64)
                fprintf(stderr, "store: put sync recovery bad fragment %s\n", path);
        }
        pthread_mutex_unlock(&r->mu);
    }
    closedir(d);
}

static void *put_sync_scan_thread(void *arg)
{
    struct put_sync_root *r = arg;
    char data[EFS_MAX_PATH + 8];
    snprintf(data, sizeof(data), "%s/data", r->root);
    uint64_t margin = 5000000000ull;
    uint64_t since = r->prev_durable_ns > margin ? r->prev_durable_ns - margin : 0;
    put_sync_scan_dir(r, data, since, 0);
    pthread_mutex_lock(&r->mu);
    r->scan_done = 1;
    fprintf(stderr, "store: put sync recovery root=%s scanned=%llu bad=%llu\n", r->root,
            (unsigned long long)r->scanned, (unsigned long long)r->bad);
    pthread_mutex_unlock(&r->mu);
    return NULL;
}

/* Attach one root. Reads the previous record, starts a new boot, and on an
 * unclean previous stop starts the verification scan. */
static void put_sync_attach(struct put_sync_root *r, const char *root,
                            uint64_t verifier, uint32_t roots)
{
    snprintf(r->root, sizeof(r->root), "%s", root);
    pthread_mutex_init(&r->mu, NULL);
    pthread_cond_init(&r->cv, NULL);
    r->fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct statfs fs;
    if (r->fd < 0 || fstatfs(r->fd, &fs) != 0) {
        if (r->fd >= 0) close(r->fd);
        r->fd = -1;
        return;
    }
    char path[EFS_MAX_PATH + 32];
    snprintf(path, sizeof(path), "%s/%s", root, PUT_SYNC_RECORD);
    int existed = access(path, F_OK) == 0;
    r->record_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (r->record_fd < 0) {
        close(r->fd);
        r->fd = -1;
        return;
    }
    struct put_sync_record previous={.clean=1};
    r->prev_existed=existed;
    if (existed) {
        ssize_t n;
        do { n=pread(r->record_fd,r->prev_record,sizeof(r->prev_record),0); } while(n<0 && errno==EINTR);
        r->prev_record_ok = n==(ssize_t)sizeof(r->prev_record);
    }
    if (existed && put_sync_record_read(r->record_fd,&previous)!=EFS_OK) {
        fprintf(stderr, "store: invalid durability record root=%s; refusing startup\n", root);
        close(r->record_fd); close(r->fd); r->fd = -1; return;
    }
    r->prev_clean = !existed || previous.clean;
    r->recovery_uncertain = !r->prev_clean;
    /* A checksum scan cannot find lost creates. Keep clean=0 until verified
     * recovery, including across later orderly shutdowns. */
    r->prev_durable_ns = previous.time_ns;
    r->boot = previous.boot + 1;
    r->verifier=verifier;r->root_count=roots;
    r->seq = 1;
    r->durable_seq = 0;
    if (put_sync_record_write(r, 0, 0) != EFS_OK) {
        close(r->record_fd);
        close(r->fd);
        r->fd = -1;
        return;
    }
    if (!existed) {
        int rc;
        do { rc = fsync(r->fd); } while (rc < 0 && errno == EINTR);
        if (rc) {
            close(r->record_fd); close(r->fd); r->fd = -1; return;
        }
    }
    if (!r->prev_clean) {
        fprintf(stderr, "store: root=%s recovery uncertain; COMMIT returns EIO until verified recovery or fresh format\n", root);
        fprintf(stderr, "store: put sync root=%s unclean previous stop: writes acknowledged "
                "after boot=%u seq=%llu were not durable; verifying fragments modified since\n",
                root, previous.boot, (unsigned long long)previous.seq);
        r->scanner_started = pthread_create(&r->scanner, NULL, put_sync_scan_thread, r) == 0;
    } else {
        r->scan_done = 1;
    }
    r->thread_started = pthread_create(&r->thread, NULL, put_sync_thread, r) == 0;
    if (!r->thread_started) {
        close(r->record_fd);
        close(r->fd);
        r->fd = -1;
        return;
    }
    fprintf(stderr, "store: put sync root=%s async boot=%u window_ms=%llu window_bytes=%llu fs=%s\n",
            root, r->boot, (unsigned long long)put_sync_window_ms,
            (unsigned long long)put_sync_window_bytes,
            (unsigned long)fs.f_type == XFS_SUPER_MAGIC_ ? "xfs" : "other");
}

static struct put_sync_root *put_sync_root_for(struct efsd_server *s, const char *path)
{
    if (!s)
        return NULL;
    put_sync_config();
    char root[EFS_MAX_PATH] = {0};
    int idx = fragment_root_lookup(s, path, root);
    if (idx < 0)
        return NULL;
    pthread_mutex_lock(&put_sync_mu);
    if (!put_sync_state[idx]) {
        put_sync_attach(&put_sync_roots[idx], root, s->put_boot_epoch, s->storage_path_count);
        put_sync_state[idx] = put_sync_roots[idx].fd >= 0 ? 1 : -1;
        if (put_sync_state[idx] < 0)
            fprintf(stderr, "store: put sync root=%s cannot attach (epoch record "
                    "unavailable)\n", root);
    }
    struct put_sync_root *r = put_sync_state[idx] > 0 ? &put_sync_roots[idx] : NULL;
    pthread_mutex_unlock(&put_sync_mu);
    return r;
}

/* Attach every root at startup so an unclean previous stop is detected and
 * reported before the first PUT. A root that cannot attach stops startup:
 * there is no other data PUT mode to fall back to. */
int server_put_sync_init(struct efsd_server *s)
{
    if (!s) return EFS_ERR_INVAL;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return EFS_ERR_IO;
    ssize_t n;
    do { n = read(fd, &s->put_boot_epoch, sizeof(s->put_boot_epoch)); } while (n < 0 && errno == EINTR);
    close(fd);
    if (n != sizeof(s->put_boot_epoch) || !s->put_boot_epoch) return EFS_ERR_IO;
    uint64_t retained=0;
    int rc=put_sync_reuse_epoch(s,&retained);
    if(rc!=EFS_OK)return rc;
    if(retained)s->put_boot_epoch=retained;
    for (uint32_t i = 0; i < s->storage_path_count && i < EFS_MAX_STORAGE_PATHS; ++i) {
        char probe[EFS_MAX_PATH + 16];
        snprintf(probe, sizeof(probe), "%s/data/x", s->storage_paths[i]);
        if (!put_sync_root_for(s, probe))
            return EFS_ERR_IO;
    }
    return EFS_OK;
}

int server_put_sync_commit(struct efsd_server *s, uint32_t *roots)
{
    (void)s;
    int rc = EFS_OK;
    uint32_t n = 0;
    for (uint32_t i = 0; i < EFS_MAX_STORAGE_PATHS; ++i) {
        pthread_mutex_lock(&put_sync_mu);
        int on = put_sync_state[i] > 0;
        pthread_mutex_unlock(&put_sync_mu);
        if (!on)
            continue;
        n++;
        if (put_sync_commit_root(&put_sync_roots[i]) != EFS_OK)
            rc = EFS_ERR_IO;
    }
    if (roots)
        *roots = n;
    return rc;
}

void server_put_sync_stats(struct efsd_server *s, struct efs_msg_sync_stats_reply *out)
{
    memset(out, 0, sizeof(*out));
    out->sync_window_ms = (uint32_t)put_sync_window_ms;
    out->sync_window_bytes = put_sync_window_bytes;
    uint64_t now = put_sync_now_ns();
    for (uint32_t i = 0; s && i < s->storage_path_count && i < EFS_MAX_STORAGE_PATHS &&
                         out->count < EFS_SYNC_STATS_ROOTS; ++i) {
        struct efs_sync_root_stats *o = &out->roots[out->count++];
        snprintf(o->root, sizeof(o->root), "%s", s->storage_paths[i]);
        pthread_mutex_lock(&put_sync_mu);
        int st = put_sync_state[i];
        pthread_mutex_unlock(&put_sync_mu);
        if (st <= 0) {
            o->mode = 0;
            continue;
        }
        struct put_sync_root *r = &put_sync_roots[i];
        pthread_mutex_lock(&r->mu);
        o->mode = 2;
        o->error = (uint32_t)(r->error || r->recovery_uncertain);
        o->unsynced_bytes = r->cur_bytes + r->inflight_bytes;
        o->unsynced_fragments = r->cur_frags + r->inflight_frags;
        uint64_t first = r->inflight_first_ns ? r->inflight_first_ns : r->cur_first_ns;
        o->oldest_unsynced_ms = first ? (now - first) / 1000000 : 0;
        o->acked_bytes = r->acked_bytes;
        o->synced_bytes = r->synced_bytes;
        o->boot = r->boot;
        o->prev_clean = r->prev_clean;
        o->current_seq = r->seq;
        o->durable_seq = r->durable_seq;
        o->barriers = r->barriers;
        o->barrier_failures = r->failures;
        o->last_barrier_ms_ago = r->last_barrier_ns ? (now - r->last_barrier_ns) / 1000000 : 0;
        o->last_barrier_us = r->last_barrier_us;
        o->recovery_scanned = r->scanned;
        o->recovery_bad = r->bad;
        o->recovery_done = (uint32_t)r->scan_done;
        pthread_mutex_unlock(&r->mu);
    }
}

/* From here PUTs may be acknowledged; an exit can no longer prove that
 * this boot changed nothing, so put_sync_abandon becomes a no-op. */
void server_put_sync_serving(void)
{
    pthread_mutex_lock(&put_sync_mu);
    put_sync_serving = 1;
    pthread_mutex_unlock(&put_sync_mu);
}

/* Startup refused after the roots were attached (quota, listen, join...).
 * Attach already rewrote each record as this boot, unclean; left so, the
 * next start would fence every root (recovery uncertain, COMMIT EIO) for a
 * boot that wrote nothing. Before serving, no PUT can have been accepted:
 * restore each untouched root's previous record verbatim (or remove a
 * record this boot created). A crash runs no exit handler and stays
 * unclean; a boot that acknowledged any PUT is never restored. */
void server_put_sync_abandon(void)
{
    pthread_mutex_lock(&put_sync_mu);
    int skip = put_sync_serving || put_sync_stopped;
    put_sync_stopped = 1;
    pthread_mutex_unlock(&put_sync_mu);
    if (skip)
        return;
    for (uint32_t i = 0; i < EFS_MAX_STORAGE_PATHS; ++i) {
        if (put_sync_state[i] <= 0)
            continue;
        struct put_sync_root *r = &put_sync_roots[i];
        pthread_mutex_lock(&r->mu);
        int touched = r->acked_bytes || r->cur_frags || r->inflight_frags ||
                      r->durable_seq;
        r->stop = 1;
        pthread_cond_broadcast(&r->cv);
        pthread_mutex_unlock(&r->mu);
        if (r->thread_started)
            pthread_join(r->thread, NULL);
        if (touched || (r->prev_existed && !r->prev_record_ok))
            continue;
        int ok;
        if (r->prev_existed) {
            ssize_t w;
            do { w = pwrite(r->record_fd, r->prev_record, sizeof(r->prev_record), 0); }
            while (w < 0 && errno == EINTR);
            int rc;
            do { rc = fdatasync(r->record_fd); } while (rc < 0 && errno == EINTR);
            ok = w == (ssize_t)sizeof(r->prev_record) && !rc;
        } else {
            char path[EFS_MAX_PATH + 32];
            snprintf(path, sizeof(path), "%s/%s", r->root, PUT_SYNC_RECORD);
            int rc;
            ok = !unlink(path);
            do { rc = fsync(r->fd); } while (rc < 0 && errno == EINTR);
            ok = ok && !rc;
        }
        fprintf(stderr, "store: put sync root=%s startup abandoned before serving; "
                "previous durability record %s\n", r->root,
                ok ? (r->prev_existed ? "restored" : "removed") : "NOT restored");
    }
}

/* Final barrier, then mark every async root clean. */
int server_put_sync_stop(struct efsd_server *s)
{
    (void)s;
    int all_clean = 1;
    pthread_mutex_lock(&put_sync_mu);
    put_sync_stopped = 1;
    pthread_mutex_unlock(&put_sync_mu);
    for (uint32_t i = 0; i < EFS_MAX_STORAGE_PATHS; ++i) {
        pthread_mutex_lock(&put_sync_mu);
        int on = put_sync_state[i] > 0;
        pthread_mutex_unlock(&put_sync_mu);
        if (!on)
            continue;
        struct put_sync_root *r = &put_sync_roots[i];
        int ok = put_sync_commit_root(r) == EFS_OK;
        pthread_mutex_lock(&r->mu);
        r->stop = 1;
        pthread_cond_broadcast(&r->cv);
        pthread_mutex_unlock(&r->mu);
        if (r->thread_started)
            pthread_join(r->thread, NULL);
        if (r->scanner_started)
            pthread_join(r->scanner, NULL);
        int clean = ok && !r->error &&
                    syncfs(r->fd) == 0 &&
                    put_sync_record_write(r, r->durable_seq, 1) == EFS_OK;
        fprintf(stderr, "store: put sync root=%s stopped %s durable_seq=%llu\n", r->root,
                clean ? "clean" : "UNCLEAN (barrier failed)",
                (unsigned long long)r->durable_seq);
        if (!clean)
            all_clean = 0;
    }
    return all_clean ? 0 : -1;
}
#endif

static int finish_fragment_put(struct shard_io_arg *a, int fd, size_t bytes)
{
#ifdef EFS_BENCH_BUILD
    /* Benchmark sync remains opt-in and keeps its existing O_SYNC semantics. */
    return finish_shard_write(fd, bytes, a->sync_write, a->direct);
#else
    /* Production PUT ACKs must be durable for both buffered and direct I/O.
     * Production does not open O_SYNC, so always establish an explicit file
     * barrier, including same-length direct writes and checksum tails. */
    if (a->sync_root) {
        /* Acknowledged now; durable at the root's next barrier or when a
         * client COMMIT (fsync) asks for one. */
        int rc = finish_shard_write(fd, bytes, 0, a->direct);
        if (rc != EFS_OK)
            return rc;
        put_sync_note(a->sync_root, bytes);
        return EFS_OK;
    }
    int rc = finish_shard_write(fd, bytes, 1, 0);
    return rc == EFS_OK ? sync_fragment_parents(a->s, a->path) : rc;
#endif
}

/* Connection and writer threads reuse aligned pages, then release them at
 * thread retirement. Raw __thread pointers leaked on every short TCP session. */
struct shard_io_buffers {
    uint8_t *tail;
    uint8_t *bounce;
    uint32_t bounce_len;
};
static pthread_key_t shard_io_buffers_key;
static pthread_once_t shard_io_buffers_once = PTHREAD_ONCE_INIT;
static int shard_io_buffers_key_error;
static void shard_io_buffers_free(void *value)
{
    struct shard_io_buffers *buffers = value;
    if (!buffers) return;
    free(buffers->tail);
    free(buffers->bounce);
    free(buffers);
}
static void shard_io_buffers_init(void)
{
    shard_io_buffers_key_error = pthread_key_create(&shard_io_buffers_key,
                                                   shard_io_buffers_free);
}
static struct shard_io_buffers *shard_io_buffers_get(void)
{
    if (pthread_once(&shard_io_buffers_once, shard_io_buffers_init) != 0 ||
        shard_io_buffers_key_error) return NULL;
    struct shard_io_buffers *buffers = pthread_getspecific(shard_io_buffers_key);
    if (!buffers) {
        buffers = calloc(1, sizeof(*buffers));
        if (!buffers) return NULL;
        if (pthread_setspecific(shard_io_buffers_key, buffers) != 0) {
            free(buffers);
            return NULL;
        }
    }
    return buffers;
}

static void *shard_io_thread(void *arg)
{
    struct shard_io_arg *a = arg;
    if (a->is_write) {
        int flags = O_WRONLY;
#ifdef EFS_BENCH_BUILD
        if (a->sync_write) flags |= O_SYNC;
#endif
        if (!a->no_create)
            flags |= O_CREAT;
        if (a->excl)
            flags |= O_EXCL;
        if (a->direct)
            flags |= O_DIRECT;
        int fd = open(a->path, flags, 0644);
        if (fd < 0 && errno == EEXIST && a->excl) {
            a->result = EFS_ERR_EXIST; /* caller: overwrite, do not charge */
            return NULL;
        }
        if (fd < 0 && errno == ENOENT && a->no_create) {
            a->result = EFS_ERR_NOT_FOUND; /* caller: charge quota + create */
            return NULL;
        }
        if (fd < 0 && errno == ENOENT) {
            make_dir_for_file(a->path);
            fd = open(a->path, flags, 0644);
        }
        if (fd < 0) {
            a->result = EFS_ERR_IO;
            return NULL;
        }
        size_t written = 0;
        if (a->direct) {
            uint32_t alloc = (a->len + 4095u) & ~4095u;
            if (alloc < 4096)
                alloc = 4096;
            /* The record lives in a 4 KiB tail after the payload so one
             * O_DIRECT write carries both. No .sum inode. */
            if (a->rec && alloc < a->len + 4096u)
                alloc = a->len + 4096u;
            /* Zero payloads (store-bench / dd if=/dev/zero): write from a
             * process-wide pre-zeroed O_DIRECT buffer — no per-PUT memset
             * or memcpy of the 64 KiB fragment. */
            int is_zero = !a->rec && (a->len > 0) &&
                          (efs_tls_write_known_zero ||
                           efs_bytes_are_zero(a->buf, a->len));
            const uint8_t *wbuf;
            if (is_zero) {
                /* Never free after publish: writers may still reference the
                 * prior buffer. Grow by replacing the pointer without free. */
                static uint8_t *zero_dio;
                static uint32_t zero_dio_alloc;
                static pthread_mutex_t zero_dio_mu = PTHREAD_MUTEX_INITIALIZER;
                pthread_mutex_lock(&zero_dio_mu);
                if (!zero_dio || zero_dio_alloc < alloc) {
                    uint8_t *fresh = NULL;
                    if (posix_memalign((void **)&fresh, 4096, alloc) != 0) {
                        pthread_mutex_unlock(&zero_dio_mu);
                        close(fd);
                        a->result = EFS_ERR_NOMEM;
                        return NULL;
                    }
                    memset(fresh, 0, alloc);
                    zero_dio = fresh;
                    zero_dio_alloc = alloc;
                }
                wbuf = zero_dio;
                pthread_mutex_unlock(&zero_dio_mu);
            } else if (((uintptr_t)a->buf & 4095u) == 0 && a->len > 0 &&
                       (a->len & 4095u) == 0) {
                /* Fast path: the RDMA recv layout places the frame payload
                 * on a 4096 boundary, so O_DIRECT can write straight out of
                 * the recv buffer — no 64 KiB bounce copy per fragment.
                 * Only when no pad is needed: the bounce path zero-pads
                 * short tails and the on-disk image must not change.
                 * With a digest (D20) the 4 KiB tail page is a second
                 * iovec from a per-thread aligned page; one writev carries
                 * payload + tail. Until Oct 1 the digest forced every
                 * fragment through the 64 KiB memcpy below. */
                if (a->rec) {
                    struct shard_io_buffers *buffers = shard_io_buffers_get();
                    if (!buffers || (!buffers->tail &&
                        posix_memalign((void **)&buffers->tail, 4096, 4096) != 0)) {
                        close(fd);
                        a->result = EFS_ERR_NOMEM;
                        return NULL;
                    }
                    memcpy(buffers->tail, a->rec, EFS_FRAG_RECORD_BYTES);
                    memset(buffers->tail + EFS_FRAG_RECORD_BYTES, 0,
                           4096 - EFS_FRAG_RECORD_BYTES);
                    static int once_zc;
                    if (!once_zc && __sync_bool_compare_and_swap(&once_zc, 0, 1))
                        fprintf(stderr, "store: put O_DIRECT zero-copy "
                                "(payload+record writev) len=%u\n", a->len);
                    struct iovec iov[2] = {
                        { .iov_base = a->buf, .iov_len = a->len },
                        { .iov_base = buffers->tail, .iov_len = 4096 },
                    };
                    int rc = writev_all(fd, iov, 2);
                    if (rc != 0) {
                        close(fd);
                        a->result = EFS_ERR_IO;
                        return NULL;
                    }
                    a->result = finish_fragment_put(a, fd, (size_t)a->len + 4096);
                    return NULL;
                }
                wbuf = a->buf;
            } else {
                /* Per-thread O_DIRECT bounce buffer for unaligned sources
                 * (TCP fallback path). posix_memalign+free per PUT was pure
                 * churn, so the buffer is cached per thread. */
                struct shard_io_buffers *buffers = shard_io_buffers_get();
                if (!buffers) {
                    close(fd);
                    a->result = EFS_ERR_NOMEM;
                    return NULL;
                }
                if (buffers->bounce_len < alloc) {
                    free(buffers->bounce);
                    buffers->bounce = NULL;
                    buffers->bounce_len = 0;
                    if (posix_memalign((void **)&buffers->bounce, 4096, alloc) != 0) {
                        close(fd);
                        a->result = EFS_ERR_NOMEM;
                        return NULL;
                    }
                    buffers->bounce_len = alloc;
                }
                static int once_bounce;
                if (a->rec && !once_bounce &&
                    __sync_bool_compare_and_swap(&once_bounce, 0, 1))
                    fprintf(stderr, "store: put O_DIRECT bounce copy "
                            "(buf align=%u len=%u)\n",
                            (unsigned)((uintptr_t)a->buf & 4095u), a->len);
                memcpy(buffers->bounce, a->buf, a->len);
                if (a->len < alloc)
                    memset(buffers->bounce + a->len, 0, alloc - a->len);
                if (a->rec)
                    memcpy(buffers->bounce + a->len, a->rec, EFS_FRAG_RECORD_BYTES);
                wbuf = buffers->bounce;
            }
            while (written < alloc) {
                ssize_t n = write(fd, wbuf + written, alloc - written);
                if (n <= 0) {
                    close(fd);
                    a->result = EFS_ERR_IO;
                    return NULL;
                }
                written += (size_t)n;
            }
        } else {
            while (written < a->len) {
                ssize_t n = write(fd, a->buf + written, a->len - written);
                if (n <= 0) {
                    close(fd);
                    a->result = EFS_ERR_IO;
                    return NULL;
                }
                written += (size_t)n;
            }
            if (a->rec) {
                ssize_t n = write(fd, a->rec, EFS_FRAG_RECORD_BYTES);
                if (n != (ssize_t)EFS_FRAG_RECORD_BYTES) {
                    close(fd);
                    a->result = EFS_ERR_IO;
                    return NULL;
                }
                written += EFS_FRAG_RECORD_BYTES;
            }
        }
        a->result = finish_fragment_put(a, fd, written);
    } else {
        uint32_t got = 0;
        a->result = read_file_bytes(a->path, a->buf, a->len, &got,
                                    a->direct, NULL, NULL);
        if (a->result == EFS_OK && got < a->len)
            memset(a->buf + got, 0, a->len - got);
    }
    return NULL;
}

/* Resolved-location cache for multi-root reads. Adaptive write
 * placement makes the root non-deterministic, so without a hint every GET
 * open()-probes roots×layouts through a deep dir walk — the top server CPU
 * cost at scale. A hit turns the probe into one open; the open itself
 * validates the entry (ENOENT -> reprobe and refill). Caveat: if an
 * overwrite left duplicate fragments on several roots, a cached entry may
 * return a different copy than canonical probe order would (both are
 * checksum-valid generations; the probe order itself is already arbitrary
 * in that case).
 *
 * Process-wide, unlocked. It was per thread, and a GET is served by
 * whichever of the hundreds of conn threads holds the conn, so nearly
 * every GET re-probed (Sep 29 ecopy window on fcstor003: 149 770 openat,
 * 51 130 of them ENOENT, for 845 pread). A torn entry is harmless: a
 * mismatched key misses, a wrong root gets ENOENT from the open and is
 * reprobed. PUT primes it (server_fragment_path). */
/* Sized so a multi-client streaming working set (several thousand
 * fragments) fits; a direct-mapped cache smaller than the cyclical
 * working set thrashes to ~0% hits. */
#define FRAG_LOC_CACHE_SIZE 65536
struct frag_loc {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint64_t gen;
    uint32_t chunk_index;
    uint32_t fragment_index;
    uint8_t root;
    uint8_t valid;
};
static struct frag_loc frag_loc_cache[FRAG_LOC_CACHE_SIZE];

static uint32_t frag_loc_slot(efs_export_id_t export_id, efs_ino_t ino,
                              uint32_t chunk_index, uint32_t fragment_index)
{
    uint64_t h = (uint64_t)ino * 0x9e3779b97f4a7c15ull;
    h ^= (uint64_t)chunk_index * 0xbf58476d1ce4e5b9ull;
    h ^= (uint64_t)fragment_index << 1;
    h ^= (uint64_t)export_id * 0x2545f4914f6cdd1dull;
    h ^= h >> 29;
    return (uint32_t)h & (FRAG_LOC_CACHE_SIZE - 1);
}

static void frag_loc_note(efs_export_id_t export_id, efs_ino_t ino,
                          uint32_t chunk_index, uint32_t fragment_index,
                          uint64_t gen, uint32_t root)
{
    struct frag_loc *loc =
        &frag_loc_cache[frag_loc_slot(export_id, ino, chunk_index,
                                      fragment_index)];

    if (root > 0xff)
        return;
    /* valid goes down first and up last so a reader racing this fill
     * sees either the old entry or the new one, never a half-written
     * one it would trust. */
    loc->valid = 0;
    loc->export_id = export_id;
    loc->ino = ino;
    loc->gen = gen;
    loc->chunk_index = chunk_index;
    loc->fragment_index = fragment_index;
    loc->root = (uint8_t)root;
    __atomic_store_n(&loc->valid, 1, __ATOMIC_RELEASE);
}

/* Combined fragment+record read. Single probe pass per GET: the data
 * open() itself is the probe, and the I25 record is the tail of that
 * file. Roots are probed low to high, same as server_find_fragment_root.
 * The result is verified here (fragment_read_verified): EFS_OK means the
 * record names exactly this object and, for data fragments, the payload
 * hashes to its digest; checksum is then the payload digest and *sum_ok
 * is EFS_STORE_SUM_PRESENT. Anything else is never served. */
static int read_fragment_impl(struct efsd_server *s, struct efs_export *ex,
                                  efs_ino_t ino, uint32_t chunk_index,
                                  uint32_t fragment_index, uint8_t *data,
                                  uint32_t *data_len,
                                  uint8_t checksum[EFS_HASH_SIZE], int *sum_ok,
                                  uint8_t rec[EFS_FRAG_RECORD_BYTES], int *rec_ok,
                                  char *found, size_t found_len)
{
    (void)checksum; /* filled by read_fragment_verified */
    *sum_ok = 0;
    *rec_ok = 0;
    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;
    if (n != 1) {
        uint32_t want = server_frag_len(ex, ino);
        int direct = s->direct_io && !efs_ino_is_meta_table(ino);
        char path[8300];
        int plen = 0;
        uint32_t got = 0;
        int rc = EFS_ERR_NOT_FOUND;
        struct frag_loc *loc =
            &frag_loc_cache[frag_loc_slot(ex->id, ino, chunk_index,
                                          fragment_index)];
        if (loc->valid && loc->export_id == ex->id && loc->ino == ino &&
            loc->chunk_index == chunk_index &&
            loc->fragment_index == fragment_index &&
            loc->gen == efs_tls_chunk_gen &&
            loc->root < n) {
            fragment_path_at(s, loc->root, ex, ino, chunk_index,
                             fragment_index, path, sizeof(path));
            plen = (int)strlen(path);
            rc = read_file_bytes(path, data, want, &got, direct, rec,
                                 rec_ok);
            if (rc == EFS_ERR_NOT_FOUND) {
                loc->valid = 0;
                rc = EFS_ERR_NOT_FOUND;
            } else if (rc != EFS_OK) {
                return rc;
            }
        }
        if (rc == EFS_ERR_NOT_FOUND) {
            for (uint32_t ri = 0; ri < n; ri++) {
                fragment_path_at(s, ri, ex, ino, chunk_index,
                                 fragment_index, path, sizeof(path));
                plen = (int)strlen(path);
                rc = read_file_bytes(path, data, want, &got, direct, rec,
                                     rec_ok);
                if (rc == EFS_OK)
                    frag_loc_note(ex->id, ino, chunk_index, fragment_index,
                                  efs_tls_chunk_gen, ri);
                if (rc != EFS_ERR_NOT_FOUND)
                    break;
            }
        }
        if (rc != EFS_OK)
            return rc;
        *data_len = got;
        snprintf(found, found_len, "%s", path);
        (void)plen;
        return EFS_OK;
    }

    uint32_t want = server_frag_len(ex, ino);
    int direct = s->direct_io && !efs_ino_is_meta_table(ino);
    char path[8300];
    int plen;
    uint32_t got = 0;
    int rc;
    fragment_path_at(s, 0, ex, ino, chunk_index, fragment_index, path,
                     sizeof(path));
    plen = (int)strlen(path);
    rc = read_file_bytes(path, data, want, &got, direct, rec, rec_ok);
    if (rc != EFS_OK)
        return rc;
    *data_len = got;
    snprintf(found, found_len, "%s", path);
    (void)plen;
    return EFS_OK;
}

/* I25 GET gate: missing/torn record, unknown format, another identity, or
 * (data fragments) a payload that does not hash to the recorded digest are
 * all refused with distinct codes; the bytes are never served. Metadata-
 * table pages verify identity only: their payload integrity is the EFSR
 * root checksums. */
static int read_fragment_verified(struct efsd_server *s, struct efs_export *ex,
                                  efs_ino_t ino, uint32_t chunk_index,
                                  uint32_t fragment_index, uint8_t *data,
                                  uint32_t *data_len,
                                  uint8_t checksum[EFS_HASH_SIZE], int *sum_ok)
{
    uint8_t rec[EFS_FRAG_RECORD_BYTES];
    int rec_ok = 0;
    char path[8300] = "";
    int rc = read_fragment_impl(s, ex, ino, chunk_index, fragment_index, data,
                                data_len, checksum, sum_ok, rec, &rec_ok,
                                path, sizeof(path));
    if (rc != EFS_OK)
        return rc;
    struct efs_frag_ident want;
    const char *field = NULL;
    fragment_ident(ex, ino, chunk_index, fragment_index,
                   server_frag_len(ex, ino), &want);
    if (efs_ino_is_meta_table(ino))
        rc = efs_frag_record_verify(rec, rec_ok ? sizeof(rec) : 0, &want,
                                    checksum, &field);
    else if (*data_len != want.payload_len)
        rc = EFS_ERR_CHECKSUM; /* torn: shorter than the export geometry */
    else
        rc = efs_frag_verify(data, *data_len, rec, rec_ok ? sizeof(rec) : 0,
                             &want, checksum, &field);
    if (rc != EFS_OK) {
        fragment_refused("get", path, rc, field, &want);
        *sum_ok = 0;
        return rc;
    }
    *sum_ok = EFS_STORE_SUM_PRESENT;
    return EFS_OK;
}



int server_read_fragment_with_sum(struct efsd_server *s, struct efs_export *ex,
                                  efs_ino_t ino, uint32_t ci, uint32_t fi,
                                  uint8_t *data, uint32_t *len, uint8_t sum[EFS_HASH_SIZE], int *sum_ok)
{
    return read_fragment_verified(s, ex, ino, ci, fi, data, len, sum, sum_ok);
}

static uint64_t compute_dir_usage(const char *path, size_t root_len)
{
    uint64_t total = 0;
    DIR *d = opendir(path);
    if (!d)
        return errno == ENOENT ? 0 : UINT64_MAX;

    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(d);
        if (!entry) {
            if (errno) total = UINT64_MAX;
            break;
        }
        if (entry->d_name[0] == '.')
            continue;
        char full_path[8192];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        /* Sharded: skip the meta-table inode leaf and everything under it. */
        if (server_storage_metadata_path(full_path + root_len))
            continue;

        struct stat st;
        if (lstat(full_path, &st) != 0) {
            if (errno == ENOENT) continue;
            total = UINT64_MAX; break;
        }
        uint64_t bytes = 0;
        if (S_ISDIR(st.st_mode)) {
            bytes = compute_dir_usage(full_path, root_len);
        } else if (S_ISREG(st.st_mode)) {
            uint64_t payload;
            bytes = fragment_payload_bytes((uint64_t)st.st_size, &payload) == EFS_OK
                ? payload : (uint64_t)st.st_size;
        }
        if (bytes == UINT64_MAX || bytes > UINT64_MAX - total) {
            total = UINT64_MAX; break;
        }
        total += bytes;
    }
    if (closedir(d) != 0) total = UINT64_MAX;
    return total;
}

uint64_t server_compute_usage(const char *path)
{
    char data_path[8192];
    snprintf(data_path, sizeof(data_path), "%s/data", path);
    int mode = server_storage_root_mode(path);
    /* Unmarked (0) or the current root format; anything else is refused. */
    return mode != 0 && mode != (int)EFS_STORAGE_ROOT_FORMAT ? UINT64_MAX
                                                           : compute_dir_usage(data_path, strlen(path));
}

/* Full inventory remains mandatory. Ordinary-file roots are independent;
 * at most three helpers plus the caller overlap their read-only walks.
 * Every helper is joined before publishing counts or returning an error. */
struct usage_scan_batch {
    struct efsd_server *server;
    uint64_t *usage;
    uint32_t count, next;
};
static void *usage_scan_worker(void *arg)
{
    struct usage_scan_batch *batch = arg;
    for (;;) {
        uint32_t root = __atomic_fetch_add(&batch->next, 1, __ATOMIC_RELAXED);
        if (root >= batch->count) break;
        batch->usage[root] = server_compute_usage(batch->server->storage_paths[root]);
    }
    return NULL;
}

int server_compute_roots_usage(struct efsd_server *s, uint64_t *usage)
{
    if (!s || !usage) return EFS_ERR_INVAL;
    uint32_t count = s->storage_path_count ? s->storage_path_count : 1;
    if (count > EFS_MAX_STORAGE_PATHS) return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < count; i++) usage[i] = UINT64_MAX;
    struct usage_scan_batch batch = { .server = s, .usage = usage, .count = count };
    pthread_t helpers[3];
    unsigned created = 0;
    while (created < 3 && created + 1 < count) {
        if (pthread_create(&helpers[created], NULL, usage_scan_worker, &batch)) break;
        created++;
    }
    usage_scan_worker(&batch);
    for (unsigned i = 0; i < created; i++) pthread_join(helpers[i], NULL);
    for (uint32_t i = 0; i < count; i++)
        if (usage[i] == UINT64_MAX) return EFS_ERR_IO;
    return EFS_OK;
}

uint64_t server_compute_local_usage(struct efsd_server *s)
{
    uint64_t usage[EFS_MAX_STORAGE_PATHS];
    if (server_compute_roots_usage(s, usage) != EFS_OK) return UINT64_MAX;
    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;
    uint64_t raw = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t bytes = usage[i];
        if (bytes == UINT64_MAX || bytes > UINT64_MAX - raw)
            return UINT64_MAX;
        raw += bytes;
    }
    return raw;
}

void server_format_storage_paths(const struct efsd_server *s, char *buf, size_t buflen)
{
    if (!buf || buflen == 0)
        return;
    buf[0] = '\0';
    if (!s)
        return;
    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;
    size_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        const char *p = (s->storage_path_count > 0) ? s->storage_paths[i]
                                                    : s->storage_path;
        if (!p || !*p)
            continue;
        int wrote;
        if (off == 0)
            wrote = snprintf(buf + off, buflen - off, "%s", p);
        else
            wrote = snprintf(buf + off, buflen - off, ",%s", p);
        if (wrote < 0 || (size_t)wrote >= buflen - off)
            break;
        off += (size_t)wrote;
    }
}

static void strip_trailing_slashes(char *p)
{
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/')
        p[--n] = '\0';
}

static int storage_path_known(const struct efsd_server *s, const char *p)
{
    for (uint32_t i = 0; i < s->storage_path_count; i++) {
        if (strcmp(s->storage_paths[i], p) == 0)
            return 1;
    }
    return 0;
}

static int prepare_storage_root(const char *root)
{
    char sub[8192];
    make_dir(root);
    snprintf(sub, sizeof(sub), "%s/data", root);
    make_dir(sub);
    snprintf(sub, sizeof(sub), "%s/meta", root);
    make_dir(sub);
    snprintf(sub, sizeof(sub), "%s/log", root);
    make_dir(sub);
    struct stat st;
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode))
        return -1;
    return 0;
}

int server_add_storage_paths(struct efsd_server *s, const char *csv,
                             uint32_t *count_out)
{
    if (!s || !csv || !*csv)
        return EFS_ADD_STORAGE_INVALID;

    char buf[EFS_MAX_PATH];
    strncpy(buf, csv, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char add[EFS_MAX_STORAGE_PATHS][EFS_MAX_PATH];
    uint32_t nadd = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t')
            tok++;
        if (!*tok)
            continue;
        strip_trailing_slashes(tok);
        if (tok[0] != '/')
            return EFS_ADD_STORAGE_INVALID;
        int dup = 0;
        for (uint32_t i = 0; i < nadd; i++) {
            if (strcmp(add[i], tok) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;
        if (nadd >= EFS_MAX_STORAGE_PATHS)
            return EFS_ADD_STORAGE_FULL;
        strncpy(add[nadd], tok, sizeof(add[0]) - 1);
        add[nadd][sizeof(add[0]) - 1] = '\0';
        nadd++;
    }
    if (nadd == 0)
        return EFS_ADD_STORAGE_INVALID;

    pthread_mutex_lock(&s->lock);
    uint32_t have = s->storage_path_count;
    char fresh[EFS_MAX_STORAGE_PATHS][EFS_MAX_PATH];
    uint32_t nfresh = 0;
    for (uint32_t i = 0; i < nadd; i++) {
        if (storage_path_known(s, add[i]))
            continue;
        if (have + nfresh >= EFS_MAX_STORAGE_PATHS) {
            pthread_mutex_unlock(&s->lock);
            return EFS_ADD_STORAGE_FULL;
        }
        memcpy(fresh[nfresh], add[i], sizeof(fresh[0]));
        nfresh++;
    }
    uint32_t total = have;
    pthread_mutex_unlock(&s->lock);

    if (nfresh == 0) {
        if (count_out)
            *count_out = have;
        return EFS_ADD_STORAGE_OK;
    }

    for (uint32_t i = 0; i < nfresh; i++) {
        int mode = server_storage_root_mode(fresh[i]);
        if ((mode != 0 && mode != (int)EFS_STORAGE_ROOT_FORMAT) ||
            prepare_storage_root(fresh[i]) != 0)
            return EFS_ADD_STORAGE_ERROR;
    }

    pthread_mutex_lock(&s->lock);
    for (uint32_t i = 0; i < nfresh; i++) {
        if (storage_path_known(s, fresh[i]))
            continue;
        if (s->storage_path_count >= EFS_MAX_STORAGE_PATHS) {
            pthread_mutex_unlock(&s->lock);
            return EFS_ADD_STORAGE_FULL;
        }
        if (server_storage_files_attach(s, fresh[i], s->storage_path_count) != EFS_OK) {
            pthread_mutex_unlock(&s->lock);
            return EFS_ADD_STORAGE_ERROR;
        }
        s->storage_path_count++;
    }
    total = s->storage_path_count;
    struct efs_node *local = server_local_node(s);
    if (local)
        server_format_storage_paths(s, local->storage_path,
                                    sizeof(local->storage_path));
    server_nodes_mark_dirty(s);
    pthread_mutex_unlock(&s->lock);

    server_writer_set_npaths(total);
    server_nodes_flush_dirty(s);

    struct efs_msg_hello h;
    memset(&h, 0, sizeof(h));
    h.version = EFS_VERSION_PACK;
    strncpy(h.build_id, EFS_BUILD_ID, sizeof(h.build_id) - 1);
    h.node_id = s->id;
    strncpy(h.addr, s->addr, sizeof(h.addr) - 1);
    h.port = s->port;
    server_format_storage_paths(s, h.storage_path, sizeof(h.storage_path));
    h.quota = s->quota;
    {
        struct efs_node *ln = server_local_node(s);
        h.used = ln ? ln->used : 0;
    }
    server_gossip_membership(s, &h);

    if (count_out)
        *count_out = total;
    printf("add-storage: now %u local root(s)\n", total);
    return EFS_ADD_STORAGE_OK;
}

struct efs_node *server_local_node(struct efsd_server *s)
{
    if (!s)
        return NULL;
    for (uint32_t i = 0; i < s->node_count; i++) {
        if (s->nodes[i].id == s->id)
            return &s->nodes[i];
    }
    /* Membership table lost this node (corrupt load / race). Re-insert so
     * fragment writes are not rejected with EFS_ERR_INVAL. */
    if (s->id != 0 && s->node_count < EFS_MAX_NODES) {
        struct efs_node *n = &s->nodes[s->node_count++];
        memset(n, 0, sizeof(*n));
        n->id = s->id;
        strncpy(n->addr, s->addr, sizeof(n->addr) - 1);
        n->port = s->port;
        server_format_storage_paths(s, n->storage_path, sizeof(n->storage_path));
        n->quota = s->quota;
        return n;
    }
    return NULL;
}

void server_dedupe_nodes_locked(struct efsd_server *s)
{
    if (!s || s->node_count <= 1)
        return;
    struct efs_node *unique = malloc(sizeof(struct efs_node) * EFS_MAX_NODES);
    if (!unique)
        return;
    memset(unique, 0, sizeof(struct efs_node) * EFS_MAX_NODES);
    uint32_t unique_count = 0;
    for (uint32_t i = 0; i < s->node_count && i < EFS_MAX_NODES; i++) {
        if (s->nodes[i].id == 0)
            continue;
        int seen = 0;
        for (uint32_t j = 0; j < unique_count; j++) {
            if (unique[j].id == s->nodes[i].id) {
                seen = 1;
                break;
            }
        }
        if (!seen)
            unique[unique_count++] = s->nodes[i];
    }
    memcpy(s->nodes, unique, sizeof(s->nodes));
    s->node_count = unique_count;
    free(unique);
}

void server_sync_local_membership(struct efsd_server *s)
{
    if (!s || s->id == 0)
        return;
    pthread_mutex_lock(&s->lock);
    server_dedupe_nodes_locked(s);
    struct efs_node *local = NULL;
    for (uint32_t i = 0; i < s->node_count; i++) {
        if (s->nodes[i].id == s->id) {
            local = &s->nodes[i];
            break;
        }
    }
    if (!local) {
        if (s->node_count >= EFS_MAX_NODES) {
            pthread_mutex_unlock(&s->lock);
            fprintf(stderr, "Cannot insert local node %u: membership full\n",
                    s->id);
            return;
        }
        local = &s->nodes[s->node_count++];
        memset(local, 0, sizeof(*local));
        local->id = s->id;
    }
    strncpy(local->addr, s->addr, sizeof(local->addr) - 1);
    local->addr[sizeof(local->addr) - 1] = '\0';
    local->port = s->port;
    server_format_storage_paths(s, local->storage_path, sizeof(local->storage_path));
    local->quota = s->quota;
    /* used left as-is (usage.bin / prior counter). */
    server_dedupe_nodes_locked(s);
    pthread_mutex_unlock(&s->lock);
}

int server_write_fragment_with_sum_sync(struct efsd_server *s, struct efs_export *ex,
                                        efs_ino_t ino, uint32_t chunk_index,
                                        uint32_t fragment_index,
                                        const uint8_t *data, uint32_t data_len,
                                        const uint8_t checksum[EFS_HASH_SIZE]);

/* path must be the server_fragment_path() result for this fragment.
 * Every fragment file carries the I25 record (frag_record.h) after its
 * payload: a 4 KiB tail page under O_DIRECT, 128 bytes otherwise. checksum
 * is the payload digest the caller already verified (data PUT) or NULL,
 * in which case the payload is hashed here. */
static int server_write_fragment_to_path(struct efsd_server *s, struct efs_export *ex,
                                         efs_ino_t ino, uint32_t chunk_index,
                                         uint32_t fragment_index,
                                         const uint8_t *data, uint32_t data_len,
                                         const char *path,
                                         const uint8_t *checksum)
{
    /* Quota charges logical fragment size once per fragment. A first
     * write (EFS_PATH_HINT_SKIP: the name cannot exist) creates in one
     * open. Any other PUT tries O_CREAT|O_EXCL first — success is a new
     * file and is charged; EEXIST is an overwrite and is not. The old
     * no-create probe failed ENOENT on every fresh fragment (~180 µs
     * and an 11-component walk, Oct 1). */
    int may_charge = (!efs_ino_is_meta_table(ino));
    int first_write = (efs_tls_path_hint == EFS_PATH_HINT_SKIP);

    struct shard_io_arg arg;
    memset(&arg, 0, sizeof(arg));
    arg.s = s;
    arg.buf = (uint8_t *)data;
    arg.len = data_len;
    arg.direct = s->direct_io && !efs_ino_is_meta_table(ino);
    if (arg.direct)
        arg.len = server_frag_len(ex, ino);
    arg.is_write = 1;
#ifdef EFS_BENCH_BUILD
    arg.sync_write = s->bench_sync;
#endif
    uint8_t digest[EFS_HASH_SIZE], rec[EFS_FRAG_RECORD_BYTES];
    struct efs_frag_ident ident;
    if (!checksum) {
        efs_hash(data, data_len, digest);
        checksum = digest;
    }
    fragment_ident(ex, ino, chunk_index, fragment_index, arg.len, &ident);
    if (efs_frag_record_encode(&ident, checksum, rec) != EFS_OK)
        return EFS_ERR_INVAL; /* incomplete identity: never an unbound file */
    arg.rec = rec;
    arg.result = EFS_ERR_IO;
    size_t plen = strlen(path);
    if (plen >= sizeof(arg.path))
        plen = sizeof(arg.path) - 1;
    memcpy(arg.path, path, plen);
    arg.path[plen] = '\0';
#ifndef EFS_BENCH_BUILD
    if (!efs_ino_is_meta_table(ino))
        arg.sync_root = put_sync_root_for(s, arg.path);
#endif

    int do_charge = 0;
    if (may_charge && !first_write) {
        int under;
        pthread_mutex_lock(&s->lock);
        struct efs_node *local = server_local_node(s);
        if (!local) {
            pthread_mutex_unlock(&s->lock);
            return EFS_ERR_INVAL;
        }
        under = data_len <= UINT64_MAX - local->used &&
                (!s->quota || (local->used <= s->quota && data_len <= s->quota - local->used));
        pthread_mutex_unlock(&s->lock);
        if (under) {
            arg.excl = 1;
            arg.no_create = 0;
            arg.result = EFS_ERR_IO;
            shard_io_thread(&arg);
            if (arg.result == EFS_OK)
                do_charge = 1; /* created; charge below, do not write again */
            else if (arg.result != EFS_ERR_EXIST)
                return EFS_ERR_IO;
        }
        if (!do_charge) {
            arg.excl = 0;
            arg.no_create = 1;
            arg.result = EFS_ERR_IO;
            shard_io_thread(&arg);
            if (arg.result == EFS_OK)
                return EFS_OK; /* overwrite: no charge */
            if (!under)
                return EFS_ERR_QUOTA;
            if (arg.result != EFS_ERR_NOT_FOUND)
                return EFS_ERR_IO;
            do_charge = 1;
        }
    } else if (may_charge) {
        do_charge = 1; /* first write: one O_CREAT, no probe open */
    }

    /* EXCL already wrote the bytes. Charge and return. */
    if (do_charge && arg.excl && arg.result == EFS_OK) {
        int flush_usage = 0;
        pthread_mutex_lock(&s->lock);
        struct efs_node *local = server_local_node(s);
        if (local) {
            if (data_len > UINT64_MAX - local->used ||
                (s->quota && (local->used > s->quota || data_len > s->quota - local->used)) ||
                server_phys_admit_locked(s, arg.path, data_len)) {
                pthread_mutex_unlock(&s->lock);
                unlink(arg.path);
                return EFS_ERR_QUOTA;
            }
            local->used += data_len;
            static uint64_t charged_since_save;
            uint64_t sum = __atomic_add_fetch(&charged_since_save,
                                              (uint64_t)data_len,
                                              __ATOMIC_RELAXED);
            if (sum >= (64ull << 20)) {
                __atomic_store_n(&charged_since_save, 0, __ATOMIC_RELAXED);
                flush_usage = 1;
            }
        }
        pthread_mutex_unlock(&s->lock);
        if (flush_usage)
            server_usage_save(s);
        return EFS_OK;
    }

    int charge_quota = do_charge;
    int flush_usage = 0;
    if (charge_quota) {
        pthread_mutex_lock(&s->lock);
        struct efs_node *local = server_local_node(s);
        if (!local) {
            pthread_mutex_unlock(&s->lock);
            return EFS_ERR_INVAL;
        }
        if (data_len > UINT64_MAX - local->used ||
                (s->quota && (local->used > s->quota || data_len > s->quota - local->used)) ||
                server_phys_admit_locked(s, arg.path, data_len)) {
            pthread_mutex_unlock(&s->lock);
            return EFS_ERR_QUOTA;
        }
        local->used += data_len;
        /* Persist at most every ~64 MiB charged (writer thread, not heartbeat). */
        static uint64_t charged_since_save;
        uint64_t sum = __atomic_add_fetch(&charged_since_save, (uint64_t)data_len,
                                          __ATOMIC_RELAXED);
        if (sum >= (64ull << 20)) {
            __atomic_store_n(&charged_since_save, 0, __ATOMIC_RELAXED);
            flush_usage = 1;
        }
        pthread_mutex_unlock(&s->lock);
        if (flush_usage)
            server_usage_save(s);
    }

    /* One full fragment on the writer-selected storage root. */
    arg.no_create = 0;
    arg.result = EFS_ERR_IO;
    shard_io_thread(&arg);
    if (arg.result != EFS_OK) {
        if (charge_quota) {
            pthread_mutex_lock(&s->lock);
            struct efs_node *local = server_local_node(s);
            if (local && local->used >= data_len)
                local->used -= data_len;
            pthread_mutex_unlock(&s->lock);
            server_usage_mark_dirty(s);
        }
        return EFS_ERR_IO;
    }
    return EFS_OK;
}

int server_write_fragment_sync(struct efsd_server *s, struct efs_export *ex,
                               efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                               const uint8_t *data, uint32_t data_len)
{
    char path[8192];
    int prc = server_fragment_path(s, ex, ino, chunk_index, fragment_index, path,
                                   sizeof(path));
    if (prc != EFS_OK)
        return prc;
    pthread_rwlock_t *im = server_inode_gc_lock(s,ex->id,ino);
    pthread_rwlock_rdlock(im);
    int dead = inode_is_dead(s, ex->id, ino);
    if (dead) { pthread_rwlock_unlock(im); return dead > 0 ? EFS_ERR_STALE : EFS_ERR_IO; }
    pthread_mutex_t *mu = fragment_mutex(ex->id, ino, chunk_index, fragment_index);
    pthread_mutex_lock(mu);
    int rc = server_write_fragment_to_path(s, ex, ino, chunk_index,
                                         fragment_index, data, data_len, path,
                                         NULL);
    pthread_mutex_unlock(mu);
    pthread_rwlock_unlock(im);
    return rc;
}

/* Combined fragment+checksum write. The digest is the tail of the
 * fragment file; there is no sidecar. */
int server_write_fragment_with_sum_sync(struct efsd_server *s, struct efs_export *ex,
                                        efs_ino_t ino, uint32_t chunk_index,
                                        uint32_t fragment_index,
                                        const uint8_t *data, uint32_t data_len,
                                        const uint8_t checksum[EFS_HASH_SIZE])
{
    char path[8192];
    int prc = server_fragment_path(s, ex, ino, chunk_index, fragment_index, path,
                                   sizeof(path));
    if (prc != EFS_OK)
        return prc;
    pthread_rwlock_t *im = server_inode_gc_lock(s,ex->id,ino);
    pthread_rwlock_rdlock(im);
    int dead = inode_is_dead(s, ex->id, ino);
    if (dead) { pthread_rwlock_unlock(im); return dead > 0 ? EFS_ERR_STALE : EFS_ERR_IO; }
    pthread_mutex_t *mu = fragment_mutex(ex->id, ino, chunk_index, fragment_index);
    pthread_mutex_lock(mu);
    int rc = server_write_fragment_to_path(s, ex, ino, chunk_index,
                                         fragment_index, data, data_len, path,
                                         checksum);
    pthread_mutex_unlock(mu);
    pthread_rwlock_unlock(im);
    return rc;
}

#define EFS_NODES_MAGIC "EFSN"
#define EFS_NODES_VERSION 1

static void nodes_path_at(struct efsd_server *s, uint32_t root_idx,
                          char *path, size_t path_len)
{
    snprintf(path, path_len, "%s/meta/cluster_nodes.bin", s->storage_paths[root_idx]);
}

void server_nodes_mark_dirty(struct efsd_server *s)
{
    s->nodes_dirty = 1;
}

void server_nodes_flush_dirty(struct efsd_server *s)
{
    pthread_mutex_lock(&s->lock);
    int dirty = s->nodes_dirty;
    s->nodes_dirty = 0;
    pthread_mutex_unlock(&s->lock);
    if (dirty)
        server_save_nodes(s);
}

void server_save_nodes(struct efsd_server *s)
{
    if (!s->persist_nodes)
        return;

    /* Snapshot membership under the lock, then do all disk I/O unlocked so a
     * slow fsync never serializes every connection handler behind s->lock. */
    struct efs_node nodes_snap[EFS_MAX_NODES];
    uint32_t node_count;
    pthread_mutex_lock(&s->lock);
    node_count = s->node_count;
    if (node_count > EFS_MAX_NODES)
        node_count = EFS_MAX_NODES;
    memcpy(nodes_snap, s->nodes, sizeof(struct efs_node) * node_count);
    pthread_mutex_unlock(&s->lock);

    uint32_t nroots = s->storage_path_count ? s->storage_path_count : 1;
    for (uint32_t ri = 0; ri < nroots; ri++) {
        char path[8192];
        nodes_path_at(s, ri, path, sizeof(path));
        make_dir_for_file(path);

        FILE *f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "Could not save cluster nodes to %s: %s\n",
                    path, strerror(errno));
            continue;
        }

        fwrite(EFS_NODES_MAGIC, 4, 1, f);
        uint32_t version = EFS_NODES_VERSION;
        fwrite(&version, sizeof(version), 1, f);
        fwrite(&node_count, sizeof(node_count), 1, f);
        if (node_count > 0)
            fwrite(nodes_snap, sizeof(struct efs_node), node_count, f);

        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
}

void server_load_nodes(struct efsd_server *s)
{
    if (!s->persist_nodes)
        return;

    char path[8192];
    uint32_t nroots = s->storage_path_count ? s->storage_path_count : 1;
    FILE *f = NULL;
    for (uint32_t ri = 0; ri < nroots; ri++) {
        nodes_path_at(s, ri, path, sizeof(path));
        f = fopen(path, "rb");
        if (f)
            break;
    }
    if (!f)
        return;

    char magic[4];
    if (fread(magic, 4, 1, f) != 1 || memcmp(magic, EFS_NODES_MAGIC, 4) != 0) {
        fclose(f);
        return;
    }

    uint32_t version;
    uint32_t node_count;
    if (fread(&version, sizeof(version), 1, f) != 1 || version != EFS_NODES_VERSION ||
        fread(&node_count, sizeof(node_count), 1, f) != 1 ||
        node_count == 0 || node_count > EFS_MAX_NODES) {
        fclose(f);
        return;
    }

    struct efs_node loaded[EFS_MAX_NODES];
    if (fread(loaded, sizeof(struct efs_node), node_count, f) != node_count) {
        fclose(f);
        return;
    }
    fclose(f);

    /* Deduplicate by node id, keeping the first occurrence. */
    uint32_t unique_count = 0;
    struct efs_node unique[EFS_MAX_NODES];
    for (uint32_t i = 0; i < node_count; i++) {
        int seen = 0;
        for (uint32_t j = 0; j < unique_count; j++) {
            if (unique[j].id == loaded[i].id) {
                seen = 1;
                break;
            }
        }
        if (!seen)
            unique[unique_count++] = loaded[i];
    }

    pthread_mutex_lock(&s->lock);
    memcpy(s->nodes, unique, sizeof(unique));
    s->node_count = unique_count;
    pthread_mutex_unlock(&s->lock);

    printf("Loaded %u persisted cluster node(s) from %s/meta/cluster_nodes.bin\n",
           unique_count, s->storage_path);
    for (uint32_t i = 0; i < unique_count; i++) {
        printf("  persisted node %u: %s:%u\n",
               unique[i].id, unique[i].addr, unique[i].port);
    }
}

int server_rejoin_cluster(struct efsd_server *s)
{
    pthread_mutex_lock(&s->lock);
    uint32_t node_count = s->node_count;
    struct efs_node nodes[EFS_MAX_NODES];
    memcpy(nodes, s->nodes, sizeof(nodes));
    pthread_mutex_unlock(&s->lock);

    if (node_count <= 1)
        return -1;

    for (uint32_t i = 0; i < node_count; i++) {
        if (nodes[i].id == s->id)
            continue;
        printf("Trying to rejoin via persisted peer %s:%u (node %u)...\n",
               nodes[i].addr, nodes[i].port, nodes[i].id);
        if (server_join_cluster(s, nodes[i].addr, nodes[i].port) == 0) {
            printf("Rejoined cluster via %s:%u\n", nodes[i].addr, nodes[i].port);
            return 0;
        }
        printf("Failed to rejoin via %s:%u\n", nodes[i].addr, nodes[i].port);
    }
    fprintf(stderr, "Could not rejoin any of the %u persisted peer(s)\n", node_count - 1);
    return -1;
}
