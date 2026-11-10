#!/usr/bin/env python3
"""K107: host_view_get never spins without bound on a publisher inside its window.

Runs the production host_publish_view and host_view_get from raft_host.c
with stub core getters. One publisher is held inside its odd-v_seq window
(a V_STORE hook blocks it after two of its stores); readers started then must
neither return a half-new view nor keep loading v_seq: each stops after
HOST_VIEW_TRIES odd loads, waits on h->view_mu, and returns the finished view
once the publisher is released. An uncontended read takes no lock and loads
v_seq twice. A free-running publisher against readers checks every returned
view is one publish's. Unfixed code never takes the mutex, so the readers'
v_seq loads pass the bound and the test fails with "spin". There is no sleep
or timer: waits are sched_yield loops and an alarm turns a hang into a failure.
"""
from pathlib import Path
import re, subprocess, tempfile
import csrc
s = csrc.module('raft_host')
a = s.index('    uint64_t v_seq;'); group = s[a:s.index('\n};', a)]
a = s.index('struct host_view {'); view = s[a:s.index('\n};', a) + 3]
tries = re.search(r'#define HOST_VIEW_TRIES[^\n]*', s)
def func(name):
    a = s.index('static void ' + name + '('); return s[a:s.index('\n}', a) + 2]
code = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define EFS_RAFT_LEADER 2
''' + (tries.group(0) if tries else '#define HOST_VIEW_TRIES 64') + r'''
#define NREADERS 8
/* Every core getter derives from one number, so a torn view shows as two different numbers. */
struct efs_raft { uint64_t n; };
static uint64_t efs_raft_commit(struct efs_raft *r) { return r->n; }
static uint64_t efs_raft_applied(struct efs_raft *r) { return r->n; }
static uint64_t efs_raft_term(struct efs_raft *r) { return r->n; }
static int efs_raft_role(struct efs_raft *r) { (void)r; return EFS_RAFT_LEADER; }
static int efs_raft_leader(struct efs_raft *r) { return (int)r->n; }
static uint32_t efs_raft_voters(struct efs_raft *r) { return (uint32_t)r->n; }
static uint64_t efs_raft_read_index(struct efs_raft *r) { return r->n; }
static uint64_t efs_raft_read_round(struct efs_raft *r) { return r->n; }
static int efs_raft_read_pending(struct efs_raft *r) { return (int)r->n; }
static int efs_raft_read_quorum(struct efs_raft *r) { return (int)r->n; }
static int efs_raft_read_proven(struct efs_raft *r, uint64_t *round, uint64_t *index) { *round = *index = r->n; return 1; }
static uint64_t efs_raft_read_context(struct efs_raft *r) { return r->n; }
struct host_group {
    struct efs_raft *r; uint8_t group; int hosted;
''' + group + r'''
};
struct efs_raft_host { struct host_group g[1]; pthread_mutex_t view_mu; };
static struct efs_raft_host host = {.g = {{.hosted = 1}}, .view_mu = PTHREAD_MUTEX_INITIALIZER};
static struct host_group *group_slot(struct efs_raft_host *h, uint8_t group) { (void)group; return h->g; }
static uint64_t seq_loads, view_locks;
static int counted_lock(pthread_mutex_t *m) {
    if (m == &host.view_mu) __atomic_add_fetch(&view_locks, 1, __ATOMIC_RELAXED);
    return pthread_mutex_lock(m);
}
#define pthread_mutex_lock counted_lock
#define __atomic_load_n(p, m) ({ if ((const void *)(p) == (const void *)&host.g[0].v_seq) { \
    __atomic_add_fetch(&seq_loads, 1, __ATOMIC_RELAXED); } __atomic_load_n(p, m); })
/* The hook holds the publisher after its second field store (v_read_context, v_commit), before v_applied. */
static pthread_mutex_t hold_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cv = PTHREAD_COND_INITIALIZER;
static int armed, in_window, released;
static void hold_hook(const void *p) {
    if (!__atomic_load_n(&armed, __ATOMIC_ACQUIRE) || p != &host.g[0].v_applied) return;
    pthread_mutex_lock(&hold_mu);
    in_window = 1; pthread_cond_broadcast(&hold_cv);
    while (!released) pthread_cond_wait(&hold_cv, &hold_mu);
    pthread_mutex_unlock(&hold_mu);
}
#define V_LOAD(p) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define V_STORE(p, v) (hold_hook(&(p)), __atomic_store_n(&(p), (v), __ATOMIC_RELEASE))
''' + view + '\n' + func('host_publish_view') + '\n' + func('host_view_get') + r'''
static struct efs_raft core;
static void publish(uint64_t n) { core.n = n; host.g[0].r = &core; host_publish_view(&host, 0); }
static uint64_t coherent(const struct host_view *v) {
    uint64_t n = v->commit;
    assert(v->has && v->applied == n && v->term == n && v->read_index == n && v->read_round == n &&
           v->proven_round == n && v->proven_index == n && v->read_context == n && v->voters == (uint32_t)n &&
           v->leader == (int)n && v->read_pending == (int)n && v->read_done == (int)n && v->proven_done == 1);
    return n;
}
static void fail(const char *msg) { puts(msg); exit(1); }
static void on_alarm(int sig) { (void)sig; static const char m[] = "FAIL: hang\n"; if (write(2, m, sizeof m - 1)) {} _exit(1); }
static uint64_t seen[NREADERS]; static int returned;
static void *reader(void *arg) {
    struct host_view v; host_view_get(&host, 0, &v);
    seen[(long)arg] = coherent(&v); __atomic_add_fetch(&returned, 1, __ATOMIC_RELEASE); return NULL;
}
static void *publisher(void *arg) { publish((uint64_t)(long)arg); return NULL; }
static int done; static uint64_t stress_reads;
static void *stress_pub(void *arg) {
    (void)arg; for (uint64_t n = 10; n < 30000; n++) publish(n);
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE); return NULL;
}
static void *stress_reader(void *arg) {
    (void)arg; struct host_view v; uint64_t last = 0, reads = 0;
    do { host_view_get(&host, 0, &v); uint64_t n = coherent(&v); assert(n >= last); last = n; reads++; }
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE));
    __atomic_add_fetch(&stress_reads, reads, __ATOMIC_RELAXED); return NULL;
}
int main(void) {
    struct host_view v; pthread_t t, r[NREADERS];
    signal(SIGALRM, on_alarm); alarm(60);
    publish(1);
    uint64_t l0 = seq_loads, m0 = view_locks;
    host_view_get(&host, 0, &v);
    assert(coherent(&v) == 1 && view_locks == m0 && seq_loads - l0 == 2);   /* fast path: no lock, two loads */
    __atomic_store_n(&armed, 1, __ATOMIC_RELEASE);
    pthread_create(&t, NULL, publisher, (void *)2L);
    pthread_mutex_lock(&hold_mu); while (!in_window) pthread_cond_wait(&hold_cv, &hold_mu); pthread_mutex_unlock(&hold_mu);
    assert(__atomic_load_n(&host.g[0].v_seq, __ATOMIC_ACQUIRE) & 1);      /* publisher is inside its window */
    assert(host.g[0].v_commit == 2 && host.g[0].v_applied == 1);          /* ... and the fields are half new */
    l0 = seq_loads; m0 = view_locks;
    for (long i = 0; i < NREADERS; i++) pthread_create(&r[i], NULL, reader, (void *)i);
    while (view_locks - m0 < NREADERS) {                                  /* every reader reaches the mutex */
        if (seq_loads - l0 > (uint64_t)NREADERS * (HOST_VIEW_TRIES + 1)) fail("FAIL: readers spin on an odd v_seq without waiting for the publisher");
        sched_yield();
    }
    assert(seq_loads - l0 <= (uint64_t)NREADERS * HOST_VIEW_TRIES);       /* at most TRIES odd loads each */
    assert(__atomic_load_n(&returned, __ATOMIC_ACQUIRE) == 0);            /* nobody returned the half-new view */
    pthread_mutex_lock(&hold_mu); released = 1; pthread_cond_broadcast(&hold_cv); pthread_mutex_unlock(&hold_mu);
    pthread_join(t, NULL);
    for (int i = 0; i < NREADERS; i++) { pthread_join(r[i], NULL); assert(seen[i] == 2); }
    __atomic_store_n(&armed, 0, __ATOMIC_RELEASE);
    pthread_t sp, sr[4];
    pthread_create(&sp, NULL, stress_pub, NULL);
    for (int i = 0; i < 4; i++) pthread_create(&sr[i], NULL, stress_reader, NULL);
    pthread_join(sp, NULL); for (int i = 0; i < 4; i++) pthread_join(sr[i], NULL);
    assert(stress_reads > 0);
    puts("host_view_get: publisher held in its window, readers wait on view_mu after HOST_VIEW_TRIES odd loads, no torn view PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-view-spin-') as d:
    p = Path(d) / 't.c'; p.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-pthread', str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True, timeout=60)
