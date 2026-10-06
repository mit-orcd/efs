#include "bench_local.h"
#include "efs/checksum.h"
#include "efs/common.h"
#include "efs/kv.h"
#include "efs/kv_lsm.h"
#include "efs/raft.h"
#include "efs/raft_disk.h"
#include "efs/store.h"
#include "server_internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Dedicated export id for bench fragments (same id efs-bench --store uses;
 * the tree under data/exports/<id>/ is removed after each path-count block). */
#define EFS_BENCH_EXPORT_ID  ((efs_export_id_t)0xB7)

#define BENCH_MAX_SLOTS   256
#define BENCH_LAT_MAX     (1u << 20) /* per-thread latency sample cap */
#define BENCH_CI_SPAN     (1u << 22) /* chunk-index space per slot */

static const int bench_qds[] = { 1, 16, 64, 256 };
#define BENCH_NQDS 4

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int mkdir_p(const char *path)
{
    char tmp[EFS_MAX_PATH];
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

static void rm_tree(const char *path)
{
    DIR *d = opendir(path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char child[EFS_MAX_PATH];
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            struct stat st;
            if (lstat(child, &st) != 0)
                continue;
            if (S_ISDIR(st.st_mode))
                rm_tree(child);
            else
                unlink(child);
        }
        closedir(d);
    }
    rmdir(path);
}

static int dir_is_empty(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return 1; /* absent: the bench creates it */
    struct dirent *de;
    int empty = 1;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
            empty = 0;
            break;
        }
    }
    closedir(d);
    return empty;
}

/* Fill with a non-zero pattern (all-zero payloads take the known-zero path
 * in the store, which is not what a PUT costs). */
static void fill_nonzero(uint8_t *buf, uint32_t len, uint64_t seed)
{
    uint64_t x = seed ? seed : 0x9E3779B97F4A7C15ULL;
    for (uint32_t i = 0; i < len; i += 8) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        uint64_t v = x | 1ULL;
        uint32_t n = len - i < 8 ? len - i : 8;
        memcpy(buf + i, &v, n);
    }
}

/* ---- per-op latency samples (exact p50/p99 at round end) ---- */

struct lat_vec {
    uint64_t *v;
    uint32_t n;
    uint32_t cap;
};

static void lat_add(struct lat_vec *lv, uint64_t us)
{
    if (lv->n >= BENCH_LAT_MAX)
        return; /* sampling stops; ops still count */
    if (lv->n == lv->cap) {
        uint32_t ncap = lv->cap ? lv->cap * 2 : 4096;
        if (ncap > BENCH_LAT_MAX)
            ncap = BENCH_LAT_MAX;
        uint64_t *nv = realloc(lv->v, (size_t)ncap * sizeof(*nv));
        if (!nv)
            return;
        lv->v = nv;
        lv->cap = ncap;
    }
    lv->v[lv->n++] = us;
}

static int u64_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* ---- /proc/diskstats: util per device over one round ---- */

#define DS_MAX_DEV 64
struct ds_row {
    char name[32];
    uint64_t io_ms;
};

static int ds_read(struct ds_row *rows)
{
    FILE *f = fopen("/proc/diskstats", "r");
    if (!f)
        return 0;
    char line[512];
    int n = 0;
    while (n < DS_MAX_DEV && fgets(line, sizeof(line), f)) {
        unsigned maj, mino;
        char name[32];
        uint64_t r1, r2, r3, r4, w1, w2, w3, w4, inflight, io_ms, wio_ms;
        int got = sscanf(line, "%u %u %31s %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                         " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                         " %" SCNu64 " %" SCNu64 " %" SCNu64,
                         &maj, &mino, name, &r1, &r2, &r3, &r4, &w1, &w2, &w3,
                         &w4, &inflight, &io_ms, &wio_ms);
        if (got != 14)
            continue;
        if (strncmp(name, "loop", 4) == 0 || strncmp(name, "ram", 3) == 0 ||
            strncmp(name, "dm-", 3) == 0)
            continue;
        strncpy(rows[n].name, name, sizeof(rows[n].name) - 1);
        rows[n].io_ms = io_ms;
        n++;
    }
    fclose(f);
    return n;
}

static void ds_print_delta(const struct ds_row *a, int na, const struct ds_row *b,
                           int nb, double wall_s)
{
    printf(" diskutil:");
    int printed = 0;
    for (int i = 0; i < nb; i++) {
        for (int j = 0; j < na; j++) {
            if (strcmp(a[j].name, b[i].name) != 0)
                continue;
            uint64_t d = b[i].io_ms - a[j].io_ms;
            double util = wall_s > 0.0 ? (double)d / (wall_s * 10.0) : 0.0;
            if (util > 200.0)
                util = 200.0;
            if (util >= 0.5)
                printf(" %s=%.0f%%", b[i].name, util);
            printed++;
            break;
        }
    }
    if (!printed)
        printf(" n/a");
    printf("\n");
}

/* ---- perf recorder (attach to this process; never strace) ---- */

static pid_t g_perf_pid = -1;

static pid_t start_perf_recorder(pid_t target, const char *perf_path)
{
    char dir[EFS_MAX_PATH];
    strncpy(dir, perf_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(dir);
    }
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char pid_str[32];
        snprintf(pid_str, sizeof(pid_str), "%d", (int)target);
        execlp("perf", "perf", "record", "-g", "-F", "999", "-p", pid_str,
               "-o", perf_path, NULL);
        _exit(1);
    }
    return pid;
}

static void stop_perf_recorder(void)
{
    if (g_perf_pid > 0) {
        kill(g_perf_pid, SIGTERM);
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

static void perf_print_top(const char *perf_path)
{
    char cmd[EFS_MAX_PATH + 160];
    snprintf(cmd, sizeof(cmd),
             "perf report --stdio --no-children --percent-limit=2 -i '%s' "
             "2>/dev/null | grep -E '^\\s+[0-9]+\\.[0-9]+%%' | head -8 | "
             "sed 's/^/PERF_TOP/'", perf_path);
    (void)system(cmd);
}

/* ---- ceiling probe: fio when present, else the same shapes in-process ---- */

static int have_fio(void)
{
    return system("command -v fio >/dev/null 2>&1") == 0;
}

static void ceil_fio(const char *root)
{
    char cmd[EFS_MAX_PATH + 512];
    printf("FIO_CEIL begin root=%s (same-host ceiling; the engine runs below "
           "are judged against these)\n", root);
    fflush(stdout);
    /* Sequential 64 KiB O_DIRECT write, flush in the clock. */
    snprintf(cmd, sizeof(cmd),
             "fio --name=ceil_wseq --rw=write --bs=64k --direct=1 "
             "--ioengine=psync --size=1G --end_fsync=1 --directory='%s' "
             "--filename=ceilfile --minimal 2>/dev/null | "
             "sed 's/^/FIO_CEIL wseq64k /'", root);
    (void)system(cmd);
    /* Sequential 64 KiB O_DIRECT read of the same file. */
    snprintf(cmd, sizeof(cmd),
             "fio --name=ceil_rseq --rw=read --bs=64k --direct=1 "
             "--ioengine=psync --size=1G --directory='%s' "
             "--filename=ceilfile --minimal 2>/dev/null | "
             "sed 's/^/FIO_CEIL rseq64k /'", root);
    (void)system(cmd);
    /* Preallocate the metadata file without per-op fsync (a 64 MiB
     * fsync-per-write fill would take minutes on a slow disk). */
    snprintf(cmd, sizeof(cmd),
             "fio --name=ceil_pre --rw=write --bs=1m --direct=1 "
             "--ioengine=psync --size=64m --directory='%s' "
             "--filename=ceilmeta >/dev/null 2>&1", root);
    (void)system(cmd);
    /* Metadata shape: small synchronous writes (WAL/log append shape),
     * bounded by runtime so a slow disk cannot stretch the bench. */
    snprintf(cmd, sizeof(cmd),
             "fio --name=ceil_wsync --rw=write --bs=4k --direct=1 --fsync=1 "
             "--ioengine=psync --size=64m --runtime=20 --time_based "
             "--directory='%s' --filename=ceilmeta --minimal 2>/dev/null | "
             "sed 's/^/FIO_CEIL wsync4k /'", root);
    (void)system(cmd);
    /* Metadata shape: random 4 KiB reads of a segment block. */
    snprintf(cmd, sizeof(cmd),
             "fio --name=ceil_rrand --rw=randread --bs=4k --direct=1 "
             "--ioengine=psync --size=64m --directory='%s' "
             "--filename=ceilmeta --minimal 2>/dev/null | "
             "sed 's/^/FIO_CEIL rrand4k /'", root);
    (void)system(cmd);
    snprintf(cmd, sizeof(cmd), "rm -f '%s'/ceilfile '%s'/ceilmeta*", root, root);
    (void)system(cmd);
    printf("FIO_CEIL end\n");
    fflush(stdout);
}

/* No fio on this host: measure the same four shapes with raw syscalls so the
 * log still carries a ceiling line (labelled RAW, not fio). */
static void ceil_raw(const char *root)
{
    char path[EFS_MAX_PATH];
    snprintf(path, sizeof(path), "%s/ceil-raw.bin", root);
    uint8_t *buf = NULL;
    if (posix_memalign((void **)&buf, 4096, EFS_FRAGMENT_SIZE) != 0)
        return;
    fill_nonzero(buf, EFS_FRAGMENT_SIZE, 7);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    if (fd < 0) {
        printf("CEIL_RAW unavailable: open %s: %s\n", path, strerror(errno));
        free(buf);
        return;
    }
    /* wseq64k: 512 MiB, flush in the clock. */
    double t0 = now_sec();
    uint64_t bytes = 0;
    for (int i = 0; i < 8192; i++) {
        if (pwrite(fd, buf, EFS_FRAGMENT_SIZE, (off_t)i * EFS_FRAGMENT_SIZE) !=
            (ssize_t)EFS_FRAGMENT_SIZE)
            break;
        bytes += EFS_FRAGMENT_SIZE;
    }
    fsync(fd);
    double wall = now_sec() - t0;
    printf("CEIL_RAW wseq64k bytes=%llu wall_s=%.3f GiB_s=%.3f\n",
           (unsigned long long)bytes, wall,
           (double)bytes / (1 << 30) / (wall > 1e-9 ? wall : 1e-9));
    close(fd);

    /* rseq64k over the file just written. */
    fd = open(path, O_RDONLY | O_DIRECT);
    if (fd >= 0) {
        t0 = now_sec();
        bytes = 0;
        for (int i = 0; i < 8192; i++) {
            if (pread(fd, buf, EFS_FRAGMENT_SIZE, (off_t)i * EFS_FRAGMENT_SIZE) !=
                (ssize_t)EFS_FRAGMENT_SIZE)
                break;
            bytes += EFS_FRAGMENT_SIZE;
        }
        wall = now_sec() - t0;
        printf("CEIL_RAW rseq64k bytes=%llu wall_s=%.3f GiB_s=%.3f\n",
               (unsigned long long)bytes, wall,
               (double)bytes / (1 << 30) / (wall > 1e-9 ? wall : 1e-9));
        close(fd);
    }

    /* wsync4k: small write + fdatasync per op (the WAL shape). */
    fd = open(path, O_WRONLY | O_TRUNC | O_DIRECT);
    if (fd >= 0) {
        uint64_t ops = 0;
        t0 = now_sec();
        double deadline = t0 + 5.0;
        while (now_sec() < deadline) {
            off_t off = (off_t)(ops % 16384) * 4096;
            if (pwrite(fd, buf, 4096, off) != 4096)
                break;
            fdatasync(fd);
            ops++;
        }
        wall = now_sec() - t0;
        printf("CEIL_RAW wsync4k ops=%llu wall_s=%.3f ops_s=%.1f avg_us=%.1f\n",
               (unsigned long long)ops, wall, (double)ops / wall,
               wall * 1e6 / (ops ? (double)ops : 1.0));
        close(fd);
    }

    /* rrand4k over a 64 MiB block. */
    fd = open(path, O_RDONLY | O_DIRECT);
    if (fd >= 0) {
        uint64_t ops = 0, x = 12345;
        t0 = now_sec();
        double deadline = t0 + 5.0;
        while (now_sec() < deadline) {
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
            off_t off = (off_t)((x >> 33) % 16384) * 4096;
            if (pread(fd, buf, 4096, off) != 4096)
                break;
            ops++;
        }
        wall = now_sec() - t0;
        printf("CEIL_RAW rrand4k ops=%llu wall_s=%.3f ops_s=%.1f avg_us=%.1f\n",
               (unsigned long long)ops, wall, (double)ops / wall,
               wall * 1e6 / (ops ? (double)ops : 1.0));
        close(fd);
    }
    unlink(path);
    free(buf);
}

/* ---- data bench: the fragment store + writer pool, the handlers' way ---- */

struct bench_slot {
    int slot;
    struct efsd_server *s;
    struct efs_export *ex;
    uint32_t frag_len;
    int reading;
    double deadline;
    uint32_t ci_base;
    uint32_t written;     /* chunks this slot has stored (across rounds) */
    uint32_t read_seq;
    uint64_t ops;
    uint64_t errors;
    struct lat_vec lat;
    uint8_t *buf;
    uint8_t sum[EFS_HASH_SIZE];
};

/* The PUT handler's store call, verbatim minus the socket: same export
 * bind, same first-write path hint, same efs_store_put into the writer pool. */
static int slot_put(struct bench_slot *sl, uint32_t chunk_index)
{
    struct efs_store st;
    struct efs_nvme_store nctx;
    struct efs_frag_id fid = {
        .export_id = sl->ex->id,
        .ino = (efs_ino_t)(0xBEEF0000u + (uint32_t)sl->slot),
        .chunk_index = chunk_index,
        .fragment_index = (uint32_t)(sl->slot % EFS_NUM_FRAGMENTS),
        .chunk_generation = 0,
    };
    efs_store_nvme_bind(&st, &nctx, sl->s, sl->ex);
    efs_tls_path_hint = EFS_PATH_HINT_SKIP; /* handler's EFS_PATH_HINT_NEW */
    int rc = efs_store_put(&st, &fid, sl->buf, sl->frag_len, sl->sum);
    efs_tls_path_hint = -1;
    efs_tls_path_used = -1;
    return rc;
}

/* The GET handler's store call: same bind, same combined fragment+sidecar
 * read, same payload hash verify (I25) before the op counts. */
static int slot_get(struct bench_slot *sl, uint32_t chunk_index)
{
    struct efs_store st;
    struct efs_nvme_store nctx;
    struct efs_frag_id fid = {
        .export_id = sl->ex->id,
        .ino = (efs_ino_t)(0xBEEF0000u + (uint32_t)sl->slot),
        .chunk_index = chunk_index,
        .fragment_index = (uint32_t)(sl->slot % EFS_NUM_FRAGMENTS),
        .chunk_generation = 0,
    };
    uint32_t data_len = sl->frag_len;
    uint8_t sum[EFS_HASH_SIZE];
    int sum_ok = 0;
    efs_store_nvme_bind(&st, &nctx, sl->s, sl->ex);
    int rc = efs_store_get(&st, &fid, sl->buf, &data_len, sum, &sum_ok);
    if (rc != 0 || data_len != sl->frag_len)
        return rc != 0 ? rc : EFS_ERR_IO;
    if (sum_ok) {
        uint8_t vh[EFS_HASH_SIZE];
        efs_hash(sl->buf, data_len, vh);
        if (memcmp(vh, sum, EFS_HASH_SIZE) != 0)
            return EFS_ERR_IO;
    }
    return EFS_OK;
}

static void *slot_main(void *arg)
{
    struct bench_slot *sl = arg;
    while (now_sec() < sl->deadline) {
        uint32_t ci;
        if (sl->reading) {
            if (sl->written == 0)
                break;
            ci = sl->ci_base + (sl->read_seq++ % sl->written);
        } else {
            ci = sl->ci_base + sl->written;
        }
        uint64_t t0 = efs_iostats_now_us();
        int rc = sl->reading ? slot_get(sl, ci) : slot_put(sl, ci);
        lat_add(&sl->lat, efs_iostats_now_us() - t0);
        if (rc == EFS_OK) {
            sl->ops++;
            if (!sl->reading)
                sl->written++;
        } else {
            sl->errors++;
        }
    }
    return NULL;
}

/* One ladder round: qd threads, each on its own slot, for time_sec. */
static void run_round(struct bench_slot *slots, int reading, int npaths,
                      int qd, double time_sec, uint32_t frag_len)
{
    pthread_t tids[BENCH_MAX_SLOTS];
    struct ds_row ds0[DS_MAX_DEV], ds1[DS_MAX_DEV];
    struct efs_msg_io_stats_reply io0, io1;

    for (int i = 0; i < qd; i++) {
        slots[i].reading = reading;
        slots[i].ops = 0;
        slots[i].errors = 0;
        slots[i].lat.n = 0;
        slots[i].read_seq = 0;
    }
    efs_iostats_snapshot(&io0);
    int nds0 = ds_read(ds0);
    double t0 = now_sec();
    double deadline = t0 + time_sec;
    for (int i = 0; i < qd; i++) {
        slots[i].deadline = deadline;
        if (pthread_create(&tids[i], NULL, slot_main, &slots[i]) != 0) {
            slots[i].errors++;
            tids[i] = 0;
        }
    }
    for (int i = 0; i < qd; i++) {
        if (tids[i])
            pthread_join(tids[i], NULL);
    }
    double wall = now_sec() - t0;
    int nds1 = ds_read(ds1);
    efs_iostats_snapshot(&io1);
    if (wall < 1e-6)
        wall = 1e-6;

    uint64_t ops = 0, errors = 0, samples = 0;
    for (int i = 0; i < qd; i++) {
        ops += slots[i].ops;
        errors += slots[i].errors;
        samples += slots[i].lat.n;
    }
    /* Exact percentiles over all recorded samples of the round. */
    uint64_t *all = malloc((size_t)(samples ? samples : 1) * sizeof(*all));
    uint64_t p50 = 0, p99 = 0, max = 0;
    if (all) {
        uint64_t k = 0;
        for (int i = 0; i < qd; i++) {
            memcpy(all + k, slots[i].lat.v,
                   (size_t)slots[i].lat.n * sizeof(*all));
            k += slots[i].lat.n;
        }
        qsort(all, (size_t)k, sizeof(*all), u64_cmp);
        if (k) {
            p50 = all[k / 2];
            p99 = all[(k * 99) / 100];
            max = all[k - 1];
        }
        free(all);
    }
    uint64_t diskw_ops = io1.cls[EFS_IOSTAT_DISK_WRITE].ops -
                         io0.cls[EFS_IOSTAT_DISK_WRITE].ops;
    uint64_t diskw_us = io1.cls[EFS_IOSTAT_DISK_WRITE].us_sum -
                        io0.cls[EFS_IOSTAT_DISK_WRITE].us_sum;
    double gib_s = (double)ops * frag_len / (1 << 30) / wall;
    printf("BENCH_OK kind=data rw=%s paths=%d qd=%d ops=%llu wall_s=%.3f "
           "ops_s=%.1f p50_us=%llu p99_us=%llu max_us=%llu GiB_s=%.3f "
           "errors=%llu diskw_ops=%llu diskw_avg_us=%llu\n",
           reading ? "read" : "write", npaths, qd, (unsigned long long)ops,
           wall, (double)ops / wall, (unsigned long long)p50,
           (unsigned long long)p99, (unsigned long long)max, gib_s,
           (unsigned long long)errors, (unsigned long long)diskw_ops,
           (unsigned long long)(diskw_ops ? diskw_us / diskw_ops : 0));
    ds_print_delta(ds0, nds0, ds1, nds1, wall);
    fflush(stdout);
}

static void bench_export_tree_rm(struct efsd_server *s)
{
    for (uint32_t pi = 0; pi < s->storage_path_count; pi++) {
        char dir[EFS_MAX_PATH];
        snprintf(dir, sizeof(dir), "%s/data/exports/%u", s->storage_paths[pi],
                 (unsigned)EFS_BENCH_EXPORT_ID);
        rm_tree(dir);
    }
}

static int run_data_bench(struct efsd_server *s, double time_sec)
{
    uint32_t npaths_total = s->storage_path_count;
    if (npaths_total == 0 || npaths_total > EFS_MAX_STORAGE_PATHS) {
        fprintf(stderr, "bench: data needs 1..%d --storage roots\n",
                EFS_MAX_STORAGE_PATHS);
        return 1;
    }
    for (uint32_t pi = 0; pi < npaths_total; pi++) {
        if (!dir_is_empty(s->storage_paths[pi])) {
            fprintf(stderr,
                    "bench: %s is not empty; --bench storage must be scratch "
                    "(never a live data root)\n", s->storage_paths[pi]);
            return 1;
        }
        if (mkdir_p(s->storage_paths[pi]) != 0 && errno != EEXIST) {
            fprintf(stderr, "bench: mkdir %s: %s\n", s->storage_paths[pi],
                    strerror(errno));
            return 1;
        }
        char sub[EFS_MAX_PATH];
        snprintf(sub, sizeof(sub), "%s/data", s->storage_paths[pi]);
        mkdir_p(sub);
        snprintf(sub, sizeof(sub), "%s/meta", s->storage_paths[pi]);
        mkdir_p(sub);
        snprintf(sub, sizeof(sub), "%s/log", s->storage_paths[pi]);
        mkdir_p(sub);
    }
    strncpy(s->storage_path, s->storage_paths[0], sizeof(s->storage_path) - 1);

    s->id = 250;
    strncpy(s->addr, "127.0.0.1", sizeof(s->addr) - 1);
    s->nodes[0].id = s->id;
    s->nodes[0].port = 0;
    s->node_count = 1;
    s->quota = 0;
    s->persist_nodes = 0;

    if (have_fio())
        ceil_fio(s->storage_paths[0]);
    else
        ceil_raw(s->storage_paths[0]);

    pthread_mutex_lock(&s->lock);
    struct efs_export *ex =
        server_export_acquire_or_create_locked(s, EFS_BENCH_EXPORT_ID);
    pthread_mutex_unlock(&s->lock);
    if (!ex) {
        fprintf(stderr, "bench: cannot create export %u\n",
                (unsigned)EFS_BENCH_EXPORT_ID);
        return 1;
    }
    uint32_t frag_len = server_frag_len(ex, (efs_ino_t)0xBEEF0000u);

    printf("bench data paths=1..%u time_s=%.3f writers=%d direct_io=%s "
           "frag_bytes=%u qds=1,16,64,256\n",
           npaths_total, time_sec,
           s->nwriters, s->direct_io ? "on" : "off", frag_len);
    fflush(stdout);

    struct bench_slot *slots = calloc(BENCH_MAX_SLOTS, sizeof(*slots));
    if (!slots)
        return 1;
    for (int i = 0; i < BENCH_MAX_SLOTS; i++) {
        slots[i].slot = i;
        slots[i].s = s;
        slots[i].ex = ex;
        slots[i].frag_len = frag_len;
        slots[i].ci_base = (uint32_t)i * BENCH_CI_SPAN;
        if (posix_memalign((void **)&slots[i].buf, 4096, frag_len) != 0) {
            fprintf(stderr, "bench: OOM\n");
            return 1;
        }
        fill_nonzero(slots[i].buf, frag_len, (uint64_t)i + 1);
        efs_hash(slots[i].buf, frag_len, slots[i].sum);
    }

    uint32_t saved_count = s->storage_path_count;
    for (uint32_t np = 1; np <= npaths_total; np++) {
        s->storage_path_count = np;
        if (server_writer_pool_start(s) != 0) {
            fprintf(stderr, "bench: writer pool start failed (paths=%u)\n", np);
            s->storage_path_count = saved_count;
            return 1;
        }
        for (int q = 0; q < BENCH_NQDS; q++)
            run_round(slots, 0, (int)np, bench_qds[q], time_sec, frag_len);
        for (int q = 0; q < BENCH_NQDS; q++)
            run_round(slots, 1, (int)np, bench_qds[q], time_sec, frag_len);
        server_writer_pool_stop(s);
        /* Bound scratch use: next path count rewrites from empty. */
        bench_export_tree_rm(s);
        for (int i = 0; i < BENCH_MAX_SLOTS; i++)
            slots[i].written = 0;
    }
    s->storage_path_count = saved_count;

    for (int i = 0; i < BENCH_MAX_SLOTS; i++) {
        free(slots[i].buf);
        free(slots[i].lat.v);
    }
    free(slots);
    server_export_put(s, ex);
    return 0;
}

/* ---- meta bench: KV segments/WAL and the Raft log, sync on ---- */

static void lat_report(const char *tag, struct lat_vec *lv, uint64_t ops,
                       uint64_t errors, double wall, uint64_t op_bytes)
{
    uint64_t p50 = 0, p99 = 0, max = 0;
    if (lv->n) {
        qsort(lv->v, lv->n, sizeof(*lv->v), u64_cmp);
        p50 = lv->v[lv->n / 2];
        p99 = lv->v[(lv->n * 99) / 100];
        max = lv->v[lv->n - 1];
    }
    if (wall < 1e-9)
        wall = 1e-9;
    printf("BENCH_OK kind=meta phase=%s ops=%llu wall_s=%.3f ops_s=%.1f "
           "p50_us=%llu p99_us=%llu max_us=%llu MB_s=%.3f errors=%llu\n",
           tag, (unsigned long long)ops, wall, (double)ops / wall,
           (unsigned long long)p50, (unsigned long long)p99,
           (unsigned long long)max,
           (double)ops * op_bytes / (1 << 20) / wall,
           (unsigned long long)errors);
    fflush(stdout);
}

static int run_meta_bench(struct efsd_server *s, double time_sec)
{
    char root[EFS_MAX_PATH];
    if (s->meta_storage[0])
        snprintf(root, sizeof(root), "%s", s->meta_storage);
    else if (s->storage_path_count)
        snprintf(root, sizeof(root), "%s", s->storage_paths[0]);
    else {
        fprintf(stderr, "bench: meta needs --meta-storage (or --storage)\n");
        return 1;
    }
    if (!dir_is_empty(root)) {
        fprintf(stderr, "bench: %s is not empty; meta bench root must be "
                "scratch\n", root);
        return 1;
    }
    if (mkdir_p(root) != 0 && errno != EEXIST) {
        fprintf(stderr, "bench: mkdir %s: %s\n", root, strerror(errno));
        return 1;
    }
    char kvdir[EFS_MAX_PATH], raftdir[EFS_MAX_PATH];
    snprintf(kvdir, sizeof(kvdir), "%s/kv", root);
    snprintf(raftdir, sizeof(raftdir), "%s/raft", root);

    if (have_fio())
        ceil_fio(root);
    else
        ceil_raw(root);

    struct lat_vec lv = { 0, 0, 0 };
    uint8_t key[24] = "benchmeta-key-";
    uint8_t val[128];
    fill_nonzero(val, sizeof(val), 3);

    /* KV write: sequential puts, production fsync-every-batch mode. The WAL
     * fsync is in the clock; enough puts cross the memtable into segments. */
    struct efs_kv_lsm_cfg cfg = { EFS_KV_LSM_SYNC, 0, 0 };
    struct efs_kv *kv = efs_kv_lsm_open(kvdir, &cfg);
    if (!kv) {
        fprintf(stderr, "bench: kv open %s failed\n", kvdir);
        return 1;
    }
    uint64_t ops = 0, errors = 0;
    double t0 = now_sec();
    double deadline = t0 + time_sec;
    while (now_sec() < deadline) {
        memcpy(key + 14, &ops, sizeof(ops));
        uint64_t t = efs_iostats_now_us();
        int rc = efs_kv_put(kv, key, sizeof(key), val, sizeof(val));
        lat_add(&lv, efs_iostats_now_us() - t);
        if (rc == EFS_OK)
            ops++;
        else
            errors++;
    }
    double wall = now_sec() - t0;
    lat_report("kv-write", &lv, ops, errors, wall, sizeof(key) + sizeof(val));

    /* Flush + compact: the compactor segment fsync (the D11 100 ms mode)
     * is its own line, wall clock. */
    struct efs_kv_lsm_stats kst;
    double tf = now_sec();
    efs_kv_lsm_flush(kv);
    double flush_s = now_sec() - tf;
    tf = now_sec();
    efs_kv_lsm_compact(kv);
    double compact_s = now_sec() - tf;
    memset(&kst, 0, sizeof(kst));
    efs_kv_lsm_stats(kv, &kst);
    printf("BENCH_OK kind=meta phase=kv-flush-compact flush_s=%.3f "
           "compact_s=%.3f mt_bytes=%llu l0_bytes=%llu n_l0=%u n_l1=%u\n",
           flush_s, compact_s, (unsigned long long)kst.mt_bytes,
           (unsigned long long)kst.l0_bytes, kst.n_l0, kst.n_l1);
    fflush(stdout);

    /* KV read: point gets over the keys just written. */
    uint64_t written_keys = ops ? ops : 1;
    uint8_t vbuf[256];
    lv.n = 0;
    ops = 0;
    errors = 0;
    uint64_t x = 99;
    t0 = now_sec();
    deadline = t0 + time_sec;
    while (now_sec() < deadline) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        uint64_t k = (x >> 33) % written_keys;
        memcpy(key + 14, &k, sizeof(k));
        uint32_t vlen = sizeof(vbuf);
        uint64_t t = efs_iostats_now_us();
        int rc = efs_kv_get(kv, key, sizeof(key), vbuf, &vlen);
        lat_add(&lv, efs_iostats_now_us() - t);
        if (rc == EFS_OK)
            ops++;
        else
            errors++;
    }
    wall = now_sec() - t0;
    lat_report("kv-read", &lv, ops, errors, wall, 0);
    efs_kv_lsm_close(kv);
    free(lv.v);

    /* Raft log: appends that fsync at QD 1, then batches behind one fsync. */
    struct efs_raft_disk *rd = efs_raft_disk_open(raftdir, EFS_RAFT_DISK_SYNC);
    if (!rd) {
        fprintf(stderr, "bench: raft disk open %s failed\n", raftdir);
        return 1;
    }
    struct efs_raft_store *rst = efs_raft_disk_group(rd, 0);
    if (!rst) {
        fprintf(stderr, "bench: raft group 0 failed\n");
        efs_raft_disk_close(rd);
        return 1;
    }
    uint8_t cmd[256];
    fill_nonzero(cmd, sizeof(cmd), 5);
    lv.v = NULL;
    lv.n = lv.cap = 0;
    ops = errors = 0;
    uint64_t idx = 1;
    t0 = now_sec();
    deadline = t0 + time_sec;
    while (now_sec() < deadline) {
        uint64_t t = efs_iostats_now_us();
        int rc = rst->append(rst, idx, 1, cmd, sizeof(cmd));
        lat_add(&lv, efs_iostats_now_us() - t);
        if (rc == EFS_OK) {
            ops++;
            idx++;
        } else {
            errors++;
        }
    }
    wall = now_sec() - t0;
    lat_report("raft-append-qd1", &lv, ops, errors, wall, sizeof(cmd));

    if (rst->batch_begin && rst->batch_end) {
        lv.n = 0;
        ops = errors = 0;
        uint64_t entries = 0;
        t0 = now_sec();
        deadline = t0 + time_sec;
        while (now_sec() < deadline) {
            uint64_t t = efs_iostats_now_us();
            int rc = rst->batch_begin(rst);
            for (int b = 0; b < 32 && rc == EFS_OK; b++) {
                rc = rst->append(rst, idx, 1, cmd, sizeof(cmd));
                if (rc == EFS_OK) {
                    idx++;
                    entries++;
                }
            }
            if (rc == EFS_OK)
                rc = rst->batch_end(rst);
            lat_add(&lv, efs_iostats_now_us() - t);
            if (rc == EFS_OK)
                ops++;
            else
                errors++;
        }
        wall = now_sec() - t0;
        if (wall < 1e-9)
            wall = 1e-9;
        uint64_t p50 = 0, p99 = 0, max = 0;
        if (lv.n) {
            qsort(lv.v, lv.n, sizeof(*lv.v), u64_cmp);
            p50 = lv.v[lv.n / 2];
            p99 = lv.v[(lv.n * 99) / 100];
            max = lv.v[lv.n - 1];
        }
        printf("BENCH_OK kind=meta phase=raft-append-batch32 batches=%llu "
               "entries=%llu wall_s=%.3f entries_s=%.1f batch_p50_us=%llu "
               "batch_p99_us=%llu batch_max_us=%llu MB_s=%.3f errors=%llu\n",
               (unsigned long long)ops, (unsigned long long)entries, wall,
               (double)entries / wall, (unsigned long long)p50,
               (unsigned long long)p99, (unsigned long long)max,
               (double)entries * sizeof(cmd) / (1 << 20) / wall,
               (unsigned long long)errors);
        fflush(stdout);
    }
    free(lv.v);

    /* Log reopen: close + open + last() in the clock (recovery cost). */
    t0 = now_sec();
    efs_raft_disk_close(rd);
    rd = efs_raft_disk_open(raftdir, EFS_RAFT_DISK_SYNC);
    double reopen_s = now_sec() - t0;
    uint64_t last_idx = 0, last_term = 0;
    if (rd) {
        struct efs_raft_store *r2 = efs_raft_disk_group(rd, 0);
        if (r2 && r2->last)
            r2->last(r2, &last_idx, &last_term);
    }
    printf("BENCH_OK kind=meta phase=raft-reopen reopen_s=%.3f last_idx=%llu "
           "entries=%llu\n",
           reopen_s, (unsigned long long)last_idx, (unsigned long long)(idx - 1));
    fflush(stdout);
    if (rd)
        efs_raft_disk_close(rd);

    rm_tree(kvdir);
    rm_tree(raftdir);
    return errors ? 1 : 0;
}

int server_run_local_bench(struct efsd_server *s, const char *kind,
                           double time_sec)
{
    if (!s || !kind || time_sec <= 0.0) {
        fprintf(stderr, "bench: invalid args (kind data|meta, --time > 0)\n");
        return 1;
    }
    setlinebuf(stdout);
    setlinebuf(stderr);
    g_server = s;

    char perf_path[EFS_MAX_PATH];
    perf_path[0] = '\0';
    if (s->perf) {
        const char *pp = getenv("EFS_PERF_PATH");
        if (pp && *pp)
            snprintf(perf_path, sizeof(perf_path), "%s", pp);
        else
            snprintf(perf_path, sizeof(perf_path), "/tmp/efsd-bench-perf-%d/perf.data",
                     (int)getpid());
        g_perf_pid = start_perf_recorder(getpid(), perf_path);
        if (g_perf_pid < 0)
            fprintf(stderr, "Warning: could not start perf; continuing\n");
        else
            printf("perf recording -> %s\n", perf_path);
    }

    int rc;
    if (strcmp(kind, "data") == 0)
        rc = run_data_bench(s, time_sec);
    else if (strcmp(kind, "meta") == 0)
        rc = run_meta_bench(s, time_sec);
    else {
        fprintf(stderr, "bench: unknown kind '%s' (data|meta)\n", kind);
        rc = 1;
    }

    stop_perf_recorder();
    if (s->perf && perf_path[0]) {
        perf_print_top(perf_path);
        printf("perf report: %s\n", perf_path);
    }
    return rc;
}
