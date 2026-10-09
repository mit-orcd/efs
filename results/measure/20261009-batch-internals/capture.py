import json,os,re,signal,subprocess,time
from pathlib import Path
base=Path('/data1/efs/batch-internals-20261009')
assert (base/'deployed-version.txt').exists()
servers={}
for pid in subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)']).decode().split():
    cmd=Path(f'/proc/{pid}/cmdline').read_bytes().replace(b'\0',b' ').decode()
    node=int(re.search(r'--node-id (\d+)',cmd)[1]);servers[node]=dict(pid=int(pid),cmd=cmd)
assert len(servers)==4
(base/'identity.json').write_text(json.dumps(servers,indent=2))
logs={n:Path(f'/data1/efs/logs/efsd-n{n}.log') for n in servers}
def records(raw):
    found=[]
    for line in raw.decode(errors='replace').splitlines():
        if 'batch-obs {' not in line:continue
        try:found.append(json.loads(line.split('batch-obs ',1)[1]))
        except json.JSONDecodeError:pass
    return found
# Ensure every daemon is emitting diagnostics before starting the workload.
for n,p in logs.items():
    with p.open('rb') as f:f.seek(max(0,p.stat().st_size-131072));r=records(f.read())
    assert r,f'node {n}: no diagnostic output'
    (base/f'baseline-n{n}.json').write_text(json.dumps(r[-1]))
offsets={n:p.stat().st_size for n,p in logs.items()}
progress=open(base/'progress.log','w')
out='/data1/efs/mnt/tiny-batch-internals-20261009'
assert not Path(out).exists()
args=['python3','/data1/efs/batch-internals-loader.py','--max-depth','8','--max-files-per-folder','1000','--max-files','50000','--workers','8',out]
loader=subprocess.Popen(args,stdout=progress,stderr=progress)
(base/'loader.pid').write_text(str(loader.pid));(base/'loader-command.json').write_text(json.dumps(args))
start=time.monotonic()
(base/'timebase.json').write_text(json.dumps(dict(start_mono_us=int(start*1000000),duration_seconds=300)))
with open(base/'samples.jsonl','w') as f:
    for i in range(61):
        due=start+i*5
        if due>time.monotonic():time.sleep(due-time.monotonic())
        s=dict(seconds=time.monotonic()-start,utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),processes={},diskstats=Path('/proc/diskstats').read_text(),meminfo=Path('/proc/meminfo').read_text())
        for pid in [s['pid'] for s in servers.values()]+[loader.pid]:
            try:s['processes'][pid]={name:Path(f'/proc/{pid}/{name}').read_text() for name in ['stat','io','status']}
            except FileNotFoundError:pass
        m=list(re.finditer(r'([\d,]+)/[\d,]+ files.*?([\d.]+) s, ([\d,]+) files/s',(base/'progress.log').read_text()))
        s['progress']=dict(created=int(m[-1][1].replace(',','')),elapsed=float(m[-1][2]),cumulative_rate=int(m[-1][3].replace(',',''))) if m else dict(created=0,elapsed=0,cumulative_rate=0)
        f.write(json.dumps(s)+'\n');f.flush()
loader.send_signal(signal.SIGUSR1)
rc=loader.wait(timeout=180);progress.close()
(base/'loader-exit.json').write_text(json.dumps(dict(returncode=rc,seconds=time.monotonic()-start)))
assert rc==0
# Let the pump publish final counters after foreground operations have drained.
time.sleep(6)
for n,p in logs.items():
    with p.open('rb') as f:f.seek(offsets[n]);raw=f.read()
    (base/f'n{n}-window.log').write_bytes(raw)
    with open(base/f'n{n}-observations.jsonl','w') as f:
        for r in records(raw):f.write(json.dumps(r)+'\n')
(base/'complete.json').write_text(json.dumps(dict(seconds=300,loader_exited=True,servers=servers)))
print(base,flush=True)
