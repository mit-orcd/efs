#!/usr/bin/env python3
"""Exercise benchmark command ownership, parsing and scratch-root protection."""
import argparse
import pathlib
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
        run('efs-bench', '--bench', 'other', '--storage', root, ok=False)
        run('efs-bench', '--bench', 'meta', '--store', '--storage', root, ok=False)
        run('efs-bench', '127.0.0.1:1', '--bench', 'meta', '--storage', root, ok=False)
        run('efs-bench', '--bench', 'data', '--storage', root, '--bench', 'meta', ok=False)
        assert list(pathlib.Path(root).iterdir()) == [sentinel]
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
