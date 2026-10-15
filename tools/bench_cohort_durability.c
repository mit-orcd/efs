/* bench_cohort_durability — local model of fragment PUT durability barriers.
 *
 * Each record is one new file, created O_CREAT|O_EXCL|O_DIRECT in a fresh
 * case directory and written as one aligned buffer (payload + tail, the
 * on-disk shape of an EFS fragment). A record is "acknowledged" only after
 * the barrier mode below makes it durable; latency is open -> acknowledge.
 *
 *   each      fsync(file), then fsync(dir)                (no grouping)
 *   groupdir  fsync(file), then a group-committed fsync(dir)
 *             (production: per-file fsync + grouped directory barrier)
 *   batch     each worker writes --cohort files, fsyncs each, then the dir
 *             once (the earlier "batch" control; per-worker cohorts)
 *   syncfs    no per-file fsync; a group-committed syncfs(dir) covers every
 *             record written before it started (one log force per cohort)
 *   syncfs+flush  as syncfs, then fdatasync of a clean probe file, which on
 *             XFS issues a device cache flush even when the log is idle
 *
 * Group commit: a waiter whose record is not yet covered either becomes the
 * leader (runs one barrier covering every ticket issued so far) or waits for
 * the running barrier. --min-cohort/--max-wait-us let a leader wait briefly
 * for more members first. A failed barrier fails every ticket it covered.
 *
 * Timed region: workers only. Afterwards every record is read back and
 * compared (outside timing), then all files and the case directory are
 * removed. Device flush/write counters come from /proc/diskstats for the
 * directory's device and bracket the timed region. Prints one JSON line.
 *
 * Build: cc -O2 -Wall -Wextra -pthread -o bench_cohort_durability bench_cohort_durability.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

enum mode { M_EACH, M_GROUPDIR, M_BATCH, M_SYNCFS, M_SYNCFS_FLUSH };
static const char *mode_names[] = {"each", "groupdir", "batch", "syncfs", "syncfs+flush"};

static struct {
    const char *dir;
    int workers, records, cohort, min_cohort, max_wait_us;
    uint32_t payload, tail;
    enum mode mode;
} cfg = {NULL, 8, 4096, 8, 1, 0, 65536, 4096, M_GROUPDIR};

static int dirfd_g = -1, probefd = -1;
static atomic_int next_record;
static atomic_int failed;
static double *lat_ms;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void fill(uint8_t *buf, uint32_t len, int rec)
{
    uint64_t x = 0x9E3779B97F4A7C15ull * (uint64_t)(rec + 1);
    for (uint32_t i = 0; i + 8 <= len; i += 8) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        memcpy(buf + i, &x, 8);
    }
}

static int fsync_retry(int fd)
{
    int rc;
    do { rc = fsync(fd); } while (rc < 0 && errno == EINTR);
    return rc;
}

/* ---- group commit ---------------------------------------------------- */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint64_t issued, covered, running;   /* tickets */
    uint64_t fail_lo, fail_hi;           /* last failed covered range */
    uint64_t barriers, members;
} gc = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0, 0, 0};

static int barrier_run(void)
{
    switch (cfg.mode) {
    case M_GROUPDIR:
        return fsync_retry(dirfd_g);
    case M_SYNCFS:
        return syncfs(dirfd_g);
    case M_SYNCFS_FLUSH: {
        int rc = syncfs(dirfd_g);
        if (rc == 0)
            do { rc = fdatasync(probefd); } while (rc < 0 && errno == EINTR);
        return rc;
    }
    default:
        return -1;
    }
}

/* Returns 0 once a successful barrier started after this ticket was issued. */
static int group_commit(void)
{
    pthread_mutex_lock(&gc.mu);
    uint64_t mine = ++gc.issued;
    while (gc.covered < mine) {
        if (gc.running) {
            pthread_cond_wait(&gc.cv, &gc.mu);
            continue;
        }
        gc.running = 1;
        if (cfg.min_cohort > 1 && cfg.max_wait_us > 0) {
            double until = now_s() + cfg.max_wait_us / 1e6;
            while (gc.issued - gc.covered < (uint64_t)cfg.min_cohort && now_s() < until) {
                pthread_mutex_unlock(&gc.mu);
                struct timespec d = {0, 20000};
                nanosleep(&d, NULL);
                pthread_mutex_lock(&gc.mu);
            }
        }
        uint64_t target = gc.issued, from = gc.covered + 1;
        pthread_mutex_unlock(&gc.mu);
        int rc = barrier_run();
        pthread_mutex_lock(&gc.mu);
        gc.barriers++;
        gc.members += target - from + 1;
        if (rc != 0) { gc.fail_lo = from; gc.fail_hi = target; }
        gc.covered = target;
        gc.running = 0;
        pthread_cond_broadcast(&gc.cv);
    }
    int bad = mine >= gc.fail_lo && mine <= gc.fail_hi && gc.fail_hi;
    pthread_mutex_unlock(&gc.mu);
    return bad ? -1 : 0;
}

/* ---- workers --------------------------------------------------------- */
static int write_record(int rec, uint8_t *buf, int *fd_out)
{
    char name[64];
    snprintf(name, sizeof(name), "f%07d", rec);
    int fd = openat(dirfd_g, name, O_CREAT | O_EXCL | O_WRONLY | O_DIRECT | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    uint32_t len = cfg.payload + cfg.tail;
    fill(buf, len, rec);
    ssize_t w;
    do { w = pwrite(fd, buf, len, 0); } while (w < 0 && errno == EINTR);
    if (w != (ssize_t)len) {
        close(fd);
        return -1;
    }
    *fd_out = fd;
    return 0;
}

static void *worker(void *arg)
{
    (void)arg;
    uint8_t *buf;
    if (posix_memalign((void **)&buf, 4096, cfg.payload + cfg.tail))
        return NULL;
    int batch_n = cfg.mode == M_BATCH ? cfg.cohort : 1;
    int recs[256], fds[256];
    double t0s[256];
    for (;;) {
        int n = 0;
        while (n < batch_n) {
            int rec = atomic_fetch_add(&next_record, 1);
            if (rec >= cfg.records)
                break;
            t0s[n] = now_s();
            recs[n] = rec;
            if (write_record(rec, buf, &fds[n]) != 0) { atomic_store(&failed, 1); free(buf); return NULL; }
            n++;
        }
        if (n == 0)
            break;
        int rc = 0;
        switch (cfg.mode) {
        case M_EACH:
            rc = fsync_retry(fds[0]);
            if (close(fds[0]) != 0) rc = -1;
            if (rc == 0) rc = fsync_retry(dirfd_g);
            break;
        case M_GROUPDIR:
            rc = fsync_retry(fds[0]);
            if (close(fds[0]) != 0) rc = -1;
            if (rc == 0) rc = group_commit();
            break;
        case M_BATCH:
            for (int i = 0; i < n; i++) {
                if (fsync_retry(fds[i]) != 0) rc = -1;
                if (close(fds[i]) != 0) rc = -1;
            }
            if (rc == 0) rc = fsync_retry(dirfd_g);
            break;
        case M_SYNCFS:
        case M_SYNCFS_FLUSH:
            if (close(fds[0]) != 0) rc = -1;
            if (rc == 0) rc = group_commit();
            break;
        }
        if (rc != 0) { atomic_store(&failed, 1); break; }
        double t1 = now_s();
        for (int i = 0; i < n; i++)
            lat_ms[recs[i]] = (t1 - t0s[i]) * 1000.0;
    }
    free(buf);
    return NULL;
}

/* ---- device counters ------------------------------------------------- */
struct disk { unsigned long long writes, sectors, flushes, busy_ms; int ok; };
static struct disk disk_read(dev_t dev)
{
    struct disk d = {0};
    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) return d;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned ma, mi; char name[64];
        unsigned long long v[17] = {0};
        int n = sscanf(line, "%u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &ma, &mi, name, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                       &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15], &v[16]);
        if (n >= 14 && ma == major(dev) && mi == minor(dev)) {
            d.writes = v[4]; d.sectors = v[6]; d.busy_ms = v[9];
            d.flushes = n >= 20 ? v[15] : 0;
            d.ok = 1;
        }
    }
    fclose(f);
    return d;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void usage(void)
{
    fprintf(stderr, "usage: bench_cohort_durability --dir NEW_DIR [--mode each|groupdir|batch|syncfs|syncfs+flush]\n"
                    "  [--workers 8] [--records 4096] [--cohort 8] [--min-cohort 1] [--max-wait-us 0]\n"
                    "  [--payload 65536] [--tail 4096]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    static struct option opts[] = {
        {"dir", 1, 0, 'd'}, {"mode", 1, 0, 'm'}, {"workers", 1, 0, 'w'}, {"records", 1, 0, 'r'},
        {"cohort", 1, 0, 'c'}, {"min-cohort", 1, 0, 'k'}, {"max-wait-us", 1, 0, 't'},
        {"payload", 1, 0, 'p'}, {"tail", 1, 0, 'T'}, {0, 0, 0, 0}};
    int o;
    while ((o = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (o) {
        case 'd': cfg.dir = optarg; break;
        case 'm': {
            int found = 0;
            for (int i = 0; i < 5; i++)
                if (!strcmp(optarg, mode_names[i])) { cfg.mode = (enum mode)i; found = 1; }
            if (!found) usage();
            break;
        }
        case 'w': cfg.workers = atoi(optarg); break;
        case 'r': cfg.records = atoi(optarg); break;
        case 'c': cfg.cohort = atoi(optarg); break;
        case 'k': cfg.min_cohort = atoi(optarg); break;
        case 't': cfg.max_wait_us = atoi(optarg); break;
        case 'p': cfg.payload = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'T': cfg.tail = (uint32_t)strtoul(optarg, NULL, 10); break;
        default: usage();
        }
    }
    if (!cfg.dir || cfg.workers < 1 || cfg.workers > 256 || cfg.records < 1 ||
        cfg.cohort < 1 || cfg.cohort > 256 || (cfg.payload + cfg.tail) % 4096)
        usage();

    /* A fresh directory only: never run inside a populated tree. */
    if (mkdir(cfg.dir, 0755) != 0) { perror("mkdir (must not exist)"); return 1; }
    dirfd_g = open(cfg.dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd_g < 0) { perror("open dir"); return 1; }
    struct stat st;
    fstat(dirfd_g, &st);
    if (cfg.mode == M_SYNCFS_FLUSH) {
        probefd = openat(dirfd_g, "probe", O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
        if (probefd < 0 || write(probefd, "p", 1) != 1 || fsync_retry(probefd) || fsync_retry(dirfd_g)) {
            perror("probe"); return 1;
        }
    }
    fsync_retry(dirfd_g);
    lat_ms = calloc((size_t)cfg.records, sizeof(double));

    pthread_t th[256];
    struct disk d0 = disk_read(st.st_dev);
    double t0 = now_s();
    for (int i = 0; i < cfg.workers; i++)
        pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < cfg.workers; i++)
        pthread_join(th[i], NULL);
    double secs = now_s() - t0;
    struct disk d1 = disk_read(st.st_dev);
    int ok = !atomic_load(&failed);

    /* Verify every record outside timing. */
    uint8_t *want = malloc(cfg.payload + cfg.tail), *got = malloc(cfg.payload + cfg.tail);
    int verified = 0;
    for (int rec = 0; ok && rec < cfg.records; rec++) {
        char name[64];
        snprintf(name, sizeof(name), "f%07d", rec);
        int fd = openat(dirfd_g, name, O_RDONLY | O_CLOEXEC);
        if (fd < 0) { ok = 0; break; }
        fill(want, cfg.payload + cfg.tail, rec);
        ssize_t r = pread(fd, got, cfg.payload + cfg.tail, 0);
        close(fd);
        if (r != (ssize_t)(cfg.payload + cfg.tail) || memcmp(want, got, (size_t)r)) { ok = 0; break; }
        verified++;
    }
    /* Remove only what this run created. */
    for (int rec = 0; rec < cfg.records; rec++) {
        char name[64];
        snprintf(name, sizeof(name), "f%07d", rec);
        unlinkat(dirfd_g, name, 0);
    }
    if (probefd >= 0) { close(probefd); unlinkat(dirfd_g, "probe", 0); }
    close(dirfd_g);
    rmdir(cfg.dir);

    qsort(lat_ms, (size_t)cfg.records, sizeof(double), cmp_d);
    int n = cfg.records;
    double mib = (double)cfg.payload * n / 1048576.0;
    printf("{\"mode\":\"%s\",\"workers\":%d,\"records\":%d,\"cohort\":%d,\"min_cohort\":%d,"
           "\"max_wait_us\":%d,\"secs\":%.3f,\"payload_mib_s\":%.2f,\"records_s\":%.1f,"
           "\"lat_ms\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f},"
           "\"group_barriers\":%llu,\"avg_group\":%.2f,"
           "\"dev\":{\"ok\":%d,\"writes\":%llu,\"write_mib\":%.1f,\"flushes\":%llu,\"busy_pct\":%.1f,"
           "\"flushes_per_record\":%.3f},\"verified\":%d,\"ok\":%s}\n",
           mode_names[cfg.mode], cfg.workers, n, cfg.cohort, cfg.min_cohort, cfg.max_wait_us,
           secs, mib / secs, n / secs,
           lat_ms[n / 2], lat_ms[(int)(n * 0.95)], lat_ms[(int)(n * 0.99)], lat_ms[n - 1],
           (unsigned long long)gc.barriers, gc.barriers ? (double)gc.members / gc.barriers : 0.0,
           d0.ok && d1.ok, d1.writes - d0.writes, (d1.sectors - d0.sectors) / 2048.0,
           d1.flushes - d0.flushes, 100.0 * (d1.busy_ms - d0.busy_ms) / 1000.0 / secs,
           (double)(d1.flushes - d0.flushes) / n, verified, ok && verified == n ? "true" : "false");
    return ok && verified == n ? 0 : 1;
}
