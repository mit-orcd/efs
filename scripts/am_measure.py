#!/usr/bin/env python3
"""Task AM reproduction: g_dgobj_mu cost inside REPORT.

A private 3-daemon loopback cluster of the production build plus the
uncommitted TEMP-AM instrumentation (dg lock count/wait, REPORT call timing
histogram, EFS_AM_SEED_DGOBJ seeding, SIGPROF dump). One mount per table
state; writer threads each loop create/write-4KiB/fsync/close so every fsync
drives a REPORT. States: empty table, one seeded pending object, dense seeded
table, and one member down during the load (real degraded marks).

Usage: python3 scripts/am_measure.py --src SRC --work DIR
         [--writers 1,8,32] [--seconds 20] [--port-base 19611]
The tree at SRC must carry the TEMP-AM instrumentation patch (the driver
refuses to draw conclusions without am-measure lines).
"""
import json, os, re, shutil, signal, subprocess, sys, threading, time
from pathlib import Path


def start_cluster(src, run, ports, down=()):
    procs = {}
    for i, port in enumerate(ports, 1):
        store = run / 'c' / f'n{i}'
        store.mkdir(parents=True, exist_ok=True)
        if i in down:
            continue
        args = [str(src / 'efsd')] + (['--join', f'127.0.0.1:{ports[0]}',
                                      '--listen', f'127.0.0.1:{port}'] if i > 1 else
                                      [f'127.0.0.1:{port}']) + [str(store)]
        procs[i] = subprocess.Popen(args, stdout=open(run / 'c' / f'n{i}.log', 'a'),
                                    stderr=subprocess.STDOUT)
    end = time.time() + 120
    while time.time() < end:
        ok = all('formed: 3 members' in (run / 'c' / f'n{i}.log').read_text(errors='replace')
                 for i in procs)
        if ok:
            return procs
        time.sleep(0.5)
    raise SystemExit('cluster never formed')


def mount(src, run, port, name, seed):
    mp = run / name
    mp.mkdir(exist_ok=True)
    log = open(run / f'{name}.log', 'a')
    env = dict(os.environ)
    if seed:
        env['EFS_AM_SEED_DGOBJ'] = str(seed)
    fuse = subprocess.Popen([str(src / 'efs-fuse'), f'127.0.0.1:{port}', str(mp), '-f'],
                            stdout=log, stderr=subprocess.STDOUT, env=env)
    end = time.time() + 30
    while time.time() < end and not os.path.ismount(mp):
        assert fuse.poll() is None, (run / f'{name}.log').read_text()[-1500:]
        time.sleep(0.2)
    return fuse, mp


def unmount(fuse, mp):
    subprocess.run(['fusermount3', '-u', str(mp)], capture_output=True)
    try:
        fuse.wait(timeout=20)
    except subprocess.TimeoutExpired:
        fuse.kill()
        fuse.wait(timeout=10)


def writers(mp, nwriters, seconds):
    """Each thread: create/write 4 KiB/fsync/close in its own dir."""
    stop = time.time() + seconds
    counts = [0] * nwriters
    errors = []

    def worker(w):
        d = mp / f'w{w:02d}'
        d.mkdir(exist_ok=True)
        k = 0
        while time.time() < stop:
            f = d / f'f{k:06d}'
            try:
                fd = os.open(f, os.O_CREAT | os.O_WRONLY, 0o644)
                os.write(fd, b'a' * 4096)
                os.fsync(fd)
                os.close(fd)
                counts[w] += 1
            except OSError as e:
                errors.append(f'w{w}: {e}')
                return
            k += 1

    threads = [threading.Thread(target=worker, args=(w,)) for w in range(nwriters)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return counts, errors


def dump_and_parse(fuse, log_path, since):
    os.kill(fuse.pid, signal.SIGPROF)
    time.sleep(0.7)
    txt = log_path.read_text(errors='replace')
    lines = [ln for ln in txt.splitlines()[since:] if 'am-measure' in ln]
    assert lines, f'no am-measure line in {log_path} (instrumentation missing?)'
    m = {}
    for kv in re.findall(r'(\w+)=(\d+)', lines[-1]):
        m[kv[0]] = int(kv[1])
    hist = {k: v for k, v in m.items() if re.fullmatch(r'h\d+', k)}
    return {k: v for k, v in m.items() if not re.fullmatch(r'h\d+', k)}, hist, len(txt.splitlines())


def pct(sorted_vals, q):
    if not sorted_vals:
        return 0
    return sorted_vals[min(len(sorted_vals) - 1, int(len(sorted_vals) * q))]


def main():
    src = Path(sys.argv[sys.argv.index('--src') + 1]).resolve()
    work = Path(sys.argv[sys.argv.index('--work') + 1]).resolve()
    wl = next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == '--writers'), '1,8,32')
    writers_list = [int(x) for x in wl.split(',')]
    secs = int(next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == '--seconds'), 20))
    pbase = int(next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == '--port-base'), 19611))

    run = work / 'run'
    shutil.rmtree(run, ignore_errors=True)
    (run / 'c').mkdir(parents=True)
    ports = [pbase, pbase + 1, pbase + 2]
    procs = start_cluster(src, run, ports)
    results = []
    try:
        states = [('empty', 0, False), ('seeded-1', 1, False),
                  ('seeded-8192', 8192, False), ('member-down', 0, True)]
        for name, seed, down in states:
            if down:
                procs[3].terminate()
                procs[3].wait(timeout=10)
                time.sleep(3)  # members notice the loss
            for w in writers_list:
                fuse, mp = mount(src, run, ports[0], f'mnt-{name}-{w}', seed)
                log_path = run / f'mnt-{name}-{w}.log'
                since = len(log_path.read_text(errors='replace').splitlines())
                t0 = time.time()
                counts, errors = writers(mp, w, secs)
                wall = time.time() - t0
                counters, hist, _ = dump_and_parse(fuse, log_path, since)
                unmount(fuse, mp)
                row = {'state': name, 'writers': w, 'seed': seed, 'member_down': down,
                       'wall_s': round(wall, 2), 'fsyncs': sum(counts),
                       'errors': errors[:3], 'counters': counters, 'hist': hist}
                results.append(row)
                print(f"[am] {name} w={w}: fsyncs={sum(counts)} "
                      f"dg_locks={counters.get('dg_locks')} dg_wait_ns={counters.get('dg_wait_ns')} "
                      f"report_calls={counters.get('report_calls')} report_ns={counters.get('report_ns')} "
                      f"notes={counters.get('dg_notes')} errors={len(errors)}", flush=True)
            if down:  # restart the member for the next state
                store = run / 'c' / 'n3'
                procs[3] = subprocess.Popen([str(src / 'efsd'), '--join', f'127.0.0.1:{ports[0]}',
                                             '--listen', f'127.0.0.1:{ports[2]}', str(store)],
                                            stdout=open(run / 'c' / 'n3.log', 'a'),
                                            stderr=subprocess.STDOUT)
                time.sleep(5)
        (work / 'am-summary.json').write_text(json.dumps(results, indent=1))
    finally:
        for p in procs.values():
            p.terminate()
        time.sleep(2)
    print('[am] done; summary at', work / 'am-summary.json')


if __name__ == '__main__':
    main()
