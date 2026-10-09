import hashlib,json,os,re,subprocess,time
from pathlib import Path

base=Path('/data1/efs/durability-tracepoints-20261009')
assert not (base/'perf.data').exists()
servers={};handles={}
for pid in subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)']).decode().split():
    cmd=Path(f'/proc/{pid}/cmdline').read_bytes().replace(b'\0',b' ').decode()
    node=int(re.search(r'--node-id (\d+)',cmd)[1])
    wal=[(int(p.name),os.readlink(p)) for p in Path(f'/proc/{pid}/fd').iterdir() if os.readlink(p).endswith('/mdraft/log/raft.log')]
    assert len(wal)==1
    fd,path=wal[0];handles[node]=os.open(path,os.O_RDONLY)
    servers[node]=dict(pid=int(pid),cmd=cmd,fd=fd,wal=path,inode=os.fstat(handles[node]).st_ino)
assert len(servers)==4
loader=2519952
assert Path(f'/proc/{loader}').exists()
progress=Path('/data1/efs/durability-batching-20261009/progress.log')
identity=dict(servers=servers,loader=loader,version=subprocess.check_output(['/data1/efs/src/efsd','--version']).decode(),method='existing workload, no strace, no restart')
(base/'identity.json').write_text(json.dumps(identity,indent=2))
args=['perf','record','-m','4M','-o',str(base/'perf.data'),'-e','syscalls:sys_enter_pwrite64','--filter',' || '.join('fd == '+str(s['fd']) for s in servers.values())]
for event in ['sys_exit_pwrite64','sys_enter_fdatasync','sys_exit_fdatasync','sys_enter_fsync','sys_exit_fsync']:args+=['-e','syscalls:'+event]
args+=['-p',','.join(str(s['pid']) for s in servers.values()),'--','sleep','300']
(base/'command.json').write_text(json.dumps(args))
log=open(base/'record.log','w');perf=subprocess.Popen(args,stderr=log)
start=time.monotonic()
with open(base/'samples.jsonl','w') as f:
    for i in range(61):
        due=start+i*5
        if due>time.monotonic():time.sleep(due-time.monotonic())
        s=dict(seconds=time.monotonic()-start,utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),processes={},diskstats=Path('/proc/diskstats').read_text(),meminfo=Path('/proc/meminfo').read_text())
        for pid in [s['pid'] for s in servers.values()]+[loader]:
            try:s['processes'][pid]={n:Path(f'/proc/{pid}/{n}').read_text() for n in ['stat','io','status']}
            except FileNotFoundError:pass
        m=list(re.finditer(r'([\d,]+)/[\d,]+ files.*?([\d.]+) s, ([\d,]+) files/s',progress.read_text()))
        if m:s['progress']=dict(created=int(m[-1][1].replace(',','')),elapsed=float(m[-1][2]),cumulative_rate=int(m[-1][3].replace(',','')))
        f.write(json.dumps(s)+'\n');f.flush()
rc=perf.wait();log.close();assert rc==0,rc
(base/'progress.log').write_bytes(progress.read_bytes())
with open(base/'perf-script.txt','w') as f,open(base/'script.log','w') as err:
    subprocess.run(['perf','script','-i',str(base/'perf.data'),'-F','comm,pid,tid,time,event,trace'],stdout=f,stderr=err,check=True)
# Keep original WAL inodes open so header verification cannot accidentally read
# a replacement. Record descriptor identity after capture and flag rotation.
end={}
for n,s in servers.items():
    end[n]=dict(path=os.readlink(f'/proc/{s["pid"]}/fd/{s["fd"]}'),inode=os.stat(f'/proc/{s["pid"]}/fd/{s["fd"]}').st_ino)
(base/'end-identity.json').write_text(json.dumps(end,indent=2))
headers={n:{} for n in servers}
for line in (base/'perf-script.txt').open():
    m=re.search(r'\s(\d+)/(\d+)\s+[\d.]+:\s+syscalls:sys_enter_pwrite64:\s+fd: (0x[\da-f]+),.*count: (0x[\da-f]+), pos: (0x[\da-f]+)',line)
    if not m:continue
    pid,fd,off=int(m[1]),int(m[3],16),int(m[5],16)
    for n,s in servers.items():
        if s['pid']==pid and s['fd']==fd and off not in headers[n]:headers[n][off]=os.pread(handles[n],13,off).hex()
(base/'wal-headers.json').write_text(json.dumps(headers))
for fd in handles.values():os.close(fd)
with open(base/'perf.data','rb') as f:
    digest=hashlib.file_digest(f,'sha256').hexdigest()
(base/'complete.json').write_text(json.dumps(dict(record_returncode=rc,seconds=300,samples=len((base/'samples.jsonl').read_text().splitlines()),raw_perf_bytes=(base/'perf.data').stat().st_size,raw_perf_sha256=digest,raw_perf_path=str(base/'perf.data'))))
print(base,flush=True)
