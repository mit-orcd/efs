#!/usr/bin/env python3
"""fi_sample.py OUT NAME=PID ... — 1 Hz TSV of cumulative CPU ticks per pid
(/proc/PID/stat utime+stime), host CPU (/proc/stat), md0 sectors written and
load average, until the file OUT.stop appears. Task FI sampler; stdlib only."""
import os, sys, time

def ticks(pid):
    try:
        f = open(f'/proc/{pid}/stat').read().rsplit(')', 1)[1].split()
        return int(f[11]) + int(f[12])
    except OSError:
        return -1

def host():
    c = open('/proc/stat').readline().split()[1:]
    return sum(map(int, c)), int(c[3]) + int(c[4])   # total, idle+iowait

def md0():
    for l in open('/proc/diskstats'):
        f = l.split()
        if f[2] == 'md0':
            return int(f[9])
    return -1

def threads(pid):
    """(tid, comm, utime+stime) of every thread of pid."""
    r = []
    try:
        for t in os.listdir(f'/proc/{pid}/task'):
            raw = open(f'/proc/{pid}/task/{t}/stat').read()
            f = raw.rsplit(')', 1)[1].split()
            r.append((t, raw.split('(', 1)[1].rsplit(')', 1)[0], int(f[11]) + int(f[12])))
    except OSError:
        pass
    return r

out = sys.argv[1]; pids = [a.split('=') for a in sys.argv[2:]]
with open(out, 'w') as o, open(out + '.threads', 'w') as th:
    o.write('t\ttotal\tidle\tmd0_wsect\tload1\t' + '\t'.join(n for n, _ in pids) + '\n')
    while not os.path.exists(out + '.stop'):
        t, i = host()
        o.write('\t'.join(map(str, [f'{time.time():.2f}', t, i, md0(), open('/proc/loadavg').read().split()[0]]
                           + [ticks(p) for _, p in pids])) + '\n')
        now = f'{time.time():.2f}'
        for n, p in pids:
            for tid, comm, tk in threads(p):
                th.write(f'{now}\t{n}\t{tid}\t{comm}\t{tk}\n')
        o.flush(); th.flush(); time.sleep(1)
