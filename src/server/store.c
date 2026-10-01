#include "server_internal.h"
#include "efs/checksum.h"
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
#define EFS_USAGE_VERSION 1

static void usage_path(struct efsd_server *s, char *path, size_t path_len)
{
    const char *root = (s->storage_path_count > 0) ? s->storage_paths[0]
                                                   : s->storage_path;
    snprintf(path, path_len, "%s/meta/usage.bin", root);
}

static int server_usage_load(struct efsd_server *s, uint64_t *used_out)
{
    char path[8192];
    usage_path(s, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    char magic[4];
    uint32_t version = 0;
    uint64_t used = 0;
    if (fread(magic, 1, 4, f) != 4 ||
        memcmp(magic, EFS_USAGE_MAGIC, 4) != 0 ||
        fread(&version, sizeof(version), 1, f) != 1 ||
        version != EFS_USAGE_VERSION ||
        fread(&used, sizeof(used), 1, f) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    *used_out = used;
    return 0;
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

/* Append "/{ci}.{fi}" or "/{ci}.{fi}.{gen}" onto a dir built by
 * format_ino_chunk_dir. gen==0 is the pre-W1 name so leftover files
 * stay readable. */
__thread uint64_t efs_tls_chunk_gen;

static inline char *path_append_frag(char *p, uint32_t chunk_index,
                                     uint32_t fragment_index)
{
    *p++ = '/';
    p = path_append_u64(p, chunk_index);
    *p++ = '.';
    p = path_append_u64(p, fragment_index);
    if (efs_tls_chunk_gen) {
        *p++ = '.';
        p = path_append_u64(p, efs_tls_chunk_gen);
    }
    *p = '\0';
    return p;
}

/* Legacy RR (chunk_index % N). Kept only as a create-path fallback when the
 * writer has not set efs_tls_write_root (e.g. inline sync without pool). */
static uint32_t stripe_root_index(const struct efsd_server *s, uint32_t chunk_index)
{
    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;
    if (n <= 1)
        return 0;
    return chunk_index % n;
}

static uint32_t write_root_index(const struct efsd_server *s, uint32_t chunk_index)
{
    if (efs_tls_write_root >= 0) {
        uint32_t n = s->storage_path_count ? s->storage_path_count : 1;
        if ((uint32_t)efs_tls_write_root < n)
            return (uint32_t)efs_tls_write_root;
    }
    return stripe_root_index(s, chunk_index);
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
    uint32_t root = write_root_index(s, chunk_index);

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
void server_unlink_fragment_files(struct efsd_server *s, struct efs_export *ex,
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
}

/* Checksum-conditional fragment delete for the data-plane GC (spec L7).
 * The digest is the tail of the fragment file (4 KiB past the payload
 * when O_DIRECT, 32 bytes when buffered). The reaper proves identity
 * by that digest before unlinking:
 *   - tail == expect_sum: this file is the dead generation → unlink;
 *   - tail != expect_sum: a different image occupies the path → MUST
 *     NOT unlink; the record may be acked;
 *   - data present but no readable tail: cannot prove identity →
 *     treated as a live mismatch (leak-not-lose);
 *   - nothing present: already gone.
 * Returns EFS_OK when no dead-generation fragment remains, EFS_ERR_EXIST
 * when a mismatched/unidentifiable live fragment remains (also ackable),
 * EFS_ERR_IO on a real I/O failure (the reaper retries).
 * There is a microscopic read-sidecar→unlink window during which a new
 * generation's PUT could land between the check and the unlink; the worst
 * case is one fragment of one chunk unavailable, which 2+1 EC repairs.
 * W1 names objects `{ci}.{fi}.{gen}` when efs_tls_chunk_gen != 0; the
 * sidecar compare still protects a leftover un-named slot. */
int server_delete_fragment_if_sum(struct efsd_server *s, struct efs_export *ex,
                                  efs_ino_t ino, uint32_t chunk_index,
                                  uint32_t fragment_index,
                                  const uint8_t expect_sum[EFS_HASH_SIZE])
{
    char path[8192];
    uint8_t got[EFS_HASH_SIZE];
    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;
    int live = 0;
    int direct = s->direct_io && !efs_ino_is_meta_table(ino);
    uint32_t payload = server_frag_len(ex, ino);

    if (!s || !ex || !expect_sum)
        return EFS_ERR_INVAL;
    for (uint32_t ri = 0; ri < n; ri++) {
        {
            int fd;
            ssize_t rn = -1;

            fragment_path_at(s, ri, ex, ino, chunk_index, fragment_index,
                             path, sizeof(path));
            fd = open(path, direct ? (O_RDONLY | O_DIRECT) : O_RDONLY);
            if (fd < 0) {
                if (errno != ENOENT)
                    live = 1;
                continue;
            }
            if (direct) {
                uint8_t *tail = NULL;
                if (posix_memalign((void **)&tail, 4096, 4096) == 0) {
                    rn = pread(fd, tail, 4096, (off_t)payload);
                    if (rn == 4096)
                        memcpy(got, tail, EFS_HASH_SIZE);
                    free(tail);
                }
            } else {
                rn = pread(fd, got, EFS_HASH_SIZE, (off_t)payload);
            }
            close(fd);
            if (direct ? rn != 4096 : rn != (ssize_t)EFS_HASH_SIZE) {
                if (access(path, F_OK) == 0)
                    live = 1;
                continue;
            }
            if (memcmp(got, expect_sum, EFS_HASH_SIZE) != 0) {
                live = 1; /* a newer generation occupies the slot */
                continue;
            }
            {
                struct stat st;
                uint64_t nbytes = 0;
                int unlinked = 0;

                if (stat(path, &st) == 0 && st.st_size > 0) {
                    nbytes = (uint64_t)st.st_size;
                    /* Quota counts the logical fragment, not the 4 KiB tail. */
                    if (payload && nbytes > payload)
                        nbytes = payload;
                }
                if (unlink(path) == 0) {
                    unlinked = 1;
                } else if (errno != ENOENT) {
                    return EFS_ERR_IO;
                }
                /* PUT charges data_len into local->used. GC must uncharge
                 * or usage.bin latches at quota while the tree is empty
                 * (node1 1.00 GiB used vs 233M du). */
                if (unlinked && nbytes) {
                    pthread_mutex_lock(&s->lock);
                    struct efs_node *ln = server_local_node(s);
                    if (ln) {
                        if (ln->used >= nbytes)
                            ln->used -= nbytes;
                        else
                            ln->used = 0;
                    }
                    pthread_mutex_unlock(&s->lock);
                    server_usage_mark_dirty(s);
                }
            }
        }
    }
    return live ? EFS_ERR_EXIST : EFS_OK;
}

/* direct: caller decides per-ino — data fragments only. Metadata pages
 * stay buffered. sum, when non-NULL, is filled from the digest stored
 * at offset want_len (a 4 KiB O_DIRECT tail, or 32 buffered bytes). */
static int read_file_bytes(const char *path,
                           uint8_t *buf, uint32_t want_len, uint32_t *got,
                           int direct, uint8_t *sum, int *sum_ok)
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
    if (sum && sum_ok && n > 0) {
        *sum_ok = 0;
        if (direct) {
            uint8_t *tail = NULL;
            if (posix_memalign((void **)&tail, 4096, 4096) == 0) {
                ssize_t rn = pread(fd, tail, 4096, (off_t)want_len);
                if (rn == 4096) {
                    memcpy(sum, tail, EFS_HASH_SIZE);
                    *sum_ok = 1;
                }
                free(tail);
            }
        } else {
            uint8_t tmp[EFS_HASH_SIZE];
            ssize_t rn = pread(fd, tmp, EFS_HASH_SIZE, (off_t)want_len);
            if (rn == (ssize_t)EFS_HASH_SIZE) {
                memcpy(sum, tmp, EFS_HASH_SIZE);
                *sum_ok = 1;
            }
        }
    }
    close(fd);
    if (n < 0)
        return EFS_ERR_IO;
    return EFS_OK;
}

struct shard_io_arg {
    struct efsd_server *s;
    char path[8192];
    uint8_t *buf;
    uint32_t len;
    int is_write;
    int direct; /* data fragments only; meta stays buffered */
    int no_create; /* write only if the file already exists (overwrite probe) */
    int excl;      /* O_CREAT|O_EXCL: EEXIST -> EFS_ERR_EXIST, no second open */
    const uint8_t *sum; /* 32-byte digest; placed in the file, not a sidecar */
    int result;
};

static void *shard_io_thread(void *arg)
{
    struct shard_io_arg *a = arg;
    if (a->is_write) {
        int flags = O_WRONLY | O_TRUNC;
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
            /* Digest lives in a 4 KiB tail after the payload so one
             * O_DIRECT write carries both. No .sum inode. */
            if (a->sum && alloc < a->len + 4096u)
                alloc = a->len + 4096u;
            /* Zero payloads (store-bench / dd if=/dev/zero): write from a
             * process-wide pre-zeroed O_DIRECT buffer — no per-PUT memset
             * or memcpy of the 64 KiB fragment. */
            int is_zero = !a->sum && (a->len > 0) &&
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
            } else if (!a->sum && ((uintptr_t)a->buf & 4095u) == 0 &&
                       a->len == alloc) {
                /* Fast path: the RDMA recv layout places the frame payload
                 * on a 4096 boundary, so O_DIRECT can write straight out of
                 * the recv buffer — no 64 KiB bounce copy per fragment.
                 * Only when no pad is needed: the bounce path zero-pads
                 * short tails and the on-disk image must not change. */
                wbuf = a->buf;
            } else {
                /* Per-thread O_DIRECT bounce buffer for unaligned sources
                 * (TCP fallback path). posix_memalign+free per PUT was pure
                 * churn, so the buffer is cached per thread. */
                static __thread uint8_t *aligned_tls;
                static __thread uint32_t aligned_tls_len;
                if (aligned_tls_len < alloc) {
                    free(aligned_tls);
                    aligned_tls = NULL;
                    if (posix_memalign((void **)&aligned_tls, 4096, alloc) != 0) {
                        aligned_tls_len = 0;
                        close(fd);
                        a->result = EFS_ERR_NOMEM;
                        return NULL;
                    }
                    aligned_tls_len = alloc;
                }
                memcpy(aligned_tls, a->buf, a->len);
                if (a->len < alloc)
                    memset(aligned_tls + a->len, 0, alloc - a->len);
                if (a->sum)
                    memcpy(aligned_tls + a->len, a->sum, EFS_HASH_SIZE);
                wbuf = aligned_tls;
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
            if (a->sum) {
                ssize_t n = write(fd, a->sum, EFS_HASH_SIZE);
                if (n != (ssize_t)EFS_HASH_SIZE) {
                    close(fd);
                    a->result = EFS_ERR_IO;
                    return NULL;
                }
            }
            /* Do not fsync per shard PUT. A single FUSE dd MiB becomes
             * ~24 fragment PUTs × 4 disks = ~96 fsyncs and caps single-stream
             * bandwidth well below 1 GiB/s. Durability relies on the page
             * cache until process exit / umount (same model as many parallel
             * FS clients); add an explicit fsync RPC later if needed. */
        }
        close(fd);
        a->result = EFS_OK;
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

/* Combined fragment+checksum read. Single probe pass per GET: the data
 * open() itself is the probe, and the digest is the 4 KiB tail of that
 * file. Roots are probed low to high, same as server_find_fragment_root.
 * *sum_ok is set when the tail supplied the sum (caller hashes otherwise). */
int server_read_fragment_with_sum(struct efsd_server *s, struct efs_export *ex,
                                  efs_ino_t ino, uint32_t chunk_index,
                                  uint32_t fragment_index, uint8_t *data,
                                  uint32_t *data_len,
                                  uint8_t checksum[EFS_HASH_SIZE], int *sum_ok)
{
    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;
    *sum_ok = 0;
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
            rc = read_file_bytes(path, data, want, &got, direct, checksum,
                                 sum_ok);
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
                rc = read_file_bytes(path, data, want, &got, direct, checksum,
                                     sum_ok);
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
    rc = read_file_bytes(path, data, want, &got, direct, checksum, sum_ok);
    if (rc != EFS_OK)
        return rc;
    *data_len = got;
    (void)plen;
    return EFS_OK;
}

/* True if path contains the sharded leaf for EFS_META_TABLE_INO. */
static int path_under_meta_table_ino(const char *path)
{
    char seg[EFS_INO_PATH_SEGS][5];
    char needle[64];
    efs_ino_path_segments(EFS_META_TABLE_INO, seg);
    snprintf(needle, sizeof(needle), "/%s/%s/%s/%s/%s",
             seg[4], seg[3], seg[2], seg[1], seg[0]);
    return strstr(path, needle) != NULL;
}

static uint64_t compute_dir_usage(const char *path)
{
    uint64_t total = 0;
    DIR *d = opendir(path);
    if (!d)
        return 0;

    char meta_ino_name[32];
    snprintf(meta_ino_name, sizeof(meta_ino_name), "%llu",
             (unsigned long long)EFS_META_TABLE_INO);

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        /* Exclude 2+1 metadata table pages from data usage / quota.
         * Legacy: flat directory named after the meta ino. */
        if (strcmp(entry->d_name, meta_ino_name) == 0)
            continue;

        char full_path[8192];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        /* Sharded: skip the meta-table inode leaf and everything under it. */
        if (path_under_meta_table_ino(full_path))
            continue;

        struct stat st;
        if (stat(full_path, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            total += compute_dir_usage(full_path);
        } else if (S_ISREG(st.st_mode)) {
            total += (uint64_t)st.st_size;
        }
    }
    closedir(d);
    return total;
}

uint64_t server_compute_usage(const char *path)
{
    char data_path[8192];
    snprintf(data_path, sizeof(data_path), "%s/data", path);
    return compute_dir_usage(data_path);
}

uint64_t server_compute_local_usage(struct efsd_server *s)
{
    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;
    uint64_t raw = 0;
    for (uint32_t i = 0; i < n; i++)
        raw += server_compute_usage(s->storage_paths[i]);
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
        if (prepare_storage_root(fresh[i]) != 0)
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
        strncpy(s->storage_paths[s->storage_path_count], fresh[i],
                sizeof(s->storage_paths[0]) - 1);
        s->storage_paths[s->storage_path_count][sizeof(s->storage_paths[0]) - 1] = '\0';
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

void server_update_local_usage(struct efsd_server *s)
{
    uint64_t used = server_compute_local_usage(s);
    pthread_mutex_lock(&s->lock);
    struct efs_node *local = server_local_node(s);
    if (local) {
        local->used = used;
        /* Keep advertised path list current (e.g. after upgrade / rejoin). */
        server_format_storage_paths(s, local->storage_path,
                                    sizeof(local->storage_path));
    }
    pthread_mutex_unlock(&s->lock);
    server_usage_save(s);
}

void server_init_local_usage(struct efsd_server *s)
{
    uint64_t used = 0;
    if (server_usage_load(s, &used) == 0) {
        pthread_mutex_lock(&s->lock);
        struct efs_node *local = server_local_node(s);
        if (local)
            local->used = used;
        pthread_mutex_unlock(&s->lock);
        return;
    }
    server_update_local_usage(s);
}

int server_write_fragment_with_sum_sync(struct efsd_server *s, struct efs_export *ex,
                                        efs_ino_t ino, uint32_t chunk_index,
                                        uint32_t fragment_index,
                                        const uint8_t *data, uint32_t data_len,
                                        const uint8_t checksum[EFS_HASH_SIZE]);

/* path must be the server_fragment_path() result for this fragment.
 * checksum, when non-NULL, is stored in the file (4 KiB tail under
 * O_DIRECT, 32 bytes otherwise). */
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
    (void)chunk_index;
    (void)fragment_index;
    int may_charge = (!efs_ino_is_meta_table(ino)) && (s->quota > 0);
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
    arg.sum = checksum;
    arg.result = EFS_ERR_IO;
    size_t plen = strlen(path);
    if (plen >= sizeof(arg.path))
        plen = sizeof(arg.path) - 1;
    memcpy(arg.path, path, plen);
    arg.path[plen] = '\0';

    int do_charge = 0;
    if (may_charge && !first_write) {
        int under;
        pthread_mutex_lock(&s->lock);
        struct efs_node *local = server_local_node(s);
        if (!local) {
            pthread_mutex_unlock(&s->lock);
            return EFS_ERR_INVAL;
        }
        under = (local->used + data_len <= s->quota);
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
            if (local->used + data_len > s->quota) {
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
        if (local->used + data_len > s->quota) {
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
    server_fragment_path(s, ex, ino, chunk_index, fragment_index, path,
                         sizeof(path));
    return server_write_fragment_to_path(s, ex, ino, chunk_index,
                                         fragment_index, data, data_len, path,
                                         NULL);
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
    server_fragment_path(s, ex, ino, chunk_index, fragment_index, path,
                         sizeof(path));
    return server_write_fragment_to_path(s, ex, ino, chunk_index,
                                         fragment_index, data, data_len, path,
                                         checksum);
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
