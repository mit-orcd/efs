#!/usr/bin/env python3
"""K79: SIGTERM'd efsd exits within its stop bound with stderr a full pipe.

Three-daemon private cluster, FUSE mount, two fixed-rate rewriting writers
under the W159 10 ms fsync interposer (its wait diagnostics fill the pipe).
Every daemon's stderr is a 4 KiB pipe nobody reads from the start of the
load; the checked unmount runs with the sink blocked, then every daemon gets
SIGTERM while the pipe is still full. Each must exit rc 0 within
pump_logging_profile.STOP_BOUND_S (50 s teardown + 5 s report) and leave no
core. Before K79 threads blocked in synchronous fprintf(stderr) held the
stop to its forced exit (rc 3).
"""
import argparse
from pathlib import Path
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts/evidence/pump-logging'))
import pump_logging_profile as profile

if not profile.k.fuse_ok():
    print('stop blocked stderr: NOTRUN (FUSE unavailable)')
    sys.exit(0)
base = Path(tempfile.mkdtemp(prefix='efs-stop-blocked-'))
print('stop blocked stderr fixture:', base, flush=True)
a = argparse.Namespace(work=base/'work', output=base/'out', sink='blocked', obs=1,
                       seconds=15, threads=2, rate=2, slow_fsync_us=10000, versioned=True,
                       require_progress=True, stop_blocked=True)
r = profile.run(a)
assert r['verdict'] == 'PASS' and r['writes'] >= 20, r
print('stop blocked stderr: daemon exit seconds after SIGTERM', r['stop_s'])
shutil.rmtree(base)
print('stop blocked stderr: PASS (full stderr pipe through SIGTERM, rc 0, within %d s, no core)' % profile.STOP_BOUND_S)
