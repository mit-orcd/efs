#!/usr/bin/env python3
"""Off-CPU time per blocking stack from a system-wide sched_switch capture.

Capture (unprivileged with kernel.perf_event_paranoid=-1):
    perf record -a -g -e sched:sched_switch -o off.data -- sleep 20
Summarize the threads of the given processes:
    perf script -i off.data -F tid,time,event,trace,ip,sym | \
        offcpu_summary.py --pid 1234 --pid 5678 [--top 25] [--frames 6]

A thread's off-CPU interval runs from its switch-out (prev_pid, with the
stack it blocked in) to its next switch-in (next_pid). Intervals still open
at the end of the capture are dropped. Times are thread-seconds, so N idle
threads waiting 20 s count 20*N; the "class" column separates idle waits
(threads with no work) from waits inside request handling.
"""
import argparse
import collections
import os
import re
import sys

# Classic "prev_pid=N ... prev_state=S ==> ... next_pid=M" and the compact
# "comm:N [prio] S ==> comm:M [prio]" format of newer kernels.
SWITCH = re.compile(r'prev_pid=(\d+) .*?prev_state=(\S+) ==> next_comm=.*? next_pid=(\d+)'
                    r'|:(\d+) \[-?\d+\] (\S+) ==> .*:(\d+) \[-?\d+\]')
HEAD = re.compile(r'^\s*(\d+)\s+([\d.]+):\s+(\S+):')

# Leaf-most matching rule wins; frames are scanned from the leaf upward.
CLASSES = [
    ('retry-sleep', ('nanosleep', 'usleep')),
    ('fuse-idle', ('fuse_dev_do_read', 'fuse_dev_read')),
    ('disk-sync', ('fsync', 'fdatasync', 'xfs_file_fsync', 'iomap_dio_rw', 'io_schedule',
                   'blkdev', 'xfs_log_force')),
    ('net-wait', ('tcp_recvmsg', 'sk_wait_data', 'inet_recvmsg', 'tcp_sendmsg', 'sk_stream_wait',
                  'ep_poll', 'do_poll', 'do_select')),
    ('lock/condvar', ('futex', 'pthread_cond', 'pthread_mutex', 'rwsem', 'mutex_lock')),
]


def classify(frames):
    for f in frames:
        for name, keys in CLASSES:
            if any(k in f for k in keys):
                return name
    return 'other'


def tids_of(pids):
    tids = set()
    for p in pids:
        try:
            tids.update(int(t) for t in os.listdir(f'/proc/{p}/task'))
        except OSError:
            pass
    return tids


def parse(stream):
    """Yield (tid, time, prev_pid, prev_state, next_pid, frames)."""
    cur = None
    frames = []
    for line in stream:
        if not line.strip():
            if cur:
                yield cur + (frames,)
            cur, frames = None, []
            continue
        if line[0] in ' \t' and cur is not None and not HEAD.match(line):
            parts = line.split()
            if len(parts) >= 2:
                # Kernel addresses start with ffff; mark them to keep user frames.
                kernel = parts[0].startswith('ffff')
                frames.append(('k:' if kernel else '') + parts[1].split('+')[0])
            continue
        m = HEAD.match(line)
        if m and 'sched_switch' in m.group(3):
            s = SWITCH.search(line)
            if s:
                g = s.groups()
                prev, state, nxt = g[0:3] if g[0] else g[3:6]
                cur = (int(m.group(1)), float(m.group(2)), int(prev), state, int(nxt))
                frames = []
    if cur:
        yield cur + (frames,)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pid', type=int, action='append', required=True, help='process to summarize (repeat)')
    ap.add_argument('--tids', help='file of thread IDs (default: the pids\' current /proc tasks)')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--frames', type=int, default=6, help='user frames kept per stack')
    a = ap.parse_args()
    tids = set(int(x) for x in open(a.tids).read().split()) if a.tids else tids_of(a.pid)
    tids.update(a.pid)
    pending = {}
    by_stack = collections.Counter()
    by_class = collections.Counter()
    count = collections.Counter()
    first = last = None
    for tid, t, prev, state, nxt, frames in parse(sys.stdin):
        first = t if first is None else first
        last = t
        if prev in tids:
            # Keep the efs/libc frames that name the wait; drop kernel noise.
            user = [f for f in frames if not f.startswith(('k:', '['))]
            pending[prev] = (t, classify(frames), ' <- '.join(user[:a.frames]) or '?', state)
        if nxt in tids and nxt in pending:
            t0, cls, stack, st = pending.pop(nxt)
            d = t - t0
            by_stack[(cls, stack)] += d
            by_class[cls] += d
            count[(cls, stack)] += 1
    span = (last - first) if first is not None else 0.0
    total = sum(by_class.values())
    print(f'threads={len(tids)} span={span:.1f}s off-cpu thread-seconds={total:.1f}')
    for cls, secs in by_class.most_common():
        print(f'  class {cls:13s} {secs:9.1f} s  {100 * secs / total if total else 0:5.1f}%')
    print(f'top {a.top} blocking stacks (thread-seconds, intervals, mean ms):')
    for (cls, stack), secs in by_stack.most_common(a.top):
        n = count[(cls, stack)]
        print(f'  {secs:8.2f} s {n:8d} {1000 * secs / n:8.2f} ms  [{cls}] {stack}')


if __name__ == '__main__':
    sys.exit(main())
