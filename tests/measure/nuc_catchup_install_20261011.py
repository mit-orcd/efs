#!/usr/bin/env python3
"""Install W52 completion only after both modes, exact 20k and cold private gates."""
import hashlib, json, os, signal, socket, subprocess, time, traceback
from pathlib import Path
base = Path('/data1/efs/devel-nuc-20261011')
src = base / 'catchup-base-src'
dst = Path('/data1/efs/src')
out = base / 'catchup-install'
out.mkdir(exist_ok=False)
env = dict(os.environ, EFS_CLUSTER='nuc', EFS_DIRECT_IO='1', EFS_TRANSPORT='tcp',
           EFS_COMMANDS='0', EFS_COMMANDS_SERVER='0', EFS_RAFT_BATCH_OVERLAP='0', EFS_RAFT_BATCH_NOOP='0', EFS_RAFT_APPLIED_INTERVAL_MS='10', EFS_BATCH_OBS='0')
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

def run(label, command, seconds=300):
    with (out / (label + '.log')).open('w') as log:
        rc = subprocess.run(list(map(str, command)), env=env, stdout=log, stderr=subprocess.STDOUT, timeout=seconds).returncode
    (out / (label + '-exit.txt')).write_text(str(rc) + '\n')
    assert rc == 0, (label, rc)

def images_at(root, manifest):
    assert all(digest(root / name) == image['sha256'] for name, image in manifest.items())

def mapped(manifest):
    images_at(dst, manifest)
    rows = []
    for p in Path('/proc').iterdir():
        if not p.name.isdigit(): continue
        try:
            exe = os.readlink(p / 'exe')
            if exe not in (str(dst / 'efsd'), str(dst / 'efs-fuse')): continue
            name = Path(exe).name
            assert digest(p / 'exe') == manifest[name]['sha256']
            rows.append(dict(pid=int(p.name), kind=name))
        except (FileNotFoundError, ProcessLookupError, PermissionError): continue
    assert sum(row['kind'] == 'efsd' for row in rows) == 4
    assert sum(row['kind'] == 'efs-fuse' for row in rows) == 2
    assert all(os.path.ismount('/data1/efs/' + mount) for mount in ('mnt', 'mnt2'))
    assert all(listening(port) for port in range(17432, 17436))
    return rows

def payloads():
    rows = json.loads((base / 'clock-cold/after.json').read_text())['payloads']
    for row in rows: assert digest(Path(row['path'])) == row['sha256'], row['path']
    return rows

def start(label):
    run(label, ['bash', '/data1/efs/w97-latest-20261010/start-preserved.sh'], 360)
    wait(lambda: all(listening(port) for port in range(17432, 17436)), 180, 'actual listeners')

stopped = changed = False
try:
    def ready():
        for name in ('catchup-base-build', 'catchup-base-live'):
            assert not (base / name / 'failure.txt').exists(), name + ' failed; refuse deployment'
            if not (base / name / 'run-exit.txt').exists(): return False
            assert (base / name / 'run-exit.txt').read_text().strip() == '0'
        return True
    wait(ready, 86400, 'full private acceptance')
    live = base / 'catchup-base-live'
    for name in ('ordinary-direct', 'ordinary-buffered', 'full-direct', 'full-buffered',
                 'long-direct', 'long-buffered', 'restore'):
        assert (live / (name + '-exit.txt')).read_text().strip() == '0'
    images = json.loads((base / 'catchup-base-build/images.json').read_text())
    expected = json.loads((base / 'catchup-base-build/provenance.json').read_text())['core_sha256']
    current = {str(p.relative_to(src)): digest(p) for folder in ('src', 'include')
               for p in (src / folder).rglob('*') if p.suffix in ('.c', '.h')}
    current['Makefile'] = digest(src / 'Makefile')
    assert current == expected, 'accepted core changed'
    images_at(src, images)
    old = json.loads((base / 'clock-install/images.json').read_text())
    before = mapped(old)
    checked = payloads()
    assert Path('/data1/efs/.mkfs-done').is_file()
    (out / 'before.json').write_text(json.dumps(dict(images=old, processes=before, payloads=checked), indent=2))
    run('source-backup', ['tar', '-czf', out / 'old-source.tar.gz', '--exclude=./results',
                         '--exclude=./.git', '--exclude=./_deps', '-C', dst, '.'])
    stopped = True
    for mount in ('mnt', 'mnt2'):
        run(mount + '-stop', [dst / 'scripts/client.sh', 'stop', '/data1/efs/' + mount], 180)
        assert not os.path.ismount('/data1/efs/' + mount)
    pids = [row['pid'] for row in before if row['kind'] == 'efsd']
    for pid in pids:
        fd = os.pidfd_open(pid); signal.pidfd_send_signal(fd, signal.SIGTERM); os.close(fd)
    wait(lambda: all(not Path('/proc', str(pid)).exists() for pid in pids), 180, 'normal daemon drain')
    assert subprocess.run(['pgrep', '-x', 'efsd'], stdout=subprocess.DEVNULL).returncode == 1
    changed = True
    run('install', ['rsync', '-a', '--exclude=/results/', '--exclude=/.git/', str(src) + '/', str(dst) + '/'])
    images_at(dst, images)
    installed = {str(p.relative_to(dst)): digest(p) for folder in ('src', 'include')
                 for p in (dst / folder).rglob('*') if p.suffix in ('.c', '.h')}
    installed['Makefile'] = digest(dst / 'Makefile')
    assert installed == expected, 'installed core differs from acceptance'
    start('start')
    after = mapped(images)
    verified = payloads()
    (out / 'images.json').write_text(json.dumps(images, indent=2))
    (out / 'installed-core-sha256.json').write_text(json.dumps(installed, indent=2))
    (out / 'after.json').write_text(json.dumps(dict(processes=after, payloads=verified,
        limits='W52-only root baseline; original regular stores preserved. Every voter supports command 52.'), indent=2))
    (out / 'run-exit.txt').write_text('0\n')
    stopped = False
except BaseException:
    (out / 'failure.txt').write_text(traceback.format_exc())
    (out / 'run-exit.txt').write_text('1\n')
    raise
finally:
    if stopped:
        # An upgraded Raft log may contain command 52. Never automatically
        # downgrade that log to the previous binary after candidate startup.
        (out / 'recovery-policy.txt').write_text('Retain live mounts/daemons and stores. No forced discard or automatic old-binary replay.\n')
        if not changed and not any(os.path.ismount('/data1/efs/' + m) for m in ('mnt', 'mnt2')):
            if subprocess.run(['pgrep', '-x', 'efsd'], stdout=subprocess.DEVNULL).returncode == 1:
                start('unchanged-source-restart'); mapped(old); payloads()
