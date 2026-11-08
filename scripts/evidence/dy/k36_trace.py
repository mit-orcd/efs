#!/usr/bin/env python3
"""Run tests/test_k36_recovery.py and report every top-level statement that
took at least GAP seconds (default 1.0), with its line, so a slow step is
pinned to the call that waited. Output lines: `GAP <line> <seconds> <source>`
on stderr, then the test's own output. Usage: k36_trace.py [GAP]."""
import linecache, runpy, sys, time
from pathlib import Path

TEST = str(Path(__file__).resolve().parents[3] / 'tests' / 'test_k36_recovery.py')
GAP = float(sys.argv[1]) if len(sys.argv) > 1 else 1.0
last = [None, time.time()]


def flush(now):
    line, t = last
    if line is not None and now - t >= GAP:
        src = linecache.getline(TEST, line).strip()
        print(f'GAP {line} {now - t:.2f} {src}', file=sys.stderr, flush=True)


def local(frame, event, arg):
    if event == 'line':
        now = time.time()
        flush(now)
        last[0], last[1] = frame.f_lineno, now
    return local


def glob(frame, event, arg):
    # Only the module body: a gap is charged to the top-level statement.
    if frame.f_code.co_filename == TEST and frame.f_code.co_name == '<module>':
        return local
    return None


sys.argv = [TEST]
sys.settrace(glob)
try:
    runpy.run_path(TEST, run_name='__main__')
finally:
    sys.settrace(None)
    flush(time.time())
