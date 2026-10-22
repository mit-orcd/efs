#!/usr/bin/env python3
"""W87 concurrent O_APPEND oracle: every acknowledged record exactly once.

Many appender processes write uniquely identified, variable-length records to
one file through O_APPEND. After every writer has fsynced and closed, the file
is read warm, then again after an optional cold step (remount, or a cluster
restart plus remount) run through --cold-cmd. Each read must contain every
acknowledged record exactly once, no foreign bytes, and every writer's own
records in issue order (a reservation-consistent order: one descriptor's
appends are issued sequentially, so their reserved offsets must increase).

A failure prints which records are missing or duplicated, the byte position
of every unparseable run (NUL holes versus torn records), and for every lost
record its acknowledged neighbours' offsets, so the loss can be matched to the
client's append-reserve trace (EFS_DCACHE_TRACE) by writer/sequence/length.

Record format (ASCII, newline terminated, no newline inside):
    K09 W<writer:02> S<seq:06> L<pad:04> <pad times 'x'>\\n
"""
import argparse
import json
import os
import random
import re
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

HEAD = re.compile(rb'^K09 W(\d{2}) S(\d{6}) L(\d{4}) (x*)$')


def record(writer, seq, pad):
    return b'K09 W%02d S%06d L%04d %s\n' % (writer, seq, pad, b'x' * pad)


WRITER = r'''
import json, os, random, sys, time
path, writer, count, mode, fsync_every, pad_max, seed, gap_us = sys.argv[1:]
writer = int(writer); count = int(count); fsync_every = int(fsync_every)
pad_max = int(pad_max); gap_us = int(gap_us)
rng = random.Random(int(seed) * 1000003 + writer)
acked = []; failed = []; fd = None
persist = mode == 'persist' or (mode == 'mixed' and writer % 2 == 0)
try:
    if persist:
        fd = os.open(path, os.O_WRONLY | os.O_APPEND)
    for seq in range(count):
        pad = rng.randrange(0, pad_max + 1)
        rec = b'K09 W%02d S%06d L%04d %s\n' % (writer, seq, pad, b'x' * pad)
        active = fd if fd is not None else os.open(path, os.O_WRONLY | os.O_APPEND)
        try:
            try:
                n = os.write(active, rec)
            except OSError as e:
                failed.append([seq, pad, 'write', e.errno]); continue
            if n != len(rec):
                failed.append([seq, pad, 'short', n]); continue
            acked.append([seq, pad])
            if fsync_every and seq % fsync_every == fsync_every - 1:
                try: os.fsync(active)
                except OSError as e: failed.append([seq, pad, 'fsync', e.errno])
        finally:
            if fd is None:
                os.close(active)
        if gap_us:
            time.sleep(gap_us / 1e6)
    if fd is not None:
        os.fsync(fd)
except OSError as e:
    failed.append([-1, 0, 'fatal', e.errno])
finally:
    if fd is not None:
        os.close(fd)
json.dump({'writer': writer, 'acked': acked, 'failed': failed, 'persist': persist}, sys.stdout)
'''


def parse(data):
    """Return (records, bad_runs). records: list of (writer, seq, pad, offset)."""
    recs, bad = [], []
    pos = 0
    for line in data.split(b'\n'):
        m = HEAD.match(line)
        if m and len(m.group(4)) == int(m.group(3)):
            recs.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), pos))
        elif line:
            kind = 'nul-hole' if line.strip(b'\0') == b'' else 'torn'
            bad.append({'offset': pos, 'len': len(line), 'kind': kind,
                        'head': line[:40].decode('latin1')})
        pos += len(line) + 1
    return recs, bad


def check(label, data, acked, failed=()):
    """acked: dict (writer, seq) -> pad. failed: (writer, seq) whose write(2)
    returned an error: each may be absent or present once (an error after the
    reservation may also leave a zero hole; POSIX permits neither loss nor a
    hole for an acknowledged write). Returns a verdict dict."""
    recs, bad = parse(data)
    failed = set(failed)
    seen = Counter((w, s) for w, s, _, _ in recs)
    where = {}
    for w, s, p, off in recs:
        where.setdefault((w, s), []).append(off)
    missing = sorted(k for k in acked if seen[k] == 0)
    dup = sorted(k for k, n in seen.items() if n > 1)
    foreign = sorted(k for k in seen if k not in acked and k not in failed)
    landed_failed = sorted(k for k in seen if k in failed)
    if failed:
        bad = [b for b in bad if b['kind'] != 'nul-hole']
    inversions = []
    last = {}
    for w, s, _, off in recs:
        if w in last and s < last[w][0]:
            inversions.append({'writer': w, 'seq': s, 'offset': off,
                               'after_seq': last[w][0], 'after_offset': last[w][1]})
        last[w] = (s, off)
    expected_len = sum(len(record(w, s, p)) for (w, s), p in acked.items())
    expected_len += sum(len(record(w, s, p)) for w, s, p, _ in recs if (w, s) in failed)
    detail = []
    for w, s in missing:
        prev = max((x for x in where if x[0] == w and x[1] < s), default=None)
        nxt = min((x for x in where if x[0] == w and x[1] > s), default=None)
        detail.append({'writer': w, 'seq': s, 'len': len(record(w, s, acked[(w, s)])),
                       'prev_present': prev and [prev[1], where[prev]],
                       'next_present': nxt and [nxt[1], where[nxt]]})
    exact = len(data) == expected_len or (failed and len(data) >= expected_len)
    ok = not (missing or dup or foreign or bad or inversions) and exact
    return {'label': label, 'pass': ok, 'bytes': len(data), 'expected_bytes': expected_len,
            'records': len(recs), 'acked': len(acked), 'missing': len(missing),
            'duplicated': len(dup), 'foreign': len(foreign), 'bad_runs': bad[:50],
            'inversions': inversions[:50], 'missing_detail': detail[:200],
            'duplicates': [list(k) for k in dup[:50]],
            'failed_writes_landed': [list(k) for k in landed_failed]}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mount', type=Path, required=True)
    ap.add_argument('--name', default='k09-append')
    ap.add_argument('--writers', type=int, default=8)
    ap.add_argument('--records', type=int, default=200)
    ap.add_argument('--mode', choices=('persist', 'perrecord', 'mixed'), default='mixed',
                    help='persist: one O_APPEND fd per writer; perrecord: open/close per record')
    ap.add_argument('--fsync-every', type=int, default=0)
    ap.add_argument('--pad-max', type=int, default=120)
    ap.add_argument('--gap-us', type=int, default=0)
    ap.add_argument('--seed', type=int, default=int(time.time()))
    ap.add_argument('--cold-cmd', default='', help='shell command run between the warm and cold reads')
    ap.add_argument('--keep', action='store_true', help='leave the file on failure')
    ap.add_argument('--strict-errors', action='store_true',
                    help='fail on any write(2)/fsync error, not only on lost/duplicated acknowledged records')
    ap.add_argument('--out', type=Path)
    a = ap.parse_args()

    path = a.mount / a.name
    with open(path, 'wb'):
        pass
    t0 = time.monotonic()
    procs = [subprocess.Popen([sys.executable, '-c', WRITER, str(path), str(w), str(a.records),
                               a.mode, str(a.fsync_every), str(a.pad_max), str(a.seed), str(a.gap_us)],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
             for w in range(a.writers)]
    reports = []
    for p in procs:
        out, err = p.communicate(timeout=1800)
        if p.returncode != 0:
            print('writer crashed rc=%d: %s' % (p.returncode, err.decode(errors='replace')[-2000:]))
            sys.exit(3)
        reports.append(json.loads(out))
    elapsed = time.monotonic() - t0
    acked = {(r['writer'], s): pad for r in reports for s, pad in r['acked']}
    failed = [(r['writer'], f) for r in reports for f in r['failed']]
    failed_ids = [(w, f[0]) for w, f in failed if f[0] >= 0]
    warm = check('warm', path.read_bytes(), acked, failed_ids)
    print('warm: %s records=%d/%d missing=%d dup=%d' % ('PASS' if warm['pass'] else 'FAIL',
          warm['records'], warm['acked'], warm['missing'], warm['duplicated']), flush=True)
    cold = None
    if a.cold_cmd:
        step = subprocess.run(a.cold_cmd, shell=True)
        if step.returncode != 0:
            cold = {'label': 'cold', 'pass': False, 'error': 'cold step rc=%d' % step.returncode,
                    'bytes': 0, 'expected_bytes': warm['expected_bytes'], 'records': 0, 'acked': len(acked),
                    'missing': len(acked), 'duplicated': 0, 'foreign': 0, 'bad_runs': [],
                    'inversions': [], 'missing_detail': []}
        else:
            cold = check('cold', path.read_bytes(), acked, failed_ids)
    verdict = {'seed': a.seed, 'writers': a.writers, 'records': a.records, 'mode': a.mode,
               'fsync_every': a.fsync_every, 'pad_max': a.pad_max, 'write_seconds': round(elapsed, 2),
               'acked': len(acked), 'write_failures': failed[:50], 'warm': warm, 'cold': cold,
               'pass': warm['pass'] and (cold is None or cold['pass']) and
                       not (a.strict_errors and failed) and not any(f[0] < 0 for _, f in failed)}
    if a.out:
        a.out.write_text(json.dumps(verdict, indent=1))
    for v in (warm, cold):
        if v:
            if v.get('error'):
                print('%s: FAIL %s' % (v['label'], v['error']))
                continue
            print('%s: %s bytes=%d/%d records=%d/%d missing=%d dup=%d foreign=%d bad_runs=%d inversions=%d' % (
                v['label'], 'PASS' if v['pass'] else 'FAIL', v['bytes'], v['expected_bytes'],
                v['records'], v['acked'], v['missing'], v['duplicated'], v['foreign'],
                len(v['bad_runs']), len(v['inversions'])))
            for d in v['missing_detail'][:20]:
                print('  missing', d)
            for b in v['bad_runs'][:20]:
                print('  bad run', b)
            for i in v['inversions'][:10]:
                print('  inversion', i)
    if failed:
        print('write failures (errno; not acknowledged):', failed[:20])
    print('seed=%d writers=%d records=%d mode=%s fsync_every=%d acked=%d in %.1fs -> %s' % (
        a.seed, a.writers, a.records, a.mode, a.fsync_every, len(acked), elapsed,
        'PASS' if verdict['pass'] else 'FAIL'))
    if verdict['pass'] or not a.keep:
        path.unlink()
    sys.exit(0 if verdict['pass'] else 1)


if __name__ == '__main__':
    main()
