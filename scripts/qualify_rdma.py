#!/usr/bin/env python3
"""Task L: private RDMA gate, retained logs, no deployed cluster changes.

Run on an otherwise idle test host with EFS_RDMA_DEV set and matching build
identity variables. --root and --output must be fresh, outside this source.
The summary runner builds first; separate RDMA commands retain positive logs
that its ordinary successful-command output does not retain. Failed fixtures
are kept for investigation. This script never installs packages or prunes data.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import shutil
import subprocess
import sys
import time


def posix(source, fixture, output):
    sys.path.insert(0, str(source / 'tests'))
    from k33_daemons import Cluster, Mount
    cluster = Cluster(fixture, 'posix', [1, 2, 3], dict(os.environ))
    mount = None
    try:
        cluster.start(1)
        cluster.start(2, join=1)
        cluster.start(3, join=1)
        for node in (1, 2, 3):
            cluster.wait_log(node, r'raft-host: formed: 3 members')
        mount = Mount(cluster, 1, 'mount')
        return subprocess.run([
            'python3', str(source / 'tests/posix/posix_suite.py'), str(mount.path),
            '--suite', 'all', '--jobs', '4', '--results', str(output),
        ]).returncode
    finally:
        if mount is not None:
            mount.close()
            mount.log.close()
        cluster.stop_all()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--posix-worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    root, output = args.root.resolve(), args.output.resolve()
    if args.posix_worker:
        return posix(source, root, output)
    for path in (root, output):
        assert not path.exists(), 'fresh directories required: ' + str(path)
        assert source not in path.parents and path not in source.parents
    assert root != output and root not in output.parents and output not in root.parents
    assert os.environ.get('EFS_GIT_ID'), 'set exact archived source EFS_GIT_ID'
    assert os.environ.get('EFS_RDMA_DEV'), 'name the real RDMA device'
    root.mkdir(parents=True)
    output.mkdir(parents=True)
    rows = []
    env = dict(os.environ, EFS_TEST_JOBS='8', EFS_BUILD_JOBS='8',
               EFS_TEST_TARGET='test-all', EFS_TRANSPORT='auto')
    (output / 'environment.json').write_text(json.dumps({
        'source': os.environ['EFS_GIT_ID'], 'root': str(root),
        'device': env['EFS_RDMA_DEV'], 'uname': list(os.uname()),
        'leak_detection': 'disabled on this host for ASan only; no LSan qualification',
    }, indent=2) + '\n')

    def run(name, command, transport='rdma', timeout=3600, extra=None):
        fixture = root / name
        fixture.mkdir()
        started = time.monotonic()
        child_env = dict(env, TMPDIR=str(fixture), EFS_TRANSPORT=transport, **(extra or {}))
        with (output / (name + '.log')).open('x') as log:
            log.write('$ ' + repr(command) + '\n'); log.flush()
            try:
                process = subprocess.Popen(command, cwd=source, env=child_env,
                                           stdout=log, stderr=subprocess.STDOUT,
                                           start_new_session=True)
                rc = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                # Give Python harnesses a catchable interrupt for their finally
                # cleanup, then bound shutdown of this owned process group.
                try:
                    os.killpg(process.pid, signal.SIGINT)
                    process.wait(timeout=60)
                except (ProcessLookupError, subprocess.TimeoutExpired):
                    pass
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
                rc = 124
                log.write('\nCONTROLLER TIMEOUT; inspect retained processes and fixtures\n')
        text = (output / (name + '.log')).read_text(errors='replace')
        status = 'NOTRUN' if rc == 3 or (rc == 0 and re.search(r'\b(NOTRUN|SKIP)\b', text)) else 'PASS' if rc == 0 else 'FAIL'
        rows.append(dict(name=name, command=command, transport=transport,
                         exit=rc, verdict=status, seconds=round(time.monotonic()-started, 3)))
        (output / (name + '.exit')).write_text(str(rc) + '\n')
        (output / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
        print(name, status, rc, flush=True)
        if rc == 124:
            raise RuntimeError('timeout: stop to inspect owned processes before further load')
        return rc

    run('full-gate', ['bash', 'scripts/make_test_summary.sh'], transport='auto', timeout=7200)
    for name in ('summary.txt', 'progress.txt', 'timings.tsv', 'cmds.txt', 'build.log', 'enum.log'):
        path = source / name
        if path.exists():
            (output / name).write_bytes(path.read_bytes())
    failures = output / 'full-gate-failures'
    failures.mkdir()
    for path in (source / 'tmp').glob('fail-*.log'):
        shutil.copy2(path, failures / path.name)
    with (output / 'version.txt').open('x') as log:
        subprocess.run(['./efsd', '--version'], cwd=source, stdout=log, stderr=subprocess.STDOUT)
    (output / 'binaries.sha256').write_text(''.join(
        hashlib.sha256((source / name).read_bytes()).hexdigest() + '  ' + name + '\n'
        for name in ('efsd', 'efs-fuse', 'efs-mgmt', 'tests/faults/efsd') if (source / name).exists()))
    run('rdma-xprt', ['./tests/test_rdma_xprt'])
    run('declaration', ['./tests/test_declaration_gate'])
    for mode in ('normal', 'interrupt', 'tcp-violation', 'deadline', 'burst', 'burst-asym', 'churn'):
        run('segmented-' + mode, ['./tests/test_rdma_segmented', mode])
    for test in ('memcheck', 'reply_budget', 'progress_ready', 'setup_reentry'):
        run('rdma-' + test, ['python3', 'tests/test_rdma_' + test + '.py'])
    for number in (1, 2):
        parents = [root / ('gc-parent-%d-%d' % (number, i)) for i in (1, 2)]
        for parent in parents:
            parent.mkdir()
        run('gc-%d' % number, ['python3', 'tests/live/gc_reclamation.py', '--transport', 'rdma',
                               '--root', str(parents[0]), '--root', str(parents[1]), '--port', '19390'])
        name = 'posix-%d' % number
        run(name, ['python3', str(Path(__file__).resolve()), '--posix-worker',
                   '--root', str(root / name), '--output', str(output / (name + '.tsv'))])
        run('k38-%d' % number, ['python3', 'tests/test_k38_shared_chunk_pressure.py'])
    run('tsan', ['make', 'tsan-check'], timeout=14400,
        extra={'EFS_SAN_JOBS': '8', 'EFS_SAN_LOGDIR': str(output / 'tsan-reports')})
    run('asan', ['make', 'asan-check'], timeout=14400,
        extra={'ASAN_OPTIONS': 'detect_leaks=0', 'EFS_SAN_JOBS': '8',
               'EFS_SAN_LOGDIR': str(output / 'asan-reports')})
    return 1 if any(row['verdict'] == 'FAIL' for row in rows) else 3 if any(row['verdict'] == 'NOTRUN' for row in rows) else 0


if __name__ == '__main__':
    raise SystemExit(main())
