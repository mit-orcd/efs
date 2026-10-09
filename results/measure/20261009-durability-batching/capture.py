import json, os, re, signal, subprocess, time
from pathlib import Path

base = Path('/data1/efs/durability-batching-20261009')
base.mkdir(exist_ok=False)
servers = {}
for pid in subprocess.check_output(['pgrep', '-f', '^/data1/efs/src/efsd .*--port 1743[2-5]( |$)']).decode().split():
    cmd = Path(f'/proc/{pid}/cmdline').read_bytes().replace(b'\0', b' ').decode()
    node = int(re.search(r'--node-id (\d+)', cmd)[1])
    descriptors = [(p.name, os.readlink(p)) for p in Path(f'/proc/{pid}/fd').iterdir()]
    wal = [(fd, path) for fd, path in descriptors if path.endswith('/mdraft/log/raft.log')]
    assert len(wal) == 1
    servers[node] = dict(pid=int(pid), cmd=cmd, fd=wal[0][0], wal=wal[0][1])
assert len(servers) == 4
(base/'identity.json').write_text(json.dumps(dict(servers=servers, version=subprocess.check_output(['/data1/efs/src/efsd', '--version']).decode()), indent=2))
out = '/data1/efs/mnt/tiny-batching-20261009'
assert not Path(out).exists()
progress = open(base/'progress.log', 'w')
loader = subprocess.Popen(['python3', '/home/efs/tiny_files.py', '--max-depth', '8', '--max-files-per-folder', '1000', '--max-files', '50000', '--workers', '8', out], stdout=progress, stderr=progress)
(base/'loader.pid').write_text(str(loader.pid))
start = time.monotonic()
schedule = [(wave, node, 30+wave*100+(node-1)*8) for wave in range(3) for node in range(1,5)]
active = []
offsets = {node: Path(f'/data1/efs/logs/efsd-n{node}.log').stat().st_size for node in servers}
with open(base/'samples.jsonl', 'w') as samples, open(base/'events.jsonl', 'w') as events:
    for tick in range(301):
        due = start+tick
        if due > time.monotonic(): time.sleep(due-time.monotonic())
        for wave, node, when in schedule:
            if tick == when:
                s = servers[node]
                err = open(base/f'w{wave}-n{node}-strace.log', 'w')
                args = ['timeout', '-s', 'INT', '6', 'strace', '-f', '-ttt', '-T', '-yy', '-xx', '-s', '16', '--trace-fds='+s['fd'], '-e', 'trace=pwrite64,fdatasync,fsync,fallocate,ftruncate', '-p', str(s['pid']), '-o', str(base/f'w{wave}-n{node}.trace')]
                active.append((subprocess.Popen(args, stderr=err),err))
                events.write(json.dumps(dict(seconds=time.monotonic()-start,wave=wave,node=node,event='trace-start',args=args))+'\n');events.flush()
        if tick % 5 == 0:
            sample = dict(seconds=time.monotonic()-start,utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),diskstats=Path('/proc/diskstats').read_text(),meminfo=Path('/proc/meminfo').read_text(),processes={})
            for pid in [s['pid'] for s in servers.values()]+[loader.pid]:
                try:sample['processes'][pid] = {name:Path(f'/proc/{pid}/{name}').read_text() for name in ['stat','io','status']}
                except FileNotFoundError:pass
            matches = list(re.finditer(r'([\d,]+)/[\d,]+ files.*?([\d.]+) s, ([\d,]+) files/s', (base/'progress.log').read_text()))
            if matches:
                m=matches[-1];sample['progress']=dict(created=int(m[1].replace(',','')),elapsed=float(m[2]),cumulative_rate=int(m[3].replace(',','')))
            samples.write(json.dumps(sample)+'\n');samples.flush()
for proc, err in active:
    rc=proc.wait();err.close()
    assert rc in (0,124), rc
if loader.poll() is None:loader.send_signal(signal.SIGINT)
(base/'capture-complete.json').write_text(json.dumps(dict(seconds=time.monotonic()-start,loader_pid=loader.pid)))
for node, off in offsets.items():
    with open(f'/data1/efs/logs/efsd-n{node}.log','rb') as f:
        f.seek(off);(base/f'n{node}-window.log').write_bytes(f.read())
# Executor context drains queued tasks on KeyboardInterrupt; never force kill.
rc=loader.wait();progress.close()
(base/'loader-exit.json').write_text(json.dumps(dict(returncode=rc,seconds=time.monotonic()-start)))
print(base, flush=True)
