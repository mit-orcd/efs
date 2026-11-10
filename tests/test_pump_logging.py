#!/usr/bin/env python3
"""AE: real proposals progress with every daemon's stderr pipe blocked.

Fresh TCP/O_DIRECT three-member cluster; two fixed-rate rewriting writers,
10 Hz stat probe, a 10 s drained phase then a 25 s blocked window on the same
cluster, exact retained payload readback. The blocked phase must write at
least half the drained phase's rate: both phases pay the same injected fsync
and the same host load, so the ratio measures K72, not the host (Task FD: an
absolute count of 50 is only 7 below what an idle host gives). The existing W159
40 ms serialized fsync interposer forces >20 ms waits, exercising the
read/apply wait diagnostics that formerly logged while holding cv_mu, and
makes every REPORT take >=100 ms so it reaches its slow-report line: K81, a
synchronous fprintf(stderr) in the connection thread withheld the reply behind
the stdio lock the telemetry reporter holds on a full pipe (fsync EIO).
Checks every operation, five-second progress windows, overflow reporting on
recovery and checked shutdown. A failed fixture is retained, never retried.
"""
import argparse
from pathlib import Path
import re
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts/evidence/pump-logging'))
import pump_logging_profile as profile

if not profile.k.fuse_ok():
    print('pump logging: NOTRUN (FUSE unavailable)')
    sys.exit(0)
base = Path(tempfile.mkdtemp(prefix='efs-pump-logging-'))
print('pump logging fixture:', base, flush=True)
profile.require_reporter_startup_failure(profile.k.EFSD, base/'unsupported-stderr', base/'startup-refusal.log')
a = argparse.Namespace(work=base/'work', output=base/'out', sink='blocked', obs=1,
                       seconds=25, drain_seconds=10, threads=2, rate=2, slow_fsync_us=40000, versioned=True, require_progress=True)
r = profile.run(a)
assert r['verdict'] == 'PASS', r
drained, blocked = r['phases']['drained'], r['phases']['controlled']
assert drained['writes'] > 0 and blocked['ops_s'] >= 0.5 * drained['ops_s'], ('blocked rate fell below half the drained rate', drained, blocked)
for n in (1, 2, 3):
    text = (a.output / ('n%d.log' % n)).read_text()
    values = re.findall(r'telemetry: overflow=(\d+)', text)
    assert values and max(map(int, values)) > 0, ('no overflow accounting', n)
    assert 'raft-obs: ' in text and 'iostats: ' in text and 'batch-obs {' in text
    # AB: term/role events are captured under h->mu and delivered whole,
    # in order, by the reporter. Formation runs on a drained sink.
    roles = re.findall(r'^(?:\S+ )?raft-obs: g(\d+) term (\d+)->(\d+) role (\w+)->(\w+) leader=(-?\d+) '
                       r'commit=\d+ applied=\d+ t=\d+ms$', text, re.M)
    assert roles, ('no role-change evidence', n)
    assert not re.search(r'raft-obs: g\d+ term(?! \d+->\d+ role \w+->\w+ leader=-?\d+ commit=\d+ applied=\d+ t=\d+ms$)', text, re.M), ('torn role record', n)
    sync = (a.output / ('n%d-sync.log' % n)).read_text()
    assert re.search(r'slow-fsync: .*calls=[1-9][0-9]+ delay_us=40000', sync), n
# AB/AE: the checked unmount (session revoke through Raft) completes while the
# sink is still blocked. Daemon exit with a blocked sink is K79
# (tests/test_stop_blocked_stderr.py).
b = argparse.Namespace(work=base/'work-stop', output=base/'out-stop', sink='blocked', obs=1,
                       seconds=10, threads=2, rate=2, slow_fsync_us=0, versioned=True,
                       require_progress=True, unmount_blocked=True)
r = profile.run(b)
assert r['verdict'] == 'PASS' and r['writes'] >= 20, r
shutil.rmtree(base)
print('pump logging: PASS (blocked sink, exact payloads, progress, explicit overflow, role events, unmount with sink blocked)')
