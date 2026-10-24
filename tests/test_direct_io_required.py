#!/usr/bin/env python3
"""D42: fragment I/O is O_DIRECT only; storage without it is refused.

A storage root on a filesystem that rejects O_DIRECT (tests/nodirect_fs, a
FUSE passthrough that answers O_DIRECT opens with EINVAL) stops efsd at
startup with a clear error, alone or as the second of two roots; nothing
falls back to buffered I/O. A root that supports O_DIRECT passes the check
and serves. The removed --direct-io, --no-direct-io and --no-persist flags
are refused by name.

NOTRUN (exit 0) without /dev/fuse, fusermount3 or tests/nodirect_fs.

Usage: make efsd tests/nodirect_fs && python3 tests/test_direct_io_required.py"""
from pathlib import Path
import errno, os, shutil, signal, socket, subprocess, sys, tempfile, time
root = Path(__file__).resolve().parents[1]
efsd, ndfs = root / 'efsd', root / 'tests/nodirect_fs'
if not efsd.exists():
    sys.exit(f'{efsd} missing: run make first')
if not Path('/dev/fuse').exists() or not shutil.which('fusermount3') or not ndfs.exists():
    print('direct I/O required: NOTRUN (no /dev/fuse, fusermount3 or tests/nodirect_fs)')
    sys.exit(0)
base = {k: v for k, v in os.environ.items() if not k.startswith('EFS_')}


def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def efsd_run(*storage, extra=()):
    return subprocess.run([str(efsd), '127.0.0.1:%d' % port(), ','.join(map(str, storage)),
                           '--discovery-port', '0', *extra],
                          env=base, capture_output=True, text=True, timeout=30)


work = Path(tempfile.mkdtemp(prefix='efs-direct-required-', dir=os.environ.get('TMPDIR')))
back, mnt, good = work / 'back', work / 'mnt', work / 'good'
for d in (back, mnt, good):
    d.mkdir()
fs = subprocess.Popen([str(ndfs), str(back), str(mnt), '-f'], stdout=subprocess.DEVNULL,
                      stderr=subprocess.PIPE, text=True)
try:
    deadline = time.time() + 10
    while not os.path.ismount(mnt):
        if fs.poll() is not None or time.time() > deadline:
            print('direct I/O required: NOTRUN (nodirect_fs did not mount: %s)'
                  % (fs.stderr.read().strip()[-300:] if fs.poll() is not None else 'timeout'))
            sys.exit(0)
        time.sleep(0.05)
    # Precondition: the mount takes buffered I/O and rejects O_DIRECT.
    probe = mnt / 'probe'
    os.close(os.open(probe, os.O_CREAT | os.O_WRONLY, 0o600))
    try:
        os.close(os.open(probe, os.O_WRONLY | os.O_DIRECT))
        raise AssertionError('nodirect_fs accepted O_DIRECT')
    except OSError as e:
        assert e.errno == errno.EINVAL, e
    probe.unlink()

    msg = 'refused: fragment I/O requires O_DIRECT'
    p = efsd_run(mnt / 'store')
    assert p.returncode == 1 and f'storage root {mnt}/store {msg}' in p.stderr, (p.returncode, p.stderr[-800:])
    assert 'Invalid argument' in p.stderr and 'listening on' not in p.stdout, p.stderr[-800:]
    # The second of two roots is checked too; the first is not used instead.
    p = efsd_run(good / 'r0', mnt / 'r1')
    assert p.returncode == 1 and f'storage root {mnt}/r1 {msg}' in p.stderr, (p.returncode, p.stderr[-800:])
    assert not list((mnt / 'r1' / 'data').iterdir()), 'probe file left behind'
    for flag in ('--direct-io', '--no-direct-io', '--no-persist'):
        p = efsd_run(good / 'r2', extra=(flag,))
        assert p.returncode == 1 and f'{flag} was removed (D42)' in p.stderr, (flag, p.stderr[-400:])
    # Control: a root that supports O_DIRECT passes the check and serves.
    srv = subprocess.Popen([str(efsd), '127.0.0.1:%d' % port(), str(good / 'ok'), '--discovery-port', '0'],
                           env=base, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline, line = time.time() + 30, ''
        while time.time() < deadline and 'listening on' not in line:
            line = srv.stdout.readline()
            if not line and srv.poll() is not None:
                break
        assert 'listening on' in line and 'direct_io=on' in line, (line, srv.poll())
    finally:
        srv.send_signal(signal.SIGTERM)
        try:
            srv.wait(timeout=30)
        except subprocess.TimeoutExpired:
            srv.kill()
            srv.wait()
finally:
    subprocess.run(['fusermount3', '-u', str(mnt)], capture_output=True)
    try:
        fs.wait(timeout=10)
    except subprocess.TimeoutExpired:
        fs.kill()
    shutil.rmtree(work, ignore_errors=True)
print('direct I/O required: a root without O_DIRECT (one of one, second of two) is refused at '
      'startup, no fallback; an O_DIRECT root serves; removed flags refused PASS')
