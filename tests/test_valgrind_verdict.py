#!/usr/bin/env python3
"""Exercise actual memcheck gates: missing/truncated/error reports cannot pass."""
from pathlib import Path
import subprocess,tempfile,os,sys,signal
root=Path(__file__).resolve().parents[1];s=(root/'tests/valgrind_leaks.sh').read_text()
a=s.index('fails=0');b=s.index('# Kill OUR daemons',a);gates=s[a:b]
reports=[('',False),('==1== Command: crashed\n',False),('==1== LEAK SUMMARY:\n==1== ERROR SUMMARY: 0 errors\n',False),('==1== All heap blocks were freed -- no leaks are possible\n==1== ERROR SUMMARY: 0 errors\n',True),('==1== LEAK SUMMARY:\n==1== definitely lost: 0 bytes in 0 blocks\n==1== indirectly lost: 0 bytes in 0 blocks\n==1== ERROR SUMMARY: 0 errors\n',True),('==1== LEAK SUMMARY:\n==1== definitely lost: 32 bytes in 1 blocks\n==1== indirectly lost: 0 bytes in 0 blocks\n==1== ERROR SUMMARY: 1 errors\n',False)]
# A leaking fork followed by a clean parent must still fail.
reports.append(('==2== LEAK SUMMARY:\n==2== definitely lost: 64 bytes in 1 blocks\n==2== indirectly lost: 8 bytes in 1 blocks\n==2== ERROR SUMMARY: 1 errors\n==1== LEAK SUMMARY:\n==1== definitely lost: 0 bytes in 0 blocks\n==1== indirectly lost: 0 bytes in 0 blocks\n==1== ERROR SUMMARY: 0 errors\n',False))
with tempfile.TemporaryDirectory(prefix='efs-vg-verdict-') as td:
 p=Path(td)/'gate.sh';p.write_text(gates+'\ngate "$1" fixture\n[ "$fails" = 0 ]\n')
 for text,good in reports:
  log=Path(td)/'fixture.vg';log.write_text(text)
  result=subprocess.run(['bash',str(p),str(log)],capture_output=True,text=True)
  assert (result.returncode==0)==good,(text,result.stdout,result.stderr)
print('valgrind verdict: empty/truncated reports refused, complete zero-loss accepted, real leaks fail PASS')

if hasattr(os,'pidfd_open'):
    a=s.index('kill_ours() {');b=s.index('\n# Start a 3-node',a)
    with tempfile.TemporaryDirectory(prefix='efs-vg-owned-') as td:
        work=Path(td);script=work/'owned.sh'
        script.write_text('WORK=$1\nOWN_PIDS=("$2")\n'+s[a:b]+'\nkill_ours TERM\n')
        for argument,wrong,retire in ((str(work/'mnt-extra'),False,False),(str(work/'mnt'),True,False),(str(work/'mnt'),False,True)):
            child=subprocess.Popen([sys.executable,'-c','import time;time.sleep(30)',argument])
            try:
                stamp=int(Path('/proc/'+str(child.pid)+'/stat').read_text().split(') ',1)[1].split()[19])+int(wrong)
                subprocess.run(['bash',str(script),str(work),str(child.pid)+':'+str(stamp)],check=True)
                if retire:assert child.wait(timeout=5)==-signal.SIGTERM
                else:assert child.poll() is None,'unrelated/stale identity was signaled'
            finally:
                if child.poll() is None:child.terminate();child.wait()
    print('valgrind cleanup: exact owned argument/start identity, prefix and stale PID refused PASS')
