#!/usr/bin/env python3
"""W87 live-gate oracle: classification of append record files (no cluster).

The live gate (tests/live/append_oracle.py) must fail every way an
acknowledged O_APPEND record can be lost or reordered, and must not fail a
file that is consistent with writes that returned an error.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'live'))
import append_oracle as o  # noqa: E402

R = o.record
acked = {(0, 0): 3, (1, 0): 0, (0, 1): 7, (1, 1): 2}
good = R(0, 0, 3) + R(1, 0, 0) + R(0, 1, 7) + R(1, 1, 2)


def verdict(data, failed=()):
    return o.check('t', data, acked, failed)


v = verdict(good)
assert v['pass'] and v['records'] == 4 and v['bytes'] == v['expected_bytes'], v

# An acknowledged record missing (overwritten by a later image, or never
# published) is detected and its present neighbours are reported.
v = verdict(R(0, 0, 3) + R(1, 0, 0) + R(1, 1, 2))
assert not v['pass'] and v['missing'] == 1, v
assert v['missing_detail'][0]['writer'] == 0 and v['missing_detail'][0]['seq'] == 1, v
assert v['missing_detail'][0]['prev_present'][0] == 0, v

# A record replaced by a zero hole of the same length: missing + nul-hole.
hole = b'\0' * (len(R(0, 1, 7)) - 1) + b'\n'
v = verdict(R(0, 0, 3) + R(1, 0, 0) + hole + R(1, 1, 2))
assert not v['pass'] and v['missing'] == 1, v
assert v['bad_runs'] and v['bad_runs'][0]['kind'] == 'nul-hole', v

# Duplicated record (retry applied twice).
v = verdict(good + R(1, 1, 2))
assert not v['pass'] and v['duplicated'] == 1, v

# One descriptor's appends out of issue order (reservation inconsistency).
v = verdict(R(0, 1, 7) + R(1, 0, 0) + R(0, 0, 3) + R(1, 1, 2))
assert not v['pass'] and v['inversions'], v

# Torn record: truncated pad.
v = verdict(R(0, 0, 3) + R(1, 0, 0) + R(0, 1, 7)[:-4] + b'\n' + R(1, 1, 2))
assert not v['pass'] and v['bad_runs'][0]['kind'] == 'torn', v

# Foreign bytes never written.
v = verdict(good + R(5, 9, 1))
assert not v['pass'] and v['foreign'] == 1, v

# A write(2) that failed may be absent, present once, or leave a zero hole.
failed = [(1, 2)]
assert verdict(good, failed)['pass']
assert verdict(good + R(1, 2, 4), failed)['pass']
assert verdict(good + b'\0' * 9 + b'\n', failed)['pass']
v = verdict(good + R(1, 2, 4) + R(1, 2, 4), failed)
assert not v['pass'] and v['duplicated'] == 1, v
# ... but never excuses a lost acknowledged record.
v = verdict(R(0, 0, 3) + R(1, 0, 0) + R(0, 1, 7), failed)
assert not v['pass'] and v['missing'] == 1, v

print('append oracle classification: pass')
