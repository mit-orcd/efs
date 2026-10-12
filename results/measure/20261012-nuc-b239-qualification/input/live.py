#!/usr/bin/env python3
"""Full private W52/W97 gates on NUC, preserving and restoring regular stores."""
import argparse, hashlib, json, os, re, signal, socket, subprocess, time, traceback
from pathlib import Path
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--variant', choices=('catchup-base', 'catchup-root'), required=True)
args = parser.parse_args()
base = Path('/data1/efs/nuc-b239-qualification')
src = base / ('source' if args.variant=='catchup-base' else 'baseline-source')
out = base / (args.variant + '-live')
out.mkdir(exist_ok=False)
installed = Path('/data1/efs/src')
env = dict(os.environ, EFS_COMMANDS='0', EFS_COMMANDS_SERVER='0', EFS_TRANSPORT='tcp',
           EFS_MD_RAFT_N='4', EFS_RAFT_FLOW='0', EFS_PACKED_WRITES='0', EFS_RAFT_FLOW_EARLY_RETIRE='0',
           EFS_RAFT_BATCH_OVERLAP='0', EFS_RAFT_BATCH_NOOP='0', EFS_RAFT_GROUP_DRAIN='0',
           EFS_RAFT_APPLIED_INTERVAL_MS='10', EFS_BATCH_OBS='0')
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def listening(port):
    try:
        with socket.create_connection(('127.0.0.1', port), .3): return True
    except OSError: return False

def wait(predicate, seconds, label):
    end = time.monotonic() + seconds
    while not predicate():
        assert time.monotonic() < end, label + ' timed out'
        time.sleep(.5)

def accepted(folder):
    assert not (folder / 'failure.txt').exists(), str(folder) + ' failed; do not stop service'
    status = folder / 'run-exit.txt'
    if not status.exists(): return False
    assert status.read_text().strip() == '0', str(folder) + ' not accepted'
    return True

def run(name, command, seconds=1800):
    print('START', name, time.strftime('%FT%TZ', time.gmtime()), flush=True)
    (out / (name + '-command.json')).write_text(json.dumps(list(map(str, command)), indent=2))
    with (out / (name + '.log')).open('w') as log:
        process = subprocess.Popen(list(map(str, command)), cwd=src, env=env, stdout=log, stderr=subprocess.STDOUT)
        try: rc = process.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            (out / (name + '-timeout.txt')).write_text('Fixture deadline; request normal fixture cleanup. No RPC timeout widened.\n')
            process.terminate()
            rc = process.wait(timeout=180)
    (out / (name + '-exit.txt')).write_text(str(rc) + '\n')
    print('END', name, rc, flush=True)
    return rc

def preserved():
    prior = json.loads((Path('/data1/efs/devel-nuc-20261011/clock-cold/after.json')).read_text())['payloads']
    # Later W52 deployment also retains these earlier bytes, plus new checks.
    checked = []
    for row in prior:
        path = Path(row['path'])
        assert digest(path) == row['sha256'], str(path)
        checked.append(row)
    return checked

stopped = False
try:
    native=base/('native' if args.variant=='catchup-base' else 'baseline-native')
    assert accepted(native), 'full normal/fault native prerequisites'
    assert (base/'sanitizers/run-exit.txt').read_text().strip()=='0'
    inputs=json.loads((base/'v030-b239dc7d-inputs.json').read_text())
    if args.variant=='catchup-root':
        for n,row in json.loads((base/'w97-baseline-inputs.json').read_text()).items():inputs[n]=row['baseline_sha256']
        assert (base/'baseline-san-exit.txt').read_text().strip()=='0'
    for n,h in inputs.items():assert digest(src/n)==h,n
    images=json.loads((native/'fault-images.json').read_text())
    assert all(digest(src/n)==r['sha256'] for n,r in images.items())
    regular = json.loads((Path('/data1/efs/devel-nuc-20261011/clock-install/images.json')).read_text())
    assert all(digest(installed / name) == row['sha256'] for name, row in regular.items())
    assert all(listening(p) for p in range(17432, 17436))
    checked = preserved()
    assert len(checked)==36, 'preserved payload inventory changed'
    assert Path('/data1/efs/.mkfs-done').is_file()
    (out / 'before.json').write_text(json.dumps(dict(images=images, regular_images=regular,
                                                     preserved_payloads=checked), indent=2))
    # Only idle regular service may be stopped. All previous long writers must
    # have their exact fsync/cold verdict before this prerequisite is published.
    processes = subprocess.check_output(['ps', '-eo', 'pid,args'], text=True)
    assert not any(re.search(r'^\s*\d+\s+(?:python3|/usr/bin/python3) .*?(tiny_files.py|balanced-load.py|drive-load.py)',
                            line) for line in processes.splitlines()), 'unrelated generator active'
    pids = subprocess.check_output(['pgrep', '-f', '^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'], text=True).split()
    assert len(pids) == 4
    assert all(digest(Path('/proc', pid, 'exe')) == regular['efsd']['sha256'] for pid in pids)
    stopped = True
    for mount in ('mnt', 'mnt2'):
        assert run(mount + '-stop', [installed / 'scripts/client.sh', 'stop', '/data1/efs/' + mount], 180) == 0
        assert not os.path.ismount('/data1/efs/' + mount)
    for pid in pids:
        fd = os.pidfd_open(int(pid)); signal.pidfd_send_signal(fd, signal.SIGTERM); os.close(fd)
    wait(lambda: all(not Path('/proc', pid).exists() for pid in pids), 180, 'regular daemon drain')
    assert subprocess.run(['pgrep', '-x', 'efsd'], stdout=subprocess.DEVNULL).returncode == 1
    common = [src / 'tests/live/gc_reclamation.py', '--transport', 'tcp', '--port', '26590',
              '--mount', '/data1/efs/mnt', '--peer-mount', '/data1/efs/mnt2',
              '--root', '/data1/efs', '--root', '/home/efs']
    codes = {}
    for mode in ('direct', 'buffered'):
        codes['ordinary-' + mode] = run('ordinary-' + mode, ['python3', *common,
            '--mode', mode, '--fixture-quota', '4G', '--ordinary-overwrite-only'])
        assert codes['ordinary-' + mode] == 0, (mode, codes)
        codes['packing-' + mode] = run('packing-' + mode, ['python3', *common, '--mode', mode, '--fixture-quota', '4G', '--mounted-packing-only'])
        assert codes['packing-' + mode] == 0, (mode,codes)
        codes['full-' + mode] = run('full-' + mode, ['python3', *common, '--mode', mode,
            '--append-burst', '2000', '--fresh-namespace', '--posix', '--continue-posix-failure',
            '--sync-writes', '--sync-write-fault', '--append-sync-boundary', '--append-replay-race',
            '--append-audit', '--stop-dirty-recovery', '--shutdown-mutations', '--wal-tail-recovery'])
        assert codes['full-' + mode] == 0, (mode, codes)
        if args.variant == 'catchup-base':
            # Complete the actual 20k unique 4-KiB O_APPEND/fsync/peer/cold gate
            # in each storage mode. The outer fixture budget is independent of
            # the unchanged client request deadline.
            codes['long-' + mode] = run('long-' + mode, ['python3', *common, '--mode', mode,
                '--append-burst-only', '--append-burst', '20000'], 10800)
            assert codes['long-' + mode] == 0, (mode, codes)
    (out / 'verdicts.json').write_text(json.dumps(codes, indent=2))
except BaseException:
    (out / 'failure.txt').write_text(traceback.format_exc())
    raise
finally:
    if stopped:
        try:
            assert not any(os.path.ismount('/data1/efs/' + m) for m in ('mnt', 'mnt2')), 'private mounts remain; refuse unsafe restore'
            assert subprocess.run(['pgrep', '-x', 'efsd'], stdout=subprocess.DEVNULL).returncode == 1, 'private daemons remain'
            normal = dict(env, EFS_CLUSTER='nuc', EFS_DIRECT_IO='1', EFS_RAFT_FLOW='1')
            with (out / 'restore.log').open('w') as log:
                rc = subprocess.run(['bash', '/data1/efs/w97-latest-20261010/start-preserved.sh'],
                                    env=normal, stdout=log, stderr=subprocess.STDOUT, timeout=360).returncode
            assert rc == 0
            wait(lambda: all(listening(p) for p in range(17432, 17436)), 180, 'restored actual listeners')
            assert all(os.path.ismount('/data1/efs/' + m) for m in ('mnt', 'mnt2'))
            assert all(digest(installed / name) == row['sha256'] for name, row in regular.items())
            mapped=[]
            for proc in Path('/proc').iterdir():
                if not proc.name.isdigit():continue
                try:
                    path=os.readlink(proc/'exe');kind=Path(path).name
                    if path not in (str(installed/'efsd'),str(installed/'efs-fuse')):continue
                    h=digest(proc/'exe');assert h==regular[kind]['sha256']
                    mapped.append(dict(pid=int(proc.name),kind=kind,sha256=h,args=(proc/'cmdline').read_bytes().decode().split('\0')))
                except (FileNotFoundError,ProcessLookupError,PermissionError):continue
            assert sum(r['kind']=='efsd' for r in mapped)==4 and sum(r['kind']=='efs-fuse' for r in mapped)==2
            (out/'restored-mapped-images.json').write_text(json.dumps(mapped,indent=2))
            (out / 'restored-preserved-payloads.json').write_text(json.dumps(preserved(), indent=2))
            (out / 'restore-exit.txt').write_text('0\n')
        except BaseException:
            failure = traceback.format_exc()
            (out / 'restore-failure.txt').write_text(failure)
            if not (out / 'failure.txt').exists(): (out / 'failure.txt').write_text(failure)
            (out / 'run-exit.txt').write_text('1\n')
            raise
    if not (out / 'failure.txt').exists() and (out / 'restore-exit.txt').exists():
        (out / 'run-exit.txt').write_text('0\n')
