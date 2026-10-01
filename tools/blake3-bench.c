/*
 * Multi-threaded BLAKE3 throughput microbench.
 *
 * Uses all CPUs in the process affinity mask by default (align --threads with
 * Slurm --cpus-per-task). Each worker hashes its own buffer in a tight loop
 * for --time seconds.
 *
 * Built with -march=native and always recompiled via `make blake3-bench`.
 */
#include "blake3.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Declared in blake3_dispatch.c; not in the public header. */
size_t blake3_simd_degree(void);

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <cpuid.h>

/*
 * Name of the implementation blake3_hash_many() will pick — same priority as
 * deps/blake3/blake3_dispatch.c (AVX-512 → AVX2 → SSE4.1 → SSE2 → portable).
 */
static const char *blake3_best_cpu_feature(void)
{
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    unsigned int max_id;

    if (!__get_cpuid(0, &max_id, &ebx, &ecx, &edx))
        return "portable";

    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        return "portable";

    int sse2 = 0, sse41 = 0, avx = 0, osxsave = 0;
#if defined(__x86_64__) || defined(_M_X64)
    sse2 = 1;
#else
    sse2 = (edx & (1u << 26)) != 0;
#endif
    sse41 = (ecx & (1u << 19)) != 0;
    osxsave = (ecx & (1u << 27)) != 0;
    avx = (ecx & (1u << 28)) != 0;

    if (osxsave && avx) {
        unsigned int xcr0_lo = 0, xcr0_hi = 0;
        __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
        uint64_t xcr0 = ((uint64_t)xcr0_hi << 32) | xcr0_lo;
        if ((xcr0 & 6) == 6) { /* XMM + YMM state */
            int avx2 = 0, avx512f = 0, avx512vl = 0;
            if (max_id >= 7) {
                unsigned int d7 = 0;
                __cpuid_count(7, 0, eax, ebx, ecx, d7);
                avx2 = (ebx & (1u << 5)) != 0;
                if ((xcr0 & 224) == 224) { /* opmask + ZMM */
                    avx512f = (ebx & (1u << 16)) != 0;
                    avx512vl = (ebx & (1u << 31)) != 0;
                }
            }
#if !defined(BLAKE3_NO_AVX512)
            if (avx512f && avx512vl)
                return "AVX-512";
#endif
#if !defined(BLAKE3_NO_AVX2)
            if (avx2)
                return "AVX2";
#endif
        }
    }
#if !defined(BLAKE3_NO_SSE41)
    if (sse41)
        return "SSE4.1";
#endif
#if !defined(BLAKE3_NO_SSE2)
    if (sse2)
        return "SSE2";
#endif
    return "portable";
}
#else
static const char *blake3_best_cpu_feature(void)
{
#if defined(__aarch64__) && !defined(__ARM_BIG_ENDIAN)
    return "NEON";
#else
    return "portable";
#endif
}
#endif

static long online_cpus(void)
{
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n > 0)
            return n;
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 1;
}

/* Fill cpus[0..n) with affinity CPUs in ascending order. */
static int affinity_cpu_list(int *cpus, int max_n)
{
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof(set), &set) != 0)
        return -1;
    int n = 0;
    for (int c = 0; c < CPU_SETSIZE && n < max_n; c++) {
        if (CPU_ISSET(c, &set))
            cpus[n++] = c;
    }
    return n;
}

/* Restrict process (+ workers) to the first want CPUs of the current mask. */
static int shrink_affinity(int want, int *cpus_out, int max_n)
{
    int all[CPU_SETSIZE];
    int nall = affinity_cpu_list(all, CPU_SETSIZE);
    if (nall < 1)
        return -1;
    int n = want < nall ? want : nall;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < n; i++) {
        CPU_SET(all[i], &set);
        if (cpus_out && i < max_n)
            cpus_out[i] = all[i];
    }
    if (sched_setaffinity(0, sizeof(set), &set) != 0)
        return -1;
    return n;
}

enum bench_mode {
    MODE_ONESHOT = 0, /* init+update+finalize per buffer (efs fragment-like) */
    MODE_STREAM = 1,  /* one hasher, repeated updates; finalize once (max SIMD) */
};

struct worker {
    pthread_t tid;
    int cpu;
    int mode;
    uint8_t *buf;
    size_t buf_len;
    volatile int *go;
    volatile int *stop;
    uint64_t bytes;
    uint64_t hashes;
};

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void *worker_main(void *arg)
{
    struct worker *w = arg;
    uint8_t out[BLAKE3_OUT_LEN];
    uint64_t bytes = 0;
    uint64_t hashes = 0;

    if (w->cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(w->cpu, &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }

    memset(w->buf, (int)((uintptr_t)w & 0xff), w->buf_len);

    /* Prime blake3 CPU-feature detection before the timed window. */
    {
        uint8_t warm[BLAKE3_OUT_LEN];
        blake3_hasher h;
        blake3_hasher_init(&h);
        blake3_hasher_update(&h, w->buf, w->buf_len);
        blake3_hasher_finalize(&h, warm, sizeof(warm));
        __asm__ __volatile__("" : : "r"(warm[0]) : "memory");
    }

    while (!*w->go)
        ;

    if (w->mode == MODE_STREAM) {
        blake3_hasher hasher;
        blake3_hasher_init(&hasher);
        while (!*w->stop) {
            for (int i = 0; i < 32; i++) {
                blake3_hasher_update(&hasher, w->buf, w->buf_len);
                bytes += w->buf_len;
                hashes++;
            }
        }
        blake3_hasher_finalize(&hasher, out, sizeof(out));
        __asm__ __volatile__("" : : "r"(out[0]) : "memory");
    } else {
        /* Check stop every 32 hashes to cut shared-flag traffic. */
        while (!*w->stop) {
            for (int i = 0; i < 32; i++) {
                blake3_hasher hasher;
                blake3_hasher_init(&hasher);
                blake3_hasher_update(&hasher, w->buf, w->buf_len);
                blake3_hasher_finalize(&hasher, out, sizeof(out));
                bytes += w->buf_len;
                hashes++;
                __asm__ __volatile__("" : : "r"(out[0]) : "memory");
            }
        }
    }

    w->bytes = bytes;
    w->hashes = hashes;
    return NULL;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--time SEC] [--threads N] [--size BYTES] [--oneshot|--stream]\n"
            "  --time     sample duration in seconds (default: 3)\n"
            "  --threads  worker count (default: CPUs in affinity mask)\n"
            "  --size     bytes hashed per update (default: 65536)\n"
            "  --stream   one hasher, repeated updates (default; max SIMD)\n"
            "  --oneshot  init+finalize per buffer (efs fragment-like)\n"
            "Tip: match --threads to Slurm --cpus-per-task.\n",
            argv0);
}

static size_t parse_size(const char *s)
{
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || end == s)
        return 0;
    if (*end == 'K' || *end == 'k') {
        v *= 1024ULL;
        end++;
    } else if (*end == 'M' || *end == 'm') {
        v *= 1024ULL * 1024ULL;
        end++;
    }
    if (*end != '\0')
        return 0;
    return (size_t)v;
}

static void sleep_sec(double sec)
{
    if (sec <= 0.0)
        return;
    struct timespec ts;
    ts.tv_sec = (time_t)sec;
    ts.tv_nsec = (long)((sec - (double)ts.tv_sec) * 1e9);
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR)
        ;
}

int main(int argc, char **argv)
{
    double duration = 3.0;
    long nthreads = online_cpus();
    size_t buf_len = 65536;
    int mode = MODE_STREAM;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            duration = atof(argv[++i]);
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            nthreads = atol(argv[++i]);
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            buf_len = parse_size(argv[++i]);
        } else if (strcmp(argv[i], "--stream") == 0) {
            mode = MODE_STREAM;
        } else if (strcmp(argv[i], "--oneshot") == 0) {
            mode = MODE_ONESHOT;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (duration <= 0.0 || nthreads < 1 || buf_len == 0) {
        usage(argv[0]);
        return 1;
    }

    int cpu_list[CPU_SETSIZE];
    /* Align process affinity to exactly --threads CPUs (avoid HT oversubscribe). */
    int ncpus = shrink_affinity((int)nthreads, cpu_list, CPU_SETSIZE);
    if (ncpus < 1) {
        fprintf(stderr, "no CPUs in affinity mask\n");
        return 1;
    }
    if (nthreads > ncpus)
        nthreads = ncpus;

    char host[256];
    if (gethostname(host, sizeof(host)) != 0)
        snprintf(host, sizeof(host), "unknown");

    /* Touch blake3 once so feature detection is primed before we report it. */
    {
        uint8_t warm[BLAKE3_OUT_LEN];
        blake3_hasher h;
        blake3_hasher_init(&h);
        blake3_hasher_update(&h, "warm", 4);
        blake3_hasher_finalize(&h, warm, sizeof(warm));
        (void)warm;
    }

    printf("blake3-bench host=%s threads=%ld affinity_cpus=%d size=%zu time=%.3fs mode=%s\n",
           host, nthreads, ncpus, buf_len, duration,
           mode == MODE_STREAM ? "stream" : "oneshot");
    printf("blake3 cpu_feature=%s simd_degree=%zu\n",
           blake3_best_cpu_feature(), blake3_simd_degree());
    fflush(stdout);

    struct worker *workers = calloc((size_t)nthreads, sizeof(*workers));
    if (!workers) {
        perror("calloc");
        return 1;
    }

    volatile int go = 0;
    volatile int stop = 0;
    for (long i = 0; i < nthreads; i++) {
        workers[i].cpu = cpu_list[i % ncpus];
        workers[i].mode = mode;
        workers[i].buf_len = buf_len;
        workers[i].buf = aligned_alloc(64, buf_len);
        workers[i].go = &go;
        workers[i].stop = &stop;
        if (!workers[i].buf) {
            perror("aligned_alloc");
            return 1;
        }
        if (pthread_create(&workers[i].tid, NULL, worker_main, &workers[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    double t0 = now_sec();
    go = 1;
    /* Sleep instead of spinning — spinning burned ~10% in clock_gettime. */
    sleep_sec(duration);
    stop = 1;
    double elapsed = now_sec() - t0;

    uint64_t total_bytes = 0;
    uint64_t total_hashes = 0;
    for (long i = 0; i < nthreads; i++) {
        pthread_join(workers[i].tid, NULL);
        total_bytes += workers[i].bytes;
        total_hashes += workers[i].hashes;
        free(workers[i].buf);
    }
    free(workers);

    double gib = (double)total_bytes / (1024.0 * 1024.0 * 1024.0);
    double gib_s = gib / elapsed;
    double mib_s = gib_s * 1024.0;

    printf("hashes=%" PRIu64 " bytes=%" PRIu64 " elapsed_s=%.3f\n",
           total_hashes, total_bytes, elapsed);
    printf("throughput=%.2f MiB/s (%.3f GiB/s)  per_thread=%.2f MiB/s\n",
           mib_s, gib_s, mib_s / (double)nthreads);
    return 0;
}
