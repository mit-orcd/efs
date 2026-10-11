#!/usr/bin/env python3
"""Verify the unchanged installed clock images after a retained startup BUSY."""
import errno,hashlib,json,os,socket,subprocess,time,traceback
from pathlib import Path

base=Path('/data1/efs/devel-nuc-20261011');out=base/'clock-install-readiness-retry';out.mkdir(exist_ok=False)
src=Path('/data1/efs/src');prior=base/'clock-install';deadline=time.monotonic()+900
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def ready():
 for port in range(17432,17436):
  try:
   with socket.create_connection(('127.0.0.1',port),.3):pass
  except OSError:return False
 return True
try:
 assert not Path('/proc/74223').exists(),'original installer still active'
 for phase in ('clock-build/run-exit.txt','clock-live/run-exit.txt','clock-live/full-direct-exit.txt','clock-live/full-buffered-exit.txt','clock-live/restore-exit.txt'):
  assert (base/phase).read_text().strip()=='0',phase
 for phase in ('source-backup','mnt-stop','mnt2-stop','install'):
  assert (prior/(phase+'-exit.txt')).read_text().strip()=='0'
 failure=(prior/'failure.txt').read_text()
 assert 'verify_images(dst,images);start(\'start\');samples()' in failure
 assert 'OSError: [Errno 16] Device or resource busy:' in failure
 assert not (prior/'run-exit.txt').exists()
 images=json.loads((base/'clock-build/images.json').read_text())
 assert all(digest(src/name)==image['sha256'] for name,image in images.items())
 expected=json.loads((base/'clock-core-expected.json').read_text())
 current={str(p.relative_to(src)):digest(p) for folder in ('src','include') for p in (src/folder).rglob('*') if p.suffix in ('.c','.h')}
 current['Makefile']=digest(src/'Makefile');assert current==expected
 while not ready():
  assert time.monotonic()<deadline,'unchanged daemon recovery did not reach all listeners'
  time.sleep(.5)
 assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
 rows=[]
 for proc in Path('/proc').iterdir():
  if not proc.name.isdigit():continue
  try:
   exe=os.readlink(proc/'exe')
   if exe not in (str(src/'efsd'),str(src/'efs-fuse')):continue
   kind=Path(exe).name;assert digest(proc/'exe')==images[kind]['sha256']
   rows.append(dict(pid=int(proc.name),kind=kind,sha256=images[kind]['sha256']))
  except (FileNotFoundError,ProcessLookupError,PermissionError):continue
 assert sum(p['kind']=='efsd' for p in rows)==4 and sum(p['kind']=='efs-fuse' for p in rows)==2
 samples=json.loads((base/'final-service.json').read_text())['existing_payload_samples_equal_on_both_mounts']
 retries=[];checked=[];read_deadline=time.monotonic()+180
 for sample in samples:
  for mount in ('mnt','mnt2'):
   path=Path('/data1/efs',mount)/sample['path']
   while True:
    try:value=digest(path);break
    except OSError as error:
     if error.errno not in (errno.EBUSY,errno.EAGAIN):raise
     retries.append(dict(path=str(path),errno=error.errno,utc=time.strftime('%FT%TZ',time.gmtime())))
     assert time.monotonic()<read_deadline,'preserved payload remained BUSY after listeners became ready'
     time.sleep(.5)
   assert value==sample['sha256'],('preserved payload changed',str(path))
   checked.append(dict(path=str(path),sha256=value))
 (out/'verified.json').write_text(json.dumps(dict(images=images,processes=rows,preserved_samples=checked,retries=retries,core_sha256=current,limits='Readiness-only verification of unchanged installed images; no daemon restart, source mutation, store replacement or failed engine gate waived.'),indent=2))
 blocked={
  'w52-clock-20000':{'blocked-prerequisite.json','run-exit.txt'},
  'w52-clock-long-profile':{'failure.txt'},'clock-cold':{'failure.txt'},
  'root-scope-build':{'failure.txt'},'root-scope-live':{'failure.txt'}}
 for name,files in blocked.items():assert {p.name for p in (base/name).iterdir()}==files,name
 report=json.loads((base/'w52-clock-20000/blocked-prerequisite.json').read_text())
 assert report['no_workload_created'] and report['failed']==['clock-install']
 assert (base/'w52-clock-20000/run-exit.txt').read_text().strip()=='125'
 for pid in (74224,74225,82278,82279,93951):assert not Path('/proc',str(pid)).exists()
 assert not (base/'root-scope-src').exists()
 for tree in ('w52-clock-20000-20261011','w52-clock-20000-before-2000-20261011'):
  assert not (Path('/data1/efs/mnt')/tree).exists()
 archive=base/'clock-install-initial-readiness-failure';prior.rename(archive);prior.mkdir()
 for name in blocked:(base/name).rename(base/(name+'-blocked-install-readiness'))
 (prior/'images.json').write_text(json.dumps(images,indent=2))
 (prior/'after.json').write_text(json.dumps(dict(images=images,processes=rows,preserved_samples_on_both_mounts=samples),indent=2))
 (prior/'qualified-retry.json').write_text(json.dumps(dict(original=str(archive),verification=str(out/'verified.json'),reason='Original payload verification ran before every recovering listener was ready; unchanged images and every prior payload now verified.',controller_commit='recorded in retry provenance'),indent=2))
 (prior/'installed-source-sha256.json').write_text(json.dumps({str(p.relative_to(src)):digest(p) for folder in ('src','include','tests') for p in (src/folder).rglob('*') if p.suffix in ('.c','.h','.py')},indent=2))
 (prior/'run-exit.txt').write_text('0\n')
 commands={
  'append':['nuc_w52_resume_20261011.py','--count','20000','--name','w52-clock-20000','--after','clock-install','--before-2000'],
  'profile':['nuc_w52_profile_20261011.py','--variant','clock'],
  'cold':['nuc_clock_cold_20261011.py'],
  'root-build':['nuc_root_scope_build_20261011.py'],
  'root-live':['nuc_root_scope_live_restore_guard_20261011.py','--variant','root-scope']}
 queued={}
 for label,args in commands.items():
  script=base/args[0];assert script.is_file()
  with (base/('clock-readiness-retry-'+label+'-controller.log')).open('w') as log:
   job=subprocess.Popen(['python3',str(script),*args[1:]],cwd=base,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
  queued[label]=dict(pid=job.pid,command=['python3',str(script),*args[1:]],script_sha256=digest(script))
 (out/'queued.json').write_text(json.dumps(queued,indent=2));(out/'run-exit.txt').write_text('0\n')
except BaseException:
 (out/'failure.txt').write_text(traceback.format_exc());raise
