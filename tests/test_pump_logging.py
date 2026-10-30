#!/usr/bin/env python3
"""AE: real proposals progress with every daemon's stderr pipe blocked.

Fresh TCP/O_DIRECT three-member cluster; two fixed-rate rewriting writers,
10 Hz stat probe, 35 s blocked window, exact retained payload readback. The existing W159
10 ms serialized fsync interposer forces >20 ms waits, exercising the
read/apply wait diagnostics that formerly logged while holding cv_mu.
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
sys.path.insert(0, str(ROOT / 'scripts'))
import pump_logging_profile as profile

if not profile.k.fuse_ok():
    print('pump logging: NOTRUN (FUSE unavailable)')
    sys.exit(0)
base = Path(tempfile.mkdtemp(prefix='efs-pump-logging-'))
print('pump logging fixture:', base, flush=True)
profile.require_reporter_startup_failure(profile.k.EFSD, base/'unsupported-stderr', base/'startup-refusal.log')
a = argparse.Namespace(work=base/'work', output=base/'out', sink='blocked', obs=1,
                       seconds=35, threads=2, rate=2, slow_fsync_us=10000, versioned=True, require_progress=True)
r = profile.run(a)
assert r['verdict'] == 'PASS' and r['writes'] >= 50, r
for n in (1, 2, 3):
    text = (a.output / ('n%d.log' % n)).read_text()
    values = re.findall(r'telemetry: overflow=(\d+)', text)
    assert values and max(map(int, values)) > 0, ('no overflow accounting', n)
    assert 'raft-obs: ' in text and 'iostats: ' in text and 'batch-obs {' in text
    sync = (a.output / ('n%d-sync.log' % n)).read_text()
    assert re.search(r'slow-fsync: .*calls=[1-9][0-9]+ delay_us=10000', sync), n
shutil.rmtree(base)
print('pump logging: PASS (blocked sink, exact payloads, progress and explicit overflow)')
