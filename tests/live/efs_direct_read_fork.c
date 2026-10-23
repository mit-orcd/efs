/* Read-only direct-read/fork qualification; no fixture writes.
 * cc -O2 -Wall -Wextra -pthread efs_direct_read_fork.c -ldl -o /tmp/read-fork
 * /tmp/read-fork ROOT PREFILL FILES_PER_WORKER STRIDE [ROUNDS [READ_REPEATS]]
 * EFS_DIAG_FORK=1 selects concurrent fork/exit/wait, every 5 ms.
 * EFS_DIAG_COW=1 additionally writes disjoint allocation prefixes after fork,
 * exercising parent COW on pages shared with in-flight read destinations.
 * READ_REPEATS rereads each open file, retaining the handle but reallocating
 * private heap buffers each pass. This targets read/fork overlap without
 * requiring repeated metadata opens.
 * No-fork, original fork, and fork+COW are distinct controls, not fixes.
 * Fixture: scripts/intel_package3_fork_fixture.py prepare ROOT --count COUNT
 * (use COUNT as both FILES_PER_WORKER and STRIDE). Historical W92 fixture
 * defaults: 10000 files per worker, stride 204800, one round.
 * Each 256 KiB file contains SHA256("W92-real-payload:" + be64(index)) repeated.
 * Mismatch dumps go only to /tmp. External timeout bounds live runs.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>

#define WORKERS 8
#define PARTS 4
#define BLOCK 65536
static unsigned char *(*checksum_sha256)(const unsigned char *, size_t,
                                         unsigned char *);
static const char *root;
static unsigned count = 10000, rounds = 1, read_repeats = 1;
static uint64_t stride = 204800;
static int prefill, cow;
static atomic_int stop, done, active_reads;
static atomic_ulong verified, forks, read_forks, prefix_writes;
/* Prefixes live until unregister; only bytes BEFORE each read destination
 * are touched. Never modify the data under test or manufacture a pass. */
static pthread_mutex_t buffers_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *buffers[WORKERS][PARTS];

static void fail(const char *what)
{
    perror(what);
    atomic_store(&stop, 1);
}

static void *worker(void *arg)
{
    unsigned w = (unsigned)(uintptr_t)arg;
    for (unsigned round = 0; round < rounds && !atomic_load(&stop); ++round) {
        for (unsigned off = 0; off < count && !atomic_load(&stop); ++off) {
            uint64_t index = stride * w + off;
            char path[4096];
            int len = snprintf(path, sizeof(path),
                "%s/worker-%02u/dir-%06u/file-%09llu", root, w, off / 1024,
                (unsigned long long)index);
            if (len < 0 || (size_t)len >= sizeof(path)) {
                errno = ENAMETOOLONG; fail("fixture path"); return NULL;
            }
            int fd = open(path, O_RDONLY);
            if (fd < 0) { fail(path); return NULL; }
            unsigned char input[25], seed[32];
            memcpy(input, "W92-real-payload:", 17);
            for (unsigned k = 0; k < 8; ++k)
                input[17 + k] = (unsigned char)(index >> (56 - 8 * k));
            if (!checksum_sha256(input, sizeof(input), seed)) abort();
            for (unsigned repeat = 0; repeat < read_repeats &&
                 !atomic_load(&stop); ++repeat) {
                unsigned char *held[PARTS] = {0};
                int bad_file = 0;
                for (unsigned part = 0; part < PARTS; ++part) {
                    unsigned char *allocation = malloc(BLOCK + 33);
                    if (!allocation) abort();
                    unsigned char *buf = allocation + 32;
                    held[part] = allocation;
                    memset(allocation, 0, 32);
                    buf[BLOCK] = 0;
                    if (prefill) memset(buf, 0xaa, BLOCK);
                    pthread_mutex_lock(&buffers_mu);
                    buffers[w][part] = allocation;
                    pthread_mutex_unlock(&buffers_mu);
                    atomic_fetch_add(&active_reads, 1);
                    ssize_t n = read(fd, buf, BLOCK);
                    int saved_errno = errno;
                    atomic_fetch_sub(&active_reads, 1);
                    int bad = -1;
                    if (n != BLOCK) bad = 0;
                    else for (int k = 0; k < BLOCK; ++k) {
                        if (buf[k] != seed[k % 32]) { bad = k; break; }
                    }
                    if (bad >= 0) {
                        atomic_store(&stop, 1);
                        fprintf(stderr,
                            "FAIL round=%u repeat=%u index=%llu part=%u bad=%d n=%zd "
                            "prefill=%d errno=%d\n", round, repeat,
                            (unsigned long long)index, part, bad, n, prefill,
                            n < 0 ? saved_errno : 0);
                        char dump[256];
                        snprintf(dump, sizeof(dump),
                            "/tmp/native-mismatch-%ld-%llu-%u.bin", (long)getpid(),
                            (unsigned long long)index, part);
                        int out = open(dump, O_WRONLY | O_CREAT | O_EXCL, 0600);
                        if (out >= 0) {
                            if (n > 0 && write(out, buf, (size_t)n) != n)
                                perror("mismatch dump");
                            close(out);
                        }
                        bad_file = 1;
                        break;
                    }
                }
                if (!bad_file) {
                    unsigned char extra;
                    if (read(fd, &extra, 1) != 0) {
                        fprintf(stderr, "FAIL trailing-byte/EOF index=%llu\n",
                                (unsigned long long)index);
                        atomic_store(&stop, 1);
                        bad_file = 1;
                    }
                }
                pthread_mutex_lock(&buffers_mu);
                for (unsigned k = 0; k < PARTS; ++k) {
                    buffers[w][k] = NULL;
                    free(held[k]);
                }
                pthread_mutex_unlock(&buffers_mu);
                if (bad_file) { close(fd); return NULL; }
                atomic_fetch_add(&verified, 1);
                if (repeat + 1 < read_repeats && lseek(fd, 0, SEEK_SET) != 0) {
                    fail("rewind"); close(fd); return NULL;
                }
            }
            if (close(fd)) { fail("close"); return NULL; }
        }
    }
    return NULL;
}

static void *forker(void *unused)
{
    (void)unused;
    while (!atomic_load(&done) && !atomic_load(&stop)) {
        if (!atomic_load(&active_reads)) { usleep(100); continue; }
        int hold[2] = {-1, -1};
        if (cow && pipe(hold)) { fail("child hold pipe"); break; }
        pid_t p = fork();
        if (!p) {
            /* Async-signal-safe child. In COW mode keep its mm alive until
             * the parent has changed the disjoint prefixes; do not let a
             * fast child exit remove the very COW sharing under test. */
            if (cow) {
                char unused_byte;
                ssize_t n;
                close(hold[1]);
                do { n = read(hold[0], &unused_byte, 1); }
                while (n < 0 && errno == EINTR);
                close(hold[0]);
                if (n < 0) _exit(2);
            }
            _exit(0);
        }
        if (p < 0) {
            if (cow) { close(hold[0]); close(hold[1]); }
            fail("fork"); break;
        }
        if (cow) close(hold[0]);
        atomic_fetch_add(&forks, 1);
        /* Sampling proves activity, not exact kernel page-pin overlap.
         * K30: page-cache reads of 64 KiB last microseconds and one sample
         * after fork missed them in every fork of a short run. Poll up to
         * 2 ms after fork; in COW mode the held child still shares the mm
         * for that whole window. */
        int seen = atomic_load(&active_reads) != 0;
        if (!seen) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            do {
                seen = atomic_load(&active_reads) != 0;
                clock_gettime(CLOCK_MONOTONIC, &t1);
            } while (!seen && (t1.tv_sec - t0.tv_sec) * 1000000000L +
                     (t1.tv_nsec - t0.tv_nsec) < 2000000L);
        }
        if (seen) atomic_fetch_add(&read_forks, 1);
        if (cow) {
            pthread_mutex_lock(&buffers_mu);
            for (unsigned w = 0; w < WORKERS; ++w) {
                for (unsigned k = 0; k < PARTS; ++k) {
                    if (buffers[w][k]) {
                        ++buffers[w][k][0];
                        atomic_fetch_add(&prefix_writes, 1);
                    }
                }
            }
            pthread_mutex_unlock(&buffers_mu);
            close(hold[1]); /* release the child only after prefix writes */
        }
        int status;
        pid_t waited;
        do { waited = waitpid(p, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited != p) { fail("waitpid"); break; }
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
            fprintf(stderr, "FAIL child status=%d\n", status);
            atomic_store(&stop, 1);
            break;
        }
        usleep(5000);
    }
    return NULL;
}

static uint64_t number(const char *arg)
{
    char *end;
    errno = 0;
    unsigned long long n = strtoull(arg, &end, 10);
    if (errno || !*arg || *end || *arg == '-') {
        fprintf(stderr, "invalid numeric argument: %s\n", arg); exit(2);
    }
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 7) return 2;
    root = argv[1];
    uint64_t n = argc > 3 ? number(argv[3]) : count;
    uint64_t r = argc > 5 ? number(argv[5]) : rounds;
    uint64_t repeats = argc > 6 ? number(argv[6]) : read_repeats;
    uint64_t fill = argc > 2 ? number(argv[2]) : 0;
    stride = argc > 4 ? number(argv[4]) : stride;
    if (!n || n > 10000 || !r || r > 1000 || !repeats || repeats > 10000 || stride < n ||
        stride > (UINT64_MAX - n) / (WORKERS - 1) || fill > 1) return 2;
    count = (unsigned)n; rounds = (unsigned)r; prefill = (int)fill;
    read_repeats = (unsigned)repeats;
    int do_fork = getenv("EFS_DIAG_FORK") != NULL;
    cow = getenv("EFS_DIAG_COW") != NULL;
    if (cow && !do_fork) {
        fprintf(stderr, "EFS_DIAG_COW requires EFS_DIAG_FORK\n"); return 2;
    }
    void *crypto = dlopen("libcrypto.so.3", RTLD_NOW | RTLD_LOCAL);
    if (!crypto) crypto = dlopen("libcrypto.so", RTLD_NOW | RTLD_LOCAL);
    if (!crypto) { fprintf(stderr, "libcrypto missing: %s\n", dlerror()); return 2; }
    *(void **)(&checksum_sha256) = dlsym(crypto, "SHA256");
    if (!checksum_sha256) return 2;
    pthread_t forks_thread, threads[WORKERS];
    unsigned started = 0;
    int err;
    if (do_fork && (err = pthread_create(&forks_thread, NULL, forker, NULL))) {
        errno = err; fail("fork thread"); return 2;
    }
    for (; started < WORKERS; ++started) {
        err = pthread_create(&threads[started], NULL, worker,
                             (void *)(uintptr_t)started);
        if (err) { errno = err; fail("reader thread"); break; }
    }
    for (unsigned i = 0; i < started; ++i) pthread_join(threads[i], NULL);
    atomic_store(&done, 1);
    if (do_fork) pthread_join(forks_thread, NULL);
    unsigned long expected = (unsigned long)WORKERS * count * rounds * read_repeats;
    if (atomic_load(&verified) != expected ||
        (do_fork && (!atomic_load(&forks) || !atomic_load(&read_forks))) ||
        (cow && !atomic_load(&prefix_writes))) atomic_store(&stop, 1);
    printf("%s files=%lu expected=%lu prefill=%d rounds=%u read_repeats=%u fork=%d cow=%d "
           "forks=%lu read_active_forks=%lu prefix_writes=%lu\n",
           atomic_load(&stop) ? "FAIL" : "PASS", atomic_load(&verified), expected,
           prefill, rounds, read_repeats, do_fork, cow, atomic_load(&forks),
           atomic_load(&read_forks), atomic_load(&prefix_writes));
    dlclose(crypto);
    return atomic_load(&stop) ? 1 : 0;
}
