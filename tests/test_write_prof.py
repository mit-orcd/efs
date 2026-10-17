#!/usr/bin/env python3
"""Write-path phase histograms: buckets, quantile bounds and the stderr line."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
code = r'''
#include "write_prof.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
static struct efs_wprof_hist hs[WP_N];
static void *adder(void *arg) {
    (void)arg;
    for (int i = 0; i < 100000; i++) efs_wprof_add(&hs[WP_MU], (uint64_t)(i % 7));
    return NULL;
}
int main(void) {
    assert(efs_wprof_bucket(0) == 0 && efs_wprof_bucket(1) == 1);
    assert(efs_wprof_bucket(2) == 2 && efs_wprof_bucket(3) == 2 && efs_wprof_bucket(4) == 3);
    assert(efs_wprof_bucket(UINT64_MAX) == WPROF_BUCKETS - 1);
    /* 90 samples of 10 us, 9 of 1000 us, 1 of 100000 us. */
    for (int i = 0; i < 90; i++) efs_wprof_add(&hs[WP_PERSIST], 10);
    for (int i = 0; i < 9; i++) efs_wprof_add(&hs[WP_PERSIST], 1000);
    efs_wprof_add(&hs[WP_PERSIST], 100000);
    const struct efs_wprof_hist *p = &hs[WP_PERSIST];
    assert(p->n == 100 && p->sum == 90 * 10 + 9 * 1000 + 100000 && p->max == 100000);
    /* Each quantile is its bucket's upper bound: never below the sample. */
    assert(efs_wprof_quantile(p->b, p->n, 500) == 15);
    assert(efs_wprof_quantile(p->b, p->n, 950) == 1023);
    assert(efs_wprof_quantile(p->b, p->n, 990) == 1023);
    assert(efs_wprof_quantile(p->b, p->n, 1000) == 131071);
    assert(efs_wprof_quantile(p->b, 0, 500) == 0);
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, adder, NULL);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    assert(hs[WP_MU].n == 400000 && hs[WP_MU].max == 6);
    char line[2048];
    assert(efs_wprof_format(line, sizeof(line), hs, 42, 3) > 0);
    assert(strstr(line, "write-phase-prof mono_us=42 arc_fallback=3"));
    assert(strstr(line, " persist=100/109900/100000/15/1023/1023"));
    assert(strstr(line, " mu=400000/"));
    assert(!strstr(line, " hold="));  /* phases without samples are omitted */
    char tiny[16];
    assert(efs_wprof_format(tiny, sizeof(tiny), hs, 42, 3) == -1);
    puts("write-prof: buckets, quantile upper bounds, concurrent adds and line format PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-write-prof-') as d:
    p = Path(d) / 'test.c'; p.write_text(code)
    cc = shlex.split(os.environ.get('CC', 'cc'))
    subprocess.run(cc + ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-pthread', '-I' + str(root / 'src/server'), str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True)
src = (root / 'src/server/raft_host.c').read_text()
# Every phase is fed somewhere in the host, and only behind write_prof_on().
for name in ['WP_HOLD', 'WP_MU', 'WP_APPEND', 'WP_SPIN', 'WP_PERSIST', 'WP_MU_AFTER',
             'WP_PROPOSE', 'WP_APPLY', 'WP_COVER', 'WP_REPORT_MU']:
    assert 'wprof_add(' + name + ',' in src.replace(' ', ''), name
assert 'wprof_add(WP_COMMIT_APPLY' in src and 'wprof_add(WP_WAKE' in src and 'wprof_add(WP_VERDICT' in src
