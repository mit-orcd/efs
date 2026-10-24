#!/usr/bin/env python3
"""A8 (D42): EFS_TRANSPORT keeps auto (default), tcp and rdma.

An explicit rdma without a usable RDMA device stops efsd and efs-fuse at
startup with a clear error; it never falls back to TCP. auto may choose TCP
and says which transport it uses; tcp says so too. An unknown value is
refused. No usable device is made deterministic with EFS_RDMA_DEV naming a
device that does not exist (the device every connection would then use).

Usage: make efsd efs-fuse && python3 tests/test_transport_select.py"""
from pathlib import Path
import os, shutil, signal, socket, subprocess, sys, tempfile, time
root = Path(__file__).resolve().parents[1]
efsd, fuse = root / 'efsd', root / 'efs-fuse'
for b in (efsd, fuse):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
base = {k: v for k, v in os.environ.items() if not k.startswith('EFS_')}
NODEV = 'efs-test-no-such-rdma-device'


def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def serve_until_listening(env, store):
    """Start efsd; return its stderr once it listens (then stop it)."""
    p = subprocess.Popen([str(efsd), '127.0.0.1:%d' % port(), str(store), '--discovery-port', '0'],
                         env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline, line = time.time() + 30, ''
        while time.time() < deadline and 'listening on' not in line:
            line = p.stdout.readline()
            if not line and p.poll() is not None:
                break
        assert 'listening on' in line, (line, p.poll())
    finally:
        p.send_signal(signal.SIGTERM)
        try:
            _, err = p.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            p.kill()
            _, err = p.communicate()
    return err


work = Path(tempfile.mkdtemp(prefix='efs-transport-', dir=os.environ.get('TMPDIR')))
try:
    mnt = work / 'mnt'
    mnt.mkdir()
    cases = 0
    # Explicit rdma without a usable device: both binaries refuse before work.
    for argv, prog in (([str(efsd), '127.0.0.1:%d' % port(), str(work / 's1')], 'efsd'),
                       ([str(fuse), '127.0.0.1:1', str(mnt)], 'efs-fuse')):
        env = dict(base, EFS_TRANSPORT='rdma', EFS_RDMA_DEV=NODEV)
        p = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=30)
        assert p.returncode == 1, (prog, p.returncode, p.stderr[-600:])
        assert (f'{prog}: EFS_TRANSPORT=rdma but no usable RDMA device named by '
                f'EFS_RDMA_DEV={NODEV}; refusing to start') in p.stderr, (prog, p.stderr[-600:])
        assert 'transport auto' not in p.stderr and 'listening on' not in p.stdout, p.stderr[-600:]
        cases += 1
    assert not (work / 's1').exists(), 'refused before touching storage'
    # Unknown value: refused, not read as auto.
    p = subprocess.run([str(efsd), '127.0.0.1:%d' % port(), str(work / 's2')],
                       env=dict(base, EFS_TRANSPORT='infiniband'), capture_output=True, text=True, timeout=30)
    assert p.returncode == 1 and 'EFS_TRANSPORT=infiniband is not auto, tcp or rdma' in p.stderr, p.stderr[-600:]
    cases += 1
    # auto with no usable device: serves over TCP and says so.
    err = serve_until_listening(dict(base, EFS_RDMA_DEV=NODEV), work / 's3')
    assert f'efsd: transport auto: tcp (no usable RDMA device named by EFS_RDMA_DEV={NODEV})' in err, err[-800:]
    cases += 1
    # tcp: says so.
    err = serve_until_listening(dict(base, EFS_TRANSPORT='tcp'), work / 's4')
    assert 'efsd: transport tcp (EFS_TRANSPORT=tcp)' in err, err[-800:]
    cases += 1
    # This host as it is: auto names what it uses; rdma refuses exactly when
    # auto finds no device.
    err = serve_until_listening(dict(base), work / 's5')
    line = next((l for l in err.splitlines() if 'efsd: transport auto: ' in l), '')
    assert line, err[-800:]
    if 'auto: tcp' in line:
        p = subprocess.run([str(efsd), '127.0.0.1:%d' % port(), str(work / 's6')],
                           env=dict(base, EFS_TRANSPORT='rdma'), capture_output=True, text=True, timeout=30)
        assert p.returncode == 1 and 'EFS_TRANSPORT=rdma but no usable RDMA device; refusing' in p.stderr, p.stderr
        cases += 1
    print('host transport (auto): ' + line.split('efsd: ', 1)[1])
finally:
    shutil.rmtree(work, ignore_errors=True)
print(f'transport selection: explicit rdma without a device refused by efsd and efs-fuse, unknown '
      f'value refused, auto falls back to tcp and says so, tcp reported ({cases} cases) PASS')
