/* hardscan — verify an IOR hard-mode file (--dataPacketType=timestamp)
 * record by record and print every bad record with its offset and the
 * first wrong word. Build on a client node, run against the FUSE mount
 * after a remount (cold) or on the same mount (cache view):
 *
 *   gcc -O2 -pthread -o /tmp/hardscan tests/tools/hardscan.c
 *   /tmp/hardscan <file> <ntasks> <segments> <G> [threads]
 *
 * IOR geometry: -t 47008 -b 47008 -s <segments>; record k (offset
 * k*47008) was written by rank k % ntasks; its 5876 words are
 * (rank << 32) | (uint32)(G + i), the 0 trailing bytes are (char)i.
 * <segments> 0 = scan to EOF. The Sep 30 tool lived in /tmp and was lost;
 * this is the same scan. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REC 47008u
#define WORDS (REC / 8)

static const char *g_path;
static int g_ntasks;
static uint32_t g_sig;
static uint64_t g_nrec;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_bad, g_short, g_done;

struct job {
    uint64_t rec0, rec1;
};

static void check_rec(uint64_t k, const uint8_t *p, size_t have)
{
    const uint64_t *w = (const uint64_t *)p;
    uint32_t rank = (uint32_t)(k % (uint64_t)g_ntasks);
    uint64_t off = k * (uint64_t)REC;
    size_t i;

    if (have < REC) {
        pthread_mutex_lock(&g_mu);
        g_short++;
        printf("SHORT rec=%" PRIu64 " off=%" PRIu64 " have=%zu\n", k, off, have);
        pthread_mutex_unlock(&g_mu);
        return;
    }
    for (i = 0; i < WORDS; i++) {
        uint64_t exp = ((uint64_t)rank << 32) | (uint32_t)(g_sig + (uint32_t)i);
        if (w[i] != exp) {
            uint32_t got_rank = (uint32_t)(w[i] >> 32);
            uint32_t got_lo = (uint32_t)w[i];
            size_t nz = 0, j;
            for (j = 0; j < WORDS; j++)
                if (w[j] == 0)
                    nz++;
            pthread_mutex_lock(&g_mu);
            g_bad++;
            printf("BAD rec=%" PRIu64 " off=%" PRIu64 " seg=%" PRIu64
                   " rank=%u chunk=%" PRIu64 " word=%zu byte=%" PRIu64
                   " exp=%016" PRIx64 " got=%016" PRIx64
                   " (got_rank=%u got_i=%d zero_words=%zu/%u)\n",
                   k, off, k / (uint64_t)g_ntasks, rank, off >> 17, i,
                   off + i * 8, exp, w[i], got_rank,
                   (int)(got_lo - g_sig), nz, WORDS);
            pthread_mutex_unlock(&g_mu);
            return;
        }
    }
}

static void *worker(void *arg)
{
    struct job *j = arg;
    int fd = open(g_path, O_RDONLY);
    size_t bufrec = 256; /* ~11.5 MiB per pread */
    uint8_t *buf;
    uint64_t k = j->rec0;

    if (fd < 0) {
        perror("open");
        exit(2);
    }
    buf = malloc((size_t)bufrec * REC);
    if (!buf)
        exit(2);
    while (k < j->rec1) {
        uint64_t n = j->rec1 - k;
        size_t want, got = 0;
        if (n > bufrec)
            n = bufrec;
        want = (size_t)n * REC;
        while (got < want) {
            ssize_t r = pread(fd, buf + got, want - got,
                              (off_t)(k * (uint64_t)REC + got));
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                pthread_mutex_lock(&g_mu);
                printf("READ_ERR rec=%" PRIu64 " off=%" PRIu64 " errno=%d %s\n",
                       k, k * (uint64_t)REC + got, errno, strerror(errno));
                g_bad++;
                pthread_mutex_unlock(&g_mu);
                got = 0;
                break;
            }
            if (r == 0)
                break;
            got += (size_t)r;
        }
        for (uint64_t i = 0; i < n; i++) {
            size_t have = got > i * REC ? got - i * REC : 0;
            if (have > REC)
                have = REC;
            check_rec(k + i, buf + i * REC, have);
        }
        pthread_mutex_lock(&g_mu);
        g_done += n;
        pthread_mutex_unlock(&g_mu);
        k += n;
    }
    free(buf);
    close(fd);
    return NULL;
}

int main(int argc, char **argv)
{
    int nthr = 8, i;
    uint64_t segs;
    struct stat st;
    pthread_t *th;
    struct job *jobs;

    if (argc < 5) {
        fprintf(stderr, "usage: %s <file> <ntasks> <segments|0> <G> [threads]\n",
                argv[0]);
        return 2;
    }
    g_path = argv[1];
    g_ntasks = atoi(argv[2]);
    segs = strtoull(argv[3], NULL, 10);
    g_sig = (uint32_t)strtoul(argv[4], NULL, 10);
    if (argc > 5)
        nthr = atoi(argv[5]);
    if (g_ntasks <= 0 || nthr <= 0)
        return 2;
    if (stat(g_path, &st) != 0) {
        perror("stat");
        return 2;
    }
    g_nrec = segs ? segs * (uint64_t)g_ntasks
                  : (uint64_t)st.st_size / REC;
    printf("file=%s size=%" PRId64 " records=%" PRIu64 " ntasks=%d G=%u threads=%d\n",
           g_path, (int64_t)st.st_size, g_nrec, g_ntasks, g_sig, nthr);
    th = calloc((size_t)nthr, sizeof(*th));
    jobs = calloc((size_t)nthr, sizeof(*jobs));
    for (i = 0; i < nthr; i++) {
        jobs[i].rec0 = g_nrec * (uint64_t)i / (uint64_t)nthr;
        jobs[i].rec1 = g_nrec * (uint64_t)(i + 1) / (uint64_t)nthr;
        pthread_create(&th[i], NULL, worker, &jobs[i]);
    }
    for (i = 0; i < nthr; i++)
        pthread_join(th[i], NULL);
    printf("DONE records=%" PRIu64 " bad=%" PRIu64 " short=%" PRIu64 "\n",
           g_done, g_bad, g_short);
    return g_bad || g_short ? 1 : 0;
}
