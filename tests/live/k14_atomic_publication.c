/* K14 / W77 observation workload (scripts/k14_atomic_cluster.sh).
 *
 *   writer FILE OFF LEN SECONDS LOG [FIRST_VERSION [COUNT]]
 *       Rewrites [OFF, OFF+LEN) with version v = FIRST.. as ONE pwrite
 *       (O_DIRECT, so one FUSE write request when LEN <= max_write), then
 *       fsync; appends "published v" to LOG after each successful fsync and
 *       "attempted v" before it. COUNT > 1 (K32): COUNT adjacent regions
 *       [OFF + i*LEN, +LEN), each its own pwrite of version v, then one
 *       fsync, so one REPORT carries COUNT units. Write/fsync errors are logged and the next
 *       version is tried (a dead mount ends the writer with status 3).
 *   reader FILE OFF LEN RLEN SECONDS SEED
 *       O_DIRECT preads of RLEN at random 4 KiB-aligned offsets inside the
 *       region; every read must be exactly one version: each 8-byte word at
 *       file offset o equals (v << 32) | (o / 8) for the v of its first word.
 *       Prints one summary line; status 1 if any read was torn.
 *   check FILE OFF LEN
 *       One read of the whole region: prints "version v" or "torn".
 *
 * A read that spans two write requests may legitimately see two versions;
 * every read here stays inside one writer request's region. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ALIGN 4096u

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void fill(uint64_t *w, uint64_t v, uint64_t off, size_t len)
{
    for (size_t i = 0; i < len / 8; i++)
        w[i] = (v << 32) | (uint32_t)((off + 8 * i) / 8);
}

/* 0: one version (*v); 1: torn (*v = first word's, *bad = first odd word). */
static int verify(const uint64_t *w, uint64_t off, size_t len, uint64_t *v, size_t *bad)
{
    *v = w[0] >> 32;
    for (size_t i = 0; i < len / 8; i++)
        if (w[i] != ((*v << 32) | (uint32_t)((off + 8 * i) / 8))) {
            *bad = i;
            return 1;
        }
    return 0;
}

static void *abuf(size_t len)
{
    void *p = NULL;
    if (posix_memalign(&p, ALIGN, len))
        return NULL;
    return p;
}

static int writer(int argc, char **argv)
{
    if (argc < 7)
        return 2;
    const char *file = argv[2];
    uint64_t off = strtoull(argv[3], NULL, 0), len = strtoull(argv[4], NULL, 0);
    double secs = atof(argv[5]);
    FILE *log = fopen(argv[6], "a");
    uint64_t v = argc > 7 ? strtoull(argv[7], NULL, 0) : 1;
    uint64_t count = argc > 8 ? strtoull(argv[8], NULL, 0) : 1;
    uint64_t *buf = abuf(len);
    int fd = open(file, O_RDWR | O_CREAT | O_DIRECT, 0644), dead = 0;
    double end = now_s() + secs;

    if (!log || !buf || fd < 0 || !count) {
        fprintf(stderr, "writer: setup %s: %s\n", file, strerror(errno));
        return 2;
    }
    setvbuf(log, NULL, _IOLBF, 0);
    while (now_s() < end) {
        fprintf(log, "attempted %" PRIu64 "\n", v);
        ssize_t n = (ssize_t)len;
        for (uint64_t i = 0; i < count && n == (ssize_t)len; i++) {
            fill(buf, v, off + i * len, len);
            n = pwrite(fd, buf, len, (off_t)(off + i * len));
        }
        int rc = n == (ssize_t)len ? fsync(fd) : -1;
        if (rc == 0) {
            fprintf(log, "published %" PRIu64 "\n", v);
            dead = 0;
        } else {
            int e = errno;
            fprintf(log, "error %" PRIu64 " n=%zd errno=%d %s\n", v, n, e, strerror(e));
            if ((e == ENOTCONN || e == EIO || e == EBADF) && ++dead >= 20)
                return 3;
            usleep(200000);
        }
        v++;
    }
    close(fd);
    fclose(log);
    return 0;
}

static int reader(int argc, char **argv)
{
    if (argc < 8)
        return 2;
    const char *file = argv[2];
    uint64_t off = strtoull(argv[3], NULL, 0), len = strtoull(argv[4], NULL, 0);
    uint64_t rlen = strtoull(argv[5], NULL, 0);
    double secs = atof(argv[6]), end = now_s() + secs;
    unsigned seed = (unsigned)strtoul(argv[7], NULL, 0);
    uint64_t *buf = abuf(rlen), reads = 0, torn = 0, errs = 0, shorts = 0;
    uint64_t vmin = UINT64_MAX, vmax = 0, changes = 0, last = UINT64_MAX;
    uint64_t steps = (len - rlen) / ALIGN + 1;
    int fd = -1;

    if (!buf || rlen > len || rlen % ALIGN || off % ALIGN)
        return 2;
    while (now_s() < end) {
        if (fd < 0 || reads % 64 == 0) { /* reopen: close-to-open freshness */
            if (fd >= 0)
                close(fd);
            fd = open(file, O_RDONLY | O_DIRECT);
            if (fd < 0) {
                errs++;
                usleep(100000);
                continue;
            }
        }
        uint64_t o = off + (uint64_t)(rand_r(&seed) % steps) * ALIGN, v;
        size_t bad = 0;
        ssize_t n = pread(fd, buf, rlen, (off_t)o);
        reads++;
        if (n < 0) {
            errs++;
            if (errs <= 5)
                fprintf(stderr, "reader %s: pread @%" PRIu64 ": %s\n", file, o, strerror(errno));
            usleep(20000);
            continue;
        }
        if ((uint64_t)n != rlen) {
            shorts++;
            continue;
        }
        if (verify(buf, o, rlen, &v, &bad)) {
            torn++;
            if (torn <= 5)
                fprintf(stderr, "TORN %s read @%" PRIu64 "+%" PRIu64 ": version %" PRIu64
                        " at start, word %zu (file offset %" PRIu64 ") holds %016" PRIx64 "\n",
                        file, o, rlen, v, bad, o + 8 * bad, buf[bad]);
            continue;
        }
        if (v < vmin)
            vmin = v;
        if (v > vmax)
            vmax = v;
        if (last != UINT64_MAX && v != last)
            changes++;
        last = v;
    }
    printf("reader %s reads=%" PRIu64 " torn=%" PRIu64 " errors=%" PRIu64 " short=%" PRIu64
           " versions=%" PRIu64 "..%" PRIu64 " changes=%" PRIu64 "\n", file, reads, torn, errs,
           shorts, vmin == UINT64_MAX ? 0 : vmin, vmax, changes);
    return torn ? 1 : 0;
}

static int check(int argc, char **argv)
{
    if (argc < 5)
        return 2;
    uint64_t off = strtoull(argv[3], NULL, 0), len = strtoull(argv[4], NULL, 0), v;
    uint64_t *buf = abuf(len);
    size_t bad = 0;
    int fd = open(argv[2], O_RDONLY | O_DIRECT);
    if (!buf || fd < 0 || pread(fd, buf, len, (off_t)off) != (ssize_t)len) {
        printf("unreadable %s\n", strerror(errno));
        return 2;
    }
    if (verify(buf, off, len, &v, &bad)) {
        printf("torn first=%" PRIu64 " word=%zu\n", v, bad);
        return 1;
    }
    printf("version %" PRIu64 "\n", v);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "writer"))
        return writer(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "reader"))
        return reader(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "check"))
        return check(argc, argv);
    fprintf(stderr, "usage: %s writer|reader|check ...\n", argv[0]);
    return 2;
}
