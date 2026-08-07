#include "bench_local.h"
#include "efs/common.h"
#include "server_internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct bench_writer_arg {
    int id;
    char dir[EFS_MAX_PATH];
    int direct_io;
    double time_sec;
    uint64_t bytes;
    int errors;
};

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

static void rm_bench_dir(const char *path)
{
    DIR *d = opendir(path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char child[EFS_MAX_PATH];
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            unlink(child);
        }
        closedir(d);
    }
    rmdir(path);
}

static void *writer_main(void *arg)
{
    struct bench_writer_arg *a = arg;
    char path[EFS_MAX_PATH];
    snprintf(path, sizeof(path), "%s/writer-%d.bin", a->dir, a->id);

    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (a->direct_io)
        flags |= O_DIRECT;
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        a->errors++;
        return NULL;
    }

    uint8_t *buf = NULL;
    size_t alloc = EFS_FRAGMENT_SIZE;
    if (a->direct_io) {
        alloc = (EFS_FRAGMENT_SIZE + 4095u) & ~4095u;
        if (posix_memalign((void **)&buf, 4096, alloc) != 0) {
            close(fd);
            a->errors++;
            return NULL;
        }
        memset(buf, 0, alloc);
    } else {
        buf = calloc(1, EFS_FRAGMENT_SIZE);
        if (!buf) {
            close(fd);
            a->errors++;
            return NULL;
        }
    }

    double deadline = now_sec() + a->time_sec;
    uint64_t seq = 0;
    while (now_sec() < deadline) {
        /* Rotate within a large sparse-friendly file via pwrite so we keep
         * fragment-sized I/Os without unbounded growth. */
        off_t off = (off_t)((seq % 1024) * EFS_FRAGMENT_SIZE);
        ssize_t n = pwrite(fd, buf, EFS_FRAGMENT_SIZE, off);
        if (n != (ssize_t)EFS_FRAGMENT_SIZE) {
            a->errors++;
            break;
        }
        a->bytes += (uint64_t)EFS_FRAGMENT_SIZE;
        seq++;
    }

    free(buf);
    close(fd);
    return NULL;
}

int server_run_local_bench(const char *path, double time_sec, int writers,
                           int direct_io)
{
    if (!path || !*path || time_sec <= 0.0 || writers < 1) {
        fprintf(stderr, "bench: invalid args (path, --time > 0, --writers >= 1)\n");
        return 1;
    }
    if (writers > EFS_MAX_WRITERS)
        writers = EFS_MAX_WRITERS;

    setlinebuf(stdout);
    setlinebuf(stderr);

    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "bench: mkdir %s: %s\n", path, strerror(errno));
        return 1;
    }

    char dir[EFS_MAX_PATH];
    snprintf(dir, sizeof(dir), "%s/.efs-bench", path);
    rm_bench_dir(dir);
    if (mkdir_p(dir) != 0 && errno != EEXIST) {
        fprintf(stderr, "bench: mkdir %s: %s\n", dir, strerror(errno));
        return 1;
    }

    printf("bench local path=%s time_s=%.3f writers=%d direct_io=%s "
           "frag_bytes=%d\n",
           path, time_sec, writers, direct_io ? "on" : "off", EFS_FRAGMENT_SIZE);
    fflush(stdout);

    struct bench_writer_arg *args = calloc((size_t)writers, sizeof(*args));
    pthread_t *tids = calloc((size_t)writers, sizeof(*tids));
    if (!args || !tids) {
        free(args);
        free(tids);
        rm_bench_dir(dir);
        return 1;
    }

    double t0 = now_sec();
    for (int i = 0; i < writers; i++) {
        args[i].id = i;
        strncpy(args[i].dir, dir, sizeof(args[i].dir) - 1);
        args[i].direct_io = direct_io;
        args[i].time_sec = time_sec;
        if (pthread_create(&tids[i], NULL, writer_main, &args[i]) != 0) {
            args[i].errors++;
            tids[i] = 0;
        }
    }
    for (int i = 0; i < writers; i++) {
        if (tids[i])
            pthread_join(tids[i], NULL);
    }
    double t1 = now_sec();
    double wall = t1 - t0;
    if (wall < 1e-6)
        wall = 1e-6;

    uint64_t total = 0;
    int errors = 0;
    for (int i = 0; i < writers; i++) {
        total += args[i].bytes;
        errors += args[i].errors;
    }

    double gib = (double)total / (1024.0 * 1024.0 * 1024.0);
    double gib_s = gib / wall;

    printf("BENCH_OK kind=local path=%s wall_s=%.3f bytes=%llu GiB_s=%.3f "
           "writers=%d direct_io=%s errors=%d\n",
           path, wall, (unsigned long long)total, gib_s, writers,
           direct_io ? "on" : "off", errors);
    fflush(stdout);

    free(args);
    free(tids);
    rm_bench_dir(dir);
    return errors ? 1 : 0;
}
