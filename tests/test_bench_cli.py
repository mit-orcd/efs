#!/usr/bin/env python3
"""Exercise benchmark command ownership, parsing and scratch-root protection."""
import argparse
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
    with tempfile.TemporaryDirectory(prefix='efs-io-refusal-') as root:
        keep = pathlib.Path(root) / 'keep'; keep.write_text('keep')
        run('efs-bench', '--bench', 'io', '--storage', root, ok=False)
        for size in ['nan', '1Mx', '8193', '-1', '999999999999999999999M']:
            run('efs-bench', '--bench', 'io', '--storage', root, '--io-size', size, ok=False)
        run('efs-bench', '--bench', 'io', '--storage', root, '--rw', 'read', '--sync', ok=False)
        assert keep.read_text() == 'keep'
    if args.smoke:
        for kind, option in [('meta', '--meta-storage'), ('data', '--storage')]:
            with tempfile.TemporaryDirectory(prefix='efs-bench-smoke-') as root:
                opts = ['--writers', '2'] if kind == 'data' else []
                out = run('efs-bench', '--bench', kind, option, root,
                          '--time', '.03', *opts, timeout=180)
                assert f'BENCH_OK kind={kind}' in out, out
                print(out, end='')
    print('benchmark CLI: ownership, parsing and scratch protection PASS')


if __name__ == '__main__':
    main()
