#!/usr/bin/env python3
"""Verify new W52 writes and old data through a normal preserved-store restart."""
import hashlib, json, os, signal, socket, subprocess, sys, time, traceback
from pathlib import Path
base = Path('/data1/efs/devel-nuc-20261011')
out = base / 'catchup-service-cold'
out.mkdir(exist_ok=False)
src = Path('/data1/efs/src')
env = dict(os.environ, EFS_CLUSTER='nuc', EFS_DIRECT_IO='1', EFS_TRANSPORT='tcp',
           EFS_COMMANDS='0', EFS_COMMANDS_SERVER='0', EFS_MD_RAFT_N='4',
            EFS_RAFT_BATCH_OVERLAP='0', EFS_RAFT_BATCH_NOOP='0', EFS_RAFT_APPLIED_INTERVAL_MS='10', EFS_BATCH_OBS='0')
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def wait(predicate, seconds, label):
    end = time.monotonic() + seconds
    while not predicate():
        assert time.monotonic() < end, label + ' timed out'
        time.sleep(.5)

def listening(port):
    try:
        with socket.create_connection(('127.0.0.1', port), .3): return True
    except OSError: return False

def mapped():
    assert all(digest(src / name) == image['sha256'] for name, image in images.items())
    rows = []
    for p in Path('/proc').iterdir():
        if not p.name.isdigit(): continue
        try:
            exe = os.readlink(p / 'exe')
            if exe not in (str(src / 'efsd'), str(src / 'efs-fuse')): continue
            name = Path(exe).name
            assert digest(p / 'exe') == images[name]['sha256']
            rows.append(dict(pid=int(p.name), kind=name))
        except (FileNotFoundError, ProcessLookupError, PermissionError): continue
    assert sum(row['kind'] == 'efsd' for row in rows) == 4
    assert sum(row['kind'] == 'efs-fuse' for row in rows) == 2
    assert all(listening(port) for port in range(17432, 17436))
    assert all(os.path.ismount('/data1/efs/' + mount) for mount in ('mnt', 'mnt2'))
    return rows

def payloads():
    rows = json.loads((base / 'clock-cold/after.json').read_text())['payloads']
    for row in rows: assert digest(Path(row['path'])) == row['sha256'], row['path']
    expected = report['warm_primary_sha256']
    assert expected == report['warm_peer_sha256']
    for mount in ('mnt', 'mnt2'):
        path = Path('/data1/efs', mount, tree, 'append-burst')
        assert append_burst.verify(path, 2000) == expected
        rows.append(dict(path=str(path), records=2000, sha256=expected))
    return rows

def restore():
    with (out / 'start.log').open('a') as log:
        rc = subprocess.run(['bash', '/data1/efs/w97-latest-20261010/start-preserved.sh'], env=env,
                            stdout=log, stderr=subprocess.STDOUT, timeout=360).returncode
    assert rc == 0
    wait(lambda: all(listening(port) for port in range(17432, 17436)), 180, 'actual restart listeners')
    after = mapped()
    checked = payloads()
    (out / 'after.json').write_text(json.dumps(dict(images=images, processes=after, payloads=checked,
        limits='Normal process restart and fresh mounts, not hardware power loss. Exact private 20k gates already passed both modes.'), indent=2))
    (out / 'restore-exit.txt').write_text('0\n')

stopped = False
try:
    def installed():
        for phase in ('catchup-base-build', 'catchup-base-live', 'catchup-install'):
            assert not (base / phase / 'failure.txt').exists(), phase + ' failed'
        marker = base / 'catchup-install/run-exit.txt'
        if not marker.exists(): return False
        assert marker.read_text().strip() == '0'
        return True
    wait(installed, 86400, 'accepted deployment')
    images = json.loads((base / 'catchup-install/images.json').read_text())
    mapped()
    sys.path.insert(0, str(src / 'tests/live'))
    import append_burst
    tree = 'w52-catchup-preserved-2000-20261011'
    primary = Path('/data1/efs/mnt') / tree
    primary.mkdir(exist_ok=False)
    work = out / 'append-2000'; work.mkdir()
    path, report = append_burst.run(primary, Path('/data1/efs/mnt2') / tree, 2000, work)
    (work / 'run-exit.txt').write_text('0\n')
    before = mapped()
    (out / 'before.json').write_text(json.dumps(dict(processes=before, payloads=payloads()), indent=2))
    stopped = True
    for mount in ('mnt', 'mnt2'):
        with (out / (mount + '-stop.log')).open('w') as log:
            rc = subprocess.run([str(src / 'scripts/client.sh'), 'stop', '/data1/efs/' + mount],
                                stdout=log, stderr=subprocess.STDOUT, timeout=180).returncode
        assert rc == 0 and not os.path.ismount('/data1/efs/' + mount)
    pids = [row['pid'] for row in before if row['kind'] == 'efsd']
    for pid in pids:
        fd = os.pidfd_open(pid); signal.pidfd_send_signal(fd, signal.SIGTERM); os.close(fd)
    wait(lambda: all(not Path('/proc', str(pid)).exists() for pid in pids), 180, 'normal daemon drain')
    assert subprocess.run(['pgrep', '-x', 'efsd'], stdout=subprocess.DEVNULL).returncode == 1
    restore(); stopped = False
    (out / 'run-exit.txt').write_text('0\n')
except BaseException:
    (out / 'failure.txt').write_text(traceback.format_exc())
    (out / 'run-exit.txt').write_text('1\n')
    raise
finally:
    if stopped:
        assert not any(os.path.ismount('/data1/efs/' + mount) for mount in ('mnt', 'mnt2'))
        assert subprocess.run(['pgrep', '-x', 'efsd'], stdout=subprocess.DEVNULL).returncode == 1
        restore()
