import os,time,signal,json,subprocess
from pathlib import Path
out=Path('/data1/efs/release-v030-results-20261012-28b2089b');parent=1575615
os.kill(parent,signal.SIGSTOP)
# The test child continues; retain its real wait status before replacing the controller.
rows=subprocess.check_output(['ps','-o','pid=','--ppid',str(parent)],text=True).split()
child=int(rows[0]) if rows else None
started_lines=[l for l in (out/'approved-progress.log').read_text().splitlines() if l.endswith(' START')]
name=started_lines[-1].split()[1]
print('draining',name,'child',child,flush=True)
if child:
 end=time.monotonic()+3700
 while True:
  raw=Path('/proc/'+str(child)+'/stat').read_text();fields=raw[raw.rfind(')')+2:].split()
  if fields[0]=='Z':break
  if time.monotonic()>end:
   os.kill(parent,signal.SIGCONT);raise RuntimeError('handoff timeout: original runner resumed')
  time.sleep(1)
 rc=os.waitstatus_to_exitcode(int(fields[49]))
 d=out/'gates'/name;row=json.loads((d/'command.json').read_text())
 t=time.mktime(time.strptime(row['started'],'%Y-%m-%dT%H:%M:%SZ'))
 row.update(returncode=rc,seconds=round(time.time()-t,3),status='PASS' if rc==0 else 'NOTRUN' if rc==77 else 'FAIL',controller_handoff='actual child wait status; original controller paused at wait')
 (d/'result.json').write_text(json.dumps(row,indent=2));(d/'run.exit').write_text(str(rc)+'\n')
 with (out/'all-gates.jsonl').open('a') as f:f.write(json.dumps(row)+'\n')
 with (out/'approved-progress.log').open('a') as f:f.write(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())+' '+name+' '+row['status']+' '+str(rc)+' split-handoff\n')
 print(name,row['status'],rc,flush=True)
os.kill(parent,signal.SIGKILL)
for line in subprocess.check_output(['ps','-eo','pid=,args='],text=True).splitlines():
 parts=line.strip().split(None,1)
 if len(parts)==2 and parts[1]=='python3 -u /tmp/amd-all-extra-approved.py':os.kill(int(parts[0]),signal.SIGTERM)
(out/'all-suites.finished').unlink(missing_ok=True)
for label,script,log in [('amd-split-direct-28b2089b','/tmp/amd-split-direct.py','split-direct-progress.log'),('amd-split-overload-28b2089b','/tmp/amd-split-overload.py','split-overload-progress.log')]:
 subprocess.run(['screen','-dmS',label,'bash','-c','exec python3 -u '+script+' > '+str(out/log)+' 2>&1'],check=True)
(out/'split-handoff.finished').write_text(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())+'\n')
