#!/usr/bin/env python3
'Sample foreground and query latency from a separate native FUSE client.\nArguments: RESULTS_DIRECTORY FIXTURE_NAME. Create RESULTS_DIRECTORY/STOP to\nfinish and close all descriptors. Raw JSONL stays outside the mounted export.\nStart after the capacity root exists; preserve any setup failures separately.'
import pathlib,subprocess,json,time,os,sys
out=pathlib.Path(sys.argv[1]);root=pathlib.Path('/tmp/efs-mount')/sys.argv[2];probe=root.parent/(sys.argv[2]+'-latency')
probe.mkdir();seq=0
with (out/'latency.jsonl').open('a',buffering=1) as log:
 while not (out/'STOP').exists():
  for name in ['stat','create_fsync_unlink','stats','find']:
   started=time.monotonic();record=dict(time_ns=time.time_ns(),operation=name,sequence=seq)
   try:
    if name=='stat':record['size']=root.stat().st_size
    elif name=='create_fsync_unlink':
     f=probe/('probe-%09d'%seq)
     with f.open('xb') as stream:stream.write(b'foreground-latency'*29);stream.flush();os.fsync(stream.fileno())
     f.unlink()
    else:
     args=[sys.executable,str(root/'.efs'/name),'--json']
     if name=='find':args=[sys.executable,str(root/'.efs/find'),'file-','--match','prefix','--limit','100','--json']
     r=subprocess.run(args,capture_output=True,timeout=30);record['returncode']=r.returncode
     if r.returncode:record['stderr']=r.stderr.decode(errors='replace')[:2000]
     else:
      data=json.loads(r.stdout);record['snapshot']=data.get('snapshot');record['complete']=data.get('complete');record['stats']=data.get('stats');record['entries']=len(data.get('entries',[]))
    record['result']='PASS' if record.get('returncode',0)==0 else 'FAIL'
   except BaseException as e:record.update(result='FAIL',error=repr(e))
   record['seconds']=time.monotonic()-started;log.write(json.dumps(record)+'\n')
  seq+=1
  for _ in range(30):
   if (out/'STOP').exists():break
   time.sleep(1)
probe.rmdir();(out/'latency-stopped').write_text('normal\n')
