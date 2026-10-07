#!/usr/bin/env python3
"""Exercise benchmark command ownership, parsing and scratch-root protection."""
import argparse
import os
import sys
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(binary, *args, ok=True, timeout=10):
    p = subprocess.run([str(ROOT / binary), *args], text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=timeout)
    assert (p.returncode == 0) == ok, (args, p.returncode, p.stdout)
    return p.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--smoke', action='store_true', help='also run real local backends')
    args = parser.parse_args()
    assert '--bench' not in run('efsd', '--help')
    assert '--bench data' in run('efs-bench', '--help')
    assert 'no cluster' in run('efs-bench', '--bench', 'data', '--help')
    run('efsd', '--bench', 'meta', ok=False)
    assert 'BENCH_OK kind=blake3' in run('efs-bench', '--bench', 'blake3', '--threads', '1',
                                        '--size', '1000', '--time', '.03', '--oneshot')
    for value in ['nan', 'inf', '1x', '0']:
        run('efs-bench', '--bench', 'blake3', '--time', value, ok=False)
    run('efs-bench', '--bench', 'blake3', '--threads', '1x', ok=False)
    run('efs-bench', '--bench', 'blake3', '--size', '18446744073709551615M', ok=False)
    with tempfile.TemporaryDirectory(prefix='efs-bench-cli-') as root:
        sentinel = pathlib.Path(root) / 'keep'
        sentinel.write_text('preserve me')
        for kind, option in [('data', '--storage'), ('meta', '--meta-storage')]:
            out = run('efs-bench', '--bench', kind, option, root, '--time', '.01', ok=False)
            assert 'scratch' in out
            assert sentinel.read_text() == 'preserve me'
        for bad in ['nan', 'inf', '0', '-1', '1x', '']:
            run('efs-bench', '--bench', 'meta', '--meta-storage', root,
                '--time', bad, ok=False)
        for bad in ['garbage', '-1', '1x']:
            run('efs-bench', '--bench', 'data', '--storage', root,
                '--writers', bad, ok=False)
        run('efs-bench', '--bench', 'data', '--storage', '', ok=False)
        for qd in ['0', '257', 'nan']:
            run('efs-bench', '--bench', 'data', '--storage', root, '--qd', qd, ok=False)
        run('efs-bench', '--bench', 'meta', '--storage', root, '--qd', '1', ok=False)
        run('efs-bench', '--bench', 'meta', '--storage', root, '--window', '1', ok=False)
        for window in ['0', '-1', '4294967295', '1x']:
            run('efs-bench', '--bench', 'data', '--storage', root, '--window', window, ok=False)
        run('efs-bench', '--bench', 'other', '--storage', root, ok=False)
        run('efs-bench', '--bench', 'meta', '--store', '--storage', root, ok=False)
        run('efs-bench', '127.0.0.1:1', '--bench', 'meta', '--storage', root, ok=False)
        run('efs-bench', '--bench', 'data', '--storage', root, '--bench', 'meta', ok=False)
        assert list(pathlib.Path(root).iterdir()) == [sentinel]
    with tempfile.TemporaryDirectory(prefix='efs-bench-window-') as root:
        out = run('efs-bench', '--bench', 'data', '--storage', root, '--time', '.05',
                  '--writers', '0', '--qd', '1', '--window', '1', '--skip-ceiling')
        rows = [dict(re.findall(r'(\w+)=([^\s]+)', line))
                for line in out.splitlines() if line.startswith('BENCH_OK ')]
        assert len(rows) == 2 and all(int(row['ops']) > 1 and row['errors'] == '0' for row in rows), out
    for kind in ['io', 'io-blake3']:
        for rw in ['read', 'write']:
            with tempfile.TemporaryDirectory(prefix='efs-raw-io-') as root:
                out = run('efs-bench', '--bench', kind, '--storage', root, '--rw', rw,
                          '--io-size', '4K', '--qd', '2', '--window', '1', '--time', '.05')
                row = dict(re.findall(r'(\w+)=([^\s]+)', next(line for line in out.splitlines() if line.startswith('BENCH_OK '))))
                assert row['checksum'] == ('none' if kind == 'io' else 'blake3')
                assert row['rw'] == rw and row['errors'] == '0' and row['verified_blocks'] == '2', out
                assert row['idle_workers'] == '0' and int(row['min_worker_ops']) > 0, out
                assert row['latency'] == 'operation_cycle', out
                assert not list(pathlib.Path(root).iterdir())
    # Preallocation/population happen before timing; each root's caller data survives.
    for kind in ['io', 'io-blake3']:
        with tempfile.TemporaryDirectory(prefix='efs-preallocated-a-') as a, tempfile.TemporaryDirectory(prefix='efs-preallocated-b-') as b:
            out = run('efs-bench', '--bench', kind, '--storage', a, '--storage', b,
                      '--rw', 'write', '--preallocate', '--io-size', '4K', '--qd', '2', '--window', '3', '--time', '.05')
            row = dict(re.findall(r'(\w+)=([^\s]+)', next(line for line in out.splitlines() if line.startswith('BENCH_OK '))))
            assert row['allocation'] == 'overwrite_preallocated' and row['prepared_blocks'] == '6', out
            assert row['paths'] == '2' and row['verified_blocks'] == '6' and row['errors'] == '0', out
            assert not list(pathlib.Path(a).iterdir()) and not list(pathlib.Path(b).iterdir())
            run('efs-bench', '--bench', kind, '--storage', a, '--rw', 'read', '--preallocate', ok=False)
            run('efs-bench', '--bench', kind, '--storage', a, '--storage', b, '--qd', '1', ok=False)
    with tempfile.TemporaryDirectory(prefix='efs-engine-a-') as a, tempfile.TemporaryDirectory(prefix='efs-engine-b-') as b:
        out = run('efs-bench', '--bench', 'data', '--storage', a, '--storage', b,
                  '--qd', '2', '--window', '1', '--time', '.05', '--writers', '2', '--skip-ceiling')
        rows = [dict(re.findall(r'(\w+)=([^\s]+)', line)) for line in out.splitlines() if line.startswith('BENCH_OK ')]
        assert {(row['paths'], row['rw']) for row in rows} == {('1', 'write'), ('1', 'read'), ('2', 'write'), ('2', 'read')}, out
        assert all(row['errors'] == '0' for row in rows), out
    # Oversubscribe the CPU without spinning at the all-ready start gate.
    with tempfile.TemporaryDirectory(prefix='efs-io-many-workers-') as root:
        out = run('efs-bench', '--bench', 'io', '--storage', root, '--rw', 'write',
                  '--io-size', '4K', '--qd', '256', '--window', '1', '--time', '1')
        row = dict(re.findall(r'(\w+)=([^\s]+)', next(line for line in out.splitlines() if line.startswith('BENCH_OK '))))
        assert row['idle_workers'] == '0' and row['verified_blocks'] == '256', out
        assert int(row['min_worker_ops']) > 0 and not list(pathlib.Path(root).iterdir()), out
        assert float(row['max_start_us']) >= 0, out
        assert row['allocation'] == 'allocate_then_overwrite', out
        assert int(row['allocation_ops']) == 256, out
        assert int(row['allocation_ops']) + int(row['overwrite_ops']) == int(row['ops']), out
    with tempfile.TemporaryDirectory(prefix='efs-io-refusal-') as root:
        keep = pathlib.Path(root) / 'keep'; keep.write_text('keep')
        run('efs-bench', '--bench', 'io', '--storage', root, ok=False)
        for size in ['nan', '1Mx', '8193', '-1', '999999999999999999999M']:
            run('efs-bench', '--bench', 'io', '--storage', root, '--io-size', size, ok=False)
        run('efs-bench', '--bench', 'io', '--storage', root, '--rw', 'read', '--sync', ok=False)
        assert keep.read_text() == 'keep'
    for rw in ['read', 'write']:
        for nw in ['0', '2']:
            with tempfile.TemporaryDirectory(prefix='efs-engine-phase-') as root:
                out = run('efs-bench', '--bench', 'data', '--storage', root,
                          '--qd', '2', '--window', '2', '--time', '.1',
                          '--writers', nw, '--rw', rw, '--full-paths',
                          '--writer-stats', '--skip-ceiling')
                rows = [dict(re.findall(r'(\w+)=([^\s]+)', line)) for line in out.splitlines()
                        if line.startswith('BENCH_OK ')]
                assert len(rows) == 1 and rows[0]['rw'] == rw, out
                assert rows[0]['idle_workers'] == '0', out
                waits = [dict(re.findall(r'(\w+)=([^\s]+)', line)) for line in out.splitlines()
                         if line.startswith('BENCH_WAIT ')]
                if rw == 'write':
                    assert len(waits) == 1 and waits[0]['jobs'] == rows[0]['ops'], out
                    assert waits[0]['queued'] == ('0' if nw == '0' else rows[0]['ops']), out
                    assert int(waits[0]['peak_active']) <= 2, out
                else:
                    assert not waits, out
    with tempfile.TemporaryDirectory(prefix='efs-engine-roots-a-') as a, tempfile.TemporaryDirectory(prefix='efs-engine-roots-b-') as b:
        out = run('efs-bench', '--bench', 'data', '--storage', a, '--storage', b,
                  '--qd', '2', '--window', '32', '--time', '.1', '--writers', '0',
                  '--rw', 'write', '--full-paths', '--skip-ceiling')
        roots = [dict(re.findall(r'(\w+)=([^\s]+)', line)) for line in out.splitlines()
                 if line.startswith('BENCH_ROOT ')]
        assert len(roots) == 2 and all(int(r['writes']) > 0 for r in roots), out
        assert sum(int(r['writes']) for r in roots) == int(dict(re.findall(r'(\w+)=([^\s]+)', next(line for line in out.splitlines() if line.startswith('BENCH_OK '))))['ops']), out
    with tempfile.TemporaryDirectory(prefix='efs-engine-invalid-') as root:
        run('efs-bench', '--bench', 'data', '--storage', root, '--rw', 'read', ok=False)
        run('efs-bench', '--bench', 'meta', '--storage', root, '--rw', 'write', ok=False)
        run('efs-bench', '--bench', 'data', '--storage', root, '--rw', 'bad', ok=False)
    if args.smoke:
        for kind, option in [('meta', '--meta-storage'), ('data', '--storage')]:
            with tempfile.TemporaryDirectory(prefix='efs-bench-smoke-') as root:
                opts = ['--writers', '2', '--qd', '2', '--window', '2', '--skip-ceiling'] if kind == 'data' else ['--skip-ceiling']
                out = run('efs-bench', '--bench', kind, option, root,
                          '--time', '.03', *opts, timeout=180)
                assert f'BENCH_OK kind={kind}' in out, out
                print(out, end='')
    if args.smoke and sys.platform.startswith('linux'):
        # Fail actual backend durability calls, rather than mocking the CLI.
        with tempfile.TemporaryDirectory(prefix='efs-bench-fault-') as tmp:
            source = pathlib.Path(tmp) / 'fault.c'
            library = pathlib.Path(tmp) / 'fault.so'
            source.write_text('#include <errno.h>\nint fsync(int fd) { (void)fd; errno=EIO; return -1; }\nint fdatasync(int fd) { return fsync(fd); }\n')
            subprocess.run(['cc', '-shared', '-fPIC', str(source), '-o', str(library)], check=True)
            root = pathlib.Path(tmp) / 'scratch'
            env = dict(os.environ, LD_PRELOAD=str(library))
            result = subprocess.run([str(ROOT / 'efs-bench'), '--bench', 'meta',
                                     '--meta-storage', str(root), '--time', '.03',
                                     '--skip-ceiling'], env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
            assert result.returncode != 0, result.stdout
            assert 'BENCH_FAIL kind=meta' in result.stdout, result.stdout
            print('metadata durability fault: rejected PASS')
    print('benchmark CLI: ownership, parsing and scratch protection PASS')


if __name__ == '__main__':
    main()
