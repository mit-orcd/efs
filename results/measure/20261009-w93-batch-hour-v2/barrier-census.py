"""Post-comparison 30s syscall census; traced rates are not A/B performance."""
import collections, gzip, json, os, re, signal, subprocess, time
from pathlib import Path

base = Path(__file__).resolve().parent / 'barrier-census'
base.mkdir()
# Open the lazy per-group checkpoint descriptors before taking their identity.
warm = Path('/data1/efs/mnt/tiny-w93-census-warmup-20261009')
warm.mkdir()
for i in range(32):
    directory = warm / str(i)
    directory.mkdir()
    with (directory / 'file').open('xb', buffering=0) as f:
        f.write(b'warmup'); os.fsync(f.fileno())
time.sleep(1)
servers = {}
for pid in subprocess.check_output(['pgrep', '-f', '^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'], text=True).split():
    cmd = Path(f'/proc/{pid}/cmdline').read_bytes().replace(b'\0', b' ').decode()
    node = int(re.search(r'--node-id (\d+)', cmd)[1])
    fds = {}
    for fd in Path(f'/proc/{pid}/fd').iterdir():
        try:
            path = os.readlink(fd)
            if '/mdraft/' in path and ('/kv/wal' in path or '/log/raft.log' in path or '/applied.' in path):
                fds[int(fd.name)] = dict(path=path, inode=fd.stat().st_ino,
                    kind='raft' if path.endswith('/raft.log') else 'kv' if path.endswith('/wal') else 'marker')
        except FileNotFoundError:
            pass
    assert any(v['kind'] == 'kv' for v in fds.values()), fds
    assert sum(v['kind'] == 'marker' for v in fds.values()) == (2 if node in (2, 3) else 1), 'marker descriptors not open; census incomplete'
    servers[int(pid)] = dict(node=node, cmd=cmd, fds=fds)
assert len(servers) == 4
(base / 'identity.json').write_text(json.dumps(servers, indent=2))
args = ['perf', 'record', '-m', '4M', '-o', str(base / 'perf.data')]
for event in ('sys_enter_fdatasync', 'sys_exit_fdatasync', 'sys_enter_fsync', 'sys_exit_fsync'):
    args += ['-e', 'syscalls:' + event]
args += ['-p', ','.join(str(pid) for pid in servers), '--', 'sleep', '30']
(base / 'command.json').write_text(json.dumps(args))
with (base / 'record.log').open('w') as err, (base / 'progress.log').open('w') as out:
    perf = subprocess.Popen(args, stderr=err)
    time.sleep(.5)
    loader = subprocess.Popen(['python3', str(base.parent / 'loader-latency.py'), '--max-depth', '8',
        '--max-files-per-folder', '1000', '--max-files', '2000000', '--workers', '8',
        '/data1/efs/mnt/tiny-w93-barrier-census-20261009'], stdout=out, stderr=out,
        env=dict(os.environ, W97_LATENCY_PATH=str(base / 'latency.json')))
    rc = perf.wait()
    loader.send_signal(signal.SIGUSR1)
    loader_rc = loader.wait(timeout=180)
assert rc == 0 and loader_rc == 0, (rc, loader_rc)
end = {}
for pid, server in servers.items():
    end[pid] = {fd: dict(path=os.readlink(f'/proc/{pid}/fd/{fd}'), inode=os.stat(f'/proc/{pid}/fd/{fd}').st_ino)
        for fd in server['fds']}
    for fd, identity in server['fds'].items():
        assert end[pid][fd]['path'] == identity['path'] and end[pid][fd]['inode'] == identity['inode'], 'FD changed; census invalid'
(base / 'end-identity.json').write_text(json.dumps(end, indent=2))
raw = subprocess.check_output(['perf', 'script', '-i', str(base / 'perf.data'), '-F', 'comm,pid,tid,time,event,trace'], text=True)
with gzip.open(base / 'perf-script.txt.gz', 'wt') as f:
    f.write(raw)
pending = {}; counts = collections.Counter(); times = collections.Counter(); errors = collections.Counter(); unmatched = 0
for line in raw.splitlines():
    if 'LOST' in line.upper():
        raise RuntimeError('lost perf events; census invalid')
    m = re.search(r'\s(\d+)/(\d+)\s+(\d+\.\d+):\s+syscalls:sys_(enter|exit)_(\w+):\s+(.*)', line)
    if not m:
        continue
    pid, tid, stamp = int(m[1]), int(m[2]), float(m[3])
    key = (pid, tid, m[5])
    if m[4] == 'enter':
        pending[key] = (stamp, int(re.search(r'fd: (0x[\da-f]+)', m[6])[1], 16))
    elif key in pending:
        start, fd = pending.pop(key)
        if fd not in servers[pid]['fds']:
            continue
        rc = int(m[6].split()[0], 16)
        label = f"n{servers[pid]['node']}:{servers[pid]['fds'][fd]['kind']}:{m[5]}"
        if rc != 0:
            errors[label] += 1
        else:
            counts[label] += 1; times[label] += (stamp - start) * 1000
    else:
        unmatched += 1
summary = dict(successful_sync_calls=counts, total_sync_ms=times, errors=errors,
    unmatched_exits=unmatched, unmatched_enters=len(pending), duration_seconds=30,
    note='Actual syscall counts; retained background work included. Tracing adds overhead; no per-file exact transaction attribution.')
(base / 'summary.json').write_text(json.dumps(summary, indent=2))
print(json.dumps(summary, indent=2))
