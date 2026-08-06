#include "server_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>

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

static void export_meta_dir_path(struct efsd_server *s, struct efs_export *ex,
                                 char *path, size_t path_len)
{
    snprintf(path, path_len, "%s/meta/exports/%s", s->storage_path, ex->name);
}

static void export_meta_path(struct efsd_server *s, struct efs_export *ex,
                             char *path, size_t path_len)
{
    snprintf(path, path_len, "%s/meta/exports/%s/metadata.bin", s->storage_path, ex->name);
}

void server_save_export(struct efsd_server *s, struct efs_export *ex)
{
    char path[8192];
    export_meta_dir_path(s, ex, path, sizeof(path));
    make_dir(path);
    export_meta_path(s, ex, path, sizeof(path));
    efs_export_save(ex, path);
}

void server_load_exports(struct efsd_server *s)
{
    char exports_dir[8192];
    snprintf(exports_dir, sizeof(exports_dir), "%.4064s/meta/exports", s->storage_path);
    make_dir(exports_dir);

    DIR *d = opendir(exports_dir);
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

        char path[EFS_MAX_PATH];
        export_meta_path(s, ex, path, sizeof(path));
        if (efs_export_load(ex, path) != 0) {
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
    /* Shard by chunk_index>>10 so large files do not dump millions of
     * fragment files into a single directory (ext4 create latency then
     * trips the client I/O timeout). */
    snprintf(path, path_len, "%s/data/exports/%s/%llu/%u/%u.%u",
             s->storage_path, ex->name,
             (unsigned long long)ino, chunk_index >> 10, chunk_index, fragment_index);
    return 0;
}

int server_read_fragment(struct efsd_server *s, struct efs_export *ex,
                         efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                         uint8_t *data, uint32_t *data_len)
{
    char path[8192];
    server_fragment_path(s, ex, ino, chunk_index, fragment_index, path, sizeof(path));

    int flags = O_RDONLY;
    if (s->direct_io)
        flags |= O_DIRECT;

    int fd = open(path, flags);
    if (fd < 0)
        return EFS_ERR_NOT_FOUND;

    ssize_t n;
    if (s->direct_io) {
        uint8_t *aligned_buf = NULL;
        if (posix_memalign((void **)&aligned_buf, 4096, EFS_FRAGMENT_SIZE) != 0) {
            close(fd);
            return EFS_ERR_NOMEM;
        }
        n = read(fd, aligned_buf, EFS_FRAGMENT_SIZE);
        if (n > 0)
            memcpy(data, aligned_buf, (size_t)n);
        free(aligned_buf);
    } else {
        n = read(fd, data, EFS_FRAGMENT_SIZE);
    }

    close(fd);
    if (n < 0)
        return EFS_ERR_IO;

    *data_len = (uint32_t)n;
    return EFS_OK;
}

static void server_fragment_sum_path(struct efsd_server *s, struct efs_export *ex,
                                    efs_ino_t ino, uint32_t chunk_index,
                                    uint32_t fragment_index, char *path, size_t path_len)
{
    snprintf(path, path_len, "%s/data/exports/%s/%llu/%u/%u.%u.sum",
             s->storage_path, ex->name,
             (unsigned long long)ino, chunk_index >> 10, chunk_index, fragment_index);
}

int server_write_fragment_sum(struct efsd_server *s, struct efs_export *ex,
                              efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                              const uint8_t checksum[EFS_HASH_SIZE])
{
    char path[8192];
    server_fragment_sum_path(s, ex, ino, chunk_index, fragment_index, path, sizeof(path));
    make_dir_for_file(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
    char path[8192];
    server_fragment_sum_path(s, ex, ino, chunk_index, fragment_index, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return EFS_ERR_NOT_FOUND;
    ssize_t n = read(fd, checksum, EFS_HASH_SIZE);
    close(fd);
    return (n == (ssize_t)EFS_HASH_SIZE) ? EFS_OK : EFS_ERR_IO;
}

static uint64_t compute_dir_usage(const char *path)
{
    uint64_t total = 0;
    DIR *d = opendir(path);
    if (!d)
        return 0;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.')
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

struct efs_node *server_local_node(struct efsd_server *s)
{
    if (!s)
        return NULL;
    for (uint32_t i = 0; i < s->node_count; i++) {
        if (s->nodes[i].id == s->id)
            return &s->nodes[i];
    }
    return NULL;
}

void server_update_local_usage(struct efsd_server *s)
{
    uint64_t used = server_compute_usage(s->storage_path);
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

int server_write_fragment(struct efsd_server *s, struct efs_export *ex,
                          efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                          const uint8_t *data, uint32_t data_len)
{
    char path[8192];
    server_fragment_path(s, ex, ino, chunk_index, fragment_index, path, sizeof(path));
    make_dir_for_file(path);

    pthread_mutex_lock(&s->lock);
    struct efs_node *local = server_local_node(s);
    if (!local) {
        pthread_mutex_unlock(&s->lock);
        return EFS_ERR_INVAL;
    }
    if (s->quota > 0 && local->used + data_len > s->quota) {
        pthread_mutex_unlock(&s->lock);
        return EFS_ERR_QUOTA;
    }
    local->used += data_len;
    pthread_mutex_unlock(&s->lock);

    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (s->direct_io)
        flags |= O_DIRECT;

    int fd = open(path, flags, 0644);
    if (fd < 0) {
        pthread_mutex_lock(&s->lock);
        local = server_local_node(s);
        if (local && local->used >= data_len)
            local->used -= data_len;
        pthread_mutex_unlock(&s->lock);
        return EFS_ERR_IO;
    }

    size_t written = 0;
    if (s->direct_io) {
        uint8_t *aligned_buf = NULL;
        if (posix_memalign((void **)&aligned_buf, 4096, EFS_FRAGMENT_SIZE) != 0) {
            close(fd);
            pthread_mutex_lock(&s->lock);
            local = server_local_node(s);
            if (local && local->used >= data_len)
                local->used -= data_len;
            pthread_mutex_unlock(&s->lock);
            return EFS_ERR_NOMEM;
        }
        memcpy(aligned_buf, data, data_len);
        if (data_len < EFS_FRAGMENT_SIZE)
            memset(aligned_buf + data_len, 0, EFS_FRAGMENT_SIZE - data_len);

        while (written < EFS_FRAGMENT_SIZE) {
            ssize_t n = write(fd, aligned_buf + written, EFS_FRAGMENT_SIZE - written);
            if (n <= 0) {
                free(aligned_buf);
                close(fd);
                pthread_mutex_lock(&s->lock);
                local = server_local_node(s);
                if (local && local->used >= data_len)
                    local->used -= data_len;
                pthread_mutex_unlock(&s->lock);
                return EFS_ERR_IO;
            }
            written += (size_t)n;
        }
        free(aligned_buf);
    } else {
        while (written < data_len) {
            ssize_t n = write(fd, data + written, data_len - written);
            if (n <= 0) {
                close(fd);
                pthread_mutex_lock(&s->lock);
                local = server_local_node(s);
                if (local && local->used >= data_len)
                    local->used -= data_len;
                pthread_mutex_unlock(&s->lock);
                return EFS_ERR_IO;
            }
            written += (size_t)n;
        }
    }

    fsync(fd);
    close(fd);
    return EFS_OK;
}

#define EFS_NODES_MAGIC "EFSN"
#define EFS_NODES_VERSION 1

static void nodes_path(struct efsd_server *s, char *path, size_t path_len)
{
    snprintf(path, path_len, "%s/meta/cluster_nodes.bin", s->storage_path);
}

void server_save_nodes(struct efsd_server *s)
{
    if (!s->persist_nodes)
        return;

    char path[8192];
    nodes_path(s, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Could not save cluster nodes to %s: %s\n", path, strerror(errno));
        return;
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

void server_load_nodes(struct efsd_server *s)
{
    if (!s->persist_nodes)
        return;

    char path[8192];
    nodes_path(s, path, sizeof(path));

    FILE *f = fopen(path, "rb");
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
