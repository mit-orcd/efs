#!/usr/bin/env python3
"""Task AK reproduction: the cold packed-read cost per read.

A private 3-daemon loopback cluster of the production efsd build (never
the fault build: its hook checks would skew the hot path), one warm mount
to create many small files (1-8 KiB), the packer at EFS_PACK_MIN_AGE_MS=500
so they pack, then cold passes through a fresh mount: every file is opened,
read and verified, each read timed; the FUSE client's CPU comes from
/proc/<pid>/stat utime+stime, and perf record samples it mid-pass for the
efs_hash/memcpy share. EFS_TRANSPORT/EFS_RDMA_DEV come from the caller (the
RDMA column runs the same script on node9901).

Usage: python3 scripts/ak_measure.py --src SRC --work DIR [--files 2000] [--seconds 40]
         [--bind 127.0.0.1] [--port-base 19421]
RDMA cannot hairpin loopback: on RoCE hosts use --bind <node-IP-on-HCA>.
"""
import hashlib, json, os, re, shutil, signal, subprocess, sys, time
from pathlib import Path


def content(name, size):
    out, k = b'', 0
    while len(out) < size:
        out += hashlib.sha256(f'{name}:{k}'.encode()).digest()
        k += 1
    return out[:size]


def proc_cpu_s(pid):
    with open(f'/proc/{pid}/stat') as f:
        parts = f.read().split()
    return (int(parts[13]) + int(parts[14])) / os.sysconf('SC_CLK_TCK')


def main():
    src = Path(sys.argv[sys.argv.index('--src') + 1]).resolve()
    work = Path(sys.argv[sys.argv.index('--work') + 1]).resolve()
    nfiles = int(next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == '--files'), 2000))
    secs = int(next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == '--seconds'), 40))
    # RDMA cannot hairpin loopback: on RoCE hosts pass --bind 10.1.99.1 (the
    # node's own IP on the HCA's interface) so the conn migrates to mlx5_0.
    bind = next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == '--bind'), '127.0.0.1')
    pbase = int(next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == '--port-base'), 19421))

    run = work / 'run'
    shutil.rmtree(run, ignore_errors=True)
    (run / 'c').mkdir(parents=True)
    ports = [pbase, pbase + 1, pbase + 2]
    procs = {}
    denv = dict(os.environ, EFS_PACK_MIN_AGE_MS='500')
    for i, port in enumerate(ports, 1):
        store = run / 'c' / f'n{i}'
        store.mkdir()
        args = [str(src / 'efsd')] + (['--join', f'{bind}:{ports[0]}',
                                      '--listen', f'{bind}:{port}'] if i > 1 else
                                      [f'{bind}:{port}']) + [str(store)]
        procs[i] = subprocess.Popen(args, stdout=open(run / 'c' / f'n{i}.log', 'w'),
                                    stderr=subprocess.STDOUT, env=denv)
    end = time.time() + 120
    while time.time() < end:
        if all('formed: 3 members' in (run / 'c' / f'n{i}.log').read_text(errors='replace')
               for i in (1, 2, 3)):
            break
        time.sleep(0.5)
    else:
        raise SystemExit('cluster never formed')

    fuse = None
    mp = run / 'mnt'
    try:
        # warm: create the files
        mp.mkdir()
        fuse = subprocess.Popen([str(src / 'efs-fuse'), f'{bind}:{ports[0]}', str(mp), '-f'],
                                stdout=open(run / 'mnt-warm.log', 'w'), stderr=subprocess.STDOUT)
        end = time.time() + 30
        while time.time() < end and not os.path.ismount(mp):
            assert fuse.poll() is None, (run / 'mnt-warm.log').read_text()[-1500:]
            time.sleep(0.2)
        manifest = {}
        sizes = []
        for k in range(nfiles):
            sz = 1 + (k * 4079) % 8192
            f = mp / f'f{k:05d}'
            body = content(str(f), sz)
            f.write_bytes(body)
            manifest[f.name] = (sz, hashlib.sha256(body).hexdigest())
            sizes.append(sz)
        # wait for the packer to cover them (the leader is not always n1:
        # watch every node's log)
        end = time.time() + 120
        while time.time() < end:
            seen = False
            for i in (1, 2, 3):
                txt = (run / 'c' / f'n{i}.log').read_text(errors='replace')
                m = re.findall(r'packed_total=(\d+)', txt)
                if m and int(m[-1]) > 0 and 'backlog=0 ' in txt.split('packer:')[-1]:
                    seen = True
                    break
            if seen:
                break
            time.sleep(1)
        packed_line = ''
        for i in (1, 2, 3):
            for ln in reversed((run / 'c' / f'n{i}.log').read_text(errors='replace').splitlines()):
                if 'packer:' in ln and 'packed_total=' in ln:
                    packed_line = ln.strip()
                    break
            if packed_line:
                break
        print('[ak] packer:', packed_line[:160], flush=True)
        total_bytes = sum(sizes)
        print(f'[ak] {nfiles} files, {total_bytes} bytes, packed; starting cold passes', flush=True)

        # cold passes until the duration, measuring the last window
        lat = []
        cpu0 = 0.0
        bytes0 = 0
        t_end = time.time() + secs
        perf = None
        passes = 0
        mp2 = run / 'mnt-cold'
        mp2.mkdir(exist_ok=True)
        while time.time() < t_end:
            # a fresh mount per pass = a cold client: unmount whichever mount
            # is live, wait for its process to exit, then mount fresh
            for m in (mp, mp2):
                if os.path.ismount(m):
                    subprocess.run(['fusermount3', '-u', str(m)], capture_output=True)
            if fuse:
                end2 = time.time() + 30
                while time.time() < end2 and fuse.poll() is None:
                    time.sleep(0.2)
                if fuse.poll() is None:
                    subprocess.run(['fusermount3', '-uz', str(mp2)], capture_output=True)
                    fuse.kill()
                    fuse.wait(timeout=10)
            (run / 'mnt-cold.log').unlink(missing_ok=True)
            fuse = subprocess.Popen([str(src / 'efs-fuse'), f'{bind}:{ports[0]}', str(mp2), '-f'],
                                    stdout=open(run / 'mnt-cold.log', 'a'), stderr=subprocess.STDOUT)
            end = time.time() + 30
            while time.time() < end and not os.path.ismount(mp2):
                assert fuse.poll() is None, (run / 'mnt-cold.log').read_text()[-1500:]
                time.sleep(0.2)
            passes += 1
            if passes >= 2 and perf is None:
                # pure cold reads: the warm phase and its upload hashing are over
                perf = subprocess.Popen(['perf', 'record', '-F', '1999', '-p', str(fuse.pid),
                                         '-o', str(work / 'perf-cold.data'), '--', 'sleep', '20'],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                cpu0 = proc_cpu_s(fuse.pid)
                bytes0 = 0
            for name, (sz, want) in manifest.items():
                p = mp2 / name
                t0 = time.perf_counter_ns()
                got = p.read_bytes()
                lat.append((time.perf_counter_ns() - t0) / 1000)
                if hashlib.sha256(got).hexdigest() != want:
                    print(f'[ak] MISMATCH {name}', flush=True)
                    raise SystemExit(1)
                if perf is not None:
                    bytes0 += sz
            if perf is not None and perf.poll() is not None:
                cpu1 = proc_cpu_s(fuse.pid)
                print(f'[ak] mid-pass window: {bytes0} bytes read, '
                      f'client CPU {cpu1 - cpu0:.2f} s '
                      f'({(cpu1 - cpu0) / (bytes0 / (1 << 30)):.2f} s/GiB)', flush=True)
                perf = None
        lat.sort()
        p50 = lat[len(lat) // 2]
        p99 = lat[int(len(lat) * 0.99)]
        print(f'[ak] cold reads: {len(lat)} reads, {total_bytes} B per pass, '
              f'p50={p50:.0f}us p99={p99:.0f}us', flush=True)
        (work / 'ak-summary.json').write_text(json.dumps({
            'files': nfiles, 'bytes_per_pass': total_bytes, 'reads': len(lat),
            'p50_us': p50, 'p99_us': p99,
        }, indent=1))
    finally:
        for m in (run / 'mnt', run / 'mnt-cold'):
            subprocess.run(['fusermount3', '-u', str(m)], capture_output=True)
        if fuse:
            fuse.kill()
        for p in procs.values():
            p.terminate()
        time.sleep(2)
    print('[ak] done; perf data at', work / 'perf-cold.data')


if __name__ == '__main__':
    main()
