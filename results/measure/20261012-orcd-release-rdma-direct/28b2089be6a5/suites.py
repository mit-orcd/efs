"""Runs each suite to its own verdict, retaining failures without blanket stop."""
import importlib.util,concurrent.futures,json,shlex,sys,subprocess
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
rev='28b2089be6a5';base=m.BULK+'/qualification-release-'+rev;out=base+'/suites';stage='/tmp/efs-orcd-release-'+rev;action=sys.argv[1]
def case(h,name,cmd,timeout=1800):
 d=out+'/'+h;body=f'mkdir -p {d};set +e; '+cmd+f' > {d}/{name}.log 2>&1; rc=$?;echo "$rc" > {d}/{name}.exit; tail -5 {d}/{name}.log; echo suite-exit=$rc;exit 0'
 try:return dict(host=h,suite=name,transcript=m.shell(h,body,timeout=timeout))
 except Exception as e:return dict(host=h,suite=name,controller_error=str(e))
if action=='native-posix':
 print(json.dumps(case('fcstor007','native-posix',f'mkdir -p /tmp/efs-native-release-{rev}; python3 -u {stage}/tests/posix/posix_suite.py /tmp/efs-native-release-{rev} --suite all --jobs 16 --timeout-s 30 --tag release-{rev}-native --results {out}/fcstor007/native-posix.tsv')),flush=True)
elif action=='mounted-posix':
 def run(h):return case(h,'mounted-posix',f'cd /tmp/efs; python3 -u tests/posix/posix_suite.py /tmp/efs-mount --suite all --jobs 16 --timeout-s 30 --tag release-{rev}-{h} --results {out}/{h}/mounted-posix.tsv')
 with concurrent.futures.ThreadPoolExecutor(max_workers=7) as pool:rows=list(pool.map(run,m.CLIENTS))
 Path('operations/orcd-release-28b2089b/mounted-posix-controllers.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(rows),flush=True)
elif action=='peer':
 # Login runs orchestration with its forwarded agent; the immutable shared
 # script path exists identically on both target nodes.
 shared=base+'/posix_2client.py';wrapper=base+'/peer-ssh.sh';parent='release-peer-'+rev
 m.shell('fcstor007',f'mkdir -p {out}/fcstor007; cp /tmp/efs/tests/posix/posix_2client.py {shared}; python3 {shared} --prepare /tmp/efs-mount --parent {parent}',timeout=120)
 subprocess.run([sys.executable,'operations/orcd-release-28b2089b/deploy.py','remount-peer'],check=True)
 text='#!/bin/bash\nexec timeout "${EFS_SSH_TIMEOUT:-15}" ssh -o BatchMode=yes -o ConnectTimeout=8 "erbmi1@$1" "$2"\n'
 m.shell('fcstor007','printf %s '+shlex.quote(text)+' > '+wrapper+';chmod +x '+wrapper,timeout=60)
 command=f'set +e; EFS_SSH={wrapper} python3 -u {shared} --remote fcstor007 fcstor009 --mnt /tmp/efs-mount --parent {parent} --results {out}/fcstor007/peer.tsv > {out}/fcstor007/peer.log 2>&1; rc=$?;echo "$rc" > {out}/fcstor007/peer.exit; tail -8 {out}/fcstor007/peer.log;exit 0'
 r=subprocess.run(['ssh','-A','-o','ControlPath=/private/tmp/efs-orcd-login-control3',m.BRIDGE,'bash -c '+shlex.quote(command)],capture_output=True,text=True,timeout=7200);print(r.stdout,r.stderr,flush=True);assert r.returncode==0
elif action.startswith('persist-'):
 phase=action.split('-',1)[1]
 print(json.dumps(case('fcstor007',action,f'cd /tmp/efs; python3 -u tests/posix/posix_persist.py /tmp/efs-mount --phase {phase} --keep --timeout-s 30 --tag release-{rev} --results {out}/fcstor007/{action}.tsv')),flush=True)
else:raise SystemExit('unknown suite')
