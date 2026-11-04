/* efs-fuse: mutation-gated operation wrappers, the operation table, destroy, startup and main. */
#include "fuse_internal.h"
#include "support.h"
#include "open.h"
#include "writeback.h"
#include "append.h"
#include "release.h"
#include "plock_init.h"
#include "ll_run.h"
#include "../stop_control.h"

/* Canonical absolute mountpoint, set once in main and logged there. The
 * .find results are rendered as host-absolute paths on the server side. */
static char g_mountpoint[EFS_MAX_PATH] = "/";

/* Nonzero once a clean unmount failed to flush, report, settle or revoke:
 * efs-fuse then exits 1, never reporting a clean completion (D42). The
 * first reason goes into the stop record. */
static int g_unmount_unclean;
static char g_unclean_reason[192];

static void unmount_unclean(const char *reason)
{
    if (!g_unmount_unclean && reason && *reason)
        snprintf(g_unclean_reason, sizeof(g_unclean_reason), "%s", reason);
    g_unmount_unclean = 1;
}

static void efs_fuse_destroy(void *userdata)
{
    (void)userdata;
    efs_client_set_stop_observer(efs_stop_phase);
    efs_stop_phase("flushing", 0, 0);
    fuse_pressure_stop();
    int wrc = efs_wb_sync();
    int drc = efs_dcache_flush_all();
    if (wrc != EFS_OK || drc != EFS_OK) {
        char why[96];
        fprintf(stderr, "efs-fuse: unmount: data flush incomplete "
                        "(writeback=%d dcache=%d)\n", wrc, drc);
        snprintf(why, sizeof(why), "data flush writeback=%d dcache=%d", wrc, drc);
        unmount_unclean(why);
    }
    if (g_wb.ready) {
        pthread_mutex_lock(&g_wb.mu);
        g_wb.shutdown = 1;
        pthread_cond_broadcast(&g_wb.not_empty);
        pthread_mutex_unlock(&g_wb.mu);
        for (int i = 0; i < g_wb.nworkers; i++)
            pthread_join(g_wb.workers[i], NULL);
        g_wb.ready = 0;
        g_wb.shutdown = 0;
    }
    /* Drain the dirty set before efs_client_shutdown frees the table.
     *
     * One forced report is not enough. efs_client_report_dirty gives up on a
     * shard after 4 attempts (~750 ms total) while its owner answers BUSY,
     * which is exactly what an owner does while it is rebuilding a shard --
     * something every node does briefly after a fresh mkfs, and again
     * whenever a joiner catches up. On give-up the records are merged back
     * into the in-memory table, and shutdown then frees it, so the writes
     * were lost with no error and no log line: files came back after the
     * remount with the correct size and all-zero contents.
     *
     * The BUSY window is short (seconds) and self-healing, so retry until the
     * report succeeds. If it still will not drain, say so -- an unmount that
     * silently discards acknowledged writes is worse than a noisy one. */
    {
        const long drain_s = 60;
        struct timespec t0, now;
        int mrc;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (uint64_t attempt = 1;; attempt++) {
            efs_stop_phase("draining", attempt, 0);
            mrc = efs_client_note_meta_change(1);
            if (mrc == EFS_OK)
                break;
            /* A fenced session can never publish again (K5/I23): waiting
             * would only delay the unmount. Report the loss once. */
            if (mrc == EFS_ERR_FENCED || efs_client_session_fenced()) {
                fprintf(stderr, "efs-fuse: mount session fenced; writes that "
                                "were never reported are discarded\n");
                mrc = EFS_OK;
                break;
            }
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - t0.tv_sec >= drain_s)
                break;
            usleep(200000);
        }
        if (mrc != EFS_OK) {
            char why[96];
            fprintf(stderr, "efs-fuse: UNMOUNT DATA LOSS: metadata flush "
                            "still failing (rc=%d) after %lds; writes that "
                            "were never reported are gone\n", mrc, drain_s);
            snprintf(why, sizeof(why), "metadata report rc=%d after %lds", mrc, drain_s);
            unmount_unclean(why);
        }
    }
    /* Free cached .find query results. */
    pthread_mutex_lock(&g_find_res_mu);
    for (int i = 0; i < EFS_FIND_RES_N; i++) {
        free(g_find_res[i].text);
        g_find_res[i].text = NULL;
        g_find_res[i].valid = 0;
    }
    pthread_mutex_unlock(&g_find_res_mu);
    efs_open_retry_stop();
    efs_client_view_stop();
    if (efs_client_shutdown() != EFS_OK)
        unmount_unclean(efs_client_shutdown_reason());
}
/* Low-level (inode-based) FUSE ops. Timeouts stay 0 (Part C later). */
static void ll_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
                       int to_set, struct fuse_file_info *fi)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_setattr_run(req, ino, attr, to_set, fi);
    efs_stop_mutation_leave();
}

static void ll_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name,
                     mode_t mode)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_mkdir_run(req, parent, name, mode);
    efs_stop_mutation_leave();
}

static void ll_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_unlink_run(req, parent, name);
    efs_stop_mutation_leave();
}

static void ll_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_rmdir_run(req, parent, name);
    efs_stop_mutation_leave();
}

static void ll_symlink(fuse_req_t req, const char *link, fuse_ino_t parent,
                       const char *name)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    uint64_t previous=efs_client_rpc_deadline_ms();
    uint64_t deadline=stats_now_ms()+30000;
    if(previous && previous<deadline)deadline=previous;
    efs_client_rpc_set_deadline_ms(deadline);
    ll_symlink_run(req, link, parent, name);
    efs_client_rpc_set_deadline_ms(previous);
    efs_stop_mutation_leave();
}

static void ll_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
                      fuse_ino_t newparent, const char *newname,
                      unsigned int flags)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_rename_run(req, parent, name, newparent, newname, flags);
    efs_stop_mutation_leave();
}

static void ll_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
                    const char *newname)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_link_run(req, ino, newparent, newname);
    efs_stop_mutation_leave();
}

static void ll_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    efs_client_map_opened((efs_ino_t)ino);
    ll_open_run(req, ino, fi);
    efs_stop_mutation_leave();
}

static void ll_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
                     size_t size, off_t off, struct fuse_file_info *fi)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_write_run(req, ino, buf, size, off, fi);
    efs_stop_mutation_leave();
}

static void ll_write_buf(fuse_req_t req, fuse_ino_t ino, struct fuse_bufvec *bufv,
                         off_t off, struct fuse_file_info *fi)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_write_buf_run(req, ino, bufv, off, fi);
    efs_stop_mutation_leave();
}

static void ll_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_flush_run(req, ino, fi);
    efs_stop_mutation_leave();
}

static void ll_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
                     struct fuse_file_info *fi)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_fsync_run(req, ino, datasync, fi);
    efs_stop_mutation_leave();
}

static void ll_fallocate(fuse_req_t req, fuse_ino_t ino, int mode, off_t offset,
                         off_t length, struct fuse_file_info *fi)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_fallocate_run(req, ino, mode, offset, length, fi);
    efs_stop_mutation_leave();
}

static void ll_mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
                     mode_t mode, dev_t rdev)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_mknod_run(req, parent, name, mode, rdev);
    efs_stop_mutation_leave();
}

static void ll_create(fuse_req_t req, fuse_ino_t parent, const char *name,
                      mode_t mode, struct fuse_file_info *fi)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    uint64_t previous=efs_client_rpc_deadline_ms();
    uint64_t deadline=stats_now_ms()+30000;
    if(previous && previous<deadline)deadline=previous;
    efs_client_rpc_set_deadline_ms(deadline);
    ll_create_run(req, parent, name, mode, fi);
    efs_client_rpc_set_deadline_ms(previous);
    efs_stop_mutation_leave();
}

static void ll_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                        const char *value, size_t size, int flags)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_setxattr_run(req, ino, name, value, size, flags);
    efs_stop_mutation_leave();
}

static void ll_removexattr(fuse_req_t req, fuse_ino_t ino, const char *name)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_removexattr_run(req, ino, name);
    efs_stop_mutation_leave();
}

static const struct fuse_lowlevel_ops efs_ll_ops = {
    .init = efs_fuse_init,
    .destroy = efs_fuse_destroy,
    .lookup = ll_lookup,
    .forget = ll_forget,
    .forget_multi = ll_forget_multi,
    .getattr = ll_getattr,
    .setattr = ll_setattr,
    .readlink = ll_readlink,
    .mknod = ll_mknod,
    .mkdir = ll_mkdir,
    .unlink = ll_unlink,
    .rmdir = ll_rmdir,
    .symlink = ll_symlink,
    .rename = ll_rename,
    .link = ll_link,
    .open = ll_open,
    .read = ll_read,
    .write = ll_write,
    .write_buf = ll_write_buf,
    .flush = ll_flush,
    .release = ll_release,
    .fsync = ll_fsync,
    .fallocate = ll_fallocate,
    .opendir = ll_opendir,
    .readdir = ll_readdir,
    .releasedir = ll_releasedir,
    .statfs = ll_statfs,
    .access = ll_access,
    .create = ll_create,
    .getlk = ll_getlk,
    .setlk = ll_setlk,
    .flock = ll_flock,
    .lseek = ll_lseek,
    .setxattr = ll_setxattr,
    .getxattr = ll_getxattr,
    .listxattr = ll_listxattr,
    .removexattr = ll_removexattr,
};

static int parse_addr(const char *str, char *host, size_t host_len, uint16_t *port)
{
    return efs_parse_host_port(str, host, host_len, port);
}

/* Pin the client to the HCA's NUMA node: CPU affinity for every thread
 * created after this call (sched_setaffinity on the main thread is
 * inherited) and MPOL_PREFERRED for the heap, so the write/read paths'
 * buffer pool, rdcache and RDMA pool live beside the HCA.
 *
 * Why (Oct 1 2026, fstor007, 2 sockets / 8 nodes, HCA on node 2): a 20 GiB
 * 1 MiB dd wrote 21.2 s unpinned and 18.0 s pinned (1.0 → 1.2 GB/s; the
 * FUSE thread's fuse_buf_copy into a cold remote-node pool buffer was
 * 0.3 ms/MiB), and the cold read went 10.4 s → 6.2 s (2.1 → 3.5 GB/s). On a
 * single-node host (fcstor) sysfs reports node -1 or 0 and this is a
 * no-op. EFS_NUMA_NODE=none disables it; =N forces node N. */
#ifndef MPOL_PREFERRED
#define MPOL_PREFERRED 1
#endif
static void numa_pin_startup(const char *host, uint16_t port)
{
    const char *env = getenv("EFS_NUMA_NODE");
    int node = -1;
    char hca[64] = "env";
    if (env && (strcmp(env, "none") == 0 || strcmp(env, "-1") == 0))
        return;
    if (env && *env && strcmp(env, "auto") != 0)
        node = atoi(env);
    else
        node = efs_rdma_numa_node_for_host(host, port, hca, sizeof(hca));
    if (node >= 0)
        efs_setting_u64("EFS_NUMA_NODE", (unsigned)node);
    if (node < 0)
        return;
    char path[128], buf[4096];
    snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/cpulist",
             node);
    FILE *f = fopen(path, "r");
    if (!f || !fgets(buf, sizeof(buf), f)) {
        if (f)
            fclose(f);
        return;
    }
    fclose(f);
    cpu_set_t set;
    CPU_ZERO(&set);
    int ncpu = 0;
    for (char *p = buf; *p && *p != '\n';) {
        char *end;
        long a = strtol(p, &end, 10), b = a;
        if (end == p)
            break;
        if (*end == '-')
            b = strtol(end + 1, &end, 10);
        for (long c = a; c <= b && c < CPU_SETSIZE; c++) {
            CPU_SET((int)c, &set);
            ncpu++;
        }
        p = (*end == ',') ? end + 1 : end;
    }
    if (ncpu == 0 || sched_setaffinity(0, sizeof(set), &set) != 0)
        return;
    unsigned long mask[2] = {0, 0};
    if (node < 128)
        mask[node / 64] |= 1UL << (node % 64);
    long mrc = syscall(SYS_set_mempolicy, MPOL_PREFERRED, mask, 128);
    fprintf(stderr, "efs: numa pin node=%d cpus=%d (hca %s) mempolicy=%s\n",
            node, ncpu, hca, mrc == 0 ? "preferred" : strerror(errno));
}

static pid_t g_perf_pid = -1;
static int g_perf_want;           /* --perf given; start after the fork */
static char g_perf_path[512];

/* --strace: `strace -f -tt -T -o <path> -p <pid>` on the daemon. ptrace
 * stops every syscall of every thread, so this is for one client and a
 * wall question (where did an fsync's 8 s go), never for a bandwidth
 * number. Needs kernel.yama.ptrace_scope=0 (runtime on the test hosts).
 * EFS_STRACE_EXPR narrows it, e.g. "trace=fsync,writev,recvfrom,futex"
 * (passed as -e). */
static pid_t g_strace_pid = -1;
static int g_strace_want;         /* --strace given; start after the fork */
static char g_strace_path[512];

/* SIGTERM makes perf finalize perf.data and strace detach; wait up to
 * ~5 s, then SIGKILL. */
static void stop_recorder(pid_t *pidp)
{
    efs_stop_recorder(pidp);
}

static void stop_perf_recorder(void)
{
    stop_recorder(&g_strace_pid);
    stop_recorder(&g_perf_pid);
}

/* Custom fuse_main replacement with explicit active and idle worker limits. */
/* libfuse >= 3.12 exposes an active-worker limit; the old API only
 * bounded idle workers and could create more while callbacks blocked. */
static struct fuse_loop_config *efs_fuse_worker_config(const struct fuse_cmdline_opts *opts)
{
    struct fuse_loop_config *config = fuse_loop_cfg_create();
    if (!config)
        return NULL;
    unsigned max_threads = opts->max_threads ? opts->max_threads : 32;
    unsigned idle_threads = opts->max_idle_threads ? opts->max_idle_threads : 8;
    if (idle_threads > max_threads)
        idle_threads = max_threads;
    fuse_loop_cfg_set_clone_fd(config, opts->clone_fd);
    fuse_loop_cfg_set_max_threads(config, max_threads);
    fuse_loop_cfg_set_idle_threads(config, idle_threads);
    fprintf(stderr, "efs-fuse: workers max=%u idle=%u\n",
            max_threads, idle_threads);

    return config;
}

static int efs_fuse_main_mt(int argc, char *argv[]);

/* Mount bootstrap. The Raft+KV host does not serve GET_META (there is no
 * serialized table), so a fetch path cannot work. The client already has the shard->group->voter mapping compiled in
 * (kv_key.h / raft.h), so all it needs from the cluster is confirmation that
 * the Raft export exists (kv_has_root) plus a local shell export to route by.
 * Poll RAFT_STATUS on the discovered nodes until one reports kv_has_root. */
/* W125: refuse to mount a cluster whose daemons do not accept this
 * client's protocol declaration, instead of mounting and then failing
 * every read. A node that refuses is incompatible; one that does not
 * answer within 3 s is counted as down. At least one node must accept
 * and none may refuse. */
static int protocol_check(void)
{
    uint32_t accepted = 0, refused = 0;
    for (uint32_t i = 0; i < g_client.node_count; i++) {
        int fd = efs_connect_tcp(g_client.nodes[i].addr, g_client.nodes[i].port);
        if (fd < 0)
            continue;
        efs_set_recv_timeout(fd, 3000);
        efs_set_send_timeout(fd, 3000);
        int rc = efs_fd_declare(fd);
        close(fd);
        if (rc == EFS_OK) {
            accepted++;
        } else if (rc != EFS_ERR_NET) {
            refused++;
            fprintf(stderr, "node %s:%u refused protocol %u (rc=%d)\n", g_client.nodes[i].addr,
                    g_client.nodes[i].port, EFS_PROTOCOL_VERSION, rc);
        } else {
            fprintf(stderr, "node %s:%u did not answer the protocol %u declaration "
                    "(older daemon or down)\n", g_client.nodes[i].addr,
                    g_client.nodes[i].port, EFS_PROTOCOL_VERSION);
        }
    }
    if (!accepted || refused) {
        fprintf(stderr, "Cluster does not support client protocol %u: upgrade the daemons "
                "before this client\n", EFS_PROTOCOL_VERSION);
        return EFS_ERR_PROTO;
    }
    return EFS_OK;
}

static int raft_bootstrap_metadata(void)
{
    for (int attempt = 0; attempt < 50; attempt++) {
        for (uint32_t i = 0; i < g_client.node_count; i++) {
            int fd = efs_connect_tcp(g_client.nodes[i].addr,
                                     g_client.nodes[i].port);
            if (fd < 0)
                continue;
            efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
            efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
            if (efs_fd_declare(fd) != EFS_OK) { /* W133: declared first */
                close(fd);
                continue;
            }
            uint8_t type = 0;
            void *payload = NULL;
            uint32_t plen = 0;
            if (efs_send_msg(fd, EFS_MSG_RAFT_STATUS, NULL, 0) == 0 &&
                efs_recv_msg(fd, &type, &payload, &plen) == 0 &&
                type == EFS_MSG_RAFT_STATUS_REPLY &&
                plen >= sizeof(struct efs_msg_raft_status_reply)) {
                struct efs_msg_raft_status_reply *r = payload;
                if (r->rc == EFS_OK && r->kv_has_root) {
                    free(payload);
                    close(fd);
                    goto ready;
                }
            }
            free(payload);
            close(fd);
        }
        usleep(100000); /* 100ms; mkfs/election may still be running */
    }
    fprintf(stderr,
            "efs-fuse: no node reports a Raft export "
            "(raft-status kv_has_root=0). Run 'efs-mgmt raft-mkfs' first.\n");
    return EFS_ERR_NOT_FOUND;

ready:
    efs_client_table_lock();
    pthread_mutex_lock(&g_client.idx_mu);
    /* main() already ran efs_export_init on this struct; re-initialising
     * without freeing would orphan the whole first table (it memsets the
     * struct, so every init-time allocation leaks). */
    efs_export_free(&g_client.export);
    efs_export_init(&g_client.export, g_client.export_id, g_client.export_name);
    g_client.export.root.shard_bits = EFS_KV_SHARD_BITS;
    g_client.export.root.shard_count = 1u << EFS_KV_SHARD_BITS;
    g_client.export.chunk_size = EFS_DEFAULT_CHUNK_SIZE;
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_table_unlock();
    fprintf(stderr,
            "meta: raft host mounted export '%s' "
            "(bits=%u, no GET_META table)\n",
            g_client.export_name, EFS_KV_SHARD_BITS);
    return EFS_OK;
}

/* A6/F1: libfuse and RPC workers share four malloc arenas, independently
 * of CPU count and whether the launcher was used. Per-core glibc defaults
 * retained tens of MiB of free holes across 74+ arenas under fixed-set
 * rewrites on large hosts. This is process policy, never a mode switch.
 * Keep glibc's initial large-allocation threshold fixed as well: otherwise
 * freeing a large transfer can raise it to MiB, putting later transient
 * frames into retained heap holes despite the bounded arena count. This
 * preserves malloc/free ownership while letting large buffers return their
 * pages on free. Other allocators do not expose these glibc policies:
 * an ASan build replaces malloc and its mallopt always fails, so the policy
 * is skipped there (K56) and stays fatal in every glibc build.
 * shortcut: relies on gcc's __SANITIZE_ADDRESS__ (the sanitizer gate's
 * compiler); a clang ASan build needs a __has_feature test. */
static int fuse_allocator_init(void)
{
#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__)
    if (!mallopt(M_ARENA_MAX, 4) || !mallopt(M_MMAP_THRESHOLD, 128 * 1024))
        return -1;
#endif
    return 0;
}

static size_t client_settings_text(char *buf, size_t size)
{
    int rows, cut;
    return efs_settings_render(buf, size, &rows, &cut);
}

int main(int argc, char **argv)
{
    /* Before transport, client pools or FUSE start any worker threads. */
    if (fuse_allocator_init()) {
        fprintf(stderr, "efs-fuse: cannot establish allocator policy\n");
        return 1;
    }
    if (argc==2 && !strcmp(argv[1],"--help-fuse")) {
        puts("efs-fuse - FUSE option reference\n");
        fuse_cmdline_help();fuse_lowlevel_help();return 0;
    }
    if (efs_help_requested(argc,argv)) { efs_cli_help(stdout,argv[0],NULL);return 0; }
    if (efs_env_removed_refuse("efs-fuse"))
        return 1;
    /* F5: the in-process client benchmark left the product binary; refuse
     * its flags rather than pass them to FUSE as mount options. */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bench") || !strcmp(argv[i], "--bench-time") ||
            !strcmp(argv[i], "--bench-mib")) {
            fprintf(stderr, "efs-fuse: %s was removed; measure the client with efs-bench "
                    "or through a mount\n", argv[i]);
            return 2;
        }
    }
#if !EFS_FAULTS
    efs_faults_env_ignored("efs-fuse");
#endif
    if (argc >= 3 && (strcmp(argv[1], "--stop") == 0 ||
                      strcmp(argv[1], "--resume") == 0)) {
        int force = argc == 4 && strcmp(argv[3], "--force-discard") == 0;
        if (argc != 3 && !force)
            return 2;
        const char *command = strcmp(argv[1], "--resume") == 0 ? "RESUME\n" :
                              force ? "FORCE\n" : "DRAIN\n";
        int rc = efs_stop_client(argv[2], command);
        if (rc)
            fprintf(stderr, "efs-fuse: stop control refused or unavailable (%d); mount retained\n", rc);
        return rc;
    }
    efs_version_check_argv("efs-fuse", argc, argv);

    /* Line-buffer logs even when stdout is a pipe (client.sh | tee). */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    efs_log_timestamps_install();
    efs_fuse_install_crash_handlers();
    efs_settings_init(EFS_SETTINGS_CLIENT);
    if (efs_transport_startup("efs-fuse") != EFS_OK)
        return 1;

    int perf = 0, strace_opt = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--perf") == 0)
            perf = 1;
        else if (strcmp(argv[i], "--strace") == 0)
            strace_opt = 1;
    }

    if (argc < 3) {
        efs_cli_help(stderr,argv[0],NULL);
        return 1;
    }

    /* Server addresses are the first arguments (they contain a colon);
     * the first argument without a colon is the mountpoint. There is one
     * export and no export label argument. */
    int arg_idx = 1;
    const char *nodes[EFS_MAX_NODES];
    uint32_t node_count = 0;
    while (arg_idx < argc && strchr(argv[arg_idx], ':') != NULL &&
           node_count < EFS_MAX_NODES) {
        nodes[node_count++] = argv[arg_idx++];
    }

    if (node_count == 0 || arg_idx >= argc) {
        fprintf(stderr, "Missing mountpoint\n");
        return 1;
    }
    const char *mountpoint = argv[arg_idx++];
    /* Options follow the mountpoint. A second positional is the removed
     * export-label form (NODE LABEL MOUNTPOINT): refuse it, never guess. */
    if (arg_idx < argc && argv[arg_idx][0] != '-') {
        fprintf(stderr, "efs-fuse: unexpected argument '%s' after mountpoint '%s'.\n"
                "Usage: efs-fuse NODE:PORT [NODE:PORT ...] MOUNTPOINT [options]\n"
                "The export label argument was removed; there is one export.\n",
                argv[arg_idx], mountpoint);
        return 1;
    }
    const char *export_name = "default";

    /* Canonical absolute mountpoint, used to render host-absolute .find
     * results. realpath(..., NULL) mallocs; fall back to the raw argument. */
    {
        char *rp = realpath(mountpoint, NULL);
        strncpy(g_mountpoint, rp ? rp : mountpoint, sizeof(g_mountpoint) - 1);
        g_mountpoint[sizeof(g_mountpoint) - 1] = '\0';
        free(rp);
        size_t mpl = strlen(g_mountpoint);
        while (mpl > 1 && g_mountpoint[mpl - 1] == '/')
            g_mountpoint[--mpl] = '\0';
    }

    {
        char ver[256];
        fprintf(stderr, "%s, build=%s: starting export=%s mount=%s\n",
                efs_version_string("efs-fuse", ver, sizeof(ver)),
                EFS_BUILD_ID, export_name, g_mountpoint);
    }

    {
        /* Before any thread or RDMA buffer exists: affinity is inherited
         * and the mempolicy applies to pages touched from here on. */
        char host[64];
        uint16_t port = 0;
        if (parse_addr(nodes[0], host, sizeof(host), &port) == 0)
            numa_pin_startup(host, port);
    }

    efs_client_init_nodes(&g_client, nodes, node_count);
    strncpy(g_client.export_name, export_name, EFS_MAX_NAME - 1);
    g_client.export_id = 1;
    pthread_mutex_init(&g_client.lock, NULL);
    efs_export_init(&g_client.export, g_client.export_id, g_client.export_name);
    efs_client_setup_mount_token();

    /* argv addresses are bootstrap only. init_nodes assigns 1..N, which
     * will not match a cluster started with --node-id 3,4,5. Always take
     * membership and real ids from LIST_NODES. */
    {
        char host[64];
        uint16_t port = 0;
        int discovered = 0;
        for (uint32_t i = 0; i < node_count; i++) {
            if (parse_addr(nodes[i], host, sizeof(host), &port) != 0)
                continue;
            if (efs_client_discover_nodes(&g_client, host, port) == 0) {
                printf("Discovered %u cluster nodes from %s\n",
                       g_client.node_count, nodes[i]);
                for (uint32_t n = 0; n < g_client.node_count; n++)
                    printf("  node id=%u %s:%u\n", g_client.nodes[n].id,
                           g_client.nodes[n].addr, g_client.nodes[n].port);
                discovered = 1;
                break;
            }
        }
        if (!discovered) {
            fprintf(stderr, "Could not discover cluster from any bootstrap node\n");
            return 1;
        }
    }

    /* Bootstrap metadata from the Raft+KV host: a thin RAFT_STATUS poll
     * for kv_has_root, then a local shell export — the client has the
     * shard->group->voter mapping compiled in (include/efs/kv_key.h,
     * include/efs/raft.h) and routes by it. No table is fetched. */
    if (protocol_check() != EFS_OK)
        return 1;
    printf("fetching metadata...\n");
    fflush(stdout);
    int rc = raft_bootstrap_metadata();
    if (rc != 0) {
        fprintf(stderr, "Could not establish the single Raft export root (%s). "
                "Check cluster health with efs-mgmt raft-status; initialize "
                "an unformatted cluster with efs-mgmt raft-mkfs.\n",
                efs_strerror(rc));
        return 1;
    }
    /* K33: the committed record this mount starts from; the serving process
     * installs later revisions without a remount (efs_client_view_start). */
    if (efs_client_view_mount() != EFS_OK)
        return 1;
    /* PUTs carry export_id; do not leave the hardcoded 1 if meta says otherwise. */
    g_client.export_id = g_client.export.id ? g_client.export.id : 1;
    if (g_client.export.name[0] &&
        strcmp(g_client.export.name, g_client.export_name) != 0) {
        /* Hard fail: writing into the wrong export used to be possible when
         * the server minted an empty export for an unknown name — the
         * client's flushes then lost every generation check against the real
         * export and looped STALE forever. Refuse instead. */
        fprintf(stderr,
                "ERROR: requested export '%s' but the cluster serves '%s' "
                "(id=%u). Refusing to mount the wrong export.\n",
                g_client.export_name, g_client.export.name, g_client.export_id);
        return 1;
    }
    printf("export id=%u name=%s\n", g_client.export_id,
           g_client.export.name[0] ? g_client.export.name : g_client.export_name);
    /* K5/I23: the mount session is established after the daemon fork, in
     * the process that serves the mount (K31): over RDMA the session RPC
     * starts the device's completion poller thread, and fork() would leave
     * the serving daemon without it. */
    {
        /* Post-step-11 there is no bootstrap table fetch: the staging table
         * starts empty and fills on demand (client-cache Part A bounds it
         * to EFS_CLIENT_META_MB). Print cache occupancy, not a table scan. */
        struct efs_inode root;
        int rrc = efs_export_get_inode(&g_client.export, EFS_ROOT_INO, &root);
        printf("meta ready staged_rows=%llu staged_chunks=%llu "
               "staged_bytes=%llu root_get=%d mode=%o\n",
               (unsigned long long)g_client.export.inode_count,
               (unsigned long long)g_client.export.chunk_count,
               (unsigned long long)efs_export_staged_bytes(&g_client.export),
               rrc, rrc == 0 ? root.mode : 0);
    }
    fflush(stdout);
    /* The server is the sole metadata writer: no client write lease, no
     * client-side root flush. The background REPORT thread starts in .init
     * (efs_client_enable_meta_batch). */

    char *fuse_argv[64];
    int fuse_argc = 0;
    fuse_argv[fuse_argc++] = argv[0];
    fuse_argv[fuse_argc++] = (char *)mountpoint;
    /* Timeouts are per lookup/getattr reply (always 0). High-level
     * attr_timeout= mount options are not valid for fuse_session_new. */
    while (arg_idx < argc && fuse_argc < 63) {
        if (strcmp(argv[arg_idx], "--perf") == 0 ||
            strcmp(argv[arg_idx], "--strace") == 0) {
            arg_idx++;
            continue;
        }
        fuse_argv[fuse_argc++] = argv[arg_idx++];
    }
    fuse_argv[fuse_argc] = NULL;

    if (perf) {
        /* The recorder is started inside efs_fuse_main_mt, AFTER the
         * daemon fork: started here it attaches to the parent, which
         * _exit()s once FUSE_INIT is answered, and perf dies with
         * "Couldn't create thread/CPU maps: No such process" (the
         * 20260929-021039-iorperf run recorded nothing on 9 clients). */
        const char *pp = getenv("EFS_PERF_PATH");
        if (pp && *pp)
            snprintf(g_perf_path, sizeof(g_perf_path), "%s", pp);
        else
            snprintf(g_perf_path, sizeof(g_perf_path), "/tmp/efs-fuse-perf-%d/perf.data", (int)getpid());
        g_perf_want = 1;
    }
    if (strace_opt) {
        const char *sp = getenv("EFS_STRACE_PATH");
        if (sp && *sp)
            snprintf(g_strace_path, sizeof(g_strace_path), "%s", sp);
        else
            snprintf(g_strace_path, sizeof(g_strace_path), "/tmp/efs-fuse-strace-%d.txt", (int)getpid());
        g_strace_want = 1;
    }

    int ret = efs_fuse_main_mt(fuse_argc, fuse_argv);
    stop_perf_recorder();
    /* destroy() already shut down on clean unmount; a repeat returns its
     * result. A failed flush, settle or revoke is not a clean exit. */
    efs_client_set_stop_observer(efs_stop_phase);
    if (efs_client_shutdown() != EFS_OK)
        unmount_unclean(efs_client_shutdown_reason());
    if (!ret && g_unmount_unclean) {
        fprintf(stderr, "efs-fuse: unmount did not complete cleanly; exit 1\n");
        ret = 1;
    }
    /* The stop record's last word: the stop tooling reports this verdict. */
    if (ret) {
        char why[sizeof(g_unclean_reason) + 32];
        snprintf(why, sizeof(why), "%s", g_unclean_reason[0] ? g_unclean_reason : "");
        if (!why[0])
            snprintf(why, sizeof(why), "efs-fuse exit %d", ret);
        efs_stop_status("failed", 0, 0, ret, why);
    } else
        efs_stop_status("done", 0, 0, 0, NULL);
    return ret;
}

/* libfuse supplies max_threads=10 and an unset idle sentinel itself,
 * so zero-value fallbacks cannot establish our bounded defaults. Insert them
 * first; later explicit user options retain ordinary last-option precedence. */
static int efs_fuse_parse_cmdline(struct fuse_args *args,
                                  struct fuse_cmdline_opts *opts)
{
    if(!args || args->argc<1 || !args->argv)return -1;
    struct fuse_args configured=FUSE_ARGS_INIT(0,NULL);
    for(int i=0;i<args->argc;i++) {
        if(fuse_opt_add_arg(&configured,args->argv[i])!=0)goto failed;
        /* Linux's default 256-page limit includes the user's first-page
         * offset. A 1 MiB malloc buffer at offset 16 otherwise becomes
         * 1 MiB-16 plus 16 bytes. Half-MiB requests fit with page headroom
         * and divide ordinary 1 MiB reads into chunk-aligned requests.
         * max_read is a mount option and must match conn->max_read in init. */
        if(i==0 && fuse_opt_add_arg(&configured,
            "-omax_read=524288,max_threads=32,max_idle_threads=8")!=0)goto failed;
    }
    fuse_opt_free_args(args);
    *args=configured;
    return fuse_parse_cmdline(args,opts);
failed:
    fuse_opt_free_args(&configured);
    return -1;
}

static int efs_fuse_main_mt(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fuse_cmdline_opts opts;
    struct fuse_loop_config *config = NULL;
    int ret = -1;

    if (efs_fuse_parse_cmdline(&args, &opts) != 0)
        return 1;

    if (opts.show_version) {
        printf("FUSE library version %s\n", fuse_pkgversion());
        ret = 0;
        goto out;
    }

    if (opts.show_help) {
        efs_cli_help(stdout,"efs-fuse",NULL);
        ret = 0;
        goto out;
    }

    if (!opts.mountpoint) {
        fprintf(stderr, "error: no mountpoint specified\n");
        ret = 1;
        goto out;
    }

    /* Do not fuse_daemonize(): it returns the parent at fuse_mount, before
     * the session loop, and it redirects stdout/stderr to /dev/null so a
     * `>fuse.log` wrapper only captures pre-mount lines. Fork first, mount
     * only in the child, and hold the parent until FUSE_INIT. */
    if (!opts.foreground) {
        int pfd[2];
        if (pipe(pfd) != 0) {
            ret = 1;
            goto out;
        }
        pid_t child = fork();
        if (child < 0) {
            close(pfd[0]);
            close(pfd[1]);
            ret = 1;
            goto out;
        }
        if (child > 0) {
            close(pfd[1]);
            int ok = efs_fuse_wait_ready(pfd[0], child, 30000);
            close(pfd[0]);
            if (ok != 0)
                fprintf(stderr,
                        "ERROR: efs-fuse did not start serving within 30s\n");
            /* Do not return through efs_client_shutdown: that would close the
             * child's dup'd server sockets. Freeing the bootstrap metadata
             * table is heap-only and safe — and keeps the parent's exit
             * leak-clean under valgrind. */
            efs_export_free(&g_client.export);
            fflush(stderr);
            _exit(ok == 0 ? 0 : 1);
        }
        close(pfd[0]);
        g_fuse_ready_wr = pfd[1];
        (void)setsid();
        fprintf(stderr, "efs-fuse daemon pid=%d\n", (int)getpid());
        fflush(stderr);
    }
    /* K5/I23: one session per mount, committed on its owner shard before
     * the first mutation. Without it every mutation would be refused, so
     * fail the mount instead of serving a read-only surprise. K31: here,
     * after the daemon fork, so the RDMA poller it may start belongs to the
     * serving process. A failure exits the child before FUSE_INIT; the
     * waiting parent reports it. */
    int session_rc = efs_client_session_start();
    if (session_rc != EFS_OK) {
        fprintf(stderr, "Could not establish the mount session (%s).\n",
                efs_strerror(session_rc));
        fflush(stderr);
        ret = 1;
        goto out;
    }
    {
        char hex[2 * EFS_OPID_UUID_LEN + 1];
        for (unsigned i = 0; i < EFS_OPID_UUID_LEN; i++)
            snprintf(hex + 2 * i, 3, "%02x", g_client.opid_uuid[i]);
        printf("session uuid=%s epoch=%u\n", hex, g_client.opid_epoch);
    }
    fflush(stdout);

    /* This is the process that serves the mount (the child, or the only
     * process under -f). Attach the recorder to it; the parent above has
     * already been excluded. */
    if (g_perf_want) {
        g_perf_pid = efs_start_perf_recorder(getpid(), g_perf_path);
        if (g_perf_pid < 0)
            fprintf(stderr, "Warning: could not start perf recorder; continuing without profiling\n");
        else
            fprintf(stderr, "efs-fuse perf recorder pid=%d -> %s\n", (int)g_perf_pid, g_perf_path);
        fflush(stderr);
    }
    if (g_strace_want) {
        g_strace_pid = efs_start_strace_recorder(getpid(), g_strace_path);
        if (g_strace_pid < 0)
            fprintf(stderr, "Warning: could not start strace; continuing without it\n");
        else
            fprintf(stderr, "efs-fuse strace pid=%d -> %s\n", (int)g_strace_pid, g_strace_path);
        fflush(stderr);
    }

    g_fuse_se = fuse_session_new(&args, &efs_ll_ops, sizeof(efs_ll_ops), NULL);
    if (g_fuse_se == NULL) {
        ret = 1;
        goto out;
    }

    if (fuse_set_signal_handlers(g_fuse_se) != 0) {
        fuse_session_destroy(g_fuse_se);
        g_fuse_se = NULL;
        ret = 1;
        goto out;
    }

    if (fuse_session_mount(g_fuse_se, opts.mountpoint) != 0) {
        fuse_remove_signal_handlers(g_fuse_se);
        fuse_session_destroy(g_fuse_se);
        g_fuse_se = NULL;
        ret = 1;
        goto out;
    }

    /* Idle limits do not bound active request workers. Each worker owns
     * a FUSE receive buffer, stack and TLS scratch outside the cache cap. */
    config = efs_fuse_worker_config(&opts);
    if (!config) {
        fuse_remove_signal_handlers(g_fuse_se);
        fuse_session_unmount(g_fuse_se);
        fuse_session_destroy(g_fuse_se);
        g_fuse_se = NULL;
        ret = 1;
        goto out;
    }

    efs_stop_settings_text = client_settings_text;
    if (efs_stop_start(opts.mountpoint, efs_control_drain) != 0) {
        fprintf(stderr, "efs-fuse: cannot establish safe-stop control channel\n");
        fuse_loop_cfg_destroy(config);
        fuse_remove_signal_handlers(g_fuse_se);
        fuse_session_unmount(g_fuse_se);
        fuse_session_destroy(g_fuse_se);
        g_fuse_se = NULL;
        ret = 1;
        goto out;
    }
    (void)ll_inval_start();
    ret = fuse_session_loop_mt(g_fuse_se, config);
    fuse_loop_cfg_destroy(config);
    efs_stop_finish();
    ll_inval_stop();

    fuse_remove_signal_handlers(g_fuse_se);
    fuse_session_unmount(g_fuse_se);
    fuse_session_destroy(g_fuse_se);
    g_fuse_se = NULL;

out:
    if (g_fuse_ready_wr >= 0) {
        close(g_fuse_ready_wr);
        g_fuse_ready_wr = -1;
    }
    free(opts.mountpoint);
    fuse_opt_free_args(&args);
    return ret;
}
