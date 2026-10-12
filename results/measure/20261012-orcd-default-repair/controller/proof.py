import importlib.util, subprocess, io, tarfile, hashlib, json, shlex
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
inputs=json.loads(Path('operations/orcd-repair-default/inputs.json').read_text());short=inputs['commit'][:12];base=inputs['base']
# Read-only source fingerprint and private process/mount cleanup audit.
code=r'''import pathlib,hashlib,json,os,subprocess,sys
inputs=json.loads(sys.stdin.read());base=pathlib.Path(inputs['base'])
bad=[p for p,h in inputs['sha256'].items() if not (base/p).is_file() or hashlib.sha256((base/p).read_bytes()).hexdigest()!=h]
assert not bad,bad
processes=[]
for p in pathlib.Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:
  if p.stat().st_uid!=os.getuid():continue
  exe=os.readlink(p/'exe');name=(p/'comm').read_text().strip()
  if name in ('efsd','efs-fuse') and exe.startswith('/tmp/efs-repair-default-'):processes.append(dict(pid=int(p.name),exe=exe))
 except (FileNotFoundError,PermissionError,ProcessLookupError):pass
mounts=[x for x in subprocess.check_output(['findmnt','-rn','-o','TARGET'],text=True).splitlines() if x.startswith('/tmp/efs-repair-default-')]
assert not processes and not mounts,(processes,mounts)
print(json.dumps(dict(status='PASS',commit=inputs['commit'],source_files=len(inputs['sha256']),source_mismatches=bad,owned_processes=processes,owned_mounts=mounts,uid=os.getuid())))
'''
r=subprocess.run(m.ssh_command('fcstor006','python3 -c '+shlex.quote(code),m.OPTIONS,m.USER,m.BRIDGE),input=json.dumps(inputs).encode(),capture_output=True,timeout=180);assert r.returncode==0,r.stderr
Path('operations/orcd-repair-default/source-cleanup.json').write_bytes(r.stdout);print(r.stdout.decode(),flush=True)
# Preserve only logs/results/control artifacts, not physical data or raft databases.
code=f'''import pathlib,tarfile,sys
files=set(pathlib.Path('/tmp').glob('efs-repair-default*.log'))|set(pathlib.Path('/tmp').glob('efs-repair-default*.exit'))|set(pathlib.Path('/tmp').glob('efs-repair-default*.txt'))
for root in ['efs-repair-default-rdma-{short}','efs-repair-default-tcp-{short}','efs-repair-default-buffered-{short}','efs-repair-default-wire-{short}']:
 p=pathlib.Path('/tmp')/root
 files.update(x for x in p.rglob('*') if x.is_file() and (x.suffix in ('.log','.json','.jsonl') or x.name.endswith('-pid')) and not any(n in ('data','meta','mdraft') for n in x.relative_to(p).parts))
with tarfile.open(fileobj=sys.stdout.buffer,mode='w|gz') as t:
 for f in sorted(files):t.add(f,arcname=str(f.relative_to('/tmp')),recursive=False)
'''
r=subprocess.run(m.ssh_command('fcstor006','python3 -c '+shlex.quote(code),m.OPTIONS,m.USER,m.BRIDGE),capture_output=True,timeout=300);assert r.returncode==0,r.stderr
p=Path('work/fix-orcd-valgrind/results/measure/20261012-orcd-default-repair');p.mkdir(parents=True,exist_ok=True)
with tarfile.open(fileobj=io.BytesIO(r.stdout),mode='r:gz') as t:
 assert all(x.isfile() and not Path(x.name).is_absolute() and '..' not in Path(x.name).parts for x in t.getmembers());t.extractall(p/'remote')
summary={'commit':inputs['commit'],'host':'fcstor006','gates':{},'repair_scope':'manual missing ordinary base role, single-root FILES 2+1; no automatic/packed/delta/container/spare replacement qualification'}
for label in ('rdma','tcp','buffered'):
 root=p/'remote'/f'efs-repair-default-{label}-{short}'
 rows=[json.loads(x) for x in (root/'results.jsonl').read_text().splitlines()];assert len(rows)==23 and all(x['status']=='PASS' for x in rows),rows
 repairs=[x for x in rows if x['layer']=='integration' and x['case']=='repair_restores_protection'];assert sorted(x['role'] for x in repairs)==[0,1,2]
 assert all(x['build'].rstrip(':')==short for x in repairs)
 assert all(x['expected_sha256']==x['actual_sha256'] and x['repaired'] and x['protected'] and x['protection_checked']=='after cold restart, before second loss' for x in repairs)
 config=json.loads((root/'config.json').read_text());assert '--repair-experiment' not in config['replay_argv'];assert config['transport']==('rdma' if label=='rdma' else 'tcp')
 journal=[json.loads(x) for x in (root/'cluster/journal.jsonl').read_text().splitlines()]
 cold=[x for x in journal if x['op']=='repair-cold-protection'];assert sorted(x['role'] for x in cold)==[0,1,2] and all(x['initialization'] is False and x['physical_roles']==3 for x in cold)
 transport=[x for x in journal if x['op']=='transport'];assert transport and all(bool(x['uverbs'])==(label=='rdma') for x in transport)
 assert (p/'remote'/f'efs-repair-default-{label}.exit').read_text().strip()=='0'
 summary['gates'][label]={'status':'PASS','rows':len(rows),'repair_roles':[0,1,2],'replay_argv':config['replay_argv'],'current_incarnation_transport_proofs':len(transport),'cold_protection_checks':len(cold)}
for label in ('fault','normal'):
 assert (p/'remote'/f'efs-repair-default-{label}-native.exit').read_text().strip()=='0';summary['gates'][label+'-native']={'status':'PASS'}
assert json.loads((p/'remote'/f'efs-repair-default-wire-{short}/result.json').read_text())['status']=='PASS'
summary['gates']['activation-boundary']={'status':'PASS'}
before=json.loads(Path('operations/orcd-repair-default/runtime-before.json').read_text());after=json.loads(Path('operations/orcd-repair-default/runtime.json').read_text());stable=lambda rows:[{k:v for k,v in row.items() if k!='processes'} | {'processes':[{k:v for k,v in proc.items() if k!='rdma_device_fds'} for proc in row['processes']]} for row in rows]
assert stable(before)==stable(after)
summary['live_preservation']={'status':'PASS','hosts':len(after),'pid_binary_hash_arguments_environment_mount_unchanged':True}
summary['source_cleanup']=json.loads(Path('operations/orcd-repair-default/source-cleanup.json').read_text())
(p/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
(p/'controller').mkdir(exist_ok=True)
for f in Path('operations/orcd-repair-default').glob('*'):
 if f.is_file() and f.suffix in ('.py','.json','.log'):(p/'controller'/f.name).write_bytes(f.read_bytes())
files={str(f.relative_to(p)):hashlib.sha256(f.read_bytes()).hexdigest() for f in p.rglob('*') if f.is_file() and f.name!='artifact-sha256.json'}
(p/'artifact-sha256.json').write_text(json.dumps(files,indent=2)+'\n');print('all gates, unchanged live cluster, source identity, cleanup and '+str(len(files))+' evidence hashes PASS')
