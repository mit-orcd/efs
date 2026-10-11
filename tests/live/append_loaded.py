"""Mixed persistent/per-record APPEND descriptions on two mounts during GC."""
import concurrent.futures,hashlib,json,os,shutil,threading,time
from collections import Counter

def run(mount,peer,work,rounds,totals,status,drained,wait,unmount,stop,start,mount_client,unmount_peer,mount_peer):
 evidence=[]
 for iteration in range(rounds):
  path=mount/f'loaded-append-{iteration:02d}';path.touch()
  garbage=mount/f'loaded-garbage-{iteration:02d}';garbage.mkdir()
  for index in range(128):
   with (garbage/str(index)).open('xb',buffering=0) as out:
    assert out.write(bytes([index])*4096)==4096;os.fsync(out.fileno())
  before=totals(status());gate=threading.Barrier(5);errors=[];writers=4;count=200
  def record(writer,index):return f'R{iteration:02d}W{writer:02d}S{index:04d}\n'.encode()
  def writer(worker):
   target=(peer if worker>=2 else mount)/path.name
   gate.wait(timeout=30);fd=os.open(target,os.O_WRONLY|os.O_APPEND) if worker%2 else None
   try:
    for index in range(count):
     active=fd if fd is not None else os.open(target,os.O_WRONLY|os.O_APPEND)
     try:
      body=record(worker,index);assert os.write(active,body)==len(body)
      if index%13==0:os.fsync(active)
     finally:
      if fd is None:os.close(active)
     time.sleep(.01)
    if fd is not None:os.fsync(fd)
   except BaseException as error:errors.append(repr(error));raise
   finally:
    if fd is not None:os.close(fd)
  def delete():
   gate.wait(timeout=30);shutil.rmtree(garbage)
  begun=time.monotonic()
  with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
   jobs=[pool.submit(writer,i) for i in range(writers)]+[pool.submit(delete)]
   for job in jobs:job.result(timeout=180)
  assert not errors,errors
  warm=path.read_bytes();expected=Counter(record(w,i) for w in range(writers) for i in range(count))
  assert Counter(warm.splitlines(keepends=True))==expected,('warm records lost/duplicated',iteration)
  assert (peer/path.name).read_bytes()==warm,('peer bytes differ',iteration)
  after=totals(status());assert after['passes']>before['passes'],(before,after)
  unmount_peer();unmount();stop();start();mount_client()
  cold=path.read_bytes();assert cold==warm,('cold bytes changed',iteration)
  assert Counter(cold.splitlines(keepends=True))==expected,('cold records lost/duplicated',iteration)
  path.unlink();wait(drained,'loaded append round physical deletion drained',180)
  evidence.append(dict(round=iteration,exact_records=800,two_mount_writers=True,mixed_description_lifetime=True,
   elapsed_s=time.monotonic()-begun,sha256=hashlib.sha256(cold).hexdigest(),before_gc=before,after_gc=after))
  (work/'loaded-append.json').write_text(json.dumps(evidence,indent=2))
  print('loaded two-mount append/GC round',iteration+1,'/',rounds,'exact 800 cold records PASS',flush=True)
  if iteration+1<rounds:mount_peer()
