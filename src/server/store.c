#include "server_internal.h"
#include "efs/local_ec.h"
#include "efs/checksum.h"
#include "storage_numa.h"
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
    char tmp[8192];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void make_dir_for_file(const char *path)
{
    char tmp[8192];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *last_slash = strrchr(tmp, '/');
    if (last_slash) {
        *last_slash = '\0';
        make_dir(tmp);
    }
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

struct efs_export *server_find_export(struct efsd_server *s, const char *name)
{
    for (uint32_t i = 0; i < s->export_count; i++) {
        if (strcmp(s->exports[i].name, name) == 0)
            return &s->exports[i];
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
    }
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

int server_fragment_path(struct efsd_server *s, struct efs_export *ex,
                         efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                         char *path, size_t path_len)
{
    /* Legacy single-disk name (n==1). */
    snprintf(path, path_len, "%s/data/exports/%u/%llu/%u/%u.%u",
             s->storage_paths[0], ex->id,
             (unsigned long long)ino, chunk_index >> 10, chunk_index, fragment_index);
    return 0;
}

static int server_shard_path(struct efsd_server *s, struct efs_export *ex,
                             efs_ino_t ino, uint32_t chunk_index,
                             uint32_t fragment_index, uint32_t shard_index,
                             char *path, size_t path_len)
{
    if (s->storage_path_count <= 1) {
        return server_fragment_path(s, ex, ino, chunk_index, fragment_index,
                                    path, path_len);
    }
    snprintf(path, path_len, "%s/data/exports/%u/%llu/%u/%u.%u.s%u",
             s->storage_paths[shard_index], ex->id,
             (unsigned long long)ino, chunk_index >> 10, chunk_index,
             fragment_index, shard_index);
    return 0;
}

static int read_file_bytes(struct efsd_server *s, const char *path,
                           uint8_t *buf, uint32_t want_len, uint32_t *got)
{
    int flags = O_RDONLY;
    if (s->direct_io)
        flags |= O_DIRECT;
    int fd = open(path, flags);
    if (fd < 0)
        return EFS_ERR_NOT_FOUND;

    ssize_t n;
    if (s->direct_io) {
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
    uint32_t path_index;
    int is_write;
    int result;
};

static void *shard_io_thread(void *arg)
{
    struct shard_io_arg *a = arg;
    server_apply_storage_affinity(a->s, a->path_index);
    if (a->is_write) {
        int flags = O_WRONLY | O_CREAT | O_TRUNC;
        if (a->s->direct_io)
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
        if (a->s->direct_io) {
            uint8_t *aligned = NULL;
            uint32_t alloc = (a->len + 4095u) & ~4095u;
            if (alloc < 4096)
                alloc = 4096;
            /* Zero payloads (store-bench / dd if=/dev/zero): write from a
             * process-wide pre-zeroed O_DIRECT buffer — no per-PUT memset
             * or memcpy of the 64 KiB fragment. */
            int is_zero = (a->len == EFS_FRAGMENT_SIZE) &&
                          (efs_tls_write_known_zero ||
                           efs_bytes_are_zero(a->buf, a->len));
            const uint8_t *wbuf;
            if (is_zero) {
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
                    free(zero_dio);
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
            fsync(fd);
        }
        close(fd);
        a->result = EFS_OK;
    } else {
        uint32_t got = 0;
        a->result = read_file_bytes(a->s, a->path, a->buf, a->len, &got);
        if (a->result == EFS_OK && got < a->len)
            memset(a->buf + got, 0, a->len - got);
    }
    return NULL;
}

int server_read_fragment(struct efsd_server *s, struct efs_export *ex,
                         efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                         uint8_t *data, uint32_t *data_len)
{
    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;

    if (n <= 1) {
        char path[8192];
        server_fragment_path(s, ex, ino, chunk_index, fragment_index, path, sizeof(path));
        uint32_t got = 0;
        int rc = read_file_bytes(s, path, data, EFS_FRAGMENT_SIZE, &got);
        if (rc != EFS_OK)
            return rc;
        *data_len = got;
        return EFS_OK;
    }

    uint32_t slen = efs_local_ec_shard_len(n);
    uint8_t (*shards)[EFS_LOCAL_EC_MAX_SHARD] =
        malloc(EFS_MAX_STORAGE_PATHS * EFS_LOCAL_EC_MAX_SHARD);
    if (!shards)
        return EFS_ERR_NOMEM;
    int have[EFS_MAX_STORAGE_PATHS];
    memset(have, 0, sizeof(have));

    struct shard_io_arg args[EFS_MAX_STORAGE_PATHS];
    pthread_t tids[EFS_MAX_STORAGE_PATHS];
    int started[EFS_MAX_STORAGE_PATHS];
    memset(started, 0, sizeof(started));

    for (uint32_t i = 0; i < n; i++) {
        memset(&args[i], 0, sizeof(args[i]));
        args[i].s = s;
        args[i].buf = shards[i];
        args[i].len = slen;
        args[i].path_index = i;
        args[i].is_write = 0;
        args[i].result = EFS_ERR_IO;
        server_shard_path(s, ex, ino, chunk_index, fragment_index, i,
                          args[i].path, sizeof(args[i].path));
        if (pthread_create(&tids[i], NULL, shard_io_thread, &args[i]) == 0)
            started[i] = 1;
        else
            shard_io_thread(&args[i]);
    }
    for (uint32_t i = 0; i < n; i++) {
        if (started[i])
            pthread_join(tids[i], NULL);
        if (args[i].result == EFS_OK)
            have[i] = 1;
    }

    int rc = efs_local_ec_decode(n, shards, have, data, EFS_FRAGMENT_SIZE);
    free(shards);
    if (rc != EFS_OK)
        return EFS_ERR_NOT_FOUND;
    *data_len = EFS_FRAGMENT_SIZE;
    return EFS_OK;
}

static void server_fragment_sum_path_at(struct efsd_server *s, uint32_t root_idx,
                                       struct efs_export *ex, efs_ino_t ino,
                                       uint32_t chunk_index, uint32_t fragment_index,
                                       char *path, size_t path_len)
{
    snprintf(path, path_len, "%s/data/exports/%u/%llu/%u/%u.%u.sum",
             s->storage_paths[root_idx], ex->id,
             (unsigned long long)ino, chunk_index >> 10, chunk_index, fragment_index);
}

int server_write_fragment_sum_sync(struct efsd_server *s, struct efs_export *ex,
                                   efs_ino_t ino, uint32_t chunk_index,
                                   uint32_t fragment_index,
                                   const uint8_t checksum[EFS_HASH_SIZE])
{
    uint32_t nroots = s->storage_path_count ? s->storage_path_count : 1;
    int ok = 0;
    for (uint32_t ri = 0; ri < nroots; ri++) {
        char path[8192];
        server_fragment_sum_path_at(s, ri, ex, ino, chunk_index, fragment_index,
                                    path, sizeof(path));
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 && errno == ENOENT) {
            make_dir_for_file(path);
            fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        }
        if (fd < 0)
            continue;
        ssize_t n = write(fd, checksum, EFS_HASH_SIZE);
        close(fd);
        if (n == (ssize_t)EFS_HASH_SIZE)
            ok++;
    }
    return (ok > 0) ? EFS_OK : EFS_ERR_IO;
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
        if (fd < 0)
            continue;
        ssize_t n = read(fd, checksum, EFS_HASH_SIZE);
        close(fd);
        if (n == (ssize_t)EFS_HASH_SIZE)
            return EFS_OK;
    }
    return EFS_ERR_NOT_FOUND;
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
        /* Exclude 2+1 metadata table pages from data usage / quota. */
        if (strcmp(entry->d_name, meta_ino_name) == 0)
            continue;

        char full_path[8192];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

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
    if (n <= 1)
        return raw;
    uint32_t k = efs_local_ec_k(n);
    if (k == 0)
        return raw;
    return (raw * (uint64_t)k) / (uint64_t)n;
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
        strncpy(n->storage_path, s->storage_path, sizeof(n->storage_path) - 1);
        n->quota = s->quota;
        return n;
    }
    return NULL;
}

void server_update_local_usage(struct efsd_server *s)
{
    uint64_t used = server_compute_local_usage(s);
    pthread_mutex_lock(&s->lock);
    struct efs_node *local = server_local_node(s);
    if (local)
        local->used = used;
    pthread_mutex_unlock(&s->lock);
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

int server_write_fragment_sync(struct efsd_server *s, struct efs_export *ex,
                               efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                               const uint8_t *data, uint32_t data_len)
{
    /* Quota charges logical fragment size (not raw EC bytes). */
    int charge_quota = (ino != EFS_META_TABLE_INO);

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
    if (charge_quota)
        local->used += data_len;
    pthread_mutex_unlock(&s->lock);

    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;

    /* n==1: write the fragment buffer directly — encode_plain was a full
     * memset+memcpy of every PUT for no benefit. */
    if (n <= 1) {
        struct shard_io_arg arg;
        memset(&arg, 0, sizeof(arg));
        arg.s = s;
        arg.buf = (uint8_t *)data;
        arg.len = data_len;
        if (s->direct_io)
            arg.len = EFS_FRAGMENT_SIZE;
        arg.path_index = 0;
        arg.is_write = 1;
        arg.result = EFS_ERR_IO;
        server_shard_path(s, ex, ino, chunk_index, fragment_index, 0,
                          arg.path, sizeof(arg.path));
        shard_io_thread(&arg);
        if (arg.result != EFS_OK) {
            if (charge_quota) {
                pthread_mutex_lock(&s->lock);
                local = server_local_node(s);
                if (local && local->used >= data_len)
                    local->used -= data_len;
                pthread_mutex_unlock(&s->lock);
            }
            return EFS_ERR_IO;
        }
        return EFS_OK;
    }

    uint8_t (*shards)[EFS_LOCAL_EC_MAX_SHARD] =
        malloc(EFS_MAX_STORAGE_PATHS * EFS_LOCAL_EC_MAX_SHARD);
    if (!shards) {
        if (charge_quota) {
            pthread_mutex_lock(&s->lock);
            local = server_local_node(s);
            if (local && local->used >= data_len)
                local->used -= data_len;
            pthread_mutex_unlock(&s->lock);
        }
        return EFS_ERR_NOMEM;
    }
    if (efs_local_ec_encode(n, data, data_len, shards) != EFS_OK) {
        free(shards);
        if (charge_quota) {
            pthread_mutex_lock(&s->lock);
            local = server_local_node(s);
            if (local && local->used >= data_len)
                local->used -= data_len;
            pthread_mutex_unlock(&s->lock);
        }
        return EFS_ERR_INVAL;
    }

    uint32_t slen = efs_local_ec_shard_len(n);
    struct shard_io_arg args[EFS_MAX_STORAGE_PATHS];
    pthread_t tids[EFS_MAX_STORAGE_PATHS];
    int started[EFS_MAX_STORAGE_PATHS];
    memset(started, 0, sizeof(started));

    for (uint32_t i = 0; i < n; i++) {
        memset(&args[i], 0, sizeof(args[i]));
        args[i].s = s;
        args[i].buf = shards[i];
        args[i].len = slen;
        args[i].path_index = i;
        args[i].is_write = 1;
        args[i].result = EFS_ERR_IO;
        server_shard_path(s, ex, ino, chunk_index, fragment_index, i,
                          args[i].path, sizeof(args[i].path));
        if (pthread_create(&tids[i], NULL, shard_io_thread, &args[i]) == 0)
            started[i] = 1;
        else
            shard_io_thread(&args[i]);
    }

    int ok = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (started[i])
            pthread_join(tids[i], NULL);
        if (args[i].result == EFS_OK)
            ok++;
    }
    free(shards);

    if (ok < (int)n) {
        if (charge_quota) {
            pthread_mutex_lock(&s->lock);
            local = server_local_node(s);
            if (local && local->used >= data_len)
                local->used -= data_len;
            pthread_mutex_unlock(&s->lock);
        }
        return EFS_ERR_IO;
    }
    return EFS_OK;
}

#define EFS_NODES_MAGIC "EFSN"
#define EFS_NODES_VERSION 1

static void nodes_path_at(struct efsd_server *s, uint32_t root_idx,
                          char *path, size_t path_len)
{
    snprintf(path, path_len, "%s/meta/cluster_nodes.bin", s->storage_paths[root_idx]);
}

void server_save_nodes(struct efsd_server *s)
{
    if (!s->persist_nodes)
        return;

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
        uint32_t node_count = s->node_count;
        fwrite(&version, sizeof(version), 1, f);
        fwrite(&node_count, sizeof(node_count), 1, f);
        if (node_count > 0)
            fwrite(s->nodes, sizeof(struct efs_node), node_count, f);

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
