/* Independent raw-file I/O driver. Never changes production checksum policy. */
#include "io_bench.h"
#include "efs/checksum.h"
#include "efs/common.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define IO_MAX_WORKERS 256
#define IO_MAX_ROOTS 32
struct io_worker {
    int fd, reading, hashing, sync_writes;
    uint32_t size, window;
    unsigned char pattern;
    uint8_t *buf, sum[EFS_HASH_SIZE];
    int *go, *ready;
    double *deadline;
    uint64_t ops, errors, latency_ns, max_ns;
};
static uint64_t now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
static int transfer(int fd, uint8_t *buf, uint32_t n, off_t off, int reading)
{
    uint32_t done = 0;
    while (done < n) {
        ssize_t rc = reading ? pread(fd, buf + done, n - done, off + done) :
                               pwrite(fd, buf + done, n - done, off + done);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) return -1;
        done += (uint32_t)rc;
    }
    return 0;
}
static void *io_worker_run(void *arg)
{
    struct io_worker *w = arg;
    __atomic_add_fetch(w->ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(w->go, __ATOMIC_ACQUIRE)) sched_yield();
    while ((double)now_ns() / 1e9 < *w->deadline) {
        off_t off = (off_t)(w->ops % w->window) * w->size;
        uint64_t start = now_ns();
        uint8_t hash[EFS_HASH_SIZE];
        if (w->hashing && !w->reading) efs_hash(w->buf, w->size, hash);
        int rc = transfer(w->fd, w->buf, w->size, off, w->reading);
        if (!rc && w->hashing && w->reading) {
            efs_hash(w->buf, w->size, hash);
            if (memcmp(hash, w->sum, sizeof(hash))) rc = -1;
        }
        if (!rc && !w->reading && w->sync_writes && fdatasync(w->fd)) rc = -1;
        uint64_t latency = now_ns() - start;
        w->latency_ns += latency;
        if (latency > w->max_ns) w->max_ns = latency;
        if (rc) { w->errors++; break; }
        w->ops++;
    }
    return NULL;
}
/* perf starts disabled for this driver. Preparation and post-run integrity
 * checks must not appear in the measured CPU profile. No hook in daemon code. */
static int perf_command(const char *command)
{
    const char *ctl = getenv("EFS_BENCH_PERF_CONTROL"), *ack = getenv("EFS_BENCH_PERF_ACK");
    if (!ctl && !ack) return 0;
    if (!ctl || !ack) return -1;
    int fd = open(ctl, O_RDWR | O_CLOEXEC), af = open(ack, O_RDWR | O_CLOEXEC);
    if (fd < 0 || af < 0) { if (fd >= 0) close(fd); if (af >= 0) close(af); return -1; }
    char buf[32]; struct pollfd p = { .fd = af, .events = POLLIN };
    int rc = write(fd, command, strlen(command)) == (ssize_t)strlen(command) ? 0 : -1;
    if (!rc && poll(&p, 1, 5000) <= 0) rc = -1;
    if (!rc) { ssize_t n = read(af, buf, sizeof(buf)); if (n < 3 || memcmp(buf, "ack", 3)) rc = -1; }
    close(fd); close(af); return rc;
}
static int empty_root(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return errno == ENOENT;
    struct dirent *e; int empty = 1;
    while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { empty = 0; break; }
    closedir(d); return empty;
}
static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s --bench io|io-blake3 --storage <empty-root> [--storage root ...]\n"
        "  [--rw read|write] [--io-size 64K] [--qd 16] [--window 64]\n"
        "  [--time 3] [--direct-io|--no-direct-io] [--sync]\n"
        "Parallel raw pread/pwrite, one file per worker; no EFS store/network.\n"
        "io-blake3 hashes each write and verifies each read inside the timed loop.\n"
        "io does no timed hashing. --window bounds blocks per worker.\n"
        "Read files are populated first; all written blocks are checked after timing.\n"
        "--sync includes fdatasync per successful write; default buffered writes\n"
        "measure page-cache acceptance. Direct I/O sizes must be 4 KiB aligned.\n", prog);
}
int efs_bench_io_main(int argc, char **argv)
{
    const char *roots[IO_MAX_ROOTS], *rw = "write";
    int nroots = 0, qd = 16, hashing = 0, direct = 0, sync_writes = 0, have_kind = 0;
    uint32_t size = 65536, window = 64;
    double duration = 3;
    for (int i = 1; i < argc; i++) {
        const char *opt = argv[i]; char *end;
        if (!strcmp(opt, "--help")) { usage(argv[0]); return 0; }
        if (!strcmp(opt, "--direct-io")) { direct = 1; continue; }
        if (!strcmp(opt, "--no-direct-io")) { direct = 0; continue; }
        if (!strcmp(opt, "--sync")) { sync_writes = 1; continue; }
        if (++i == argc) goto invalid;
        const char *v = argv[i];
        if (!strcmp(opt, "--bench")) {
            if (have_kind++ || (strcmp(v, "io") && strcmp(v, "io-blake3"))) goto invalid;
            hashing = !strcmp(v, "io-blake3");
        } else if (!strcmp(opt, "--storage")) {
            if (!*v || nroots == IO_MAX_ROOTS) goto invalid;
            roots[nroots++] = v;
        } else if (!strcmp(opt, "--rw")) {
            if (strcmp(v, "read") && strcmp(v, "write")) goto invalid;
            rw = v;
        } else if (!strcmp(opt, "--time")) {
            errno = 0; duration = strtod(v, &end);
            if (errno || end == v || *end || !isfinite(duration) || duration <= 0) goto invalid;
        } else if (!strcmp(opt, "--io-size")) {
            errno = 0; unsigned long long n = strtoull(v, &end, 10);
            uint64_t mult = 1;
            if (*end == 'K' || *end == 'k') { mult = 1024; end++; }
            else if (*end == 'M' || *end == 'm') { mult = 1024 * 1024; end++; }
            if (errno || end == v || *end || *v == '-' || n > (16 * 1024 * 1024) / mult) goto invalid;
            n *= mult;
            if (n < 4096 || n % 4096) goto invalid;
            size = (uint32_t)n;
        } else if (!strcmp(opt, "--qd") || !strcmp(opt, "--window")) {
            errno = 0; unsigned long n = strtoul(v, &end, 10);
            if (errno || end == v || *end || *v == '-' || n < 1) goto invalid;
            if (!strcmp(opt, "--qd")) { if (n > IO_MAX_WORKERS) goto invalid; qd = (int)n; }
            else { if (n > UINT32_MAX || n * (uint64_t)size > INT64_MAX) goto invalid; window = (uint32_t)n; }
        } else goto invalid;
    }
    if (!have_kind || !nroots || (sync_writes && !strcmp(rw, "read"))) goto invalid;
    /* Validate every root before creating any files. */
    for (int i = 0; i < nroots; i++) if (!empty_root(roots[i])) {
        fprintf(stderr, "io bench: scratch root not empty/accessible: %s\n", roots[i]); return 1;
    }
    struct io_worker *w = calloc((size_t)qd, sizeof(*w));
    pthread_t *threads = calloc((size_t)qd, sizeof(*threads));
    char (*paths)[4096] = calloc((size_t)qd, sizeof(*paths));
    if (!w || !threads || !paths) { free(w); free(threads); free(paths); return 1; }
    int go = 0, ready = 0, created = 0, rc = 1;
    double deadline = 0;
    for (int i = 0; i < qd; i++) w[i].fd = -1;
    for (int i = 0; i < nroots; i++) if (mkdir(roots[i], 0755) && errno != EEXIST) goto done;
    for (int i = 0; i < qd; i++) {
        struct io_worker *a = &w[i];
        a->reading = !strcmp(rw, "read"); a->hashing = hashing; a->sync_writes = sync_writes;
        a->size = size; a->window = window; a->pattern = (unsigned char)(i % 251 + 1);
        a->go = &go; a->ready = &ready; a->deadline = &deadline;
        if (snprintf(paths[i], sizeof(paths[i]), "%s/io-worker-%d.bin", roots[i % nroots], i) >= (int)sizeof(paths[i])) goto done;
        a->fd = open(paths[i], O_CREAT | O_EXCL | O_RDWR | (direct ? O_DIRECT : 0), 0600);
        if (a->fd < 0) goto done;
        if (posix_memalign((void **)&a->buf, 4096, size)) goto done;
        memset(a->buf, a->pattern, size);
        if (hashing) efs_hash(a->buf, size, a->sum);
        if (a->reading) {
            for (uint32_t b = 0; b < window; b++) if (transfer(a->fd, a->buf, size, (off_t)b * size, 0)) goto done;
            if (fdatasync(a->fd)) goto done;
        }
    }
    for (int i = 0; i < qd; i++) {
        if (pthread_create(&threads[i], NULL, io_worker_run, &w[i])) goto stop;
        created++;
    }
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != qd) sched_yield();
    if (perf_command("enable\n")) goto stop;
    uint64_t start = now_ns(); deadline = (double)start / 1e9 + duration;
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
    created = 0;
    uint64_t end = now_ns();
    if (perf_command("disable\n")) goto done;
    uint64_t ops = 0, errors = 0, ns = 0, max_ns = 0, verified = 0;
    /* Validation and end-of-run flushing are outside measured work. */
    for (int i = 0; i < qd; i++) {
        struct io_worker *a = &w[i]; ops += a->ops; errors += a->errors; ns += a->latency_ns;
        if (!a->ops) errors++;
        if (a->max_ns > max_ns) max_ns = a->max_ns;
        uint64_t resident = a->reading ? window : (a->ops < window ? a->ops : window);
        if (fdatasync(a->fd)) errors++;
        for (uint64_t b = 0; b < resident; b++) {
            if (transfer(a->fd, a->buf, size, (off_t)b * size, 1)) { errors++; break; }
            uint32_t j; for (j = 0; j < size && a->buf[j] == a->pattern; j++);
            if (j != size) { errors++; break; }
            verified++;
        }
    }
    double wall = (double)(end - start) / 1e9;
    printf("BENCH_OK kind=io checksum=%s rw=%s paths=%d qd=%d io_bytes=%u window_per_slot=%u "
           "direct=%d sync=%d ops=%llu bytes=%llu wall_s=%.6f GiB_s=%.6f avg_us=%.3f max_us=%.3f "
           "verified_blocks=%llu errors=%llu\n", hashing ? "blake3" : "none", rw, nroots, qd, size, window,
           direct, sync_writes, (unsigned long long)ops, (unsigned long long)(ops * size), wall,
           (double)ops * size / (1ull << 30) / wall, ops ? (double)ns / ops / 1000 : 0, (double)max_ns / 1000,
           (unsigned long long)verified, (unsigned long long)errors);
    rc = errors ? 1 : 0;
    goto done;
stop:
    /* Release waiting threads with an already-expired deadline. */
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
done:
    if (rc) fprintf(stderr, "io bench failed: %s\n", strerror(errno));
    for (int i = 0; i < qd; i++) {
        if (w[i].fd >= 0) { close(w[i].fd); unlink(paths[i]); }
        free(w[i].buf);
    }
    free(w); free(threads); free(paths); return rc;
invalid:
    usage(argv[0]); return 1;
}
