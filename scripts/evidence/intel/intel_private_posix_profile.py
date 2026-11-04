#!/usr/bin/env python3
"""Profile an existing owned private Intel fixture; never restart/mount services.

Run locally with --output OUTSIDE_GIT. Both hosts need perf and strace and
permission to trace their own processes. Existing private diagnostic mounts
must be present (4 on Intel1, 3 on Intel2). Raw recordings stay on the hosts;
only small result/manifest files are fetched. strace windows perturb timing:
this diagnostic run is not an uninstrumented acceptance result.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import threading
import time


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()


def stop_profiler(p):
    if p.poll() is None:
        p.send_signal(signal.SIGINT)
    try:
        return p.wait(timeout=20)
    except subprocess.TimeoutExpired:
        p.terminate()
        return p.wait(timeout=10)


def remote(a):
    root = Path('/data1/efs/w100-private')
    runtime = root / 'runtime-deadline-posix'
    source = Path(a.source_root)
    assert source.parent == root
    out = Path(a.remote_output)
    assert out.parent == root / 'results'
    out.mkdir(exist_ok=False)
    nclients = 4 if a.host == 'intel1_efs' else 3
    mounts = [runtime / 'mnt'] + [runtime / 'diag-mounts' / ('client-%d' % i)
                                for i in range(1, nclients)]
    targets = []
    for d in Path('/proc').iterdir():
        if not d.name.isdigit():
            continue
        try:
            args = [x.decode() for x in (d / 'cmdline').read_bytes().split(b'\0') if x]
            if not args:
                continue
            kind = Path(args[0]).name
            if kind == 'efs-fuse' and args[-1] in list(map(str, mounts)):
                label = 'client-%d' % list(map(str, mounts)).index(args[-1])
                assert ':27432' in args[1]
            elif kind == 'efsd' and '--storage' in args and 'w100-private-deadline-posix/' in args[args.index('--storage') + 1]:
                label = 'node-' + args[args.index('--node-id') + 1]
                assert args[args.index('--port') + 1] in ('27432', '27433')
                assert '--direct-io' in args
                assert args[args.index('--quota') + 1] == '16G'
            else:
                continue
            assert d.stat().st_uid == os.getuid()
            exe = os.readlink(d / 'exe')
            expected_source = source if kind == 'efs-fuse' else root / 'devel-rpcdeadline'
            assert exe.startswith(str(expected_source) + '/')
            version = subprocess.check_output([str(d / 'exe'), '--version'], text=True).strip()
            if kind == 'efs-fuse':
                assert a.expected_commit in version, version
            env = dict(x.decode().split('=', 1) for x in (d / 'environ').read_bytes().split(b'\0') if b'=' in x)
            keys = ['EFS_TRANSPORT', 'EFS_COMMANDS', 'EFS_COMMANDS_SERVER', 'EFS_EXPIRY']
            assert env.get('EFS_TRANSPORT') == 'tcp'
            for key in keys[1:]:
                assert env.get(key) == '0', (label, key, env.get(key))
            targets.append(dict(label=label, pid=int(d.name), args=args, exe=exe,
                                sha256=digest(d / 'exe'), version=version,
                                features={k: env.get(k) for k in keys}))
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    assert len(targets) == nclients + 2, targets
    assert len(set(t['label'] for t in targets)) == nclients + 2
    for mount in mounts:
        assert os.path.ismount(mount)
        assert not (mount / '.efs').exists()
    pids = ','.join(str(t['pid']) for t in targets)
    (out / 'manifest.json').write_text(json.dumps(dict(host=a.host, targets=targets,
        clock=dict(wall=time.time(), monotonic=time.monotonic()),
        start_at=a.start_at, windows=[10, 40, 70], window_seconds=8), indent=2) + '\n')
    # Prove perf access before starting the workload.
    probe = subprocess.run(['perf', 'record', '-q', '-e', 'cpu-clock', '-F', '49',
        '--call-graph', 'dwarf,8192', '-p', pids, '-o', str(out / 'perf-probe.data'),
        '--', 'sleep', '0.25'], capture_output=True, text=True)
    (out / 'perf-probe.txt').write_text(probe.stderr)
    assert probe.returncode == 0, probe.stderr
    logs = {p: p.stat().st_size for p in list(runtime.glob('*.log')) +
            list((runtime / 'diag-mounts').glob('*.log'))}
    profs, files = [], []
    done = threading.Event()
    strace_rows = []
    started = time.monotonic()

    def sample_resources():
        with (out / 'resources.jsonl').open('w') as f:
            while not done.is_set():
                row = dict(wall=time.time(), monotonic=time.monotonic(), processes=[])
                row['cpu'] = Path('/proc/stat').read_text().splitlines()[0]
                for t in targets:
                    p = Path('/proc') / str(t['pid'])
                    stat = (p/'stat').read_text().rsplit(')', 1)[1].split()
                    row['processes'].append(dict(label=t['label'], pid=t['pid'],
                        utime=int(stat[11]), stime=int(stat[12]),
                        io=(p/'io').read_text(), status=(p/'status').read_text()))
                f.write(json.dumps(row)+'\n'); f.flush()
                done.wait(1)

    def trace_windows():
        for offset in (10, 40, 70):
            delay = a.start_at + offset - time.time()
            if done.wait(max(0, delay)):
                break
            launched = []
            for t in targets:
                err = open(out / ('strace-%s-%d.stderr' % (t['label'], offset)), 'w')
                cmd = ['strace', '-f', '-qq', '-ttt', '-T', '-yy', '-s', '0',
                       '-e', 'trace=futex,fdatasync,fsync,poll,ppoll,epoll_wait,epoll_pwait,read,write,pread64,pwrite64,recvfrom,sendto,recvmsg,sendmsg',
                       '-o', str(out / ('strace-%s-%d.log' % (t['label'], offset))),
                       '-p', str(t['pid'])]
                launched.append((t, subprocess.Popen(cmd, stdout=err, stderr=err), err, cmd))
            done.wait(8)
            for t, p, err, cmd in launched:
                rc = stop_profiler(p)
                err.close()
                strace_rows.append(dict(label=t['label'], offset=offset, rc=rc, command=cmd))

    try:
        for name, cmd in [
            ('record', ['perf', 'record', '-q', '-e', 'cpu-clock', '-F', '49', '--call-graph', 'dwarf,8192', '-p', pids, '-o', str(out / 'perf.data')]),
            ('stat', ['perf', 'stat', '-e', 'task-clock,context-switches,cpu-migrations,page-faults', '-p', pids, '-o', str(out / 'perf-stat.txt')])]:
            f = open(out / ('perf-%s.stderr' % name), 'w'); files.append(f)
            profs.append(subprocess.Popen(cmd, stdout=f, stderr=f))
        worker = threading.Thread(target=trace_windows)
        worker.start()
        sampler = threading.Thread(target=sample_resources)
        sampler.start()
        time.sleep(max(0, a.start_at - time.time()))
        assert all(p.poll() is None for p in profs), 'profiler failed before workload'

        def suite(pair):
            i, mount = pair
            tag = 'profile-%s-%d' % (a.host, i)
            cmd = ['timeout', '--kill-after=10s', '900s', 'python3', str(source / 'tests/posix/posix_suite.py'),
                   str(mount), '--suite', 'core', '--jobs', '16', '--timeout-s', '30',
                   '--tag', tag, '--results', str(out / (tag + '.tsv'))]
            begin = time.time()
            with open(out / (tag + '.log'), 'w') as f:
                r = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=930)
            row = dict(client=i, command=cmd, rc=r.returncode, begin=begin, end=time.time())
            print(json.dumps(row), flush=True)
            return row
        with concurrent.futures.ThreadPoolExecutor(nclients) as pool:
            rows = list(pool.map(suite, enumerate(mounts)))
    finally:
        done.set()
        if 'worker' in locals():
            worker.join(timeout=40)
        if 'sampler' in locals():
            sampler.join(timeout=5)
        perf_rc = [stop_profiler(p) for p in profs]
        for f in files:
            f.close()
        (out / 'traces.json').write_text(json.dumps(strace_rows, indent=2) + '\n')
        for path, offset in logs.items():
            with path.open('rb') as f:
                f.seek(offset)
                with (out / ('runtime-' + str(path.relative_to(runtime)).replace('/', '__'))).open('wb') as dest:
                    for b in iter(lambda: f.read(1024 * 1024), b''):
                        dest.write(b)
    (out / 'summary.json').write_text(json.dumps(dict(host=a.host, suites=rows,
        elapsed=time.monotonic()-started, perf_rc=perf_rc), indent=2) + '\n')
    for cmd, filename in [
        (['perf', 'report', '--stdio', '-i', str(out/'perf.data'), '--percent-limit', '0.5', '--sort', 'comm,dso,symbol'], 'perf-report.txt'),
        (['perf', 'report', '--stdio', '-i', str(out/'perf.data'), '--no-children', '--call-graph', 'none', '--percent-limit', '0.3', '--sort', 'comm,symbol'], 'perf-flat.txt')]:
        with open(out/filename, 'w') as f:
            subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=120)
    print(json.dumps(dict(host=a.host, output=str(out), complete=True)), flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--remote', action='store_true')
    p.add_argument('--host', choices=['intel1_efs', 'intel2_efs'])
    p.add_argument('--remote-output')
    p.add_argument('--start-at', type=float)
    p.add_argument('--output', type=Path)
    p.add_argument('--source-root', default='/data1/efs/w100-private/devel-profile-37ff1b4d')
    p.add_argument('--expected-commit', default='37ff1b4d')
    a = p.parse_args()
    if a.remote:
        return remote(a)
    assert a.output
    a.output.mkdir(parents=True, exist_ok=False)
    run = 'profile-seven-' + time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())
    remote_out = '/data1/efs/w100-private/results/' + run
    for host in ('intel1_efs', 'intel2_efs'):
        subprocess.run(['scp', __file__, host+':/data1/efs/w100-private/'+run+'.py'], check=True)
    start = time.time() + 12
    def one(host):
        cmd = ['python3', '/data1/efs/w100-private/'+run+'.py', '--remote', '--host', host,
               '--remote-output', remote_out, '--start-at', str(start),
               '--source-root', a.source_root, '--expected-commit', a.expected_commit]
        with (a.output/(host+'.log')).open('w') as f:
            r = subprocess.run(['ssh', host, shlex.join(cmd)], stdout=f, stderr=subprocess.STDOUT, timeout=1200)
        dest = a.output/host; dest.mkdir()
        for pattern in ('*.tsv', '*summary.json', '*manifest.json', '*traces.json', 'perf-*.txt', '*.stderr'):
            subprocess.run(['scp', host+':'+remote_out+'/'+pattern, str(dest)], check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return dict(host=host, rc=r.returncode, remote_output=remote_out)
    with concurrent.futures.ThreadPoolExecutor(2) as pool:
        rows = list(pool.map(one, ('intel1_efs', 'intel2_efs')))
    (a.output/'run.json').write_text(json.dumps(rows, indent=2)+'\n')
    print(json.dumps(rows, indent=2))
    return int(any(r['rc'] for r in rows))


if __name__ == '__main__':
    raise SystemExit(main())
