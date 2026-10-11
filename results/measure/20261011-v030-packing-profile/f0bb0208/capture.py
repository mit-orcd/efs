from pathlib import Path
import sys,time,json,subprocess
src,out=map(Path,sys.argv[1:]);end=time.monotonic()+60
while time.monotonic()<end:
 ids=[];suites=0
 for proc in Path('/proc').glob('[0-9]*'):
  try:cmd=(proc/'cmdline').read_bytes().replace(b'\0',b' ').decode()
  except (OSError,UnicodeError):continue
  if str(src/'tests/posix/posix_suite.py') in cmd and '--worker' not in cmd:suites+=1
  if cmd.startswith(str(src/'efsd')+' ') or cmd.startswith(str(src/'efs-fuse')+' '):ids.append(proc.name)
 if len(ids)==11 and suites==7:break
 time.sleep(.25)
else:raise RuntimeError(('missing runtime identities',ids,suites))
(out/'capture-identities.json').write_text(json.dumps({'source':'f0bb0208','pids':ids,'suites':suites,'scope':'instrumented direct seven clients jobs16, no acceptance claim'},indent=2)+'\n')
r=subprocess.run(['perf','record','-o',str(out/'perf.data'),'-F','99','--call-graph','dwarf,8192','-p',','.join(ids),'--','sleep','35'])
(out/'capture.exit').write_text(str(r.returncode)+'\n')
