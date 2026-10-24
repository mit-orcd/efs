#!/usr/bin/env python3
"""A7/D42 measurement: idle cost of the always-on .efs query workers.

Starts a private three-member loopback cluster from one build (BIN holds
efsd and efs-mgmt), waits until both metadata groups have a leader, lets it
settle, then samples every efsd for IDLE seconds with no client: process CPU
(utime+stime), RSS, and, when the build names it, the `efsd-query` thread's
CPU and context switches. Prints one JSON object; stores and logs live under
WORK and are deleted at the end (--keep keeps them).

  python3 tests/measure/a7_query_idle.py --bin TREE --work /tmp/a7q --port 19600 \\
      [--env NAME=VALUE ...] [--settle 30] [--idle 120] [--label L]

Never touches deployed clusters: loopback ports PORT..PORT+2 only, discovery
off, refuses ports 17432-17435 and 27432-27433."""
import argparse, json, os, re, shutil, signal, subprocess, sys, time
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--bin', required=True)
p.add_argument('--work', required=True)
p.add_argument('--port', type=int, default=19600)
p.add_argument('--env', action='append', default=[])
p.add_argument('--settle', type=float, default=30)
p.add_argument('--idle', type=float, default=120)
p.add_argument('--label', default='')
p.add_argument('--keep', action='store_true')
a = p.parse_args()
ports = [a.port + i for i in range(3)]
if any(17432 <= x <= 17435 or x in (27432, 27433) for x in ports):
    sys.exit('reserved port')
bin_dir, work = Path(a.bin).resolve(), Path(a.work)
if work.exists():
    sys.exit(f'{work} exists; give an empty path')
work.mkdir(parents=True)
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_')}
env.update(EFS_TRANSPORT='tcp')
for item in a.env:
    k, v = item.split('=', 1)
    env[k] = v
ticks = os.sysconf('SC_CLK_TCK')
procs = []


def start(i):
    ep = f'127.0.0.1:{ports[i]}'
    argv = [str(bin_dir / 'efsd')] + (['--join', f'127.0.0.1:{ports[0]}', '--listen', ep] if i else [ep])
    argv += [str(work / f'n{i + 1}'), '--quota', '1G', '--discovery-port', '0']
    log = open(work / f'n{i + 1}.log', 'ab')
    procs.append(subprocess.Popen(argv, env=env, stdout=log, stderr=subprocess.STDOUT))


def leaders():
    r = subprocess.run([str(bin_dir / 'efs-mgmt'), 'raft-status', f'127.0.0.1:{ports[0]}'],
                       capture_output=True, text=True, timeout=10, env=env)
    return re.findall(r'group (\d+) hosted=\d+ role=\S+ leader=(-?\d+)', r.stdout)


def cpu(pid, tid=None):
    path = f'/proc/{pid}/stat' if tid is None else f'/proc/{pid}/task/{tid}/stat'
    f = open(path).read().rsplit(')', 1)[1].split()
    return (int(f[11]) + int(f[12])) / ticks


def query_tid(pid):
    for t in os.listdir(f'/proc/{pid}/task'):
        try:
            if open(f'/proc/{pid}/task/{t}/comm').read().strip() == 'efsd-query':
                return t
        except OSError:
            pass
    return None


def switches(pid, tid):
    s = open(f'/proc/{pid}/task/{tid}/status').read()
    return sum(int(x) for x in re.findall(r'ctxt_switches:\s+(\d+)', s))


def rss_kib(pid):
    return int(re.search(r'VmRSS:\s+(\d+)', open(f'/proc/{pid}/status').read()).group(1))


result = {'label': a.label, 'bin': str(bin_dir), 'env': a.env, 'idle_s': a.idle, 'settle_s': a.settle}
try:
    start(0)
    time.sleep(1)
    start(1)
    start(2)
    end = time.time() + 120
    while time.time() < end:
        got = leaders() if all(x.poll() is None for x in procs) else None
        if got is not None and len(got) == 2 and all(int(l) >= 0 for _, l in got):
            break
        time.sleep(1)
    else:
        raise SystemExit('cluster did not form: ' + (work / 'n1.log').read_text()[-2000:])
    result['formed_after_s'] = round(120 - (end - time.time()), 1)
    time.sleep(a.settle)
    pids = [x.pid for x in procs]
    tids = {pid: query_tid(pid) for pid in pids}
    c0 = {pid: cpu(pid) for pid in pids}
    q0 = {pid: (cpu(pid, t), switches(pid, t)) for pid, t in tids.items() if t}
    t0 = time.time()
    time.sleep(a.idle)
    dt = time.time() - t0
    nodes = []
    for pid in pids:
        n = {'pid': pid, 'cpu_s': round(cpu(pid) - c0[pid], 3), 'rss_kib': rss_kib(pid)}
        n['cpu_pct'] = round(100 * n['cpu_s'] / dt, 3)
        if tids[pid]:
            c, w = q0[pid]
            n['query_thread_cpu_s'] = round(cpu(pid, tids[pid]) - c, 3)
            n['query_thread_switches_per_s'] = round((switches(pid, tids[pid]) - w) / dt, 2)
        nodes.append(n)
    result.update(measured_s=round(dt, 1), nodes=nodes,
                  total_cpu_pct=round(sum(n['cpu_pct'] for n in nodes), 3),
                  query_threads=sum(1 for t in tids.values() if t))
finally:
    for x in procs:
        if x.poll() is None:
            x.send_signal(signal.SIGTERM)
    for x in procs:
        try:
            x.wait(timeout=60)
        except subprocess.TimeoutExpired:
            x.kill()
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
print(json.dumps(result, indent=1))
