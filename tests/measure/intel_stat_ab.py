#!/usr/bin/env python3
"""Bounded interleaved, read-only stat comparison of two Intel-export mounts.

Uses six existing immutable tiny-file controls. Both mounts must show identical
inodes/bytes/size. Alternate A/B order on every pair; first round is warm-up.
No cluster control or optional features. Run without tracers for timings.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time

SUFFIXES = (
    'folder_115666/file_000000.txt', 'folder_115402/file_000000.txt',
    'folder_115402/file_000001.txt', 'file_000002.txt', 'file_000001.txt',
    'file_000000.txt',
)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('old', type=Path)
    ap.add_argument('new', type=Path)
    ap.add_argument('--rounds', type=int, default=6)
    ap.add_argument('--cycles', type=int, default=20)
    a = ap.parse_args()
    assert 1 <= a.rounds <= 20 and 1 <= a.cycles <= 100
    paths, controls = [], []
    for mount in (a.old, a.new):
        assert subprocess.check_output(['findmnt', '-rn', '-M', str(mount), '-o', 'FSTYPE'], text=True).strip() == 'fuse.efs-fuse'
        assert not os.path.lexists(mount/'.efs')
        group = [mount/'tiny-intel2-1m-20261010'/s for s in SUFFIXES]
        check = []
        for p in group:
            st = p.stat()
            with p.open('rb') as f:
                data = f.read(1025)
                assert f.read(1) == b''
            assert 1 <= len(data) <= 512 and len(data) == st.st_size
            check.append((st.st_ino, st.st_size, hashlib.sha256(data).hexdigest()))
        paths.append(group)
        controls.append(check)
    assert controls[0] == controls[1]
    all_samples = [[], []]
    for r in range(a.rounds+1):
        samples = [[], []]
        for cycle in range(a.cycles):
            for i in range(len(SUFFIXES)):
                for mode in ((0, 1) if (cycle*len(SUFFIXES)+i+r) % 2 == 0 else (1, 0)):
                    start = time.perf_counter_ns()
                    st = os.stat(paths[mode][i])
                    end = time.perf_counter_ns()
                    assert (st.st_ino, st.st_size) == controls[mode][i][:2]
                    samples[mode].append((end-start)/1000)
        for mode, name in enumerate(('old', 'new')):
            v = sorted(samples[mode])
            print(json.dumps(dict(round=r, warmup=r == 0, mode=name,
                                  count=len(v), mean_us=statistics.mean(v),
                                  median_us=statistics.median(v),
                                  p95_us=v[int(.95*(len(v)-1))], max_us=max(v),
                                  over_10ms=sum(x > 10000 for x in v))), flush=True)
            if r:
                all_samples[mode].extend(v)
    old, new = (statistics.mean(v) for v in all_samples)
    print(json.dumps(dict(summary=True, old_mean_us=old, new_mean_us=new,
                          reduction_percent=100*(1-new/old),
                          controls_identical=True,
                          tails=[dict(mode=name, median_us=statistics.median(v),
                                      p95_us=sorted(v)[int(.95*(len(v)-1))],
                                      max_us=max(v), over_10ms=sum(x > 10000 for x in v))
                                 for name,v in zip(('old','new'),all_samples)])), flush=True)


if __name__ == '__main__':
    main()
