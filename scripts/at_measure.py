#!/usr/bin/env python3
"""Task AT reproduction: put_win_cv broadcast cost per satisfied waiter.

A private 3-daemon loopback cluster of the production build plus the
uncommitted TEMP-AT instrumentation (open/close/wait/wake counters,
iteration and duration histograms, SIGPROF dump). One mount; writer
threads each loop create/write-1MiB/fsync/close so every fsync waits for
its inode's in-flight PUT windows. Phases: 1, 8 and 32 independent-file
writers.

Usage: python3 scripts/at_measure.py --src SRC --work DIR
         [--writers 1,8,32] [--seconds 20] [--port-base 19741]
The tree at SRC must carry the TEMP-AT instrumentation patch.
"""
import json, os, re, shutil, signal, subprocess, sys, threading, time
from pathlib import Path


def start_cluster(src, run, ports):
    procs = {}
    for i, port in enumerate(ports, 1):
        store = run / 'c' / f'n{i}'
        store.mkdir(parents=True, exist_ok=True)
        args = [str(src / 'efsd')] + (['--join', f'127.0.0.1:{ports[0]}',
                                      '--listen', f'127.0.0.1:{port}'] if i > 1 else
                                      [f'127.0.0.1:{port}']) + [str(store)]
        procs[i] = subprocess.Popen(args, stdout=open(run / 'c' / f'n{i}.log', 'a'),
                                    stderr=subprocess.STDOUT)
    end = time.time() + 120
    while time.time() < end:
        if all('formed: 3 members' in (run / 'c' / f'n{i}.log').read_text(errors='replace')
               for i in procs):
            return procs
        time.sleep(0.5)
    raise SystemExit('cluster never formed')


def writers(mp, nwriters, seconds):
    stop = time.time() + seconds
    counts = [0] * nwriters
    errors = []
    lat = [[] for _ in range(nwriters)]

    def worker(w):
        d = mp / f'w{w:02d}'
        d.mkdir(exist_ok=True)
        k = 0
        while time.time() < stop:
            f = d / f'f{k:06d}'
            try:
                fd = os.open(f, os.O_CREAT | os.O_WRONLY, 0o644)
                os.write(fd, b'a' * (1 << 20))
                t0 = time.monotonic()
                os.fsync(fd)
                lat[w].append(time.monotonic() - t0)
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
    all_lat = sorted(x for xs in lat for x in xs)
    return counts, errors, all_lat


def main():
    src = Path(sys.argv[sys.argv.index('--src') + 1]).resolve()
    work = Path(sys.argv[sys.argv.index('--work') + 1]).resolve()
    wl = next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == '--writers'), '1,8,32')
    writers_list = [int(x) for x in wl.split(',')]
    secs = int(next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == '--seconds'), 20))
    pbase = int(next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == '--port-base'), 19741))

    run = work / 'run'
    shutil.rmtree(run, ignore_errors=True)
    (run / 'c').mkdir(parents=True)
    ports = [pbase, pbase + 1, pbase + 2]
    procs = start_cluster(src, run, ports)
    results = []
    fuse = None
    try:
        mp = run / 'mnt'
        mp.mkdir()
        for w in writers_list:
            log = open(run / 'mnt.log', 'a')
            fuse = subprocess.Popen([str(src / 'efs-fuse'), f'127.0.0.1:{ports[0]}', str(mp), '-f'],
                                    stdout=log, stderr=subprocess.STDOUT)
            end = time.time() + 30
            while time.time() < end and not os.path.ismount(mp):
                assert fuse.poll() is None, (run / 'mnt.log').read_text()[-1500:]
                time.sleep(0.2)
            counts, errors, lat = writers(mp, w, secs)
            os.kill(fuse.pid, signal.SIGPROF)
            time.sleep(0.7)
            lines = [ln for ln in (run / 'mnt.log').read_text(errors='replace').splitlines()
                     if 'at-measure' in ln]
            assert lines, 'no at-measure line (instrumentation missing?)'
            m = dict((k, int(v)) for k, v in re.findall(r'(\w+)=(\d+)', lines[-1]))
            subprocess.run(['fusermount3', '-u', str(mp)], capture_output=True)
            try:
                fuse.wait(timeout=20)
            except subprocess.TimeoutExpired:
                fuse.kill()
                fuse.wait(timeout=10)
            n = len(lat)
            row = {'writers': w, 'fsyncs': sum(counts), 'errors': errors[:3],
                   'p50_ms': lat[n // 2] * 1000 if n else 0,
                   'p95_ms': lat[int(n * 0.95)] * 1000 if n else 0,
                   'p99_ms': lat[int(n * 0.99)] * 1000 if n else 0,
                   'counters': m}
            results.append(row)
            print(f"[at] w={w}: fsyncs={sum(counts)} p50={row['p50_ms']:.1f}ms "
                  f"p95={row['p95_ms']:.1f}ms p99={row['p99_ms']:.1f}ms "
                  f"wakes={m.get('wakes')} sat={m.get('sat')} locks={m.get('locks')} "
                  f"open={m.get('open')} close={m.get('close')} errors={len(errors)}", flush=True)
        (work / 'at-summary.json').write_text(json.dumps(results, indent=1))
    finally:
        if fuse and fuse.poll() is None:
            subprocess.run(['fusermount3', '-u', str(run / 'mnt')], capture_output=True)
            fuse.kill()
        for p in procs.values():
            p.terminate()
        time.sleep(2)
    print('[at] done; summary at', work / 'at-summary.json')


if __name__ == '__main__':
    main()
