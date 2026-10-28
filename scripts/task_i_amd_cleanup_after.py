#!/usr/bin/env python3
"""Wait for AMD, preserve/recover one owned K42 fixture, then qualify anew.

OLD_SOURCE FIXTURE ORPHAN_PID NEW_COMMIT NEW_EVIDENCE_ROOT
No format, discard, external process signal or failed-test retry is performed.
The old store is restarted only with its unchanged original binary, solely to
verify its six synthetic lines and cleanly retire the stranded peer.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('old_source', type=Path)
p.add_argument('fixture', type=Path)
p.add_argument('pid', type=int)
p.add_argument('commit')
p.add_argument('evidence', type=Path)
a = p.parse_args()
source = Path.cwd().resolve()
old, fixture, art = a.old_source.resolve(), a.fixture.resolve(), a.evidence
assert socket.gethostname().startswith('amd-')
assert source.parent.parent == old.parent.parent == Path.home()/'efs-taskI'
assert source.name == old.name == 'src' and source != old
assert fixture.is_relative_to(old/'tmp') and fixture.name == 'c'
assert fixture.parent.name.startswith('efs-k42-')
assert art.parent == Path.home()/'efs-artifacts' and not art.exists()
assert len(a.commit) >= 9 and all(c in '0123456789abcdef' for c in a.commit)
art.mkdir()
log = (art/'controller.log').open('x', buffering=1)
os.dup2(log.fileno(), 1)
os.dup2(log.fileno(), 2)
sys.path.insert(0, str(source/'scripts'))
sys.path.insert(0, str(source/'tests'))
from task_i_host_sample import foreign_jobs
from mount_idle import wait_fixture_unused

def stage(s):
    (art/'stage').write_text(s+'\n')
    print(time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), s, flush=True)

def identity():
    proc = Path('/proc')/str(a.pid)
    assert proc.stat().st_uid == os.getuid()
    args = (proc/'cmdline').read_bytes().rstrip(b'\0').decode().split('\0')
    assert len(args) == 4 and args[0] == str(old/'efs-fuse')
    assert args[2:] == [str(fixture/'peer2'), '-f']
    assert (proc/'exe').resolve() == old/'efs-fuse'
    fields = (proc/'stat').read_text().rsplit(')', 1)[1].split()
    return dict(pid=a.pid, args=args, start_ticks=int(fields[19]))

owner = identity()
pidfd = os.pidfd_open(a.pid)  # pin identity, never signal through it
(art/'owner.json').write_text(json.dumps(owner, indent=2)+'\n')
processes = []

def run(args, name, timeout=90, cwd=old, env=None):
    with (art/name).open('x') as out:
        result = subprocess.run(args, stdout=out, stderr=subprocess.STDOUT,
                                cwd=cwd, env=env, timeout=timeout)
    (art/(name+'.exit')).write_text(str(result.returncode)+'\n')
    assert result.returncode == 0, f'{name}: exit {result.returncode}'

rc = 1
try:
    stage('WAIT_QUIET')
    deadline = time.monotonic()+14400
    quiet = 0
    while time.monotonic() < deadline:
        assert identity() == owner, 'stranded peer identity changed; no action'
        jobs = [j for j in foreign_jobs() if j['pid'] != a.pid]
        (art/'last-preflight.json').write_text(json.dumps(dict(epoch=time.time(), jobs=jobs), indent=2)+'\n')
        quiet = 0 if jobs else quiet+1
        if quiet == 2:
            break
        time.sleep(20)
    assert quiet == 2, 'AMD remained occupied; no store touched'
    stage('PRESERVE_FAILED_FIXTURE')
    original_id = old.parent.name.rsplit('-', 1)[1]
    binary_manifest = Path.home()/'efs-artifacts'/f'taskI-amd-{original_id}'/'binaries-after.sha256'
    for line in binary_manifest.read_text().splitlines():
        digest, name = line.split(None, 1)
        assert name in ('efsd', 'tests/faults/efsd', 'efs-fuse', 'efs-mgmt')
        assert hashlib.sha256((old/name).read_bytes()).hexdigest() == digest, 'original binary changed'
    shutil.copyfile(binary_manifest, art/'original-binaries.sha256')
    saved = art/'original-fixture'
    saved.mkdir()
    for n in (1, 2, 3):
        store = fixture/f'n{n}'
        assert store.is_dir() and not store.is_symlink()
        run(['cp', '-a', '--reflink=auto', '--', str(store), str(saved)], f'copy-n{n}.log')
    for f in fixture.glob('*.log'):
        shutil.copyfile(f, saved/f.name)
    hold = fixture.parent/'append-hold'
    if hold.exists():
        shutil.copyfile(hold, saved/'append-hold')
    rows = []
    for f in sorted(saved.rglob('*')):
        if f.is_symlink():
            raise AssertionError('unexpected symlink in synthetic evidence')
        if f.is_file():
            relative = f.relative_to(saved)
            digest = hashlib.sha256(f.read_bytes()).hexdigest()
            if relative.parts[0] in ('n1', 'n2', 'n3'):
                assert hashlib.sha256((fixture/relative).read_bytes()).hexdigest() == digest, 'store copy differs'
            rows.append(digest+'  '+str(relative)+'\n')
    (saved/'SHA256SUMS').write_text(''.join(rows))
    stage('RECOVER_ORIGINAL_FOR_CLEANUP')
    argv, endpoints = [], []
    for n in (1, 2, 3):
        line = next(l for l in (fixture/f'n{n}.log').read_text().splitlines() if l.startswith('==== start '))
        args = shlex.split(line.split(' ', 3)[3])
        endpoint = args[1] if n == 1 else args[4]
        assert endpoint.startswith('127.0.0.1:') and int(endpoint.rsplit(':', 1)[1]) > 1024
        endpoints.append(endpoint)
        expected = [str(old/'tests/faults/efsd')]
        expected += [endpoint] if n == 1 else ['--join', endpoints[0], '--listen', endpoint]
        expected += [str(fixture/f'n{n}'), '--quota', '1G', '--writers', '2', '--discovery-port', '0']
        assert args == expected, args
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', int(endpoint.rsplit(':', 1)[1])))
        argv.append(args)
    assert owner['args'][1] == endpoints[1]
    env = {k:v for k,v in os.environ.items() if not k.startswith(('EFS_FAULT_', 'EFS_TEST_'))}
    env['EFS_TRANSPORT'] = 'tcp'
    (art/'restart-argv.json').write_text(json.dumps(argv, indent=2)+'\n')
    for n, args in enumerate(argv, 1):
        with (art/f'restart-n{n}.log').open('x') as out:
            processes.append(subprocess.Popen(args, cwd=old, env=env, stdout=out, stderr=subprocess.STDOUT))
    run([sys.executable, str(old/'scripts/orcd_test_ready.py'), '--mgmt', str(old/'efs-mgmt'), '--timeout', '90', *endpoints], 'readiness.log', timeout=110, env=env)
    # Exact verification of this tiny synthetic fixture, never a sampled pass.
    verify = "from pathlib import Path; import sys; p=Path(sys.argv[1]);\nfor n in ('peer1','peer2'):\n actual=(p/(n+'.log')).read_bytes(); expected=(n+' first\\n'+n+' second\\nprimary held\\n').encode(); assert actual==expected,(n,actual); print(n,'exact PASS')"
    run([sys.executable, '-c', verify, str(fixture/'peer2')], 'payloads.log', timeout=60, env=env)
    wait_fixture_unused(fixture/'peer2')
    run(['bash', str(old/'scripts/client.sh'), 'stop', str(fixture/'peer2')], 'stop-peer.log', timeout=120, env=env)
    end = time.monotonic()+5
    while Path('/proc', str(a.pid)).exists() and time.monotonic() < end:
        time.sleep(0.05)
    assert not Path('/proc', str(a.pid)).exists(), 'peer controller did not retire'
    assert str(fixture/'peer2') not in Path('/proc/self/mountinfo').read_text(), 'mount still present'
    stage('STOP_RECOVERED_FIXTURE')
    for proc in processes:
        proc.send_signal(signal.SIGTERM)
    for n, proc in enumerate(processes, 1):
        result = proc.wait(timeout=60)
        (art/f'restart-n{n}.exit').write_text(str(result)+'\n')
        assert result == 0, 'cleanup daemon shutdown failed'
    processes.clear()
    os.close(pidfd)
    stage('QUALIFICATION')
    rc = subprocess.call(['bash', 'scripts/task_i_amd.sh', a.commit, str(art/'qualification')], cwd=source)
    stage('FINISHED')
finally:
    # No force kill/discard: a refusal leaves its logs/stores intact and blocks
    # the subsequent qualification. Stop only daemons started above, orderly.
    for proc in processes:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
    for n, proc in enumerate(processes, 1):
        try:
            result = proc.wait(timeout=60)
            (art/f'restart-n{n}-cleanup.exit').write_text(str(result)+'\n')
        except subprocess.TimeoutExpired:
            (art/'cleanup-blocked').write_text('daemon still owns store; no force kill\n')
    if (art/'stage').read_text().strip() != 'FINISHED':
        (art/'incomplete').write_text('cleanup/qualification incomplete; retain all evidence\n')
    (art/'controller-exit').write_text(str(rc)+'\n')
sys.exit(rc)
