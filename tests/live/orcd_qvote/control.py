import subprocess,shlex,json,concurrent.futures,time,sys,hashlib,os
from pathlib import Path
HERE=Path(__file__).resolve().parent
P=Path(os.environ['EFS_QVOTE_ARTIFACTS']).resolve()
P.mkdir(parents=True,exist_ok=True)
if Path(__file__).resolve().parents[3] in P.parents:
 raise RuntimeError('Keep raw artifacts outside the Git repository')
REPO=str(Path(__file__).resolve().parents[3])
REV='fa29f9277eb604f7212bad164b283df85697c9a0'
RUN=os.environ.get('EFS_QVOTE_RUN','qvote-fa29f9277-20261016')
if not RUN or any(x not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for x in RUN):raise ValueError('Invalid run name')
BASE='/home/erbmi1/orcd/scratch/efs/orcd-test/'+RUN
STORE_PREFIX='/data1/01/efs-'+RUN
SRC='/tmp/efs-'+RUN
HOSTS=[f'fcstor{i:03}' for i in range(3,14)]; SERVERS=HOSTS[:4]; CLIENTS=HOSTS[4:]
SEED='172.16.223.57:17432'
LIB='/home/erbmi1/orcd/scratch/efs/orcd-test/deps/libfuse/lib'
ENV='export EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2 EFS_COMMANDS=1 EFS_COMMANDS_SERVER=1 EFS_EXPIRY=0 EFS_REPAIR_EXPERIMENT=0 EFS_MD_RAFT_N=4\n'
def run(h,body,timeout=120):
 cmd=['ssh','-A','-o','ConnectTimeout=20','erbmi1@orcd-login.mit.edu',shlex.join(['ssh','-o','BatchMode=yes','-o','ConnectTimeout=15',h,'bash -c '+shlex.quote('set -euo pipefail\n'+body)])]
 r=subprocess.run(cmd,capture_output=True,text=True,timeout=timeout)
 if r.returncode:raise RuntimeError(h+': '+r.stdout[-3000:]+r.stderr[-1000:])
 return r.stdout

def parallel(hosts,f):
 with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
  fs={ex.submit(f,h):h for h in hosts}; result={}
  for fut in concurrent.futures.as_completed(fs):
   h=fs[fut]
   try:result[h]=fut.result()
   except Exception as e:result[h]={'error':str(e)}
   print(h,str(result[h])[-1600:],flush=True)
  if any(isinstance(v,dict) and 'error' in v for v in result.values()):raise RuntimeError(result)
  return result

def upload(local,remote):
 with open(local,'rb') as f:subprocess.run(['ssh','-A','erbmi1@orcd-login.mit.edu','mkdir -p '+BASE+'; cat > '+shlex.quote(remote)],stdin=f,check=True)

def prepare():
 with open(P/'source.tgz','wb') as f:subprocess.run(['git','-C',REPO,'archive','--format=tar.gz',REV,'Makefile','src','include','deps','scripts','tests','docs'],stdout=f,check=True)
 patch=subprocess.check_output(['git','-C',REPO,'show','e71fdfa56','--format=','--','src/server/raft_host.c','tests/test_receive_persist.py'])
 (P/'candidate.patch').write_bytes(patch)
 upload(P/'source.tgz',BASE+'/source.tgz');upload(P/'candidate.patch',BASE+'/candidate.patch')
 def stage(h):
  return run(h,f'''test ! -e {SRC}; mkdir -p {SRC} {BASE}/{h}; tar -xzf {BASE}/source.tgz -C {SRC}
cd {SRC}
export PKG_CONFIG_PATH={LIB}/pkgconfig
make -j8 EFS_GIT_ID={REV[:12]} EFS_GIT_BRANCH=devel EFS_GIT_DESCRIBE={REV[:12]} FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,{LIB}" efsd efs-fuse efs-mgmt >{BASE}/{h}/build.log 2>&1
cp efsd efsd-off
sha256sum efsd efs-fuse efs-mgmt >{BASE}/{h}/binary-off.sha256
'''+(f'''patch -p1 < {BASE}/candidate.patch >{BASE}/{h}/patch.log
make -j8 EFS_GIT_ID={REV[:12]} EFS_GIT_BRANCH=qvote-experiment EFS_GIT_DESCRIBE={REV[:12]}-qvote efsd >{BASE}/{h}/build-on.log 2>&1
cp efsd efsd-on
sha256sum efsd-on >{BASE}/{h}/binary-on.sha256
python3 tests/test_receive_persist.py >{BASE}/{h}/receive-persist.log 2>&1
patch -R -p1 < {BASE}/candidate.patch >/dev/null
cp efsd-off efsd
''' if h in SERVERS else '')+f'cat {BASE}/{h}/binary-off.sha256',1800)
 parallel(HOSTS,stage)

def inventory():
 code="""import pathlib,os,json
rows=[]
for p in pathlib.Path('/proc').iterdir():
 try:
  if not p.name.isdigit() or p.stat().st_uid!=os.getuid():continue
  a=(p/'cmdline').read_bytes().split(b'\\0');comm=(p/'comm').read_text().strip()
  if comm not in ('efsd','efs-fuse','python','python3','dd'):continue
  rows.append(dict(pid=int(p.name),comm=comm,args=[x.decode() for x in a if x],exe=os.readlink(p/'exe')))
 except (FileNotFoundError,PermissionError,ProcessLookupError):pass
print(json.dumps(rows))"""
 rows=parallel(HOSTS,lambda h:json.loads(run(h,'python3 -c '+shlex.quote(code))))
 (P/'inventory.json').write_text(json.dumps(rows,indent=2));return rows

if __name__=='__main__':
 if sys.argv[1]=='prepare':prepare()
 elif sys.argv[1]=='inventory':inventory()
