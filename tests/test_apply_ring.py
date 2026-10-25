#!/usr/bin/env python3
"""Apply-result ring read without h->mu: slot protocol and every reader's semantics.

Part 1 runs apply_ring.h alone: wraparound, reset, bounded retries and a
writer racing readers (each copy must be one writer's complete record, so rc
always matches its own extra). It also runs under ThreadSanitizer, and
fails with the reason when that build or run cannot happen (W138: a SKIP
is not a pass). gcc rejects the seqlock's atomic_thread_fence under
-fsanitize=thread (-Werror=tsan) because TSan does not model fences; the
sanitizer build alone allows that diagnostic, so a clean TSan run checks
the atomic payload accesses, not the fence ordering: the torn-record
stress check is the ordering gate, and the plain-field negative control
shows TSan does see seqlock races in this harness. Part 2 executes the
production readers from
raft_host.c (host_arc_read/verdict, host_wait_verdict,
host_wait_settled, host_wait_refs' host_apply_rc
and host_pub_batch_wait) and checks they keep their own BUSY / STALE /
NOT_PRIMARY rules, never take h->mu on the fast path, and fall back to it
when a writer holds the slot.
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
text = (root / 'src/server/raft_host.c').read_text()

def function(name):
    match = re.search(r'^static [^\n;]*\b' + name + r'\([^;]+?\)\n\{', text, re.M)
    if not match: raise RuntimeError(name)
    return text[match.start():text.index('\n}', match.end()) + 2]

cc = shlex.split(os.environ.get('CC', 'cc'))
flags = ['-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
         '-I' + str(root / 'include'), '-I' + str(root / 'src/server')]

ring = r'''
#include "apply_ring.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#define RING 8
static struct efs_arc_slot slots[RING];
static int stop;
static unsigned long long copies, torn;
static void put(uint64_t idx) {
    efs_arc_store(&slots[idx % RING], idx, idx * 3, (int32_t)(idx % 5) - 2, idx * 7, idx * 11);
}
static int coherent(const struct efs_arc_view *v) {
    return v->term == v->idx * 3 && v->rc == (int32_t)(v->idx % 5) - 2 &&
           v->extra == v->idx * 7 && v->apply_us == v->idx * 11;
}
#ifndef WRITES
#define WRITES 4000000
#endif
static void *writer(void *arg) {
    (void)arg;
    for (uint64_t i = RING; i < WRITES; i++) put(i);
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    return NULL;
}
static void *reader(void *arg) {
    uintptr_t k = (uintptr_t)arg;
    unsigned long long n = 0, bad = 0;
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        struct efs_arc_view v;
        if (efs_arc_load(&slots[k % RING], &v, 64)) {
            n++;
            if (!coherent(&v) || (v.idx && v.idx % RING != k % RING)) bad++;
        }
    }
    __atomic_fetch_add(&copies, n, __ATOMIC_RELAXED);
    __atomic_fetch_add(&torn, bad, __ATOMIC_RELAXED);
    return NULL;
}
int main(void) {
    struct efs_arc_view v;
    for (uint64_t i = 1; i <= RING; i++) put(i);
    assert(efs_arc_load(&slots[3], &v, 1) && v.idx == 3 && coherent(&v));
    /* Wraparound: index 3 + RING replaces index 3 in the same slot. */
    put(3 + RING);
    assert(efs_arc_load(&slots[3], &v, 1) && v.idx == 3 + RING && coherent(&v));
    /* Reset clears every field through the protocol. */
    for (int s = 0; s < RING; s++) efs_arc_store(&slots[s], 0, 0, 0, 0, 0);
    for (int s = 0; s < RING; s++)
        assert(efs_arc_load(&slots[s], &v, 1) && !v.idx && !v.term && !v.rc && !v.extra && !v.apply_us);
    /* A writer stuck mid-record: readers give up after the bound. */
    __atomic_store_n(&slots[5].seq, slots[5].seq + 1, __ATOMIC_RELAXED);
    assert(!efs_arc_load(&slots[5], &v, 1000));
    __atomic_store_n(&slots[5].seq, slots[5].seq + 1, __ATOMIC_RELEASE);
    assert(efs_arc_load(&slots[5], &v, 1));
    for (uint64_t i = 1; i <= RING; i++) put(i);
    pthread_t w, r[3];
    for (uintptr_t k = 0; k < 3; k++) pthread_create(&r[k], NULL, reader, (void *)(k + 1));
    pthread_create(&w, NULL, writer, NULL);
    pthread_join(w, NULL);
    for (int k = 0; k < 3; k++) pthread_join(r[k], NULL);
    assert(copies > 0 && torn == 0);
    printf("apply ring slot: wraparound, reset, bounded retries, %llu concurrent copies, 0 torn PASS\n", copies);
}
'''

host = r'''
#include "efs/common.h"
#include "apply_ring.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define HOST_APPLY_RC_RING 8
#define HOST_APPLY_RC_MASK 7
struct host_group { uint8_t group; int hosted; struct efs_arc_slot arc[HOST_APPLY_RC_RING]; };
struct efs_raft_host { pthread_mutex_t mu; struct host_group g[2];
    uint64_t obs_arc_miss, obs_arc_term_miss, obs_pub_ring[64], obs_pub_n, obs_pub_max_us; };
struct host_pub_batch { uint8_t group; uint64_t last_idx, t0; uint64_t *idxs, *terms; uint32_t nidx; int overflow; };
static uint64_t g_wprof_arc_fallback;
static int fast_path, locked;
#define HOST_ARC_READ_TRIES 64
static struct host_group *group_slot(struct efs_raft_host *h, uint8_t group) {
    for (int i = 0; i < 2; i++) if (h->g[i].group == group) return &h->g[i];
    return NULL;
}
/* Count every h->mu acquisition by the code under test. */
static int counted_lock(pthread_mutex_t *m) { if (__atomic_load_n(&fast_path, __ATOMIC_RELAXED)) abort(); __atomic_fetch_add(&locked, 1, __ATOMIC_RELAXED); return pthread_mutex_lock(m); }
#define pthread_mutex_lock counted_lock
static int host_wait_applied_own(struct efs_raft_host *h, uint8_t group, uint64_t idx, int *hint) {
    (void)h; (void)group; (void)idx; (void)hint; return EFS_OK;
}
static int write_prof_on(void) { return 0; }
static uint64_t wprof_now(void) { return 0; }
static void wprof_verdict(uint64_t t0, uint64_t seen, uint64_t apply_us) { (void)t0; (void)seen; (void)apply_us; }
static uint64_t now_us_(void) { return 0; }
static void host_pub_batch_reset(struct host_pub_batch *b) {
    free(b->idxs); free(b->terms); b->idxs = b->terms = NULL; b->nidx = 0; b->last_idx = 0; b->overflow = 0;
}
''' + '\n'.join(function(n) for n in [
    'host_arc_read', 'host_arc_verdict', 'host_apply_rc', 'host_arc_apply_us',
    'host_wait_verdict', 'host_wait_settled', 'host_pub_batch_wait']) + r'''
static struct efs_raft_host h = {.mu = PTHREAD_MUTEX_INITIALIZER, .g = {{.group = 0, .hosted = 1}, {.group = 2, .hosted = 0}}};
static void put(uint64_t idx, uint64_t term, int rc, uint64_t extra) {
    efs_arc_store(&h.g[0].arc[idx & HOST_APPLY_RC_MASK], idx, term, rc, extra, 0);
}
static int batch(uint64_t *idx, uint64_t *term, uint32_t n, int overflow, uint8_t group) {
    struct host_pub_batch b = {.group = group, .last_idx = idx[n - 1], .nidx = n, .overflow = overflow};
    b.idxs = malloc(n * sizeof(uint64_t)); b.terms = malloc(n * sizeof(uint64_t));
    memcpy(b.idxs, idx, n * sizeof(uint64_t)); memcpy(b.terms, term, n * sizeof(uint64_t));
    return host_pub_batch_wait(&h, &b, NULL);
}
static int writer_odd;
static void *slow_writer(void *arg) {
    (void)arg;
    pthread_mutex_lock(&h.mu);
    struct efs_arc_slot *s = &h.g[0].arc[4];
    uint32_t q = __atomic_load_n(&s->seq, __ATOMIC_RELAXED);
    __atomic_store_n(&s->seq, q + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&writer_odd, 1, __ATOMIC_RELEASE);
    struct timespec ts = {0, 50 * 1000 * 1000}; nanosleep(&ts, NULL);
    __atomic_store_n(&s->idx, 12, __ATOMIC_RELAXED);
    __atomic_store_n(&s->term, 9, __ATOMIC_RELAXED);
    __atomic_store_n(&s->rc, EFS_ERR_EXIST, __ATOMIC_RELAXED);
    __atomic_store_n(&s->extra, 77, __ATOMIC_RELAXED);
    __atomic_store_n(&s->seq, q + 2, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&h.mu);
    return NULL;
}
int main(void) {
    fast_path = 1;
    /* host_wait_verdict / host_apply_rc: verdict, BUSY on overwrite, NOT_PRIMARY on term, OK unhosted. */
    put(11, 7, EFS_ERR_STALE, 0);
    assert(host_wait_verdict(&h, 0, 11, 7, NULL) == EFS_ERR_STALE);
    assert(host_wait_verdict(&h, 0, 11, 0, NULL) == EFS_ERR_STALE);   /* forwarded: index alone */
    assert(host_wait_verdict(&h, 0, 11, 8, NULL) == EFS_ERR_NOT_PRIMARY && h.obs_arc_term_miss == 1);
    put(19, 7, EFS_OK, 0);  /* 19 wraps onto 11's slot */
    assert(host_wait_verdict(&h, 0, 11, 7, NULL) == EFS_ERR_BUSY && h.obs_arc_miss == 1);
    assert(host_apply_rc(&h, 0, 19, 7) == EFS_OK && host_apply_rc(&h, 0, 27, 7) == EFS_ERR_BUSY);
    assert(host_wait_verdict(&h, 2, 11, 7, NULL) == EFS_OK);          /* not hosted */
    assert(host_wait_verdict(&h, 4, 11, 7, NULL) == EFS_OK);          /* no slot */
    /* host_wait_settled: only a term mismatch on our own index is an error. */
    assert(host_wait_settled(&h, 0, 19, 7, NULL) == EFS_OK);
    assert(host_wait_settled(&h, 0, 19, 6, NULL) == EFS_ERR_NOT_PRIMARY);
    assert(host_wait_settled(&h, 0, 11, 7, NULL) == EFS_OK);          /* overwritten: not checked */
    assert(host_wait_settled(&h, 0, 19, 0, NULL) == EFS_OK);
    assert(host_wait_settled(&h, 2, 19, 6, NULL) == EFS_OK);
    /* host_pub_batch_wait: every entry's own verdict; misses and truncation are STALE. */
    put(1, 7, EFS_OK, 0); put(2, 7, EFS_OK, 0); put(3, 7, EFS_OK, 0);
    uint64_t i3[] = {1, 2, 3}, t3[] = {7, 7, 7};
    assert(batch(i3, t3, 3, 0, 0) == EFS_OK);
    put(2, 7, EFS_ERR_BUSY, 0);
    assert(batch(i3, t3, 3, 0, 0) == EFS_ERR_BUSY);                 /* last applied OK, middle did not */
    put(2, 6, EFS_OK, 0);
    assert(batch(i3, t3, 3, 0, 0) == EFS_ERR_STALE);
    put(2, 7, EFS_OK, 0); put(10, 7, EFS_OK, 0);                      /* 10 overwrites 2 */
    assert(batch(i3, t3, 3, 0, 0) == EFS_ERR_STALE);
    put(2, 7, EFS_ERR_BUSY, 0); put(3, 6, EFS_OK, 0);
    assert(batch(i3, t3, 3, 0, 0) == EFS_ERR_BUSY);                 /* a non-STALE error outranks STALE */
    put(3, 7, EFS_OK, 0); put(2, 7, EFS_OK, 0);
    assert(batch(i3, t3, 3, 1, 0) == EFS_ERR_STALE);                /* overflow */
    assert(batch(i3, t3, 3, 0, 2) == EFS_OK);                       /* forwarded batch */
    assert(locked == 0 && g_wprof_arc_fallback == 0);
    /* A writer holding the slot under h->mu: the reader falls back to the lock
     * and returns that writer's complete record. */
    fast_path = 0;
    pthread_t w; pthread_create(&w, NULL, slow_writer, NULL);
    while (!__atomic_load_n(&writer_odd, __ATOMIC_ACQUIRE)) ;
    assert(host_wait_verdict(&h, 0, 12, 9, NULL) == EFS_ERR_EXIST);
    pthread_join(w, NULL);
    assert(g_wprof_arc_fallback == 1 && locked == 2);  /* writer + reader fallback */
    puts("apply ring readers: verdict/settled/extra/batch semantics, wraparound, term mismatch, lock-free fast path and h->mu fallback PASS");
}
'''

with tempfile.TemporaryDirectory(prefix='efs-apply-ring-') as d:
    d = Path(d)
    (d / 'ring.c').write_text(ring); (d / 'host.c').write_text(host)
    subprocess.run(cc + flags + [str(d / 'ring.c'), '-o', str(d / 'ring')], check=True)
    subprocess.run([str(d / 'ring')], check=True, timeout=120)
    # ThreadSanitizer half (W138): it runs, or this test fails with the reason.
    # Fewer writes than the native stress run: TSan slows the loop ~10x.
    tsan_flags = flags + ['-fsanitize=thread', '-DWRITES=400000']
    def tsan_build(extra, out):
        return subprocess.run(cc + extra + tsan_flags + [str(d / 'ring.c'), '-o', str(d / out)], capture_output=True, text=True)
    tsan = tsan_build([], 'ring-tsan')
    if tsan.returncode != 0 and re.search(r'\[-W(error=)?tsan\]', tsan.stderr):
        # gcc: 'atomic_thread_fence' is not supported with '-fsanitize=thread'.
        # The fences stay (they are the seqlock's ordering); the sanitizer
        # build alone keeps the diagnostic non-fatal.
        tsan_flags.append('-Wno-error=tsan')
        tsan = tsan_build([], 'ring-tsan')
    if tsan.returncode != 0:
        raise SystemExit('apply ring under ThreadSanitizer: FAIL, the TSan build did not compile '
                         '(W138: this gate runs or fails, it never skips):\n' + tsan.stderr)
    env = dict(os.environ, TSAN_OPTIONS='halt_on_error=1 exitcode=66')
    run = subprocess.run([str(d / 'ring-tsan')], capture_output=True, text=True, timeout=600, env=env)
    if run.returncode == 66 or 'ThreadSanitizer:' in run.stderr:
        raise SystemExit('ThreadSanitizer reported a race:\n' + run.stderr)
    if run.returncode != 0:
        raise SystemExit('apply ring under ThreadSanitizer: FAIL, the TSan binary did not run (rc=%d; runtime '
                         'missing? W138: this gate runs or fails, it never skips):\n%s' % (run.returncode, run.stderr))
    print('apply ring slot under ThreadSanitizer: no data race on the atomic payload PASS '
          '(limit: TSan does not model the fences; the torn-record stress check is the ordering gate)')
    # Negative control: the same seqlock with plain payload fields
    # (the race WG21 P1478 describes) must be reported.
    hdr = (root / 'src/server/apply_ring.h').read_text()
    hdr = re.sub(r'__atomic_store_n\(&s->(idx|term|rc|extra|apply_us), ([^,]+), __ATOMIC_RELAXED\)', r's->\1 = \2', hdr)
    hdr = re.sub(r'__atomic_load_n\(&s->(idx|term|rc|extra|apply_us), __ATOMIC_RELAXED\)', r's->\1', hdr)
    assert hdr.count('s->idx = idx') == 1 and 'v->idx = s->idx;' in hdr
    plain = d / 'plain'; plain.mkdir(); (plain / 'apply_ring.h').write_text(hdr)
    neg_build = tsan_build(['-I' + str(plain)], 'ring-plain')
    if neg_build.returncode != 0:
        raise SystemExit('apply ring negative control: FAIL, the plain-field TSan build did not compile:\n' + neg_build.stderr)
    neg = subprocess.run([str(d / 'ring-plain')], capture_output=True, text=True, timeout=600, env=env)
    assert neg.returncode != 0 and 'ThreadSanitizer: data race' in neg.stderr, 'TSan missed the plain-field seqlock race: rc=%d %s' % (neg.returncode, neg.stderr[-600:] + neg.stdout[-300:])
    print('apply ring negative control: plain payload fields are a reported data race PASS')
    subprocess.run(cc + flags + [str(d / 'host.c'), '-o', str(d / 'host')], check=True)
    subprocess.run([str(d / 'host')], check=True, timeout=60)

# Writers: both host_apply stamps and the attach reset go through the slot protocol.
apply = function('host_apply')
assert apply.count('efs_arc_store(&g->arc[index & HOST_APPLY_RC_MASK]') == 2
attach = function('attach_group')
assert 'efs_arc_store(&h->g[gi].arc[s], 0, 0, 0, 0, 0);' in attach
assert not re.search(r'arc_(idx|term|rc|extra)\[', text)
