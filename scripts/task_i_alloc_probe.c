/* Diagnostic-only LD_PRELOAD observer. Never trims or changes malloc policy.
 * Only a newly launched efs-fuse starts the observer thread. The driver writes
 * the actual load-start epoch; snapshots follow at 20, 40 and 60 minutes.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <malloc.h>
#include <limits.h>

static char output[PATH_MAX];
static double wall(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static void *observe(void *unused) {
    (void)unused;
    char path[PATH_MAX + 128];
    double start = 0;
    snprintf(path, sizeof(path), "%s/load-start", output);
    while (start < 1e9) {
        FILE *f = fopen(path, "r");
        if (f) { if (fscanf(f, "%lf", &start) != 1) start = 0; fclose(f); }
        if (start < 1e9) sleep(1);
    }
    for (int minute = 20; minute <= 60; minute += 20) {
        while (wall() < start + minute * 60) sleep(1);
        struct mallinfo2 m = mallinfo2();
        snprintf(path, sizeof(path), "%s/%ld-%dm.xml", output, (long)getpid(), minute);
        FILE *f = fopen(path, "w");
        if (f) { malloc_info(0, f); fclose(f); }
        snprintf(path, sizeof(path), "%s/%ld-%dm.json", output, (long)getpid(), minute);
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "{\"pid\":%ld,\"elapsed_s\":%.3f,\"arena\":%zu,\"uordblks\":%zu,\"fordblks\":%zu,\"hblkhd\":%zu,\"keepcost\":%zu}\n",
                    (long)getpid(), wall()-start, m.arena, m.uordblks, m.fordblks, m.hblkhd, m.keepcost);
            fclose(f);
        }
    }
    return NULL;
}
__attribute__((constructor)) static void start_observer(void) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe)-1);
    if (n < 0) return;
    exe[n] = 0;
    char *name = strrchr(exe, '/');
    if (!name || strcmp(name+1, "efs-fuse")) return;
    const char *dir = getenv("TASK_I_ALLOC_DIR");
    if (!dir || !*dir || strlen(dir) >= sizeof(output)) return;
    strcpy(output, dir);
    pthread_t t;
    if (!pthread_create(&t, NULL, observe, NULL)) pthread_detach(t);
}
