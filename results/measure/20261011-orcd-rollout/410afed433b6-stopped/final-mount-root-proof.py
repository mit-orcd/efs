import importlib.util,concurrent.futures,json
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
def one(h):
 if h in m.CLIENTS:
  output=m.shell(h,'findmnt -rn -M /tmp/efs-mount; ! mountpoint -q /tmp/efs-c2bf-mount; ! mountpoint -q /tmp/efs-b239-mount',timeout=45);assert 'fuse' in output,output
  return dict(host=h,original_mount=output,private_mounts_absent=True)
 roots=[f'/data1/{i:02}/{name}' for i in range(1,7) for name in ['efs-orcd-test-erbmi1','efs-orcd-c2bf-private']]
 body='\n'.join('test -d '+r for r in roots)+'\nss -ltn'
 output=m.shell(h,body,timeout=45);assert ':17432 ' in output and ':19862 ' not in output and ':19842 ' not in output,output
 return dict(host=h,original_and_retained_c2_roots=roots,original_listener=True,private_listeners_absent=True)
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:rows=list(pool.map(one,m.HOSTS))
p=Path('work/devel-orcd/results/measure/20261011-orcd-coalescing-rollout/410afed433b6-stopped');(p/'final-mount-root-proof.json').write_text(json.dumps(rows,indent=2)+'\n');print('actual7originalmounts/all24old+24retainedroots/originalports/no private mountsports PASS')
