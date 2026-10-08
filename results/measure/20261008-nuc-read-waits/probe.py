import subprocess,time,json
exe='/data1/efs/src/efs-mgmt';out={'started':time.time(),'before':{},'after':{},'samples':[]}
for p in range(17432,17436):out['before'][p]=subprocess.check_output([exe,'raft-status',f'127.0.0.1:{p}'],text=True)
for r in range(12):
 for ino,g in ([(11434383,0),(1946256,2)] if r%2==0 else [(1946256,2),(11434383,0)]):
  for port in (17432,17433):
   t=time.monotonic();x=subprocess.run([exe,'raft-getattr',f'127.0.0.1:{port}',str(ino)],capture_output=True,text=True,timeout=10)
   out['samples'].append({'repeat':r,'ino':ino,'group':g,'port':port,'ms':1000*(time.monotonic()-t),'rc':x.returncode,'stdout':x.stdout,'stderr':x.stderr})
 time.sleep(.1)
for p in range(17432,17436):out['after'][p]=subprocess.check_output([exe,'raft-status',f'127.0.0.1:{p}'],text=True)
out['finished']=time.time();print(json.dumps(out,indent=2))
