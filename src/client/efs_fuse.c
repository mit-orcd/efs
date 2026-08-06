#define FUSE_USE_VERSION 26

#include "client_internal.h"
#include "efs/common.h"
#include "efs/network.h"
#include "efs/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fuse.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/statvfs.h>
#include <utime.h>
#include <time.h>

/* Generate a per-mount inode namespace so concurrent clients never assign the
 * same inode number to different files. The namespace occupies the high bits
 * of the 64-bit ino; the low 40 bits are a per-client counter. */
static void efs_client_setup_ino_namespace(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    srandom((unsigned)(ts.tv_nsec ^ ts.tv_sec ^ (getpid() << 8)));
    uint64_t tag = ((uint64_t)getpid() << 32) ^
                   ((uint64_t)ts.tv_sec << 12) ^
                   (uint64_t)ts.tv_nsec ^
                   ((uint64_t)random() << 20);
    tag &= 0x7FFFFF; /* 23 bits, keeps the ino positive */
    if (tag == 0)
        tag = 1;
    g_client.ino_namespace = tag << 40;
    g_client.ino_counter = 1;
}

static int efs_fuse_getattr(const char *path, struct stat *stbuf)
{
    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;

    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->st_ino = ino.ino;
    stbuf->st_mode = ino.mode;
    stbuf->st_nlink = ino.nlink;
    stbuf->st_size = ino.size;
    stbuf->st_mtime = ino.mtime;
    stbuf->st_ctime = ino.ctime;
    stbuf->st_uid = ino.uid;
    stbuf->st_gid = ino.gid;
    return 0;
}

static int efs_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                            off_t offset, struct fuse_file_info *fi)
{
    (void)offset;
    (void)fi;

    struct efs_inode parent;
    int rc = efs_client_lookup(path, &parent);
    if (rc != 0)
        return -ENOENT;
    if (!efs_mode_is_dir(parent.mode))
        return -ENOTDIR;

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    pthread_mutex_lock(&g_client.lock);
    for (uint64_t i = 0; i < g_client.export.inode_count; i++) {
        if (g_client.export.inodes[i].parent == parent.ino &&
            g_client.export.inodes[i].ino != parent.ino) {
            struct stat st;
            memset(&st, 0, sizeof(st));
            st.st_ino = g_client.export.inodes[i].ino;
            st.st_mode = g_client.export.inodes[i].mode;
            filler(buf, g_client.export.inodes[i].name, &st, 0);
        }
    }
    pthread_mutex_unlock(&g_client.lock);
    return 0;
}

static int efs_fuse_open(const char *path, struct fuse_file_info *fi)
{
    (void)fi;
    struct efs_inode ino;
    return efs_client_lookup(path, &ino) == 0 ? 0 : -ENOENT;
}

static int efs_fuse_read(const char *path, char *buf, size_t size, off_t offset,
                         struct fuse_file_info *fi)
{
    (void)fi;
    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;
    if (efs_mode_is_dir(ino.mode))
        return -EISDIR;

    size_t got = 0;
    rc = efs_client_read(ino.ino, (uint64_t)offset, size, buf, &got);
    if (rc != 0)
        return -EIO;
    return (int)got;
}

static int efs_fuse_write(const char *path, const char *buf, size_t size,
                          off_t offset, struct fuse_file_info *fi)
{
    (void)fi;
    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;
    if (efs_mode_is_dir(ino.mode))
        return -EISDIR;

    rc = efs_client_write_no_replicate(ino.ino, (uint64_t)offset, size, buf);
    if (rc == EFS_ERR_QUOTA)
        return -ENOSPC;
    if (rc != 0)
        return -EIO;
    return (int)size;
}

static int efs_fuse_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi;
    char *p = strdup(path);
    char *base = strrchr(p, '/');
    if (!base) {
        free(p);
        return -EINVAL;
    }
    *base = '\0';
    base++;
    char name[EFS_MAX_NAME];
    strncpy(name, base, EFS_MAX_NAME - 1);
    name[EFS_MAX_NAME - 1] = '\0';

    struct efs_inode parent;
    int rc = efs_client_lookup(p[0] ? p : "/", &parent);
    free(p);
    if (rc != 0)
        return -ENOENT;
    if (!efs_mode_is_dir(parent.mode))
        return -ENOTDIR;

    struct fuse_context *ctx = fuse_get_context();
    efs_ino_t ino = efs_client_create(parent.ino, name, S_IFREG | mode,
                                      ctx->uid, ctx->gid);
    if (ino == 0)
        return -EEXIST;
    return 0;
}

static int efs_fuse_mkdir(const char *path, mode_t mode)
{
    char *p = strdup(path);
    char *base = strrchr(p, '/');
    if (!base) {
        free(p);
        return -EINVAL;
    }
    *base = '\0';
    base++;
    char name[EFS_MAX_NAME];
    strncpy(name, base, EFS_MAX_NAME - 1);
    name[EFS_MAX_NAME - 1] = '\0';

    struct efs_inode parent;
    int rc = efs_client_lookup(p[0] ? p : "/", &parent);
    free(p);
    if (rc != 0)
        return -ENOENT;
    if (!efs_mode_is_dir(parent.mode))
        return -ENOTDIR;

    struct fuse_context *ctx = fuse_get_context();
    efs_ino_t ino = efs_client_create(parent.ino, name, S_IFDIR | mode,
                                      ctx->uid, ctx->gid);
    if (ino == 0)
        return -EEXIST;
    return 0;
}

static int efs_fuse_unlink(const char *path)
{
    char *p = strdup(path);
    char *base = strrchr(p, '/');
    if (!base) {
        free(p);
        return -EINVAL;
    }
    *base = '\0';
    base++;
    char name[EFS_MAX_NAME];
    strncpy(name, base, EFS_MAX_NAME - 1);
    name[EFS_MAX_NAME - 1] = '\0';

    struct efs_inode parent;
    int rc = efs_client_lookup(p[0] ? p : "/", &parent);
    free(p);
    if (rc != 0)
        return -ENOENT;

    return efs_client_unlink(parent.ino, name, false) == 0 ? 0 : -EIO;
}

static int efs_fuse_rmdir(const char *path)
{
    char *p = strdup(path);
    char *base = strrchr(p, '/');
    if (!base) {
        free(p);
        return -EINVAL;
    }
    *base = '\0';
    base++;
    char name[EFS_MAX_NAME];
    strncpy(name, base, EFS_MAX_NAME - 1);
    name[EFS_MAX_NAME - 1] = '\0';

    struct efs_inode parent;
    int rc = efs_client_lookup(p[0] ? p : "/", &parent);
    free(p);
    if (rc != 0)
        return -ENOENT;

    return efs_client_unlink(parent.ino, name, true) == 0 ? 0 : -EIO;
}

static int efs_fuse_statfs(const char *path, struct statvfs *stbuf)
{
    (void)path;
    memset(stbuf, 0, sizeof(*stbuf));

    pthread_mutex_lock(&g_client.lock);

    uint64_t used_logical = 0;
    for (uint64_t i = 0; i < g_client.export.inode_count; i++) {
        struct efs_inode *ino = &g_client.export.inodes[i];
        if (!efs_mode_is_dir(ino->mode))
            used_logical += ino->size;
    }

    uint64_t min_quota = 0;
    for (uint32_t i = 0; i < g_client.node_count; i++) {
        uint64_t q = g_client.nodes[i].quota;
        if (q > 0 && (min_quota == 0 || q < min_quota))
            min_quota = q;
    }

    /* 2+1 erasure coding: 3 fragments per chunk, 1.5x physical for 1x logical.
       The cluster is limited by the smallest node because every chunk places
       one fragment on each node. Logical capacity = 2 * min_quota. */
    uint64_t total_logical = 0;
    if (min_quota > 0 && g_client.node_count > 0)
        total_logical = min_quota * 2;

    uint64_t avail = (total_logical > used_logical) ? total_logical - used_logical : 0;

    stbuf->f_bsize = 512;
    stbuf->f_frsize = 512;
    stbuf->f_blocks = total_logical / 512;
    stbuf->f_bfree = avail / 512;
    stbuf->f_bavail = avail / 512;
    stbuf->f_files = g_client.export.inode_count;
    stbuf->f_ffree = 0;
    stbuf->f_namemax = EFS_MAX_NAME;

    pthread_mutex_unlock(&g_client.lock);
    return 0;
}

static int efs_fuse_chmod(const char *path, mode_t mode)
{
    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;
    if (efs_client_chmod(ino.ino, mode) != 0)
        return -EIO;
    return 0;
}

static int efs_fuse_chown(const char *path, uid_t uid, gid_t gid)
{
    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;
    if (efs_client_chown(ino.ino, uid, gid) != 0)
        return -EIO;
    return 0;
}

static int efs_fuse_utime(const char *path, struct utimbuf *ubuf)
{
    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;

    uint64_t mtime;
    if (ubuf)
        mtime = (uint64_t)ubuf->modtime;
    else
        mtime = (uint64_t)time(NULL);

    if (efs_client_utime(ino.ino, mtime) != 0)
        return -EIO;
    return 0;
}

static int efs_fuse_truncate(const char *path, off_t size)
{
    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;
    if (efs_mode_is_dir(ino.mode))
        return -EISDIR;

    if (efs_client_truncate(ino.ino, (uint64_t)size) != 0)
        return -EIO;
    return 0;
}

static int efs_fuse_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    /* Coalesced: only flushes every meta_batch_ops releases/creates. */
    efs_client_note_meta_change(0);
    return 0;
}

static void efs_fuse_destroy(void *userdata)
{
    (void)userdata;
    /* Final flush so the last dirty batch is not lost on unmount. */
    efs_client_note_meta_change(1);
}

static int efs_fuse_rename(const char *from, const char *to)
{
    struct efs_inode src, dst_parent;
    if (efs_client_lookup(from, &src) != 0)
        return -ENOENT;

    char *p = strdup(to);
    char *base = strrchr(p, '/');
    if (!base) {
        free(p);
        return -EINVAL;
    }
    *base = '\0';
    base++;
    char name[EFS_MAX_NAME];
    strncpy(name, base, EFS_MAX_NAME - 1);
    name[EFS_MAX_NAME - 1] = '\0';

    int rc = efs_client_lookup(p[0] ? p : "/", &dst_parent);
    free(p);
    if (rc != 0)
        return -ENOENT;
    if (!efs_mode_is_dir(dst_parent.mode))
        return -ENOTDIR;

    if (efs_client_rename(src.ino, dst_parent.ino, name) != 0)
        return -EIO;
    return 0;
}

static void *efs_fuse_init(struct fuse_conn_info *conn)
{
    /* Ask the kernel for large writes so we are not forced into a 4 KiB
     * RMW of every 128 KiB chunk. */
    if (conn) {
        if (conn->capable & FUSE_CAP_BIG_WRITES)
            conn->want |= FUSE_CAP_BIG_WRITES;
        if (conn->max_write == 0 || conn->max_write > EFS_CHUNK_SIZE)
            conn->max_write = EFS_CHUNK_SIZE;
    }
    /* Coalesce metadata PUTs so bulk creates are not O(n^2) full-metadata
     * syncs. Override with EFS_META_BATCH_OPS for heavy profiling loads. */
    uint32_t batch = 4096;
    const char *env = getenv("EFS_META_BATCH_OPS");
    if (env && *env) {
        unsigned long v = strtoul(env, NULL, 10);
        if (v > 0 && v < 1000000)
            batch = (uint32_t)v;
    }
    efs_client_enable_meta_batch(batch);
    return NULL;
}

static struct fuse_operations efs_ops = {
    .getattr = efs_fuse_getattr,
    .statfs  = efs_fuse_statfs,
    .readdir = efs_fuse_readdir,
    .open    = efs_fuse_open,
    .read    = efs_fuse_read,
    .write   = efs_fuse_write,
    .create  = efs_fuse_create,
    .mkdir   = efs_fuse_mkdir,
    .unlink   = efs_fuse_unlink,
    .rmdir    = efs_fuse_rmdir,
    .chmod    = efs_fuse_chmod,
    .chown    = efs_fuse_chown,
    .utime    = efs_fuse_utime,
    .truncate = efs_fuse_truncate,
    .rename   = efs_fuse_rename,
    .release  = efs_fuse_release,
    .init     = efs_fuse_init,
    .destroy  = efs_fuse_destroy,
};

static int parse_addr(const char *str, char *host, size_t host_len, uint16_t *port)
{
    const char *colon = strrchr(str, ':');
    if (!colon)
        return -1;
    size_t hlen = (size_t)(colon - str);
    if (hlen >= host_len)
        return -1;
    memcpy(host, str, hlen);
    host[hlen] = '\0';
    *port = (uint16_t)atoi(colon + 1);
    return 0;
}

static int mkdir_p(const char *path)
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
    return mkdir(tmp, 0755);
}

static pid_t g_perf_pid = -1;

static pid_t start_perf_recorder(pid_t target, const char *perf_path)
{
    char perf_dir[8192];
    strncpy(perf_dir, perf_path, sizeof(perf_dir) - 1);
    perf_dir[sizeof(perf_dir) - 1] = '\0';
    char *slash = strrchr(perf_dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(perf_dir);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork perf");
        return -1;
    }
    if (pid == 0) {
        char pid_str[32];
        snprintf(pid_str, sizeof(pid_str), "%d", (int)target);
        /* -g uses frame pointers (binaries are built with
         * -fno-omit-frame-pointer) so perf report can show call stacks. */
        execlp("perf", "perf", "record", "-g", "-F", "999", "-p", pid_str, "-o",
               perf_path, NULL);
        perror("exec perf");
        _exit(1);
    }
    return pid;
}

static void stop_perf_recorder(void)
{
    if (g_perf_pid > 0) {
        kill(g_perf_pid, SIGTERM);
        /* Wait (up to ~5s) for perf to finalize and write perf.data. */
        for (int i = 0; i < 50; i++) {
            if (waitpid(g_perf_pid, NULL, WNOHANG) == g_perf_pid)
                break;
            usleep(100000);
        }
        kill(g_perf_pid, SIGKILL);
        waitpid(g_perf_pid, NULL, 0);
        g_perf_pid = -1;
    }
}

int main(int argc, char **argv)
{
    int perf = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--perf") == 0) {
            perf = 1;
            break;
        }
    }

    if (argc < 4) {
        fprintf(stderr,
                "Usage: %s <node1:port> [<node2:port> ...] <export-name> <mountpoint> [fuse options] [--perf]\n"
                "       A single server address is enough; the client will discover the rest.\n",
                argv[0]);
        return 1;
    }

    /* Server addresses are the first arguments (they contain a colon).
       The first argument without a colon is the export name. */
    int arg_idx = 1;
    const char *nodes[EFS_MAX_NODES];
    uint32_t node_count = 0;
    while (arg_idx < argc && strchr(argv[arg_idx], ':') != NULL &&
           node_count < EFS_MAX_NODES) {
        nodes[node_count++] = argv[arg_idx++];
    }

    if (node_count == 0 || arg_idx >= argc) {
        fprintf(stderr, "Missing export name and mountpoint\n");
        return 1;
    }

    const char *export_name = argv[arg_idx++];
    if (arg_idx >= argc) {
        fprintf(stderr, "Missing mountpoint\n");
        return 1;
    }
    const char *mountpoint = argv[arg_idx++];

    efs_client_init_nodes(&g_client, nodes, node_count);
    strncpy(g_client.export_name, export_name, EFS_MAX_NAME - 1);
    g_client.export_id = 1;
    pthread_mutex_init(&g_client.lock, NULL);
    efs_export_init(&g_client.export, g_client.export_id, g_client.export_name);
    efs_client_setup_ino_namespace();

    if (node_count == 1) {
        char host[64];
        uint16_t port = 0;
        if (parse_addr(nodes[0], host, sizeof(host), &port) != 0 ||
            efs_client_discover_nodes(&g_client, host, port) != 0) {
            fprintf(stderr, "Could not discover cluster from %s\n", nodes[0]);
            return 1;
        }
        printf("Discovered %u cluster nodes from %s\n", g_client.node_count, nodes[0]);
    }

    /* Fetch initial metadata from one of the nodes. */
    int rc = -1;
    for (uint32_t i = 0; i < g_client.node_count; i++) {
        rc = efs_client_fetch_metadata(g_client.nodes[i].addr, g_client.nodes[i].port);
        if (rc == 0)
            break;
    }
    if (rc != 0) {
        fprintf(stderr, "Could not fetch metadata from any node\n");
        return 1;
    }

    char *fuse_argv[64];
    int fuse_argc = 0;
    fuse_argv[fuse_argc++] = argv[0];
    fuse_argv[fuse_argc++] = (char *)mountpoint;
    /* Prefer large writes so FUSE does not chop every write into 4 KiB and
     * force a 128 KiB RMW per call. big_writes raises the kernel limit;
     * max_write matches our chunk size. */
    fuse_argv[fuse_argc++] = "-o";
    fuse_argv[fuse_argc++] = "big_writes,max_write=131072";
    while (arg_idx < argc && fuse_argc < 63) {
        if (strcmp(argv[arg_idx], "--perf") == 0) {
            arg_idx++;
            continue;
        }
        fuse_argv[fuse_argc++] = argv[arg_idx++];
    }
    fuse_argv[fuse_argc] = NULL;

    char perf_path[512];
    if (perf) {
        const char *pp = getenv("EFS_PERF_PATH");
        if (pp && *pp)
            snprintf(perf_path, sizeof(perf_path), "%s", pp);
        else
            snprintf(perf_path, sizeof(perf_path), "/tmp/efs-fuse-perf-%d/perf.data", (int)getpid());
        g_perf_pid = start_perf_recorder(getpid(), perf_path);
        if (g_perf_pid < 0) {
            fprintf(stderr, "Warning: could not start perf recorder; continuing without profiling\n");
        }
    }

    int ret = fuse_main(fuse_argc, fuse_argv, &efs_ops, NULL);
    stop_perf_recorder();
    return ret;
}
