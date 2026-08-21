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

static void make_dir(const char *path)
{
    size_t n = strlen(path) + 1;
    char *tmp = malloc(n);
    if (!tmp)
        return;
    memcpy(tmp, path, n);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
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

static int path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

void server_migrate_old_layout(struct efsd_server *s)
{
    char old_exports[8192], new_data[8192], new_meta[8192];
    snprintf(old_exports, sizeof(old_exports), "%s/exports", s->storage_path);
    snprintf(new_data, sizeof(new_data), "%s/data/exports", s->storage_path);
    snprintf(new_meta, sizeof(new_meta), "%s/meta/exports", s->storage_path);

    if (!path_exists(old_exports))
        return;
    if (path_exists(new_data) || path_exists(new_meta))
        return;

    fprintf(stderr, "Migrating old storage layout to data/meta/log subdirectories\n");

    DIR *d = opendir(old_exports);
    if (!d)
        return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        char old_export[8192];
        snprintf(old_export, sizeof(old_export), "%s/%s", old_exports, entry->d_name);

        struct stat st;
        if (stat(old_export, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        char new_meta_export[8192];
        snprintf(new_meta_export, sizeof(new_meta_export), "%s/%s", new_meta, entry->d_name);
        make_dir(new_meta_export);

        char old_meta[8192], new_meta_file[8192];
        snprintf(old_meta, sizeof(old_meta), "%s/metadata.bin", old_export);
        snprintf(new_meta_file, sizeof(new_meta_file), "%s/metadata.bin", new_meta_export);
        if (path_exists(old_meta)) {
            if (rename(old_meta, new_meta_file) != 0) {
                fprintf(stderr, "Failed to migrate metadata for export %s: %s\n",
                        entry->d_name, strerror(errno));
            }
        }

        char new_data_export[8192];
        snprintf(new_data_export, sizeof(new_data_export), "%s/%s", new_data, entry->d_name);
        make_dir(new_data_export);

        DIR *ed = opendir(old_export);
        if (!ed)
            continue;
        struct dirent *eentry;
        while ((eentry = readdir(ed)) != NULL) {
            if (eentry->d_name[0] == '.')
                continue;
            if (strcmp(eentry->d_name, "metadata.bin") == 0)
                continue;
            char old_sub[8192], new_sub[8192];
            snprintf(old_sub, sizeof(old_sub), "%s/%s", old_export, eentry->d_name);
            snprintf(new_sub, sizeof(new_sub), "%s/%s", new_data_export, eentry->d_name);
            if (rename(old_sub, new_sub) != 0) {
                fprintf(stderr, "Failed to migrate data for export %s/%s: %s\n",
                        entry->d_name, eentry->d_name, strerror(errno));
            }
        }
        closedir(ed);
    }
    closedir(d);

    char old_nodes[8192], new_nodes[8192];
    snprintf(old_nodes, sizeof(old_nodes), "%s/cluster_nodes.bin", s->storage_path);
    snprintf(new_nodes, sizeof(new_nodes), "%s/meta/cluster_nodes.bin", s->storage_path);
    if (path_exists(old_nodes)) {
        make_dir_for_file(new_nodes);
        if (rename(old_nodes, new_nodes) != 0) {
            fprintf(stderr, "Failed to migrate cluster_nodes.bin: %s\n", strerror(errno));
        }
    }
}

static int export_is_placeholder(const struct efs_export *ex)
{
    return ex && (ex->name[0] == '\0' || strcmp(ex->name, "pending") == 0);
}

struct efs_export *server_find_export(struct efsd_server *s, const char *name)
{
    if (!s || !name || !*name)
        return NULL;

    for (uint32_t i = 0; i < s->export_count; i++) {
        if (strcmp(s->exports[i].name, name) == 0)
            return &s->exports[i];
    }

    /* Reclaim a bootstrap placeholder left by PUT_META with an empty name. */
    for (uint32_t i = 0; i < s->export_count; i++) {
        if (export_is_placeholder(&s->exports[i])) {
            struct efs_export *ex = &s->exports[i];
            strncpy(ex->name, name, EFS_MAX_NAME - 1);
            ex->name[EFS_MAX_NAME - 1] = '\0';
            if (ex->id == 0)
                ex->id = i + 1;
            server_save_export(s, ex);
            return ex;
        }
    }

    if (s->export_count >= EFS_MAX_EXPORTS)
        return NULL;

    struct efs_export *ex = &s->exports[s->export_count++];
    efs_export_init(ex, s->export_count, name);
    server_save_export(s, ex);
    return ex;
}

struct efs_export *server_get_export(struct efsd_server *s, efs_export_id_t id)
{
    for (uint32_t i = 0; i < s->export_count; i++) {
        if (s->exports[i].id == id)
            return &s->exports[i];
    }
    return NULL;
}

/* Exact-name lookup with NO side effects. server_find_export creates (or
 * rebrands a placeholder into) an export on miss — correct for CREATE_EXPORT,
 * disastrous on read paths: a GET_META for a typo'd name used to MINT an
 * empty export and serve it, mounting the client onto a void table whose
 * flushes then lost every generation check against the real export[0]. */
struct efs_export *server_find_export_no_create(struct efsd_server *s,
                                                const char *name)
{
    if (!s || !name || !*name)
        return NULL;
    for (uint32_t i = 0; i < s->export_count; i++) {
        if (!export_is_placeholder(&s->exports[i]) &&
            strcmp(s->exports[i].name, name) == 0)
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

struct efs_export *server_export_acquire(struct efsd_server *s,
                                         efs_export_id_t id)
{
    pthread_mutex_lock(&s->lock);
    struct efs_export *ex = server_export_acquire_locked(s, id);
    pthread_mutex_unlock(&s->lock);
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

static void export_meta_dir_path_at(struct efsd_server *s, uint32_t root_idx,
                                    struct efs_export *ex, char *path, size_t path_len)
{
    snprintf(path, path_len, "%s/meta/exports/%s", s->storage_paths[root_idx], ex->name);
}

static void export_meta_path_at(struct efsd_server *s, uint32_t root_idx,
                                struct efs_export *ex, char *path, size_t path_len)
{
    snprintf(path, path_len, "%s/meta/exports/%s/metadata.bin",
             s->storage_paths[root_idx], ex->name);
}

static void export_efsm_path_at(struct efsd_server *s, uint32_t root_idx,
                                struct efs_export *ex, char *path, size_t path_len)
{
    snprintf(path, path_len, "%s/meta/exports/%s/metadata.efsm",
             s->storage_paths[root_idx], ex->name);
}

static void export_meta_path(struct efsd_server *s, struct efs_export *ex,
                             char *path, size_t path_len)
{
    export_meta_path_at(s, 0, ex, path, path_len);
}

void server_save_export(struct efsd_server *s, struct efs_export *ex)
{
    /* Mirror small metadata to every local root so a data-disk loss cannot
     * take out metadata.bin. */
    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;
    for (uint32_t ri = 0; ri < n; ri++) {
        char path[8192];
        export_meta_dir_path_at(s, ri, ex, path, sizeof(path));
        make_dir(path);
        export_meta_path_at(s, ri, ex, path, sizeof(path));
        if (efs_export_save(ex, path) != EFS_OK)
            fprintf(stderr, "Failed to save metadata for export %s on %s\n",
                    ex->name, s->storage_paths[ri]);
        if (ex->inode_count > 0 && !ex->meta_needs_rebuild) {
            char *blob = NULL;
            size_t blen = 0;
            if (efs_export_serialize(ex, &blob, &blen) == 0) {
                export_efsm_path_at(s, ri, ex, path, sizeof(path));
                FILE *f = fopen(path, "wb");
                if (f) {
                    if (fwrite(blob, 1, blen, f) != blen)
                        fprintf(stderr, "Failed to write %s\n", path);
                    fflush(f);
                    fsync(fileno(f));
                    fclose(f);
                }
                free(blob);
            }
        }
    }
}

static void rm_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char child[8192];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
            rm_tree(child);
        else
            unlink(child);
    }
    closedir(d);
    rmdir(path);
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

int server_destroy_export(struct efsd_server *s, const char *name)
{
    if (!s || !name)
        return EFS_ERR_INVAL;

    pthread_mutex_lock(&s->lock);
    int idx = -1;
    if (name[0]) {
        for (uint32_t i = 0; i < s->export_count; i++) {
            if (strcmp(s->exports[i].name, name) == 0) {
                idx = (int)i;
                break;
            }
        }
    } else {
        for (uint32_t i = 0; i < s->export_count; i++) {
            if (export_is_placeholder(&s->exports[i])) {
                idx = (int)i;
                break;
            }
        }
    }
    if (idx < 0) {
        pthread_mutex_unlock(&s->lock);
        return EFS_ERR_NOT_FOUND;
    }

    /* Fence new acquires, then wait for in-flight I/O to drain before we
     * compact/free the slot. */
    s->export_destroying[idx] = 1;
    while (s->export_inflight[idx] > 0)
        pthread_cond_wait(&s->export_idle_cv, &s->lock);

    struct efs_export doomed = s->exports[idx];
    efs_export_id_t id = doomed.id;
    char doomed_name[EFS_MAX_NAME];
    strncpy(doomed_name, doomed.name, EFS_MAX_NAME - 1);
    doomed_name[EFS_MAX_NAME - 1] = '\0';

    /* Drop from the table before unlocking so new I/O cannot find it. */
    uint32_t last = s->export_count - 1;
    if ((uint32_t)idx != last)
        s->exports[idx] = s->exports[last];
    /* Slot `last` is being cleared; move its lifetime counters down to idx if
     * we compacted, then clear the tail slot's counters. */
    if ((uint32_t)idx != last) {
        s->export_inflight[idx] = 0;
        s->export_destroying[idx] = 0;
    }
    /* Keep the incremental-rebuild blob cache aligned with the compaction:
     * the doomed slot's cache is freed; the moved slot's cache follows it. */
    free(s->meta_blob_cache[idx]);
    free(s->meta_blob_sums[idx]);
    s->meta_blob_cache[idx] = NULL;
    s->meta_blob_sums[idx] = NULL;
    s->meta_blob_cache_len[idx] = 0;
    s->meta_blob_cache_gen[idx] = 0;
    s->meta_blob_pages[idx] = 0;
    if ((uint32_t)idx != last) {
        s->meta_blob_cache[idx] = s->meta_blob_cache[last];
        s->meta_blob_sums[idx] = s->meta_blob_sums[last];
        s->meta_blob_cache_len[idx] = s->meta_blob_cache_len[last];
        s->meta_blob_cache_gen[idx] = s->meta_blob_cache_gen[last];
        s->meta_blob_pages[idx] = s->meta_blob_pages[last];
    }
    s->meta_blob_cache[last] = NULL;
    s->meta_blob_sums[last] = NULL;
    s->meta_blob_cache_len[last] = 0;
    s->meta_blob_cache_gen[last] = 0;
    s->meta_blob_pages[last] = 0;
    memset(&s->exports[last], 0, sizeof(s->exports[last]));
    s->export_inflight[last] = 0;
    s->export_destroying[last] = 0;
    s->export_count--;
    s->export_meta_dirty = 1;
    pthread_mutex_unlock(&s->lock);

    uint32_t nroots = s->storage_path_count ? s->storage_path_count : 1;
    for (uint32_t ri = 0; ri < nroots; ri++) {
        char path[8192];
        if (doomed_name[0]) {
            snprintf(path, sizeof(path), "%s/meta/exports/%s",
                     s->storage_paths[ri], doomed_name);
            rm_tree(path);
        }
        snprintf(path, sizeof(path), "%s/data/exports/%u",
                 s->storage_paths[ri], id);
        rm_tree(path);
    }

    efs_export_free(&doomed);
    /* Trees gone — one scan to refresh the cached counter, then persist. */
    server_update_local_usage(s);
    printf("Destroyed export id=%u name=%s\n", id,
           doomed_name[0] ? doomed_name : "(unnamed)");
    return EFS_OK;
}

void server_load_exports(struct efsd_server *s)
{
    uint32_t nroots = s->storage_path_count ? s->storage_path_count : 1;
    char exports_dir[8192];
    DIR *d = NULL;
    for (uint32_t ri = 0; ri < nroots; ri++) {
        snprintf(exports_dir, sizeof(exports_dir), "%.4064s/meta/exports",
                 s->storage_paths[ri]);
        make_dir(exports_dir);
        d = opendir(exports_dir);
        if (d)
            break;
    }
    if (!d)
        return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        if (s->export_count >= EFS_MAX_EXPORTS)
            break;

        struct efs_export *ex = &s->exports[s->export_count++];
        efs_export_init(ex, s->export_count, entry->d_name);

        int loaded = 0;
        for (uint32_t ri = 0; ri < nroots; ri++) {
            char path[EFS_MAX_PATH];
            export_meta_path_at(s, ri, ex, path, sizeof(path));
            if (efs_export_load(ex, path) == 0) {
                loaded = 1;
                if (ex->meta_fragmented && ex->inode_count <= 1) {
                    char epath[EFS_MAX_PATH];
                    export_efsm_path_at(s, ri, ex, epath, sizeof(epath));
                    FILE *ef = fopen(epath, "rb");
                    if (ef) {
                        if (fseek(ef, 0, SEEK_END) == 0) {
                            long elen = ftell(ef);
                            if (elen > 0 && fseek(ef, 0, SEEK_SET) == 0) {
                                char *ebuf = malloc((size_t)elen);
                                if (ebuf &&
                                    fread(ebuf, 1, (size_t)elen, ef) ==
                                        (size_t)elen) {
                                    struct efs_export_root keep;
                                    memset(&keep, 0, sizeof(keep));
                                    if (efs_export_root_copy(&keep, &ex->root) ==
                                        0) {
                                        if (efs_export_deserialize(
                                                ex, ebuf, (size_t)elen) == 0) {
                                            efs_export_root_move(&ex->root,
                                                                 &keep);
                                            ex->meta_fragmented = 1;
                                            ex->meta_needs_rebuild = 0;
                                            fprintf(stderr,
                                                    "loaded live tables for %s "
                                                    "inodes=%llu\n",
                                                    ex->name,
                                                    (unsigned long long)
                                                        ex->inode_count);
                                        } else {
                                            efs_export_root_move(&ex->root,
                                                                 &keep);
                                            ex->meta_fragmented = 1;
                                        }
                                    }
                                }
                                free(ebuf);
                            }
                        }
                        fclose(ef);
                    }
                }
                break;
            }
        }
        if (!loaded) {
            fprintf(stderr, "Failed to load metadata for export %s\n", entry->d_name);
            s->export_count--;
        }
    }
    closedir(d);
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

/* Append "/{ci}.{fi}" onto a dir built by format_ino_chunk_dir. */
static inline char *path_append_frag(char *p, uint32_t chunk_index,
                                     uint32_t fragment_index)
{
    *p++ = '/';
    p = path_append_u64(p, chunk_index);
    *p++ = '.';
    p = path_append_u64(p, fragment_index);
    *p = '\0';
    return p;
}

/* Pre-sharding layout: .../exports/{id}/{ino}/{chunk>>10} */
static int format_ino_chunk_dir_legacy(char *path, size_t path_len,
                                       const char *root, efs_export_id_t export_id,
                                       efs_ino_t ino, uint32_t chunk_index)
{
    return snprintf(path, path_len, "%s/data/exports/%u/%llu/%u",
                    root, export_id, (unsigned long long)ino, chunk_index >> 10);
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
                             char *path, size_t path_len);
static void fragment_path_at_legacy(struct efsd_server *s, uint32_t root_idx,
                                    struct efs_export *ex, efs_ino_t ino,
                                    uint32_t chunk_index, uint32_t fragment_index,
                                    char *path, size_t path_len);

static void fragment_path_at(struct efsd_server *s, uint32_t root_idx,
                             struct efs_export *ex, efs_ino_t ino,
                             uint32_t chunk_index, uint32_t fragment_index,
                             char *path, size_t path_len)
{
    int n = format_ino_chunk_dir(path, path_len, s->storage_paths[root_idx],
                                 ex->id, ino, chunk_index);
    if (n > 0 && (size_t)n + 32 <= path_len)
        path_append_frag(path + n, chunk_index, fragment_index);
}

static void fragment_path_at_legacy(struct efsd_server *s, uint32_t root_idx,
                                    struct efs_export *ex, efs_ino_t ino,
                                    uint32_t chunk_index, uint32_t fragment_index,
                                    char *path, size_t path_len)
{
    char dir[8192];
    format_ino_chunk_dir_legacy(dir, sizeof(dir), s->storage_paths[root_idx],
                                ex->id, ino, chunk_index);
    snprintf(path, path_len, "%s/%u.%u", dir, chunk_index, fragment_index);
}

int server_find_fragment_root(struct efsd_server *s, struct efs_export *ex,
                              efs_ino_t ino, uint32_t chunk_index,
                              uint32_t fragment_index)
{
    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;
    char path[8192];
    for (uint32_t ri = 0; ri < n; ri++) {
        fragment_path_at(s, ri, ex, ino, chunk_index, fragment_index, path,
                         sizeof(path));
        if (access(path, F_OK) == 0)
            return (int)ri;
        fragment_path_at_legacy(s, ri, ex, ino, chunk_index, fragment_index,
                                path, sizeof(path));
        if (access(path, F_OK) == 0)
            return (int)ri;
    }
    return -1;
}

/* Legacy local-EC shard names (removed); only unlinked for cleanup. */
static void legacy_ec_shard_path(struct efsd_server *s, uint32_t root_idx,
                                 struct efs_export *ex, efs_ino_t ino,
                                 uint32_t chunk_index, uint32_t fragment_index,
                                 char *path, size_t path_len)
{
    char dir[8192];
    format_ino_chunk_dir(dir, sizeof(dir), s->storage_paths[root_idx], ex->id,
                         ino, chunk_index);
    snprintf(path, path_len, "%s/%u.%u.s%u", dir, chunk_index, fragment_index,
             root_idx);
}

static void legacy_ec_shard_path_flat(struct efsd_server *s, uint32_t root_idx,
                                      struct efs_export *ex, efs_ino_t ino,
                                      uint32_t chunk_index, uint32_t fragment_index,
                                      char *path, size_t path_len)
{
    char dir[8192];
    format_ino_chunk_dir_legacy(dir, sizeof(dir), s->storage_paths[root_idx],
                                ex->id, ino, chunk_index);
    snprintf(path, path_len, "%s/%u.%u.s%u", dir, chunk_index, fragment_index,
             root_idx);
}

int server_fragment_path(struct efsd_server *s, struct efs_export *ex,
                         efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                         char *path, size_t path_len)
{
    fragment_path_at(s, write_root_index(s, chunk_index), ex, ino, chunk_index,
                     fragment_index, path, path_len);
    return 0;
}

static int server_fragment_path_legacy(struct efsd_server *s, struct efs_export *ex,
                                       efs_ino_t ino, uint32_t chunk_index,
                                       uint32_t fragment_index,
                                       char *path, size_t path_len)
{
    fragment_path_at_legacy(s, write_root_index(s, chunk_index), ex, ino,
                            chunk_index, fragment_index, path, path_len);
    return 0;
}

/* Resolve on-disk fragment across all local roots (adaptive write placement
 * means chunk_index % N is no longer authoritative). */
static int resolve_fragment_path(struct efsd_server *s, struct efs_export *ex,
                                 efs_ino_t ino, uint32_t chunk_index,
                                 uint32_t fragment_index,
                                 char *path, size_t path_len)
{
    int found = server_find_fragment_root(s, ex, ino, chunk_index, fragment_index);
    if (found >= 0) {
        fragment_path_at(s, (uint32_t)found, ex, ino, chunk_index, fragment_index,
                         path, path_len);
        if (access(path, F_OK) == 0)
            return 0;
        fragment_path_at_legacy(s, (uint32_t)found, ex, ino, chunk_index,
                                fragment_index, path, path_len);
        if (access(path, F_OK) == 0)
            return 0;
    }
    /* Create path: writer TLS root, else legacy RR. */
    uint32_t ri = write_root_index(s, chunk_index);
    fragment_path_at(s, ri, ex, ino, chunk_index, fragment_index, path, path_len);
    return -1;
}

/* True if this fragment is already on disk (overwrite must not re-charge quota). */
static int fragment_exists_on_disk(struct efsd_server *s, struct efs_export *ex,
                                   efs_ino_t ino, uint32_t chunk_index,
                                   uint32_t fragment_index)
{
    return server_find_fragment_root(s, ex, ino, chunk_index, fragment_index) >= 0;
}

/* Unlink fragment data + checksum sidecars (stripe + legacy + old EC shards). */
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

        fragment_path_at_legacy(s, ri, ex, ino, chunk_index, fragment_index, path,
                                sizeof(path));
        unlink(path);
        snprintf(sum_path, sizeof(sum_path), "%s.sum", path);
        unlink(sum_path);

        legacy_ec_shard_path(s, ri, ex, ino, chunk_index, fragment_index, path,
                             sizeof(path));
        unlink(path);
        legacy_ec_shard_path_flat(s, ri, ex, ino, chunk_index, fragment_index, path,
                                  sizeof(path));
        unlink(path);
    }
}

/* direct: caller decides per-ino — data fragments only. Metadata pages and
 * .sum sidecars stay buffered: they are small, randomly re-read (rebuild
 * sweeps, verify, catch-up), and the kernel cache is a feature there. */
static int read_file_bytes(const char *path,
                           uint8_t *buf, uint32_t want_len, uint32_t *got,
                           int direct)
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
    int result;
};

static void *shard_io_thread(void *arg)
{
    struct shard_io_arg *a = arg;
    if (a->is_write) {
        int flags = O_WRONLY | O_CREAT | O_TRUNC;
        if (a->direct)
            flags |= O_DIRECT;
        int fd = open(a->path, flags, 0644);
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
            uint8_t *aligned = NULL;
            uint32_t alloc = (a->len + 4095u) & ~4095u;
            if (alloc < 4096)
                alloc = 4096;
            /* Zero payloads (store-bench / dd if=/dev/zero): write from a
             * process-wide pre-zeroed O_DIRECT buffer — no per-PUT memset
             * or memcpy of the 64 KiB fragment. */
            int is_zero = (a->len > 0) &&
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
            } else {
                if (posix_memalign((void **)&aligned, 4096, alloc) != 0) {
                    close(fd);
                    a->result = EFS_ERR_NOMEM;
                    return NULL;
                }
                memcpy(aligned, a->buf, a->len);
                if (a->len < alloc)
                    memset(aligned + a->len, 0, alloc - a->len);
                wbuf = aligned;
            }
            while (written < alloc) {
                ssize_t n = write(fd, wbuf + written, alloc - written);
                if (n <= 0) {
                    free(aligned);
                    close(fd);
                    a->result = EFS_ERR_IO;
                    return NULL;
                }
                written += (size_t)n;
            }
            free(aligned);
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
                                    a->direct);
        if (a->result == EFS_OK && got < a->len)
            memset(a->buf + got, 0, a->len - got);
    }
    return NULL;
}

int server_read_fragment(struct efsd_server *s, struct efs_export *ex,
                         efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                         uint8_t *data, uint32_t *data_len)
{
    char path[8192];
    resolve_fragment_path(s, ex, ino, chunk_index, fragment_index, path,
                          sizeof(path));
    uint32_t want = server_frag_len(ex, ino);
    uint32_t got = 0;
    int rc = read_file_bytes(path, data, want, &got,
                             s->direct_io && !efs_ino_is_meta_table(ino));
    if (rc != EFS_OK)
        return rc;
    *data_len = got;
    return EFS_OK;
}

/* Per-thread resolved-location cache for multi-root reads. Adaptive write
 * placement makes the root non-deterministic, so without a hint every GET
 * open()-probes roots×layouts through a deep dir walk — the top server CPU
 * cost at scale. A hit turns the probe into one open; the open itself
 * validates the entry (ENOENT -> reprobe and refill). Caveat: if an
 * overwrite left duplicate fragments on several roots, a cached entry may
 * return a different copy than canonical probe order would (both are
 * checksum-valid generations; the probe order itself is already arbitrary
 * in that case). */
/* Sized so a conn thread's share of a multi-client streaming working set
 * (several thousand fragments) fits; a direct-mapped cache smaller than
 * the cyclical working set thrashes to ~0% hits. */
#define FRAG_LOC_CACHE_SIZE 16384
struct frag_loc {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint32_t chunk_index;
    uint32_t fragment_index;
    uint8_t root;
    uint8_t legacy;
    uint8_t valid;
};
static __thread struct frag_loc frag_loc_cache[FRAG_LOC_CACHE_SIZE];

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

/* Combined fragment+checksum read. Single probe pass per GET: the data
 * open() itself is the probe (no access()+open() doubling), and the .sum
 * sidecar rides the same resolved path instead of running a second
 * all-roots probe loop. Probe order matches server_find_fragment_root
 * (roots low to high, new layout before legacy) so duplicate-shadowing
 * semantics are unchanged. *sum_ok is set when the sidecar supplied the
 * sum (caller hashes otherwise). */
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
        char dir[8192];
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
            loc->root < n) {
            if (loc->legacy)
                format_ino_chunk_dir_legacy(dir, sizeof(dir),
                                            s->storage_paths[loc->root],
                                            ex->id, ino, chunk_index);
            else
                format_ino_chunk_dir(dir, sizeof(dir),
                                     s->storage_paths[loc->root], ex->id,
                                     ino, chunk_index);
            plen = snprintf(path, sizeof(path), "%s/%u.%u", dir, chunk_index,
                            fragment_index);
            rc = read_file_bytes(path, data, want, &got, direct);
            if (rc == EFS_ERR_NOT_FOUND) {
                loc->valid = 0;
                rc = EFS_ERR_NOT_FOUND;
            } else if (rc != EFS_OK) {
                return rc;
            }
        }
        if (rc == EFS_ERR_NOT_FOUND) {
            for (uint32_t ri = 0; ri < n; ri++) {
                format_ino_chunk_dir(dir, sizeof(dir), s->storage_paths[ri],
                                     ex->id, ino, chunk_index);
                plen = snprintf(path, sizeof(path), "%s/%u.%u", dir,
                                chunk_index, fragment_index);
                rc = read_file_bytes(path, data, want, &got, direct);
                if (rc == EFS_ERR_NOT_FOUND) {
                    format_ino_chunk_dir_legacy(dir, sizeof(dir),
                                                s->storage_paths[ri], ex->id,
                                                ino, chunk_index);
                    plen = snprintf(path, sizeof(path), "%s/%u.%u", dir,
                                    chunk_index, fragment_index);
                    rc = read_file_bytes(path, data, want, &got, direct);
                    if (rc == EFS_OK) {
                        loc->export_id = ex->id;
                        loc->ino = ino;
                        loc->chunk_index = chunk_index;
                        loc->fragment_index = fragment_index;
                        loc->root = (uint8_t)ri;
                        loc->legacy = 1;
                        loc->valid = 1;
                    }
                } else if (rc == EFS_OK) {
                    loc->export_id = ex->id;
                    loc->ino = ino;
                    loc->chunk_index = chunk_index;
                    loc->fragment_index = fragment_index;
                    loc->root = (uint8_t)ri;
                    loc->legacy = 0;
                    loc->valid = 1;
                }
                if (rc != EFS_ERR_NOT_FOUND)
                    break;
            }
        }
        if (rc != EFS_OK)
            return rc;
        *data_len = got;
        if (plen > 0 && (size_t)plen + 5 <= sizeof(path)) {
            memcpy(path + plen, ".sum", 5);
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                ssize_t rn = read(fd, checksum, EFS_HASH_SIZE);
                close(fd);
                if (rn == (ssize_t)EFS_HASH_SIZE)
                    *sum_ok = 1;
            }
        }
        return EFS_OK;
    }

    uint32_t want = server_frag_len(ex, ino);
    int direct = s->direct_io && !efs_ino_is_meta_table(ino);
    char dir[8192];
    char path[8300];
    format_ino_chunk_dir(dir, sizeof(dir), s->storage_paths[0], ex->id, ino,
                         chunk_index);
    int plen = snprintf(path, sizeof(path), "%s/%u.%u", dir, chunk_index,
                        fragment_index);
    uint32_t got = 0;
    int rc = read_file_bytes(path, data, want, &got, direct);
    if (rc == EFS_ERR_NOT_FOUND) {
        format_ino_chunk_dir_legacy(dir, sizeof(dir), s->storage_paths[0],
                                    ex->id, ino, chunk_index);
        plen = snprintf(path, sizeof(path), "%s/%u.%u", dir, chunk_index,
                        fragment_index);
        rc = read_file_bytes(path, data, want, &got, direct);
    }
    if (rc != EFS_OK)
        return rc;
    *data_len = got;

    if (plen > 0 && (size_t)plen + 5 <= sizeof(path)) {
        memcpy(path + plen, ".sum", 5);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            ssize_t rn = read(fd, checksum, EFS_HASH_SIZE);
            close(fd);
            if (rn == (ssize_t)EFS_HASH_SIZE)
                *sum_ok = 1;
        }
    }
    return EFS_OK;
}

static void server_fragment_sum_path_at(struct efsd_server *s, uint32_t root_idx,
                                       struct efs_export *ex, efs_ino_t ino,
                                       uint32_t chunk_index, uint32_t fragment_index,
                                       char *path, size_t path_len)
{
    char dir[8192];
    format_ino_chunk_dir(dir, sizeof(dir), s->storage_paths[root_idx], ex->id, ino,
                         chunk_index);
    snprintf(path, path_len, "%s/%u.%u.sum", dir, chunk_index, fragment_index);
}

static void server_fragment_sum_path_at_legacy(struct efsd_server *s, uint32_t root_idx,
                                              struct efs_export *ex, efs_ino_t ino,
                                              uint32_t chunk_index,
                                              uint32_t fragment_index,
                                              char *path, size_t path_len)
{
    char dir[8192];
    format_ino_chunk_dir_legacy(dir, sizeof(dir), s->storage_paths[root_idx],
                                ex->id, ino, chunk_index);
    snprintf(path, path_len, "%s/%u.%u.sum", dir, chunk_index, fragment_index);
}

/* Write the checksum sidecar for an already-resolved fragment path. The
 * fragment write just created the parent dir, so a plain open suffices — no
 * make_dir walk (saves the per-PUT dir-resolution the .sum used to redo). */
static int write_sum_for_path(const char *frag_path,
                              const uint8_t checksum[EFS_HASH_SIZE])
{
    char spath[8200];
    size_t flen = strlen(frag_path);
    if (flen + 5 > sizeof(spath))
        return EFS_ERR_IO;
    memcpy(spath, frag_path, flen);
    memcpy(spath + flen, ".sum", 5);
    int fd = open(spath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return EFS_ERR_IO;
    ssize_t n = write(fd, checksum, EFS_HASH_SIZE);
    close(fd);
    return (n == (ssize_t)EFS_HASH_SIZE) ? EFS_OK : EFS_ERR_IO;
}

int server_write_fragment_sum_sync(struct efsd_server *s, struct efs_export *ex,
                                   efs_ino_t ino, uint32_t chunk_index,
                                   uint32_t fragment_index,
                                   const uint8_t checksum[EFS_HASH_SIZE])
{
    char path[8192];
    uint32_t ri = write_root_index(s, chunk_index);
    server_fragment_sum_path_at(s, ri, ex, ino, chunk_index, fragment_index,
                                path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 && errno == ENOENT) {
        make_dir_for_file(path);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (fd < 0)
        return EFS_ERR_IO;
    ssize_t n = write(fd, checksum, EFS_HASH_SIZE);
    close(fd);
    return (n == (ssize_t)EFS_HASH_SIZE) ? EFS_OK : EFS_ERR_IO;
}

int server_read_fragment_sum(struct efsd_server *s, struct efs_export *ex,
                             efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                             uint8_t checksum[EFS_HASH_SIZE])
{
    uint32_t nroots = s->storage_path_count ? s->storage_path_count : 1;
    for (uint32_t ri = 0; ri < nroots; ri++) {
        char path[8192];
        server_fragment_sum_path_at(s, ri, ex, ino, chunk_index, fragment_index,
                                    path, sizeof(path));
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            server_fragment_sum_path_at_legacy(s, ri, ex, ino, chunk_index,
                                               fragment_index, path, sizeof(path));
            fd = open(path, O_RDONLY);
        }
        if (fd < 0)
            continue;
        ssize_t n = read(fd, checksum, EFS_HASH_SIZE);
        close(fd);
        if (n == (ssize_t)EFS_HASH_SIZE)
            return EFS_OK;
    }
    return EFS_ERR_NOT_FOUND;
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

bool server_would_exceed_quota(struct efsd_server *s, uint64_t fragment_size)
{
    if (s->quota == 0)
        return false;
    struct efs_node *local = server_local_node(s);
    if (!local)
        return true;
    return local->used + fragment_size > s->quota;
}

int server_write_fragment_with_sum_sync(struct efsd_server *s, struct efs_export *ex,
                                        efs_ino_t ino, uint32_t chunk_index,
                                        uint32_t fragment_index,
                                        const uint8_t *data, uint32_t data_len,
                                        const uint8_t checksum[EFS_HASH_SIZE]);

/* path must be the server_fragment_path() result for this fragment — built
 * once by the caller so with_sum doesn't format it twice per PUT. */
static int server_write_fragment_to_path(struct efsd_server *s, struct efs_export *ex,
                                         efs_ino_t ino, uint32_t chunk_index,
                                         uint32_t fragment_index,
                                         const uint8_t *data, uint32_t data_len,
                                         const char *path)
{
    /* Quota charges logical fragment size (not raw EC bytes), and only once
     * per fragment — overwrites must not inflate the cached used counter.
     * Skip the on-disk exists probe when quota is unlimited: it is several
     * access() syscalls per PUT and dominates single-stream create workloads. */
    int is_new = 1;
    if (s->quota > 0)
        is_new = !fragment_exists_on_disk(s, ex, ino, chunk_index, fragment_index);
    int charge_quota = (!efs_ino_is_meta_table(ino)) && is_new && (s->quota > 0);

    pthread_mutex_lock(&s->lock);
    struct efs_node *local = server_local_node(s);
    if (!local) {
        pthread_mutex_unlock(&s->lock);
        return EFS_ERR_INVAL;
    }
    if (charge_quota && s->quota > 0 && local->used + data_len > s->quota) {
        pthread_mutex_unlock(&s->lock);
        return EFS_ERR_QUOTA;
    }
    int flush_usage = 0;
    if (charge_quota) {
        local->used += data_len;
        /* Persist at most every ~64 MiB charged (writer thread, not heartbeat). */
        static uint64_t charged_since_save;
        uint64_t sum = __atomic_add_fetch(&charged_since_save, (uint64_t)data_len,
                                          __ATOMIC_RELAXED);
        if (sum >= (64ull << 20)) {
            __atomic_store_n(&charged_since_save, 0, __ATOMIC_RELAXED);
            flush_usage = 1;
        }
    }
    pthread_mutex_unlock(&s->lock);
    if (flush_usage)
        server_usage_save(s);

    /* One full fragment on the writer-selected storage root. */
    struct shard_io_arg arg;
    memset(&arg, 0, sizeof(arg));
    arg.s = s;
    arg.buf = (uint8_t *)data;
    arg.len = data_len;
    arg.direct = s->direct_io && !efs_ino_is_meta_table(ino);
    if (arg.direct)
        arg.len = server_frag_len(ex, ino);
    arg.is_write = 1;
    arg.result = EFS_ERR_IO;
    size_t plen = strlen(path);
    if (plen >= sizeof(arg.path))
        plen = sizeof(arg.path) - 1;
    memcpy(arg.path, path, plen);
    arg.path[plen] = '\0';
    shard_io_thread(&arg);
    if (arg.result != EFS_OK) {
        if (charge_quota) {
            pthread_mutex_lock(&s->lock);
            local = server_local_node(s);
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
                                         fragment_index, data, data_len, path);
}

/* Combined fragment+checksum write: one path format per PUT — the .sum goes
 * next to the fragment in the same just-created directory. */
int server_write_fragment_with_sum_sync(struct efsd_server *s, struct efs_export *ex,
                                        efs_ino_t ino, uint32_t chunk_index,
                                        uint32_t fragment_index,
                                        const uint8_t *data, uint32_t data_len,
                                        const uint8_t checksum[EFS_HASH_SIZE])
{
    char path[8192];
    server_fragment_path(s, ex, ino, chunk_index, fragment_index, path,
                         sizeof(path));
    int rc = server_write_fragment_to_path(s, ex, ino, chunk_index,
                                           fragment_index, data, data_len, path);
    if (rc != EFS_OK)
        return rc;
    return write_sum_for_path(path, checksum);
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
            server_fetch_metadata_from(s, nodes[i].addr, nodes[i].port);
            return 0;
        }
        printf("Failed to rejoin via %s:%u\n", nodes[i].addr, nodes[i].port);
    }
    fprintf(stderr, "Could not rejoin any of the %u persisted peer(s)\n", node_count - 1);
    return -1;
}
