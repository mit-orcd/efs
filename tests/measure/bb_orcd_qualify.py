#!/usr/bin/env python3
"""BB ORCD build and gated real-HCA qualification; never operate another cluster.

Build can run while the assigned-host gate is pending. Qualification requires
its closed source-matching zero-failure receipt and unchanged built binaries.
"""
import argparse, hashlib, json, os, subprocess, time, traceback
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('private',type=Path);p.add_argument('commit')
p.add_argument('--phase',choices=('build','qualify'),required=True)
p.add_argument('--loopback-proof',type=Path)
p.add_argument('--quiet-proof',type=Path)
p.add_argument('--fuse-prefix',type=Path,default=Path('/data1/efs/deps/libfuse'))
a=p.parse_args();root=a.private.resolve();source=root/'src';out=root/a.phase
assert root.parent==Path('/tmp') and root.name=='efs-taskBB-orcd-'+a.commit
assert root.stat().st_uid==os.getuid() and source.is_dir() and not out.exists()
lib=a.fuse_prefix/('lib64' if (a.fuse_prefix/'lib64/pkgconfig').is_dir() else 'lib')
assert (lib/'pkgconfig/fuse3.pc').is_file()
out.mkdir();state=dict(verdict='RUNNING',phase=a.phase,source=a.commit,commands=[])
env={k:v for k,v in os.environ.items() if not k.startswith('EFS_')}
env.update(EFS_GIT_ID=a.commit,EFS_GIT_DESCRIBE=a.commit,EFS_GIT_BRANCH='agent/devel-bb',
 EFS_TRANSPORT='tcp',EFS_RDMA_DEV='mlx5_0',EFS_FUSE_PREFIX=str(a.fuse_prefix),
 PKG_CONFIG_PATH=str(lib/'pkgconfig'),LD_LIBRARY_PATH=str(lib)+':'+env.get('LD_LIBRARY_PATH',''))
def sha(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for b in iter(lambda:f.read(1<<20),b''):h.update(b)
 return h.hexdigest()
def save():
 t=out/'state.tmp';t.write_text(json.dumps(state,indent=2)+'\n');t.replace(out/'state.json')
def run(name,argv,transport='tcp',timeout=900,required=True):
 state['current']=name;save();tmp=root/'tmp'/name;tmp.mkdir(parents=True)
 e=dict(env,EFS_TRANSPORT=transport,TMPDIR=str(tmp));start=time.monotonic();log=out/(name+'.log')
 with log.open('x') as f:
  f.write(json.dumps(dict(argv=list(map(str,argv)),transport=transport,tmp=str(tmp)))+'\n');f.flush()
  rc=subprocess.run(list(map(str,argv)),cwd=source,env=e,stdout=f,stderr=subprocess.STDOUT,timeout=timeout).returncode
 text=log.read_text(errors='replace');state['commands'].append(dict(name=name,rc=rc,elapsed_s=time.monotonic()-start,sha256=sha(log)));save()
 assert rc==0,(name,rc,text[-3000:])
 if required:assert 'NOTRUN' not in text and 'SKIP' not in text,(name,'required hardware gate did not execute')
 return text
try:
 if a.phase=='build':
  libs=subprocess.check_output(['pkg-config','--libs','fuse3'],env=env,text=True).strip()
  run('build',['make','-j8',f'FUSE_LIBS={libs} -Wl,-rpath,{lib}','all','tests/faults/efsd','tests/test_declaration_gate','tests/test_rdma_xprt','tests/test_rdma_segmented'],timeout=1800)
  run('version',[source/'efsd','--version'])
  state['binary_sha256']={n:sha(source/n) for n in ('efsd','efs-fuse','efs-mgmt','tests/faults/efsd')}
  state['verdict']='BUILD_COMPLETE_NOT_QUALIFIED'
 else:
  assert a.loopback_proof and a.loopback_proof.is_file(),'closed xorinox gate required'
  proof=json.loads(a.loopback_proof.read_text())
  assert proof['source']==a.commit and proof['verdict'] in ('PASS_SCOPED','PASS_EXECUTED_WITH_HOST_LIMITS')
  assert proof['gate_rc'] in (0,3) and ' FAIL 0 ' in proof['gate_summary']
  if proof.get('run_id','').endswith('-quiet1'):
   assert a.quiet_proof and a.quiet_proof.is_file(),'closed quiet comparison required'
   quiet=json.loads(a.quiet_proof.read_text())
   assert quiet['source']==a.commit and quiet['verdict']=='PASS_SCOPED_OBSERVED_QUIET' and not quiet['overlap']
   assert quiet['candidate_state_sha256']==sha(a.loopback_proof)
   state['quiet_proof_sha256']=sha(a.quiet_proof)
  build=json.loads((root/'build/state.json').read_text())
  assert build['source']==a.commit and build['verdict']=='BUILD_COMPLETE_NOT_QUALIFIED'
  assert all(sha(source/n)==h for n,h in build['binary_sha256'].items())
  state.update(loopback_proof_sha256=sha(a.loopback_proof),build_proof_sha256=sha(root/'build/state.json'),binary_sha256=build['binary_sha256'])
  # Also close the declaration RDMA branch skipped by the no-HCA xorinox
  # gate. This target is native-only and does not relink the four products.
  run('build-declaration',['make','tests/test_declaration_gate'])
  run('declaration-rdma',[source/'tests/test_declaration_gate'],transport='rdma')
  for test in ('client_metadata_authority','inode_pending_eof','map_fresh_open','readdir_permission_authority','stage_evict_lru'):
   run('unit-'+test,['python3','tests/test_'+test+'.py'])
  for transport in ('tcp','rdma'):
   matrix_driver=Path(__file__).resolve().parents[1]/'live/bb_overlay_authority.py'
   assert matrix_driver.is_file(),'stage the matching external matrix tool with this controller'
   run('matrix-'+transport,['python3',matrix_driver,'--source',source,'--output',out/('matrix-'+transport)],transport=transport,timeout=600)
   matrix=json.loads((out/('matrix-'+transport)/'result.json').read_text())
   assert matrix['verdict']=='PASS' and len(matrix['cases'])==8 and not matrix['cleanup_errors'],matrix
   if transport=='rdma':assert len(matrix.get('rdma_runtime',[]))==2 and all(len(x['processes'])==5 for x in matrix['rdma_runtime'])
  run('rdma-xprt',[source/'tests/test_rdma_xprt'],transport='rdma')
  for mode in ('normal','interrupt','tcp-violation','deadline','burst','burst-asym','churn'):
   text=run('rdma-segmented-'+mode,[source/'tests/test_rdma_segmented',mode],transport='rdma');assert 'PASS' in text
  for n in ('ga','gb'):(root/n).mkdir()
  text=run('gc-posix-rdma',['python3','tests/live/gc_reclamation.py','--transport','rdma','--root',root/'ga','--root',root/'gb','--port','19960','--posix'],transport='rdma',timeout=3600)
  assert 'GC reclamation PASS' in text and 'POSIX suite: 218/218 pass' in text and 'POSIX 2-client: 72/72 pass' in text,text[-3000:]
  assert all(sha(source/n)==h for n,h in build['binary_sha256'].items())
  state['verdict']='PASS_SCOPED_SINGLE_HOST_HCA'
except BaseException:
 state.update(verdict='FAIL',error=traceback.format_exc());raise
finally:save()
