import os,select,signal,time,json,traceback
from pathlib import Path
b=Path('/data1/efs/nuc-b239-qualification');out=b/'hold-after-fixture';out.mkdir();r=json.loads((b/'queued-launch-hold.json').read_text());parent=r['controller']['pid'];child=r['active_fixture']['pid']
def stat(pid):return (Path('/proc')/str(pid)/'stat').read_text().rsplit(')',1)[1].split()
def write(n,v):(out/n).write_text(json.dumps(v,indent=2))
try:
 assert stat(parent)[0]=='T'
 assert stat(child)[19]==r['active_fixture']['stat_fields'][19]
 fd=os.pidfd_open(child);deadline=(b/'catchup-base-live/long-direct-command.json').stat().st_mtime+10800
 timed_out=False
 while not select.select([fd],[],[],min(2,max(.01,deadline-time.time())))[0]:
  if time.time()>=deadline:
   signal.pidfd_send_signal(fd,signal.SIGTERM);timed_out=True
   write('fixture-timeout.json',dict(original_budget_seconds=10800,action='Normal private fixture SIGTERM cleanup; no RPC budget widened'))
   assert select.select([fd],[],[],180)[0], 'normal fixture cleanup exceeds original180s; keep launches held, no force-discard'
   break
 fields=stat(child);assert fields[0]=='Z' and fields[19]==r['active_fixture']['stat_fields'][19]
 rc=os.waitstatus_to_exitcode(int(fields[49]));os.close(fd)
 write('active-fixture-exit.json',dict(pid=child,exit=rc,timed_out=timed_out,utc=time.strftime('%FT%TZ',time.gmtime()),marker_scope='Actual owned fixture process exit; parent recipe marker may remain absent after deliberate supersession.'))
 # Restore via the original controller's finally, only after fixture cleanup.
 assert not os.path.ismount('/data1/efs/mnt') and not os.path.ismount('/data1/efs/mnt2')
 import subprocess
 assert subprocess.run(['pgrep','-x','efsd'],stdout=subprocess.DEVNULL).returncode==1
 assert stat(parent)[0]=='T'
 pfd=os.pidfd_open(parent);signal.pidfd_send_signal(pfd,signal.SIGINT);signal.pidfd_send_signal(pfd,signal.SIGCONT)
 assert select.select([pfd],[],[],600)[0], 'superseded controller restoration incomplete; inspect, do not force'
 os.close(pfd)
 with (out/'preservation-proof.log').open('w') as f:proof=subprocess.run(['python3',str(b/'proof.py'),str(out/'restored-service.json')],stdout=f,stderr=subprocess.STDOUT,timeout=120).returncode
 assert proof==0, 'preserved f52 restoration verification failed'
 write('hold-complete.json',dict(current_fixture_exit=rc,original_service_restored=True,queued_buffered_gates='NOTRUN/held by manager before replacement candidate',performance_comparisons='NOTRUN/held',utc=time.strftime('%FT%TZ',time.gmtime())))
 (out/'run-exit.txt').write_text('0')
except BaseException:
 (out/'failure.txt').write_text(traceback.format_exc());(out/'run-exit.txt').write_text('1');raise
