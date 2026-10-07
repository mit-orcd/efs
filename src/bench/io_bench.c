/* Independent raw-file I/O driver. Never changes production checksum policy. */
#include "io_bench.h"
#include "efs/checksum.h"
#include "efs/common.h"
#include "perf_control.h"
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
    pthread_cond_t ready_cv;
    unsigned ready;
    uint64_t deadline_ns, release_ns;
};
struct io_worker {
    int fd, reading, hashing, sync_writes;
    uint32_t size, window;
    unsigned char pattern;
    uint8_t *buf, sum[EFS_HASH_SIZE];
    struct io_start *start;
    /* Independent release gates avoid a 256-worker mutex convoy while early
     * workers saturate the CPU. The common deadline still includes launch. */
    pthread_mutex_t go_mu;
    pthread_cond_t go_cv;
    int released, abort;
    uint64_t start_delay_ns;
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
    pthread_mutex_lock(&w->go_mu);
    pthread_mutex_lock(&gate->mu);
    ++gate->ready;
    pthread_cond_signal(&gate->ready_cv);
    pthread_mutex_unlock(&gate->mu);
    while (!w->released)
        pthread_cond_wait(&w->go_cv, &w->go_mu);
    uint64_t deadline = gate->deadline_ns;
    int abort = w->abort;
    pthread_mutex_unlock(&w->go_mu);
    if (abort) return NULL;

    uint64_t ops = 0, errors = 0, latency_ns = 0, max_ns = 0;
    uint32_t block = 0;
    const uint32_t size = w->size, window = w->window;
    /* One integer clock read per operation replaces three reads and floating
     * point deadline division. Latency covers the complete worker operation
     * cycle, including its loop bookkeeping, rather than syscall time alone. */
    uint64_t start = now_ns();
    w->start_delay_ns = start - gate->release_ns;
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
        "  [--time 3] [--direct-io|--no-direct-io] [--sync] [--preallocate]\n"
        "Parallel raw pread/pwrite, one file per worker; no EFS store/network.\n"
        "io-blake3 hashes each write and verifies each read inside the timed loop.\n"
        "io does no timed hashing. --window bounds blocks per worker.\n"
        "--preallocate allocates, populates and flushes the write window before timing.\n"
        "Read files are populated first; all written blocks are checked after timing.\n"
        "--sync includes fdatasync per successful write; default buffered writes\n"
        "measure page-cache acceptance. Direct I/O sizes must be 4 KiB aligned.\n", prog);
}
int efs_bench_io_main(int argc, char **argv)
{
    const char *roots[IO_MAX_ROOTS], *rw = "write";
    int nroots = 0, qd = 16, hashing = 0, direct = 0, sync_writes = 0, have_kind = 0, preallocate = 0;
    uint32_t size = 65536, window = 64;
    double duration = 3;
    for (int i = 1; i < argc; i++) {
        const char *opt = argv[i]; char *end;
        if (!strcmp(opt, "--help")) { usage(argv[0]); return 0; }
        if (!strcmp(opt, "--direct-io")) { direct = 1; continue; }
        if (!strcmp(opt, "--no-direct-io")) { direct = 0; continue; }
        if (!strcmp(opt, "--preallocate")) { preallocate = 1; continue; }
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
    if (!have_kind || !nroots || qd < nroots || ((sync_writes || preallocate) && !strcmp(rw, "read")) ||
        (uint64_t)size * window > INT64_MAX) goto invalid;
    /* Validate every root before creating any files. */
    for (int i = 0; i < nroots; i++) if (!empty_root(roots[i])) {
        fprintf(stderr, "io bench: scratch root not empty/accessible: %s\n", roots[i]); return 1;
    }
    struct io_worker *w = calloc((size_t)qd, sizeof(*w));
    pthread_t *threads = calloc((size_t)qd, sizeof(*threads));
    char (*paths)[4096] = calloc((size_t)qd, sizeof(*paths));
    if (!w || !threads || !paths) { free(w); free(threads); free(paths); return 1; }
    int created = 0, initialized = 0, rc = 1, saved_error = 0;
    const char *phase = "scratch creation";
    struct io_start gate = {.mu = PTHREAD_MUTEX_INITIALIZER,
        .ready_cv = PTHREAD_COND_INITIALIZER};
    for (int i = 0; i < qd; i++) w[i].fd = -1;
    for (int i = 0; i < nroots; i++) if (mkdir(roots[i], 0755) && errno != EEXIST) {
        saved_error = errno; goto done;
    }
    phase = "worker preparation";
    for (int i = 0; i < qd; i++) {
        struct io_worker *a = &w[i];
        phase = "worker preparation";
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
        if (preallocate) {
#ifdef __linux__
            int prc = posix_fallocate(a->fd, 0, (off_t)size * window);
#else
            int prc = ENOTSUP;
#endif
            if (prc) { phase = "preallocation"; saved_error = prc; goto done; }
        }
        if (a->reading || preallocate) {
            phase = "window population";
            for (uint32_t b = 0; b < window; b++) if (transfer(a->fd, a->buf, size, (off_t)b * size, 0)) { saved_error = errno; goto done; }
            phase = "window flush";
            if (fdatasync(a->fd)) { saved_error = errno; goto done; }
        }
    }
    phase = "worker gate initialization";
    for (int i = 0; i < qd; ++i) {
        int grc = pthread_mutex_init(&w[i].go_mu, NULL);
        if (grc) { saved_error = grc; goto done; }
        grc = pthread_cond_init(&w[i].go_cv, NULL);
        if (grc) { pthread_mutex_destroy(&w[i].go_mu); saved_error = grc; goto done; }
        ++initialized;
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
    uint64_t start = now_ns();
    gate.release_ns = start;
    gate.deadline_ns = start + (uint64_t)(duration * 1e9);
    for (int i = 0; i < created; ++i) {
        pthread_mutex_lock(&w[i].go_mu);
        w[i].released = 1;
        pthread_cond_signal(&w[i].go_cv);
        pthread_mutex_unlock(&w[i].go_mu);
    }
    for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
    created = 0;
    uint64_t end = now_ns();
    phase = "perf disable";
    if (perf_command("disable\n")) { saved_error = errno; goto done; }
    phase = "timed operations or verification";
    uint64_t ops = 0, errors = 0, ns = 0, max_ns = 0, verified = 0, idle = 0;
    uint64_t min_ops = UINT64_MAX, max_ops = 0, max_start_ns = 0, allocation_ops = 0;
    /* Validation and end-of-run flushing are outside measured work. */
    for (int i = 0; i < qd; i++) {
        struct io_worker *a = &w[i]; ops += a->ops; errors += a->errors; ns += a->latency_ns;
        if (!a->ops) { errors++; idle++; }
        if (a->ops < min_ops) min_ops = a->ops;
        if (a->ops > max_ops) max_ops = a->ops;
        if (a->start_delay_ns > max_start_ns) max_start_ns = a->start_delay_ns;
        if (!a->reading && !preallocate) allocation_ops += a->ops < window ? a->ops : window;
        if (a->error_no) fprintf(stderr, "io bench: worker=%d I/O failed: %s\n", i, strerror(a->error_no));
        if (a->max_ns > max_ns) max_ns = a->max_ns;
        uint64_t resident = a->reading || preallocate ? window : (a->ops < window ? a->ops : window);
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
           "verified_blocks=%llu errors=%llu idle_workers=%llu min_worker_ops=%llu max_worker_ops=%llu max_start_us=%.3f latency=operation_cycle allocation=%s prepared_blocks=%llu allocation_ops=%llu overwrite_ops=%llu\n", errors ? "BENCH_FAIL" : "BENCH_OK", hashing ? "blake3" : "none", rw, nroots, qd, size, window,
           direct, sync_writes, (unsigned long long)ops, (unsigned long long)(ops * size), wall,
           (double)ops * size / (1ull << 30) / wall, ops ? (double)ns / ops / 1000 : 0, (double)max_ns / 1000,
           (unsigned long long)verified, (unsigned long long)errors,
           (unsigned long long)idle, (unsigned long long)min_ops, (unsigned long long)max_ops, (double)max_start_ns / 1000, preallocate ? "overwrite_preallocated" : !strcmp(rw,"read") ? "populated_read" : "allocate_then_overwrite",
           (unsigned long long)((preallocate || !strcmp(rw,"read")) ? (uint64_t)qd * window : 0),
           (unsigned long long)allocation_ops, (unsigned long long)(!strcmp(rw,"write") ? ops - allocation_ops : 0));
    rc = errors ? 1 : 0;
    goto done;
stop:
    /* Partial creation/perf failure wakes every owned thread for cleanup. */
    for (int i = 0; i < created; ++i) {
        pthread_mutex_lock(&w[i].go_mu);
        w[i].abort = 1; w[i].released = 1;
        pthread_cond_signal(&w[i].go_cv);
        pthread_mutex_unlock(&w[i].go_mu);
    }
    for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
done:
    if (rc) fprintf(stderr, "io bench failed during %s%s%s\n", phase,
                    saved_error ? ": " : "", saved_error ? strerror(saved_error) : "");
    for (int i = 0; i < qd; i++) {
        if (w[i].fd >= 0) { close(w[i].fd); unlink(paths[i]); }
        free(w[i].buf);
    }
    for (int i = 0; i < initialized; ++i) {
        pthread_cond_destroy(&w[i].go_cv); pthread_mutex_destroy(&w[i].go_mu);
    }
    pthread_cond_destroy(&gate.ready_cv);
    pthread_mutex_destroy(&gate.mu);
    free(w); free(threads); free(paths); return rc;
invalid:
    usage(argv[0]); return 1;
}
