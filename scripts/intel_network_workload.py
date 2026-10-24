#!/usr/bin/env python3
"""Bounded Intel cross-host correctness/traffic diagnostic; never restart servers.
Run: python3 scripts/intel_network_workload.py run --run-id ID --output /outside/git
Remote files and raw telemetry live under /data1/efs/network-check/ID.
Only uniquely owned fixtures are created/removed. Failed fixtures are retained.
"""
import argparse, concurrent.futures, hashlib, json, os, pathlib, re, shlex
import subprocess, sys, time
HOSTS=['intel1_efs','intel2_efs']
IPS=['10.1.0.208','10.1.0.210']
FLAGS={'EFS_TRANSPORT':'tcp','EFS_COMMANDS':'0','EFS_COMMANDS_SERVER':'0',
       'EFS_EXPIRY':'0','EFS_RPC_PROF':'1'}
def command(args,**kwargs):
    return subprocess.run(args,check=True,**kwargs)
def dump(path,value):
    pathlib.Path(path).write_text(json.dumps(value,indent=2)+'\n')
def ssh(host,args,**kwargs):
    return command(['ssh',host,shlex.join([str(x) for x in args])],**kwargs)
def counters():
    out={}
    for name in ('snmp','netstat'):
        lines=pathlib.Path('/proc/net/'+name).read_text().splitlines()
        for i in range(0,len(lines)-1,2):
            keys=lines[i].split();values=lines[i+1].split()
            for key,value in zip(keys[1:],values[1:]):
                if key in ('RetransSegs','OutSegs','InErrs','OutRsts','TCPTimeouts',
                           'TCPLostRetransmit','TCPFastRetrans','TCPSynRetrans',
                           'TCPSpuriousRTOs','TCPAbortOnTimeout'):
                    out[keys[0]+key]=int(value)
    for name in ('rx_packets','tx_packets','rx_errors','tx_errors','rx_dropped','tx_dropped'):
        out['nic:'+name]=int(pathlib.Path('/sys/class/net/enp128s0/statistics/'+name).read_text())
    return out
def driver_stats():
    text=subprocess.check_output(['ethtool','-S','enp128s0'],text=True,timeout=5)
    return {k:int(v) for k,v in re.findall(r'^\s*([\w]+):\s*(\d+)\s*$',text,re.M)}
def sockets(peer):
    text=subprocess.check_output(['ss','-tinHe','dst',peer],text=True,timeout=5)
    rows={};lines=text.splitlines()
    for i,line in enumerate(lines):
        fields=line.split()
        if len(fields)<5 or fields[0]!='ESTAB':continue
        if not any(fields[j].rsplit(':',1)[-1] in ('17432','17433') for j in (3,4)):continue
        detail=lines[i+1] if i+1<len(lines) else ''
        ino=re.search(r'ino:(\d+)',line)
        key=fields[3]+'>'+fields[4]+'#'+(ino.group(1) if ino else '?')
        row={k:int(v) for k,v in re.findall(r'\b(bytes_sent|bytes_received|bytes_retrans|segs_out|data_segs_out):(\d+)',detail)}
        retrans=re.search(r'\bretrans:(\d+)/(\d+)',detail)
        row['retrans_total']=int(retrans.group(2)) if retrans else 0
        row['send_q']=int(fields[2]);row['recv_q']=int(fields[1])
        rtt=re.search(r'\brtt:([\d.]+)/',detail)
        if rtt:row['rtt_ms']=float(rtt.group(1))
        rows[key]=row
    return rows
def observe(root,peer):
    dump(root/'observer-start.json',{'time':time.time(),'driver':driver_stats()})
    (root/'observer.pid').write_text(str(os.getpid()))
    with (root/'network.ndjson').open('w') as log:
        while True:
            row={'time':time.time(),'phase':(root/'phase').read_text().strip()}
            try:row.update(counters=counters(),sockets=sockets(peer))
            except Exception as e:row['error']=repr(e)
            log.write(json.dumps(row)+'\n');log.flush()
            if (root/'stop-observer').exists():break
            time.sleep(1)
    dump(root/'observer-end.json',{'time':time.time(),'driver':driver_stats()})
def block(owner,index,size):
    seed=hashlib.sha256(('intel-network-v1:'+owner+':'+str(index)).encode()).digest()
    return seed*(size//len(seed))
def worker(root,run_id,host_index,phase):
    mount=root/'mnt';assert os.path.ismount(mount)
    assert not (mount/'.efs').exists()
    owner=HOSTS[host_index if phase.endswith('write') else 1-host_index]
    large=phase.startswith('large');count=16 if large else 10000
    size=64*1024*1024 if large else 4096
    directory=mount/('net-'+run_id+'-'+owner+('-large' if large else '-small'))
    writing=phase.endswith('write')
    if writing:
        directory.mkdir() # Exclusive ownership: never reuse a previous run.
        if not large:
            for bucket in range(64):(directory/('%02d'%bucket)).mkdir()
    process=None
    for proc in pathlib.Path('/proc').iterdir():
        if not proc.name.isdigit():continue
        try:
            args=[x.decode() for x in (proc/'cmdline').read_bytes().split(b'\0') if x]
            if not args or pathlib.Path(args[0]).name!='efs-fuse' or str(mount) not in args:continue
            env=dict(x.decode().split('=',1) for x in (proc/'environ').read_bytes().split(b'\0') if b'=' in x)
            for key,value in FLAGS.items():assert env.get(key)==value,(key,env.get(key))
            process={'pid':int(proc.name),'args':args,'features':{k:env[k] for k in FLAGS}}
        except (FileNotFoundError,ProcessLookupError,PermissionError):continue
    assert process,'diagnostic client missing'
    started=time.time();errors=[];verified=0
    def one(index):
        path=directory/('f%05d'%index) if large else directory/('%02d'%(index%64))/('f%05d'%index)
        payload=block(owner,index,256*1024 if large else 4096)
        try:
            if writing:
                with path.open('xb',buffering=0) as stream:
                    remaining=size
                    while remaining:
                        chunk=memoryview(payload)[:min(remaining,len(payload))]
                        length=len(chunk)
                        while chunk:
                            written=stream.write(chunk)
                            if not written:raise OSError('zero write')
                            chunk=chunk[written:]
                        remaining-=length
                    os.fsync(stream.fileno())
                return {'index':index,'ok':True}
            with path.open('rb',buffering=0) as stream:
                remaining=size
                while remaining:
                    expected=payload[:min(remaining,len(payload))]
                    got=bytearray()
                    while len(got)<len(expected):
                        part=stream.read(len(expected)-len(got))
                        if not part:raise AssertionError('early EOF')
                        got.extend(part)
                    if got!=expected:raise AssertionError('payload mismatch')
                    remaining-=len(expected)
                if stream.read(1):raise AssertionError('unexpected extra bytes')
            if not large:
                renamed=path.with_suffix('.renamed');os.rename(path,renamed);renamed.unlink()
            else:path.unlink()
            return {'index':index,'ok':True}
        except Exception as e:return {'index':index,'ok':False,'error':repr(e)}
    with concurrent.futures.ThreadPoolExecutor(4) as pool:
        for result in pool.map(one,range(count)):
            if result['ok']:verified+=1
            else:errors.append(result)
    if not writing and not errors:
        if not large:
            for bucket in range(64):(directory/('%02d'%bucket)).rmdir()
        directory.rmdir()
        assert not directory.exists()
    # Force a later uncached lookup so the 2s RPC profiler publishes its tail.
    time.sleep(3)
    try:os.stat(mount/('net-'+run_id+'-'+phase+'-tail-does-not-exist'))
    except FileNotFoundError:pass
    else:raise AssertionError('exclusive profiler marker unexpectedly exists')
    value={'phase':phase,'owner':owner,'started':started,'finished':time.time(),
           'seconds':time.time()-started,'count':count,'size':size,'workers':4,
           'ok':verified,'errors':errors,'logical_bytes':verified*size,'process':process}
    dump(root/(phase+'.json'),value);print(json.dumps(value),flush=True)
    return int(bool(errors))
def summarize(root):
    samples=[json.loads(x) for x in (root/'network.ndjson').read_text().splitlines()]
    good=[x for x in samples if 'counters' in x and 'sockets' in x]
    assert len(good)>1
    groups={};previous=good[0]
    for current in good[1:]:
        group=groups.setdefault(current['phase'],{'seconds':0,'host_delta':{},'efs_socket_delta':{},'new_sockets':0,'disappeared_sockets':0,'max_rtt_ms':0,'max_send_q':0})
        group['seconds']+=current['time']-previous['time']
        for key,val in current['counters'].items():
            group['host_delta'][key]=group['host_delta'].get(key,0)+val-previous['counters'][key]
        group['disappeared_sockets']+=len(previous['sockets'].keys()-current['sockets'].keys())
        for key,val in current['sockets'].items():
            old=previous['sockets'].get(key)
            if old is None:group['new_sockets']+=1;old={}
            for metric in ('bytes_sent','bytes_received','bytes_retrans','retrans_total','segs_out','data_segs_out'):
                delta=val.get(metric,0)-old.get(metric,0)
                if delta<0:raise AssertionError('socket counter regressed: '+key)
                group['efs_socket_delta'][metric]=group['efs_socket_delta'].get(metric,0)+delta
            group['max_rtt_ms']=max(group['max_rtt_ms'],val.get('rtt_ms',0))
            group['max_send_q']=max(group['max_send_q'],val['send_q'])
        previous=current
    begin=json.loads((root/'observer-start.json').read_text());end=json.loads((root/'observer-end.json').read_text())
    value={'samples':len(samples),'sample_errors':[x for x in samples if 'error' in x],
           'phases':groups,'driver_delta':{k:end['driver'][k]-v for k,v in begin['driver'].items()},'rpc':{}}
    for phase in ('large-write','large-read','small-write','small-read'):
        path=root/(phase+'-fuse.log')
        if not path.exists():continue
        text=path.read_text(errors='replace')
        profiles=re.findall(r'RPC-PROF calls=(\d+).*?busy_n=(\d+).*?checkout_wait_n=(\d+)',text)
        value['rpc'][phase]={'last_profile':list(map(int,profiles[-1])) if profiles else None,
             'logged_transport_retries':len(re.findall(r'inode-rpc: retry .*why=(send|recv)',text)),
             'slow_ok_retried':len(re.findall(r'inode-rpc: slow-ok .*attempts=(?:[2-9]|[1-9][0-9]+)\b',text)),
             'retry_exhaustion_lines':len(re.findall(r'exhausted|-> EBUSY|stop-refused|UNMOUNT DATA LOSS',text))}
    dump(root/'summary.json',value)
    return value

def run(a):
    local=a.output;local.mkdir(parents=True,exist_ok=True)
    remote='/data1/efs/network-check/'+a.run_id
    script=remote+'/driver.py';active=set();observers=set()
    def both(fn):
        with concurrent.futures.ThreadPoolExecutor(2) as pool:return list(pool.map(fn,range(2)))
    def control(i,name,value=''):
        return ssh(HOSTS[i],['python3',script,'control','--run-id',a.run_id,'--name',name,'--value',value],capture_output=True,text=True)
    def profile(i,stage):
        result=ssh(HOSTS[i],['python3',remote+'/verify.py'],capture_output=True,text=True)
        (local/(HOSTS[i]+'-'+stage+'-profile.json')).write_text(result.stdout)
    try:
        for i,host in enumerate(HOSTS):
            ssh(host,['mkdir','-p',remote])
            ssh(host,['test','!','-e',remote+'/driver.py'])
            command(['scp',__file__,host+':'+script])
            command(['scp',str(pathlib.Path(__file__).with_name('intel_package3_verify.py')),host+':'+remote+'/verify.py'])
            profile(i,'before')
            identity=ssh(host,['python3',script,'identity','--run-id',a.run_id],capture_output=True,text=True)
            (local/(host+'-identity.json')).write_text(identity.stdout)
            control(i,'phase','baseline')
            args=['python3',script,'observe','--run-id',a.run_id,'--index',str(i)]
            shell='nohup '+shlex.join(args)+' >'+shlex.quote(remote+'/observer.log')+' 2>&1 </dev/null &'
            command(['ssh',host,shell]);observers.add(i)
        time.sleep(10)
        failures=[]
        for phase in ('large-write','large-read','small-write','small-read'):
            both(lambda i:control(i,'phase',phase+'-setup'))
            for i,host in enumerate(HOSTS):
                seed=IPS[1-i]+':17432'
                env=['env']+[k+'='+v for k,v in FLAGS.items()]+['EFS_FUSE_LOG='+remote+'/'+phase+'-fuse.log']
                active.add(i)
                start=ssh(host,env+['/data1/efs/src/scripts/client.sh',seed,remote+'/mnt'],capture_output=True,text=True,timeout=90)
                (local/(host+'-'+phase+'-mount.log')).write_text(start.stdout+start.stderr)
            both(lambda i:control(i,'phase',phase))
            def work(i):
                with (local/(HOSTS[i]+'-'+phase+'.log')).open('w') as log:
                    result=subprocess.run(['ssh',HOSTS[i],shlex.join(['timeout','--signal=TERM','--kill-after=10s','900s','python3',script,'worker','--run-id',a.run_id,'--index',str(i),'--phase',phase])],stdout=log,stderr=subprocess.STDOUT,timeout=930)
                command(['scp',HOSTS[i]+':'+remote+'/'+phase+'.json',str(local/(HOSTS[i]+'-'+phase+'.json'))])
                return result.returncode
            results=both(work)
            both(lambda i:control(i,'phase',phase+'-shutdown'))
            for i,host in enumerate(HOSTS):
                stopped=ssh(host,['/data1/efs/src/scripts/client.sh','stop',remote+'/mnt'],capture_output=True,text=True,timeout=90)
                (local/(host+'-'+phase+'-stop.log')).write_text(stopped.stdout+stopped.stderr);active.remove(i)
            if any(results):failures.append({'phase':phase,'exit_codes':results})
        both(lambda i:control(i,'phase','final-quiet'));time.sleep(3)
    finally:
        for i in list(active):
            try:ssh(HOSTS[i],['/data1/efs/src/scripts/client.sh','stop',remote+'/mnt'],timeout=90);active.remove(i)
            except Exception as e:print('Normal stop failed; mount retained:',HOSTS[i],repr(e),file=sys.stderr)
        for i in observers:
            control(i,'stop-observer')
        time.sleep(3)
        for i in observers:
            result=ssh(HOSTS[i],['python3',script,'summary','--run-id',a.run_id],capture_output=True,text=True)
            (local/(HOSTS[i]+'-summary.json')).write_text(result.stdout)
            raw=local/(HOSTS[i]+'-raw');raw.mkdir(exist_ok=True)
            for pattern in ('*.json','*.log','network.ndjson'):
                command(['scp',HOSTS[i]+':'+remote+'/'+pattern,str(raw)])
        if not active:both(lambda i:profile(i,'after'))
    if active:raise RuntimeError('Diagnostic mounts retained; no force cleanup')
    if failures:raise RuntimeError('Workload failures: '+str(failures))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=['run','worker','observe','summary','control','identity'])
    p.add_argument('--run-id',required=True);p.add_argument('--output',type=pathlib.Path);p.add_argument('--index',type=int,choices=[0,1],default=0)
    p.add_argument('--phase',choices=['large-write','large-read','small-write','small-read']);p.add_argument('--name',choices=['phase','stop-observer']);p.add_argument('--value',default='')
    a=p.parse_args();assert re.fullmatch(r'[A-Za-z0-9_-]{1,80}',a.run_id)
    root=pathlib.Path('/data1/efs/network-check')/a.run_id
    if a.mode=='run':assert a.output;run(a)
    elif a.mode=='worker':return worker(root,a.run_id,a.index,a.phase)
    elif a.mode=='observe':observe(root,IPS[1-a.index])
    elif a.mode=='summary':print(json.dumps(summarize(root)))
    elif a.mode=='control':(root/a.name).write_text(a.value+'\n')
    else:
        out={}
        for name in ('efsd','efs-fuse'):
            path=pathlib.Path('/data1/efs/src')/name
            out[name]={'version':subprocess.check_output([str(path),'--version'],text=True).strip(),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
        out['route']=subprocess.check_output(['ip','route','get',IPS[1-a.index]],text=True).strip()
        out['link']=subprocess.check_output(['ethtool','enp128s0'],text=True)
        print(json.dumps(out))
    return 0
if __name__=='__main__':sys.exit(main())
