#!/usr/bin/env python3
"""Task FN follow-up: give the devel control tree the same measurements the fix tree logs.

Run in the root of a `git archive cff8eb424` tree (devel before K108). It adds, to
src/server/raft_host.c only, the import start time, the step count and the
longest slice (one efs_kv_batch per pump call), and an `import applied ...
steps= step_max_us= total_ms=` line when the last slice lands, in the format the
fix tree logs. No behaviour changes. The control is therefore devel plus this patch.
"""
import sys
p='src/server/raft_host.c'; s=open(p).read()
def sub(old,new):
    global s
    assert s.count(old)==1,old; s=s.replace(old,new)
sub("    int import_applying;","    uint64_t import_t0_us, import_step_max_us; uint32_t import_steps;\n    int import_applying;")
sub("""        h->g[gi].import_state = SNAP_IMP_RUN;
        gen = h->g[gi].import_gen;""","""        h->g[gi].import_state = SNAP_IMP_RUN;
        h->g[gi].import_t0_us = now_us_(); h->g[gi].import_step_max_us = 0; h->g[gi].import_steps = 0;
        gen = h->g[gi].import_gen;""")
sub("""        *rc = n ? efs_kv_batch(h->kv, diff + at, n) : EFS_OK;
        __atomic_store_n(&h->snap_batch, 0, __ATOMIC_RELEASE);""","""        uint64_t step_t0 = now_us_();
        *rc = n ? efs_kv_batch(h->kv, diff + at, n) : EFS_OK;
        uint64_t step_us = now_us_() - step_t0;
        __atomic_store_n(&h->snap_batch, 0, __ATOMIC_RELEASE);""")
sub("""        g->import_applying = 0;
        if (g->import_state != SNAP_IMP_APPLY) {""","""        g->import_applying = 0;
        g->import_steps++; if (step_us > g->import_step_max_us) g->import_step_max_us = step_us;
        if (g->import_state != SNAP_IMP_APPLY) {""")
sub("""        g->part_off = 0;
        g->import_state = SNAP_IMP_IDLE;
        snap_import_free_diff(g);""","""        g->part_off = 0;
        g->import_state = SNAP_IMP_IDLE;
        efs_telemetry_log(0, "raft-snap: import applied group=%u incl=%llu diff_keys=%llu buf_peak=0 steps=%u step_max_us=%llu total_ms=%llu\\n", g->group, (unsigned long long)g->import_incl, (unsigned long long)g->import_ni, g->import_steps, (unsigned long long)g->import_step_max_us, (unsigned long long)((now_us_() - g->import_t0_us) / 1000));
        snap_import_free_diff(g);""")
open(p,'w').write(s)
