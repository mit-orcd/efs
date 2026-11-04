#!/usr/bin/env python3
"""Opt-in POSIX diagnostic receiver; never use observer runs as normal acceptance.

OUTPUT OBSERVER_DIRECTORY POSIX_SUITE [suite arguments...]. The actual suite
and worker arguments/deadlines are preserved. The capability-safe observer is
loaded only by fresh workers through PYTHONPATH. A full sink cannot block them.
"""
import os,runpy,socket,sys,threading
output,observer,suite=sys.argv[1:4]
address='/tmp/efs-posix-phase-'+str(os.getpid())+'.sock'
sock=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);sock.bind(address);sock.settimeout(.1)
stop=threading.Event();errors=[]
def receive():
 try:
  with open(output,'wb') as f:
   while True:
    try:f.write(sock.recv(4096))
    except socket.timeout:
     if stop.is_set():break
 except Exception as exc:errors.append(repr(exc))
worker=threading.Thread(target=receive,daemon=True);worker.start()
os.environ['EFS_POSIX_PHASE_SOCKET']=address;os.environ['EFS_POSIX_PHASE_CLIENT']=os.path.basename(output)
os.environ['PYTHONPATH']=observer+os.pathsep+os.environ.get('PYTHONPATH','')
sys.argv=sys.argv[3:]
try:runpy.run_path(suite,run_name='__main__')
finally:
 stop.set();worker.join(2);sock.close();os.unlink(address)
 if errors:print('phase receiver errors:',errors,file=sys.stderr)
