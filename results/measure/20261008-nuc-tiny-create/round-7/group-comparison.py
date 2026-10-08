import concurrent.futures,json,os,signal,time,statistics,subprocess
from pathlib import Path
baseline=json.loads(Path('/data1/efs/profile-tiny-20261008/group-comparison-r7.json').read_text())
pid=int(subprocess.check_output(['pgrep','-f','^python3 ./tiny_files.py .* /data1/efs/mnt/tiny-profile-r7-fix$']))
rows=[]
prefix='r7-prealloc-'+str(time.time_ns())
def worker(directory):
    samples=[]
    for i in range(32):
        t=time.perf_counter()
        with open(Path(directory)/(prefix+f'-{i:03}.txt'),'xb') as f: f.write(b'x'*256)
        samples.append((time.perf_counter()-t)*1000)
    return samples
os.kill(pid,signal.SIGSTOP)
try:
    for run,group in enumerate(['0','2','0','2']):
        prefix+='-'+str(run)
        t=time.perf_counter()
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            samples=list(pool.map(worker,baseline['groups'][group]))
        elapsed=time.perf_counter()-t
        rows.append(dict(group=int(group),files=256,seconds=elapsed,files_per_sec=256/elapsed,mean_file_ms=statistics.mean(x for s in samples for x in s)))
    print(json.dumps(dict(root=baseline['root'],groups=baseline['groups'],results=rows),indent=2),flush=True)
finally: os.kill(pid,signal.SIGCONT)
