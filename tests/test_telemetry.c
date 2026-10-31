#include "efs/telemetry.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/eventfd.h>

static uint64_t renders;
static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec/1e9;
}
static void render(FILE *out, const void *data)
{
    fprintf(out, "sample=%llu\n", (unsigned long long)*(const uint64_t *)data);
    __atomic_add_fetch(&renders, 1, __ATOMIC_RELEASE);
}
static void *produce(void *arg)
{
    struct efs_telemetry *r = arg;
    for (uint64_t i=0; i<1000; i++) {
        efs_telemetry_submit(r, &i, sizeof(i));
        efs_telemetry_printf(r, "event=%llu\n", (unsigned long long)i);
        efs_telemetry_log(0, "transport-event=%llu\n", (unsigned long long)i);
    }
    return NULL;
}
static void blocked(int recover, int socket_sink)
{
    int fds[2], saved = dup(2);
    if (socket_sink) {
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, fds)==0);
        int capacity=4096;
        assert(!setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)));
    } else {
        assert(pipe2(fds, O_CLOEXEC|O_NONBLOCK) == 0);
        assert(fcntl(fds[1], F_SETPIPE_SZ, 4096) == 4096);
    }
    char fill[4096]; memset(fill, 'X', sizeof(fill));
    while (write(fds[1], fill, sizeof(fill))>0) {}
    assert(errno==EAGAIN);
    int flags = fcntl(fds[1], F_GETFL);
    assert(fcntl(fds[1], F_SETFL, flags & ~O_NONBLOCK) == 0);
    assert(dup2(fds[1], 2) == 2);
    int original = fcntl(2, F_GETFL);
    struct efs_telemetry *r = efs_telemetry_start(render);
    assert(r);
    assert(fcntl(2, F_GETFL) == original); /* critical path stays blocking */
    uint64_t v = 7, before = __atomic_load_n(&renders, __ATOMIC_ACQUIRE);
    efs_telemetry_submit(r, &v, sizeof(v));
    double end = now()+1;
    while (__atomic_load_n(&renders, __ATOMIC_ACQUIRE) == before && now()<end) usleep(1000);
    pthread_t producers[4];
    double start = now();
    for (unsigned i=0;i<4;i++) assert(!pthread_create(&producers[i], NULL, produce, r));
    for (unsigned i=0;i<4;i++) pthread_join(producers[i], NULL);
    double submit_s = now()-start;
    int overflow = 0;
    if (recover) {
        char capture[131072]; size_t used=0;
        end = now()+2;
        while (now()<end && used<sizeof(capture)-1) {
            ssize_t n = read(fds[0], capture+used, sizeof(capture)-used-1);
            if (n>0) used+=(size_t)n;
            else { assert(n<0 && errno==EAGAIN); usleep(1000); }
            capture[used]=0;
            char *p = strstr(capture, "telemetry: overflow=");
            if (p && strtoull(p+20, NULL, 10)>0) { overflow=1; break; }
        }
    }
    start=now();
    efs_telemetry_stop(r);
    double stop_s=now()-start;
    dup2(saved, 2); close(saved); close(fds[0]); close(fds[1]);
    assert(submit_s<1);
    assert(stop_s<.5);
    assert(!recover || overflow);
}
static void regular(void)
{
    FILE *f = tmpfile(); assert(f);
    int saved = dup(2);
    fflush(stderr);
    dup2(fileno(f), 2);
    fprintf(stderr, "critical-before\n");
    struct efs_telemetry *r = efs_telemetry_start(render); assert(r);
    uint64_t n=19, before=__atomic_load_n(&renders, __ATOMIC_ACQUIRE);
    efs_telemetry_submit(r, &n, sizeof(n));
    char text[512]={0};
    double end=now()+2;
    while (now()<end) {
        assert(pread(fileno(f), text, sizeof(text)-1, 0)>=0);
        if (strstr(text,"sample=19\n")) break;
        /* Admission may deliberately drop on a contended mailbox. Repeat a
         * periodic snapshot until one is rendered; do not assume lossless
         * first delivery when testing the shared file offset. */
        if (__atomic_load_n(&renders, __ATOMIC_ACQUIRE) == before)
            efs_telemetry_submit(r, &n, sizeof(n));
        usleep(1000);
    }
    efs_telemetry_stop(r);
    efs_telemetry_log(1, "critical-after\n"); fflush(stderr);
    memset(text,0,sizeof(text));
    assert(pread(fileno(f), text, sizeof(text)-1, 0)>=0);
    dup2(saved,2); close(saved); fclose(f);
    const char *last = strstr(text,"critical-after\n");
    if (strncmp(text,"critical-before\n",16) || !strstr(text,"sample=19\n") ||
        !last || last[15]) printf("unexpected regular-sink contents: %s\n", text);
    assert(!strncmp(text,"critical-before\n",16));
    assert(strstr(text,"sample=19\n"));
    assert(last && !last[15]);
}
/* K79 fault exits: every routine line queued before efs_telemetry_flush() is
 * on the sink when it returns (no stop, the reporter keeps running). */
static void flushed(void)
{
    FILE *f = tmpfile(); assert(f);
    int saved = dup(2);
    fflush(stderr);
    dup2(fileno(f), 2);
    struct efs_telemetry *r = efs_telemetry_start(render); assert(r);
    for (int i = 0; i < 24; i++) efs_telemetry_log(0, "queued=%d\n", i);
    efs_telemetry_flush();
    char text[8192]={0};
    assert(pread(fileno(f), text, sizeof(text)-1, 0)>=0);
    int seen = 0;
    for (int i = 0; i < 24; i++) { char l[32]; snprintf(l, sizeof(l), "queued=%d\n", i); seen += !!strstr(text, l); }
    efs_telemetry_stop(r);
    dup2(saved,2); close(saved); fclose(f);
    if (seen != 24) printf("flush: %d of 24 queued lines on the sink\n", seen);
    assert(seen == 24);
    efs_telemetry_flush();   /* unbound: returns at once */
}
int main(void)
{
    setenv("EFS_LOG_TS","0",1);
    int saved = dup(2), unsupported = eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC);
    assert(unsupported >= 0);
    assert(dup2(unsupported,2)==2);
    assert(!efs_telemetry_start(render));
    dup2(saved,2); close(saved); close(unsupported);
    regular(); flushed(); blocked(1, 0); blocked(0, 0); blocked(1, 1); blocked(0, 1);
    puts("telemetry: bounded concurrent producers, overflow recovery, critical fd flags, file offsets, flush and blocked stop PASS");
    return 0;
}
