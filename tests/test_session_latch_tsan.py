#!/usr/bin/env python3
"""K5 review finding S3: the client's mount-session latches (session_started,
session_fenced) are read without opid_mu on every mutation, so every access to
them must be atomic. This runs the production efs_client_session_fence /
_fenced / _start / _check from src/client/inode_rpc.c with checker threads
racing mount start and several fencer threads, natively and under
ThreadSanitizer. Checks: once a checker sees FENCED it never sees OK again;
every checker ends fenced; exactly one fence log line per round (the atomic
exchange elects one logger); TSan reports no data race.

The TSan half runs or fails with the reason (W138: a SKIP is not a pass). A
negative control rebuilds the review's pattern (plain latch store, atomic
loads elsewhere) and requires TSan to report it, so a clean run means the
harness can see this race.
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = (root / 'src/client/inode_rpc.c').read_text()
a = s.index('void efs_client_session_fence(int rc)')
b = s.index('/* REGISTER/ESTABLISH refuse', a)
latch = s[a:b]
# Production must not touch either latch with a plain access: every use is
# the operand of an __atomic builtin.
for field in ('started', 'fenced'):
    uses = re.findall(r'g_client\.session_' + field, s)
    atomic = re.findall(r'__atomic_\w+\(&g_client\.session_' + field, s)
    assert uses and len(uses) == len(atomic), (field, len(uses), len(atomic))

harness = r'''
#include "efs/protocol.h"
#include "efs/meta_cmd.h"
#include "efs/session.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static pthread_mutex_t opid_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { uint8_t opid_uuid[EFS_OPID_UUID_LEN]; uint32_t opid_epoch;
    int session_started, session_fenced; uint8_t append_session_shards[512]; } g_client;
const char *efs_strerror(int rc) { (void)rc; return "test"; }
static int efs_client_publication_id(uint64_t sequence, struct efs_opid *out) {
    memset(out, 0, sizeof(*out)); out->client_uuid[0] = 9; out->session_epoch = 1;
    out->seq = sequence; return EFS_OK;
}
uint32_t efs_kv_session_shard(const uint8_t uuid[EFS_OPID_UUID_LEN]) { (void)uuid; return 31; }
static void lease_command_be(uint8_t *out, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; i++) out[i] = (uint8_t)(value >> (8 * (bytes - 1 - i)));
}
static unsigned long owner_queries;
static int session_command_reply(uint32_t shard, const uint8_t *command, uint32_t bytes,
                                 uint64_t *known, int probe) {
    (void)shard; (void)command; (void)bytes; (void)probe;
    __atomic_fetch_add(&owner_queries, 1, __ATOMIC_RELAXED);
    *known = 1 | ((uint64_t)EFS_SESSION_ACTIVE << 32); /* ACTIVE at our epoch */
    return EFS_OK;
}
static int publication_session_command(uint32_t shard, const uint8_t *command, uint32_t bytes) {
    (void)shard; (void)command; (void)bytes; assert(0); return EFS_ERR_INVAL;
}
''' + latch + r'''
#ifndef ROUNDS
#define ROUNDS 200
#endif
#define CHECKERS 6
#define FENCERS 3
static int go, fence_now, stop;
static unsigned long checks, regressions, unfenced;
static void *checker(void *arg) {
    (void)arg; int seen = 0; unsigned long n = 0, bad = 0;
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) ;
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        int rc = efs_client_session_check(); n++;
        if (rc == EFS_ERR_FENCED) seen = 1;
        else if (rc != EFS_OK || seen) bad++;
    }
    if (efs_client_session_check() != EFS_ERR_FENCED) __atomic_fetch_add(&unfenced, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&checks, n, __ATOMIC_RELAXED);
    __atomic_fetch_add(&regressions, bad, __ATOMIC_RELAXED);
    return NULL;
}
static void *fencer(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&fence_now, __ATOMIC_ACQUIRE)) ;
    efs_client_session_fence(EFS_ERR_FENCED);
    return NULL;
}
static void pause_us(long us) { struct timespec ts = {0, us * 1000}; nanosleep(&ts, NULL); }
int main(void) {
    for (unsigned r = 0; r < ROUNDS; r++) {
        /* No thread runs between rounds: reset with the same atomic stores. */
        __atomic_store_n(&g_client.session_started, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_client.session_fenced, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&go, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&fence_now, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&stop, 0, __ATOMIC_RELEASE);
        pthread_t c[CHECKERS], f[FENCERS];
        for (int i = 0; i < CHECKERS; i++) pthread_create(&c[i], NULL, checker, NULL);
        for (int i = 0; i < FENCERS; i++) pthread_create(&f[i], NULL, fencer, NULL);
        __atomic_store_n(&go, 1, __ATOMIC_RELEASE);      /* checkers race mount start */
        pause_us(200 + (r % 7) * 50);
        __atomic_store_n(&fence_now, 1, __ATOMIC_RELEASE); /* fencers race each other */
        for (int i = 0; i < FENCERS; i++) pthread_join(f[i], NULL);
        pause_us(200);
        __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
        for (int i = 0; i < CHECKERS; i++) pthread_join(c[i], NULL);
    }
    printf("checks=%lu regressions=%lu unfenced=%lu owner_queries=%lu rounds=%u\n",
           checks, regressions, unfenced, owner_queries, (unsigned)ROUNDS);
    assert(checks > 0 && !regressions && !unfenced && owner_queries > 0);
    return 0;
}
'''

cc = shlex.split(os.environ.get('CC', 'cc'))
flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
         '-I' + str(root / 'include'), '-I' + str(root / 'src/client')]
env = dict(os.environ, TSAN_OPTIONS='halt_on_error=1 exitcode=66')

def build(src, out, extra):
    return subprocess.run(cc + flags + extra + [str(src), '-o', str(out)], capture_output=True, text=True)

def run(binary, rounds):
    r = subprocess.run([str(binary)], capture_output=True, text=True, timeout=600, env=env)
    return r, r.stderr.count('efs-client: session ')

with tempfile.TemporaryDirectory(prefix='efs-session-latch-') as d:
    d = Path(d)
    src = d / 'latch.c'; src.write_text(harness)
    native = build(src, d / 'latch', [])
    if native.returncode:
        raise SystemExit('session latch harness did not compile:\n' + native.stderr)
    r, logs = run(d / 'latch', 200)
    if r.returncode or logs != 200:
        raise SystemExit('session latch native run: FAIL rc=%d fence logs=%d (want 200)\n%s%s'
                         % (r.returncode, logs, r.stdout, r.stderr[-2000:]))
    print('session latch native: ' + r.stdout.strip() + ', one fence log per round PASS')

    tsan = build(src, d / 'latch-tsan', ['-fsanitize=thread', '-DROUNDS=60'])
    if tsan.returncode:
        raise SystemExit('session latch under ThreadSanitizer: FAIL, the TSan build did not compile '
                         '(W138: this gate runs or fails, it never skips):\n' + tsan.stderr)
    r, logs = run(d / 'latch-tsan', 60)
    if r.returncode == 66 or 'ThreadSanitizer:' in r.stderr:
        raise SystemExit('ThreadSanitizer reported a race on the session latch:\n' + r.stderr[-4000:])
    if r.returncode or logs != 60:
        raise SystemExit('session latch under ThreadSanitizer: FAIL rc=%d fence logs=%d (runtime missing? '
                         'W138: this gate runs or fails, it never skips)\n%s'
                         % (r.returncode, logs, r.stderr[-2000:]))
    print('session latch under ThreadSanitizer: no data race, ' + r.stdout.strip() + ' PASS')

    # Negative control: the reviewed pattern, a plain latch store (under
    # opid_mu, as before the fix) racing the unlocked atomic loads.
    old = 'int first=!__atomic_exchange_n(&g_client.session_fenced,1,__ATOMIC_ACQ_REL);'
    assert harness.count(old) == 1
    neg = harness.replace(old, 'pthread_mutex_lock(&opid_mu);int first=!g_client.session_fenced;'
                               'g_client.session_fenced=1;pthread_mutex_unlock(&opid_mu);')
    nsrc = d / 'plain.c'; nsrc.write_text(neg)
    nb = build(nsrc, d / 'plain-tsan', ['-fsanitize=thread', '-DROUNDS=60'])
    if nb.returncode:
        raise SystemExit('session latch negative control did not compile:\n' + nb.stderr)
    r, _ = run(d / 'plain-tsan', 60)
    assert r.returncode != 0 and 'ThreadSanitizer: data race' in r.stderr, \
        'TSan missed the plain-store latch race: rc=%d %s' % (r.returncode, r.stderr[-800:])
    print('session latch negative control: plain store with atomic loads is a reported data race PASS')
