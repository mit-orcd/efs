"""Count actual sync syscalls in an explicitly enabled perf tracepoint window."""
import atexit,json,os,select,signal,subprocess,time
class SyncCounters:
 def __init__(self,cell,servers):
  self.cell=cell;self.jobs=[];self.records=[]
  atexit.register(self.stop,False)
  try:
   for node,v in servers.items():
    ctl=cell/f'n{node}-sync.ctl';ack=cell/f'n{node}-sync.ack'
    os.mkfifo(ctl);os.mkfifo(ack)
    write=os.open(ctl,os.O_RDWR|os.O_NONBLOCK);read=os.open(ack,os.O_RDWR|os.O_NONBLOCK)
    output=(cell/f'n{node}-sync.csv').open('w')
    proc=subprocess.Popen(['perf','stat','-D','-1','--control',f'fifo:{ctl},{ack}','-x,','-e','syscalls:sys_enter_fdatasync,syscalls:sys_enter_fsync','-p',str(v['pid'])],stdout=subprocess.DEVNULL,stderr=output)
    self.jobs.append((node,proc,output,write,read))
  except BaseException:self.stop(False);raise
 def command(self,cmd):
  began=time.monotonic_ns()
  for node,p,out,w,r in self.jobs:
   assert p.poll() is None,f'perf n{node} exited before {cmd}'
   os.write(w,(cmd+'\n').encode())
   ready,_,_=select.select([r],[],[],10)
   assert ready,f'perf n{node} did not acknowledge {cmd}'
   reply=os.read(r,4096).decode();assert 'ack' in reply,reply
   self.records.append(dict(node=node,command=cmd,ack=reply,mono_us=time.monotonic_ns()//1000))
  self.records.append(dict(command=cmd,begin_mono_us=began//1000,end_mono_us=time.monotonic_ns()//1000))
 def stop(self,validate=True):
  jobs=self.jobs;self.jobs=[];errors=[]
  for node,p,out,w,r in jobs:
   if p.poll() is None:p.send_signal(signal.SIGINT)
   try:rc=p.wait(timeout=10)
   except subprocess.TimeoutExpired:p.terminate();rc=p.wait(timeout=10)
   out.close();os.close(w);os.close(r)
   if validate:
    rows=[line.split(',') for line in (self.cell/f'n{node}-sync.csv').read_text().splitlines() if 'syscalls:sys_enter_' in line]
    if len(rows)!=2 or not all(row[0].strip().isdigit() for row in rows):errors.append((node,rc,rows))
  (self.cell/'sync-control.json').write_text(json.dumps(self.records,indent=2));self.jobs=[]
  assert not errors,errors
