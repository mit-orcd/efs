/* W95 private-root, equal-durability sustained churn comparison.
 * This is an independent protocol experiment, never a production store tool.
 * File cohorts have per-object atomic replacement; indexed cohorts are atomic
 * together (a stronger guarantee). Both acknowledge after directory durability.
 * Compile with principles_index.c and libefs.a; run MODEL ROOT ROUNDS COHORT MIX.
 * ROOT must already exist and be empty; successful fixtures are retained. */
#include "efs/checksum.h"
#include "src/bench/principles_index.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef NDEBUG
#error This measurement requires assertions (including checked I/O) enabled
#endif
#define OBJECTS 128u
#define MAX_BODY 65536u
#define QUOTA (64u << 20)
static uint64_t now(void)
{
    struct timespec t;
    assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec;
}
static void putbe(uint8_t *p, uint64_t v, unsigned n)
{
    while (n) {
        p[--n] = (uint8_t)v;
        v >>= 8;
    }
}
static void all(int fd, void *p, size_t n, int readit)
{
    size_t k = 0;
    while (k < n) {
        ssize_t got =
            readit ? read(fd, (uint8_t *)p + k, n - k) : write(fd, (uint8_t *)p + k, n - k);
        if (got < 0 && errno == EINTR)
            continue;
        assert(got > 0);
        k += (size_t)got;
    }
}
static unsigned size_for(unsigned id, unsigned mixed)
{
    static const unsigned small[] = {1, 17, 128, 512};
    return mixed && id % 16 == 0 ? MAX_BODY : small[id % 4];
}
static void body(uint8_t *b, unsigned id, unsigned round, unsigned len)
{
    for (unsigned i = 0; i < len; i++)
        b[i] = (uint8_t)(id * 31 + round * 7 + i);
}
static int cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static uint64_t allocation(int dir)
{
    int fd = openat(dir, ".", O_RDONLY | O_DIRECTORY);
    assert(fd >= 0);
    DIR *d = fdopendir(fd);
    assert(d);
    uint64_t bytes = 0;
    struct dirent *de;
    for (;;) {
        errno = 0;
        de = readdir(d);
        if (!de) {
            assert(!errno);
            break;
        }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        struct stat st;
        assert(!fstatat(dir, de->d_name, &st, AT_SYMLINK_NOFOLLOW));
        assert(S_ISREG(st.st_mode));
        bytes += (uint64_t)st.st_blocks * 512;
    }
    assert(!closedir(d));
    return bytes;
}
int main(int argc, char **argv)
{
    assert(argc == 6);
    int indexed = !strcmp(argv[1], "indexed");
    assert(indexed || !strcmp(argv[1], "files"));
    unsigned rounds = (unsigned)strtoul(argv[3], NULL, 10),
             cohort = (unsigned)strtoul(argv[4], NULL, 10),
             mixed = (unsigned)strtoul(argv[5], NULL, 10);
    assert(rounds > 0 && rounds <= 10000 && cohort > 0 && cohort <= OBJECTS &&
           OBJECTS % cohort == 0 && mixed <= 1);
    int dir = open(argv[2], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    assert(dir >= 0);
    int scan = openat(dir, ".", O_RDONLY | O_DIRECTORY);
    assert(scan >= 0);
    DIR *d = fdopendir(scan);
    assert(d);
    struct dirent *de;
    for (;;) {
        errno = 0;
        de = readdir(d);
        if (!de) {
            assert(!errno);
            break;
        }
        assert(!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."));
    }
    assert(!closedir(d));
    struct principles_index *m =
        indexed ? principles_index_open(argv[2], OBJECTS, QUOTA, 1, 0) : NULL;
    assert(!indexed || m);
    assert(!fsync(dir));
    uint8_t *data = malloc((size_t)cohort * MAX_BODY), *record = malloc(MAX_BODY + 52),
            *check = malloc(MAX_BODY);
    assert(data && record && check);
    uint64_t gen[OBJECTS] = {0}, written = 0, reclaimed = 0, peak = 0;
    unsigned alive[OBJECTS] = {0}, last[OBJECTS] = {0};
    uint64_t *lat = malloc((size_t)rounds * OBJECTS * sizeof(*lat));
    assert(lat);
    size_t samples = 0;
    struct principles_index_stats initial = {0};
    if (m)
        assert(!principles_index_stats(m, &initial));
    uint64_t start = now();
    for (unsigned r = 0; r < rounds; r++)
        for (unsigned first = 0; first < OBJECTS; first += cohort) {
            struct principles_change changes[OBJECTS];
            for (unsigned i = 0; i < cohort; i++) {
                unsigned id = first + i, len = size_for(id, mixed);
                int del = alive[id] && (r + id) % 4 == 0;
                body(data + (size_t)i * MAX_BODY, id, r, len);
                changes[i] = (struct principles_change){id, del ? 0 : len, gen[id],
                                                        del ? NULL : data + (size_t)i * MAX_BODY};
            }
            uint64_t begin = now();
            if (m)
                assert(!principles_index_publish(m, changes, cohort));
            else {
                for (unsigned i = 0; i < cohort; i++) {
                    struct principles_change *c = &changes[i];
                    char name[32];
                    snprintf(name, sizeof(name), "obj-%u", c->object);
                    if (!c->data)
                        assert(!unlinkat(dir, name, 0));
                    else {
                        memcpy(record, "IP01", 4);
                        putbe(record + 4, c->object, 4);
                        putbe(record + 8, c->expected_generation + 1, 8);
                        putbe(record + 16, c->len, 4);
                        memcpy(record + 20, c->data, c->len);
                        efs_hash(record, c->len + 20, record + c->len + 20);
                        int fd =
                            openat(dir, ".next", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
                        assert(fd >= 0);
                        all(fd, record, c->len + 52, 0);
                        assert(!fsync(fd));
                        assert(!close(fd));
                        assert(!renameat(dir, ".next", dir, name));
                        written += c->len + 52;
                    }
                }
                assert(!fsync(dir));
            }
            uint64_t end = now();
            for (unsigned i = 0; i < cohort; i++) {
                unsigned id = first + i;
                lat[samples++] = end - begin;
                gen[id]++;
                alive[id] = changes[i].data != NULL;
                last[id] = r;
            }
            uint64_t allocated = allocation(dir);
            assert(allocated <= QUOTA);
            if (allocated > peak)
                peak = allocated;
            /* GC cost belongs to sustained throughput, not publication latency. */
            if (m) {
                uint64_t removed;
                assert(!principles_index_collect(m, &removed));
                reclaimed += removed;
            }
            allocated = allocation(dir);
            assert(allocated <= QUOTA);
            if (allocated > peak)
                peak = allocated;
        }
    uint64_t elapsed = now() - start;
    struct principles_index_stats st = {0};
    if (m) {
        assert(!principles_index_stats(m, &st));
        principles_index_close(m);
        m = principles_index_open(argv[2], OBJECTS, QUOTA, 0, 1);
        assert(m);
    }
    /* Independent read-only reopen and exact-byte verification is outside timed interval. */
    unsigned live = 0;
    uint64_t logical = 0;
    for (unsigned id = 0; id < OBJECTS; id++) {
        unsigned len = size_for(id, mixed);
        body(record, id, last[id], len);
        if (m) {
            uint32_t got = MAX_BODY;
            uint64_t g;
            int rc = principles_index_get(m, id, check, &got, &g);
            assert(g == gen[id]);
            if (alive[id])
                assert(!rc && got == len && !memcmp(record, check, len));
            else
                assert(rc == -1 && errno == ENOENT);
        } else {
            char name[32];
            snprintf(name, sizeof(name), "obj-%u", id);
            int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW);
            if (!alive[id])
                assert(fd == -1 && errno == ENOENT);
            else {
                assert(fd >= 0);
                all(fd, check, 20, 1);
                uint8_t h[20];
                memcpy(h, "IP01", 4);
                putbe(h + 4, id, 4);
                putbe(h + 8, gen[id], 8);
                putbe(h + 16, len, 4);
                assert(!memcmp(h, check, 20));
                all(fd, check, len, 1);
                assert(!memcmp(record, check, len));
                uint8_t digest[32], stored[32];
                memmove(record + 20, record, len);
                memcpy(record, h, 20);
                efs_hash(record, len + 20, digest);
                all(fd, stored, 32, 1);
                assert(!memcmp(digest, stored, 32));
                uint8_t extra;
                assert(read(fd, &extra, 1) == 0);
                assert(!close(fd));
            }
        }
        if (alive[id]) {
            live++;
            logical += len;
        }
    }
    if (m)
        principles_index_close(m);
    qsort(lat, samples, sizeof(*lat), cmp);
    printf("CHURN_OK model=%s rounds=%u cohort=%u mixed=%u ops=%zu ops_s=%.3f p50_us=%.3f "
           "p95_us=%.3f p99_us=%.3f allocated=%llu peak_allocated=%llu live_bytes=%llu "
           "live_objects=%u body_written=%llu index_written=%llu reclaimed=%llu verified=%u\n",
           argv[1], rounds, cohort, mixed, samples, (double)samples * 1e9 / elapsed,
           lat[samples / 2] / 1000., lat[(samples - 1) * 95 / 100] / 1000.,
           lat[(samples - 1) * 99 / 100] / 1000., (unsigned long long)allocation(dir),
           (unsigned long long)peak, (unsigned long long)logical, live,
           (unsigned long long)(indexed ? st.body_written_bytes - initial.body_written_bytes
                                        : written),
           (unsigned long long)(st.index_written_bytes - initial.index_written_bytes),
           (unsigned long long)reclaimed, OBJECTS);
    assert(!close(dir));
    free(data);
    free(record);
    free(check);
    free(lat);
    return 0;
}
