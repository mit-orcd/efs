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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define IO_MAX_WORKERS 256
#define IO_MAX_ROOTS 32
struct io_start {
    pthread_mutex_t mu;
    pthread_cond_t ready_cv, go_cv;
    unsigned ready;
    int released, abort;
    uint64_t deadline_ns;
};
struct io_worker {
    int fd, reading, hashing, sync_writes;
    uint32_t size, window;
    unsigned char pattern;
    uint8_t *buf, sum[EFS_HASH_SIZE];
    struct io_start *start;
    int error_no;
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
        if (rc <= 0) { if (!rc) errno = EIO; return -1; }
        done += (uint32_t)rc;
    }
    return 0;
}
static void *io_worker_run(void *arg)
{
    struct io_worker *w = arg;
    struct io_start *gate = w->start;
    pthread_mutex_lock(&gate->mu);
    ++gate->ready;
    pthread_cond_signal(&gate->ready_cv);
    while (!gate->released)
        pthread_cond_wait(&gate->go_cv, &gate->mu);
    uint64_t deadline = gate->deadline_ns;
    int abort = gate->abort;
    pthread_mutex_unlock(&gate->mu);
    if (abort) return NULL;

    uint64_t ops = 0, errors = 0, latency_ns = 0, max_ns = 0;
    uint32_t block = 0;
    const uint32_t size = w->size, window = w->window;
    /* One integer clock read per operation replaces three reads and floating
     * point deadline division. Latency covers the complete worker operation
     * cycle, including its loop bookkeeping, rather than syscall time alone. */
    uint64_t start = now_ns();
    while (start < deadline) {
        off_t off = (off_t)block * size;
        uint8_t hash[EFS_HASH_SIZE];
        if (w->hashing && !w->reading) efs_hash(w->buf, size, hash);
        int rc = transfer(w->fd, w->buf, size, off, w->reading);
        if (!rc && w->hashing && w->reading) {
            efs_hash(w->buf, size, hash);
            if (memcmp(hash, w->sum, sizeof(hash))) { errno = EIO; rc = -1; }
        }
        if (!rc && !w->reading && w->sync_writes && fdatasync(w->fd)) rc = -1;
        int error_no = rc ? errno : 0;
        uint64_t end = now_ns(), latency = end - start;
        latency_ns += latency;
        if (latency > max_ns) max_ns = latency;
        if (rc) { errors++; w->error_no = error_no; break; }
        ops++;
        if (++block == window) block = 0; /* no division in the hot loop */
        start = end;
    }
    w->ops = ops; w->errors = errors;
    w->latency_ns = latency_ns; w->max_ns = max_ns;
    return NULL;
}
/* perf starts disabled for this driver. Preparation and post-run integrity
 * checks must not appear in the measured CPU profile. No hook in daemon code. */
static int perf_command(const char *command)
{
    const char *ctl = getenv("EFS_BENCH_PERF_CONTROL"), *ack = getenv("EFS_BENCH_PERF_ACK");
    if (!ctl && !ack) return 0;
    if (!ctl || !ack) { errno = EINVAL; return -1; }
    int fd = open(ctl, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    int af = open(ack, O_RDWR | O_CLOEXEC);
    if (af < 0) { int saved = errno; close(fd); errno = saved; return -1; }
    char buf[32]; struct pollfd p = { .fd = af, .events = POLLIN };
    ssize_t written = write(fd, command, strlen(command));
    int rc = written == (ssize_t)strlen(command) ? 0 : -1;
    if (rc && written >= 0) errno = EIO;
    if (!rc) { int prc = poll(&p, 1, 5000); if (prc <= 0) { if (!prc) errno = ETIMEDOUT; rc = -1; } }
    if (!rc) { ssize_t n = read(af, buf, sizeof(buf)); if (n < 3 || memcmp(buf, "ack", 3)) { if (n >= 0) errno = EPROTO; rc = -1; } }
    int saved = errno;
    close(fd); close(af); errno = saved; return rc;
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
            if (errno || end == v || *end || !isfinite(duration) || duration < 1e-9 || duration > 86400) goto invalid;
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
    int created = 0, rc = 1, saved_error = 0;
    const char *phase = "scratch creation";
    struct io_start gate = {.mu = PTHREAD_MUTEX_INITIALIZER,
        .ready_cv = PTHREAD_COND_INITIALIZER, .go_cv = PTHREAD_COND_INITIALIZER};
    for (int i = 0; i < qd; i++) w[i].fd = -1;
    for (int i = 0; i < nroots; i++) if (mkdir(roots[i], 0755) && errno != EEXIST) {
        saved_error = errno; goto done;
    }
    phase = "worker preparation";
    for (int i = 0; i < qd; i++) {
        struct io_worker *a = &w[i];
        a->reading = !strcmp(rw, "read"); a->hashing = hashing; a->sync_writes = sync_writes;
        a->size = size; a->window = window; a->pattern = (unsigned char)(i % 251 + 1);
        a->start = &gate;
        if (snprintf(paths[i], sizeof(paths[i]), "%s/io-worker-%d.bin", roots[i % nroots], i) >= (int)sizeof(paths[i])) { saved_error = ENAMETOOLONG; goto done; }
        a->fd = open(paths[i], O_CREAT | O_EXCL | O_RDWR | (direct ? O_DIRECT : 0), 0600);
        if (a->fd < 0) { saved_error = errno; goto done; }
        int alloc_rc = posix_memalign((void **)&a->buf, 4096, size);
        if (alloc_rc) { saved_error = alloc_rc; goto done; }
        memset(a->buf, a->pattern, size);
        if (hashing) efs_hash(a->buf, size, a->sum);
        if (a->reading) {
            for (uint32_t b = 0; b < window; b++) if (transfer(a->fd, a->buf, size, (off_t)b * size, 0)) { saved_error = errno; goto done; }
            if (fdatasync(a->fd)) { saved_error = errno; goto done; }
        }
    }
    for (int i = 0; i < qd; i++) {
        int thread_rc = pthread_create(&threads[i], NULL, io_worker_run, &w[i]);
        if (thread_rc) { phase = "thread creation"; saved_error = thread_rc; goto stop; }
        created++;
    }
    pthread_mutex_lock(&gate.mu);
    while (gate.ready != (unsigned)qd)
        pthread_cond_wait(&gate.ready_cv, &gate.mu);
    pthread_mutex_unlock(&gate.mu);
    phase = "perf enable";
    if (perf_command("enable\n")) { saved_error = errno; goto stop; }
    pthread_mutex_lock(&gate.mu);
    uint64_t start = now_ns();
    gate.deadline_ns = start + (uint64_t)(duration * 1e9);
    gate.released = 1;
    pthread_cond_broadcast(&gate.go_cv);
    pthread_mutex_unlock(&gate.mu);
    for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
    created = 0;
    uint64_t end = now_ns();
    phase = "perf disable";
    if (perf_command("disable\n")) { saved_error = errno; goto done; }
    phase = "timed operations or verification";
    uint64_t ops = 0, errors = 0, ns = 0, max_ns = 0, verified = 0, idle = 0;
    uint64_t min_ops = UINT64_MAX, max_ops = 0;
    /* Validation and end-of-run flushing are outside measured work. */
    for (int i = 0; i < qd; i++) {
        struct io_worker *a = &w[i]; ops += a->ops; errors += a->errors; ns += a->latency_ns;
        if (!a->ops) { errors++; idle++; }
        if (a->ops < min_ops) min_ops = a->ops;
        if (a->ops > max_ops) max_ops = a->ops;
        if (a->error_no) fprintf(stderr, "io bench: worker=%d I/O failed: %s\n", i, strerror(a->error_no));
        if (a->max_ns > max_ns) max_ns = a->max_ns;
        uint64_t resident = a->reading ? window : (a->ops < window ? a->ops : window);
        if (fdatasync(a->fd)) { errors++; fprintf(stderr, "io bench: worker=%d flush: %s\n", i, strerror(errno)); }
        for (uint64_t b = 0; b < resident; b++) {
            if (transfer(a->fd, a->buf, size, (off_t)b * size, 1)) { errors++; fprintf(stderr, "io bench: worker=%d verification read: %s\n", i, strerror(errno)); break; }
            uint32_t j; for (j = 0; j < size && a->buf[j] == a->pattern; j++);
            if (j != size) { errors++; fprintf(stderr, "io bench: worker=%d block=%llu data mismatch\n", i, (unsigned long long)b); break; }
            verified++;
        }
    }
    double wall = (double)(end - start) / 1e9;
    if (idle) fprintf(stderr, "io bench: %llu/%d workers completed no I/O before the deadline\n", (unsigned long long)idle, qd);
    printf("%s kind=io checksum=%s rw=%s paths=%d qd=%d io_bytes=%u window_per_slot=%u "
           "direct=%d sync=%d ops=%llu bytes=%llu wall_s=%.6f GiB_s=%.6f avg_us=%.3f max_us=%.3f "
           "verified_blocks=%llu errors=%llu idle_workers=%llu min_worker_ops=%llu max_worker_ops=%llu latency=operation_cycle\n", errors ? "BENCH_FAIL" : "BENCH_OK", hashing ? "blake3" : "none", rw, nroots, qd, size, window,
           direct, sync_writes, (unsigned long long)ops, (unsigned long long)(ops * size), wall,
           (double)ops * size / (1ull << 30) / wall, ops ? (double)ns / ops / 1000 : 0, (double)max_ns / 1000,
           (unsigned long long)verified, (unsigned long long)errors,
           (unsigned long long)idle, (unsigned long long)min_ops, (unsigned long long)max_ops);
    rc = errors ? 1 : 0;
    goto done;
stop:
    /* Partial creation/perf failure wakes every owned thread for cleanup. */
    pthread_mutex_lock(&gate.mu);
    gate.abort = 1; gate.released = 1;
    pthread_cond_broadcast(&gate.go_cv);
    pthread_mutex_unlock(&gate.mu);
    for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
done:
    if (rc) fprintf(stderr, "io bench failed during %s%s%s\n", phase,
                    saved_error ? ": " : "", saved_error ? strerror(saved_error) : "");
    for (int i = 0; i < qd; i++) {
        if (w[i].fd >= 0) { close(w[i].fd); unlink(paths[i]); }
        free(w[i].buf);
    }
    pthread_cond_destroy(&gate.ready_cv); pthread_cond_destroy(&gate.go_cv);
    pthread_mutex_destroy(&gate.mu);
    free(w); free(threads); free(paths); return rc;
invalid:
    usage(argv[0]); return 1;
}
