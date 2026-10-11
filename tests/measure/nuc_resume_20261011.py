#!/usr/bin/env python3
"""Measure syscall phases on the preserved NUC cluster; never reset its stores."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import threading
import time

base = Path('/data1/efs/devel-nuc-20261011')
out = base / 'resume-20261011'
out.mkdir(exist_ok=False)
installed = Path('/data1/efs/src')
expected = json.loads((base / 'deployment-20261011/installed-images.json').read_text())
deadline = time.monotonic() + 360
while not all(os.path.ismount('/data1/efs/' + m) for m in ('mnt', 'mnt2')):
    assert time.monotonic() < deadline, 'cluster startup timeout'
    time.sleep(1)
processes = []
for proc in Path('/proc').iterdir():
    if not proc.name.isdigit():
        continue
    try:
        exe = Path(os.readlink(proc / 'exe'))
        if exe in (installed / 'efsd', installed / 'efs-fuse'):
            assert hashlib.sha256((proc / 'exe').read_bytes()).hexdigest() == expected[exe.name]['sha256']
            processes.append(dict(pid=int(proc.name), kind=exe.name))
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        continue
assert sum(p['kind'] == 'efsd' for p in processes) == 4
assert sum(p['kind'] == 'efs-fuse' for p in processes) == 2
prior = json.loads((base / 'final-service.json').read_text())
for sample in prior['existing_payload_samples_equal_on_both_mounts']:
    for mount in ('mnt', 'mnt2'):
        path = Path('/data1/efs', mount) / sample['path']
        assert hashlib.sha256(path.read_bytes()).hexdigest() == sample['sha256']
(out / 'deployment-acceptance.json').write_text(json.dumps(dict(processes=processes, images=expected,
    preserved_samples_on_both_mounts=len(prior['existing_payload_samples_equal_on_both_mounts'])), indent=2))

tree = Path('/data1/efs/mnt/w97-resume-20261011-1616')
tree.mkdir(exist_ok=False)
stop = threading.Event()
lock = threading.Lock()
totals = [0, 0, 0, 0]  # successful close count, open/write/close nanoseconds
samples = []
errors = []
origin = time.monotonic()
def worker(worker_id):
    folder = tree / str(worker_id)
    folder.mkdir()
    count = 0
    try:
        while not stop.is_set() and count < 12500:
            payload = hashlib.shake_256(f'{worker_id}:{count}'.encode()).digest(512)
            before = time.monotonic_ns()
            fd = os.open(folder / f'{count:06d}.txt', os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
            opened = time.monotonic_ns()
            try:
                assert os.write(fd, payload) == len(payload)
                written = time.monotonic_ns()
            finally:
                os.close(fd)
            closed = time.monotonic_ns()
            with lock:
                totals[0] += 1
                totals[1] += opened - before
                totals[2] += written - opened
                totals[3] += closed - written
            count += 1
        if count:
            samples.append(dict(path=f'{worker_id}/{count-1:06d}.txt', sha256=hashlib.sha256(payload).hexdigest()))
    except BaseException as error:
        errors.append(repr(error)); stop.set()
        raise

profilers = []
def profile(label):
    ids = ','.join(str(p['pid']) for p in processes)
    log = (out / (label + '-perf.log')).open('w')
    proc = subprocess.Popen(['perf', 'record', '-F', '99', '-g', '-p', ids,
        '-o', str(out / (label + '.data')), '--', 'sleep', '30'], stdout=log, stderr=log)
    profilers.append((label, proc, log))
    server_ids = [str(p['pid']) for p in processes if p['kind'] == 'efsd']
    log = (out / (label + '-strace.txt')).open('w')
    proc = subprocess.Popen(['timeout', '--signal=INT', '10', 'strace', '-f', '-c', '-w',
        *[arg for pid in server_ids for arg in ('-p', pid)]], stdout=log, stderr=log)
    profilers.append((label + '-strace', proc, log))

try:
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        workers = [pool.submit(worker, n) for n in range(8)]
        last = origin
        previous = [0, 0, 0, 0]
        next_window = origin + 30
        early = late = False
        with (out / 'windows.jsonl').open('w') as stream, (out / 'host.jsonl').open('w') as host:
            while time.monotonic() - origin < 300 and not stop.is_set() and not all(w.done() for w in workers):
                now = time.monotonic()
                if not early and now-origin >= 15:
                    profile('early'); early = True
                if not late and now-origin >= 240:
                    profile('late'); late = True
                host.write(json.dumps(dict(elapsed_s=now-origin, diskstats=Path('/proc/diskstats').read_text(),
                    processes={str(p['pid']): (Path('/proc')/str(p['pid'])/'stat').read_text() for p in processes}))+'\n'); host.flush()
                if now >= next_window:
                    with lock: current = totals.copy()
                    delta = [a-b for a,b in zip(current, previous)]
                    row = dict(elapsed_s=now-origin, closed_files=current[0], files_s=delta[0]/(now-last),
                        mean_ms={phase: delta[i]/max(delta[0],1)/1e6 for i,phase in enumerate(('open','write','close'),1)},
                        profiling_overlap=(15 <= now-origin <= 75 or 240 <= now-origin <= 300))
                    stream.write(json.dumps(row)+'\n'); stream.flush(); print(row, flush=True)
                    previous=current;last=now;next_window=now+30
                time.sleep(1)
        stop.set()
        for worker in workers: worker.result()
    for sample in samples:
        for mount in ('mnt','mnt2'):
            path=Path('/data1/efs',mount)/tree.name/sample['path']
            assert hashlib.sha256(path.read_bytes()).hexdigest()==sample['sha256']
    assert not errors, errors
    (out/'tiny-result.json').write_text(json.dumps(dict(totals=totals,samples=samples,
        scope='512-byte create-write-close, eight workers, bounded to 100000 files; sampled peer bytes, no cold/durable gate'),indent=2))
    (out/'run-exit.txt').write_text('0\n')
finally:
    stop.set()
    for label,proc,log in profilers:
        rc=proc.wait(timeout=40);log.close()
        (out/(label+'-exit.txt')).write_text(str(rc)+'\n')
        if label in ('early','late') and rc==0:
            with (out/(label+'-report.txt')).open('w') as report:
                subprocess.run(['perf','report','--stdio','--no-children','-i',str(out/(label+'.data'))],stdout=report,stderr=subprocess.STDOUT,check=True)
