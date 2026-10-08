#!/usr/bin/env python3
"""Independent W90 matrix; fresh private scratch, bounded work and raw evidence."""
import argparse
import datetime
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import time
import statistics
import zipfile

ROOT=Path(__file__).resolve().parents[1]

def ints(value):
    try: result=[int(v) for v in value.split(',')]
    except ValueError: raise argparse.ArgumentTypeError('comma-separated integers required')
    if not result or any(n<=0 for n in result):raise argparse.ArgumentTypeError('positive values required')
    return result

def parser():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,default=ROOT/'efs-bench')
    p.add_argument('--storage-root',type=Path,action='append',required=True,help='scratch parent outside live EFS fragment roots')
    p.add_argument('--output',type=Path)
    p.add_argument('--layouts',default='files,punch,compact')
    p.add_argument('--occupancies',type=ints,default=[50,80,95])
    p.add_argument('--payload-sizes',type=ints,default=[131072],help='bytes; EFS fragment=65536, logical chunk=131072')
    p.add_argument('--qds',type=ints,default=[1],help='blocking workers, not asynchronous requests per worker')
    p.add_argument('--io-modes',default='direct,buffered')
    p.add_argument('--persist',choices=['none','each','batch'],default='batch')
    p.add_argument('--capacity-per-worker',type=int,default=2*1024**3)
    p.add_argument('--container-bytes',type=int,default=1024**3)
    p.add_argument('--steps',type=int,help='fixed per-worker operations; default six operations per capacity slot')
    p.add_argument('--seconds',type=int,default=0,help='minimum measured seconds per worker; steps remains a hard cap')
    p.add_argument('--warmup',type=int,default=1000)
    p.add_argument('--read-percent',type=int,default=50)
    p.add_argument('--batch-size',type=int,default=32)
    p.add_argument('--repeat',type=int,default=1)
    p.add_argument('--timeout',type=int,default=600,help='case deadline seconds; timeout invalidates case')
    p.add_argument('--preallocate',action='store_true',help='container-only variation; files stay reference')
    p.add_argument('--extent-hint',type=int,default=0,help='container-only XFS hint in bytes')
    p.add_argument('--file-batch-sync',action='store_true',default=None,help='files only: defer per-file body fsyncs (automatic for batch persistence)')
    p.add_argument('--file-immediate-sync',dest='file_batch_sync',action='store_false',default=None,help='legacy reference: flush every replacement body immediately')
    p.add_argument('--filestreams',action='store_true',help='per-private-directory XFS allocation flag')
    p.add_argument('--compare',action='store_true',help='alternate layout order per repeat and summarize matched samples')
    p.add_argument('--seed',type=int,default=305419896)
    p.add_argument('--hot-set-percent',type=int,default=10)
    p.add_argument('--hot-access-percent',type=int,default=0,help='0 selects uniform; otherwise fraction directed at hot set')
    p.add_argument('--dry-run',action='store_true')
    return p

def cases(a):
    layouts=a.layouts.split(',');modes=a.io_modes.split(',')
    if a.file_batch_sync is True and a.persist!='batch':raise ValueError('file-batch-sync requires batch persistence')
    if any(s not in ['files','punch','compact'] for s in layouts) or any(s not in ['direct','buffered'] for s in modes):raise ValueError('unknown layout or I/O mode')
    if any(n not in [50,80,95] for n in a.occupancies):raise ValueError('occupancy must be 50,80,95')
    if not 1<=a.repeat<=10 or a.timeout<1 or not 0<=a.seconds<=3600 or a.warmup<0 or not 1<=a.read_percent<=99:raise ValueError('invalid repeat, deadline, warmup or read percentage')
    if not 1<=a.batch_size<=100000 or any(q>64 for q in a.qds):raise ValueError('invalid batch/QD')
    if a.container_bytes<16384 or a.container_bytes%4096 or a.capacity_per_worker<2*a.container_bytes:raise ValueError('capacity per worker must hold at least two containers')
    if any(s<4096 or s>1048576 or s%4096 for s in a.payload_sizes):raise ValueError('payload sizes must be aligned 4 KiB..1 MiB')
    if not 1<=a.seed<2**64 or not 1<=a.hot_set_percent<=99 or not 0<=a.hot_access_percent<=100:raise ValueError('invalid seed/locality')
    if any(q<len(a.storage_root) for q in a.qds):raise ValueError('each target needs at least one worker')
    if a.compare and (len(layouts)<2 or len(set(layouts))!=len(layouts)):raise ValueError('comparison needs distinct layouts')
    for payload,occupancy,qd,mode,repeat,li in itertools.product(a.payload_sizes,a.occupancies,a.qds,modes,range(a.repeat),range(len(layouts))):
        layout=(layouts if not a.compare or repeat%2==0 else layouts[::-1])[li]
        stride=payload+4096
        slots=(a.capacity_per_worker//a.container_bytes)*(a.container_bytes//stride)
        steps=a.steps if a.steps is not None else (10000000 if a.seconds else slots*6)
        if not 1<=steps<=10000000:raise ValueError('invalid steps')
        argv=['--bench','churn','--layout',layout,'--persist',a.persist,
              '--capacity-bytes',str(a.capacity_per_worker*qd),'--container-bytes',str(a.container_bytes),
              '--payload-size',str(payload),'--occupancy',str(occupancy),'--qd',str(qd),
              '--steps',str(steps),'--warmup',str(a.warmup),'--read-percent',str(a.read_percent),
              '--batch-size',str(a.batch_size),'--seed',str(a.seed),'--hot-set-percent',str(a.hot_set_percent),
              '--hot-access-percent',str(a.hot_access_percent),'--direct-io' if mode=='direct' else '--no-direct-io']
        if a.seconds:argv+=['--seconds',str(a.seconds)]
        if layout=='files' and a.persist=='batch' and a.file_batch_sync is not False:argv+=['--file-batch-sync']
        if layout!='files' and a.preallocate:argv+=['--preallocate']
        if layout!='files' and a.extent_hint:argv+=['--extent-hint',str(a.extent_hint)]
        if a.filestreams:argv+=['--filestreams']
        yield dict(name=f'{payload}-{occupancy}-qd{qd}-{mode}-{layout}-r{repeat+1}',args=argv,
                   payload=payload,occupancy=occupancy,qd=qd,mode=mode,layout=layout,repeat=repeat+1)

def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()

def comparison_report(out,results):
    groups={}
    for c in results:
        key=(c['payload'],c['occupancy'],c['qd'],c['mode'])
        groups.setdefault(key,{}).setdefault(c['layout'],[]).append(c)
    lines=['# Matched layout comparison','',
           'Simulator evidence only. CPU is measured worker user+system time including final required barriers; setup/verification excluded.',
           'Ops/s sums reads and replacements. Replacements retire an old version. Target rates use the common process measurement interval.',
           'Sample ranges describe observed variation, not confidence intervals. A single short run is functional evidence only.','',
           '| Payload | Occupancy | Workers | I/O | Layout | Pass/total | Median ops/s | Min–max ops/s | Median read/write p99 ms | Median CPU µs/op | Median allocated bytes |',
           '|---|---:|---:|---|---|---:|---:|---:|---:|---:|---:|']
    for key,layouts in groups.items():
        for layout,cases_ in layouts.items():
            good=[c['fields'] for c in cases_ if c['status']=='PASS']
            def med(field):return statistics.median(float(f.get(field,0)) for f in good) if good else 0
            rates=[float(f['ops_s']) for f in good] or [0]
            lines.append(f'| {key[0]} | {key[1]}% | {key[2]} | {key[3]} | {layout} | {len(good)}/{len(cases_)} | {med("ops_s"):.0f} | {min(rates):.0f}–{max(rates):.0f} | {med("read_p99_us")/1000:.3f}/{med("write_p99_us")/1000:.3f} | {med("cpu_us_per_op"):.2f} | {med("allocated_after"):.0f} |')
    lines+=['','## Target samples','', '| Case | Target | Workers | Ops/s | Allocated bytes |', '|---|---:|---:|---:|---:|']
    for c in results:
        for t in c.get('targets',[]):lines.append(f'| {c["name"]} | {t["target"]} | {t["workers"]} | {t["ops_s"]} | {t["allocated_after"]} |')
    if results:
        f=results[0].get('fields',{})
        lines[2:2]=[f'Persistence: {f.get("persist","unknown")}; batch: {f.get("batch","unknown")}; seed: {f.get("seed","unknown")}; hot set/access: {f.get("hot_set_pct","unknown")}/{f.get("hot_access_pct","unknown")}%.' , '']
    (out/'COMPARISON.md').write_text('\n'.join(lines)+'\n')

def report(out,results):
    lines=['# W90 storage churn results','',
           'Independent workload with a volatile index and worker-local reclamation; not production GC or crash recovery.',
           'Container size is a fixed logical file length; sparse and preallocated allocation are distinct. Occupancy is bounded data-slot capacity with one extra cleaning container per worker.',
           'Buffered reads can be cached. No global cache drop, mount change or host-disk occupancy change is performed. Gross released blocks do not equal net free space or NAND erasure.',
           'each/batch times required body/directory sync. Deferred file batching still flushes each pending live body; legacy immediate scheduling is a separate reference. No durable index/recovery is modeled.',
           '', '| Payload | Occupancy | Workers | I/O | Layout | File flush | Status | Ops/s | Read p99 ms | Write p99 ms | Copy / new record bytes | Cleanings |',
           '|---|---:|---:|---|---|---|---|---:|---:|---:|---:|---:|']
    for c in results:
        f=c.get('fields',{});writes=int(f.get('writes',0));copies=int(f.get('copy_bytes',0));stride=int(f.get('record_bytes',1))
        copy_ratio=copies/(writes*stride) if writes else 0
        file_flush=('deferred' if f.get('file_batch_sync')=='1' else 'immediate') if c['layout']=='files' else '—'
        lines.append(f"| {c['payload']//1024} KiB | {c['occupancy']}% | {c['qd']} | {c['mode']} | {c['layout']} | {file_flush} | {c['status']} | {float(f.get('ops_s',0)):.0f} | {int(f.get('read_p99_us',0))/1000:.3f} | {int(f.get('write_p99_us',0))/1000:.3f} | {copy_ratio:.2f} | {f.get('cleanings','—')} |")
    (out/'SUMMARY.md').write_text('\n'.join(lines)+'\n')
    comparison_report(out,results)
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')

def main():
    a=parser().parse_args();matrix=list(cases(a));binary=a.binary.resolve()
    if a.dry_run:
        for c in matrix:print(c['name']+': '+shlex.join([str(binary),*c['args'],*[arg for i in range(len(a.storage_root)) for arg in ('--storage',f'<private-empty-target-{i}>')]]))
        return 0
    if not binary.is_file():raise ValueError('build efs-bench first')
    parents=[p.resolve() for p in a.storage_root]
    if len(set(parents))!=len(parents):raise ValueError('duplicate scratch parents')
    if any(not p.is_dir() for p in parents):raise ValueError('scratch parents must already exist')
    out=(a.output or ROOT/'logs'/('churn-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))).resolve()
    out.mkdir(parents=True,exist_ok=False)
    sources={str(p.relative_to(ROOT)):digest(p) for p in [ROOT/'src/bench/storage_churn.c',ROOT/'src/bench/storage_churn.h',ROOT/'src/bench/latency.h',ROOT/'scripts/bench_churn.py',ROOT/'src/bench/perf_control.h',ROOT/'src/client/efs_bench.c',ROOT/'Makefile',*[p for p in [ROOT/'src/bench/storage_ranges.c',ROOT/'src/bench/storage_ranges.h'] if p.is_file()]]}
    manifest={'binary':str(binary),'binary_sha256':digest(binary),'sources':sources,'arguments':vars(a),'uname':os.uname(),'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat()}
    (out/'manifest.json').write_text(json.dumps(manifest,default=str,indent=2)+'\n')
    shutil.copy2(binary,out/'efs-bench')
    with zipfile.ZipFile(out/'sources.zip','w',compression=zipfile.ZIP_DEFLATED) as archive:
        for name in sources:archive.write(ROOT/name,name)
    manifest['targets']=[{'target':i,'root':str(p),'device_id':p.stat().st_dev} for i,p in enumerate(parents)]
    (out/'manifest.json').write_text(json.dumps(manifest,default=str,indent=2)+'\n')
    for cmd,name in [*([['findmnt','-T',str(p),'-o','TARGET,SOURCE,FSTYPE,OPTIONS'],f'filesystem-{i}'] for i,p in enumerate(parents)),(['lsblk','-d','-o','NAME,MODEL,SIZE,ROTA'],'devices')]:
        try:
            p=subprocess.run(cmd,text=True,capture_output=True);metadata=p.stdout+p.stderr
        except OSError as e:metadata=f'unavailable: {e}\n'
        (out/(name+'.txt')).write_text(metadata)
    results=[];failed=False;traces={}
    for n,c in enumerate(matrix,1):
        case_dir=out/c['name'];case_dir.mkdir()
        scratches=[Path(tempfile.mkdtemp(prefix='w90-',dir=p)) for p in parents]
        cmd=[str(binary),*c['args'],*[arg for scratch in scratches for arg in ('--storage',str(scratch))]]
        (case_dir/'command.json').write_text(json.dumps(cmd,indent=2)+'\n')
        begin=time.monotonic();print(f'[{n}/{len(matrix)}] {c["name"]}',flush=True)
        try:
            p=subprocess.run(cmd,text=True,capture_output=True,timeout=a.timeout)
            stdout,stderr=p.stdout,p.stderr
            line=next((s for s in stdout.splitlines() if s.startswith('BENCH_OK ')),None)
            fields=dict(re.findall(r'(\w+)=([^\s]+)',line or ''))
            workload=next((s for s in stdout.splitlines() if s.startswith('WORKLOAD ')), '')
            fields.update(dict(re.findall(r'(\w+)=([^\s]+)',workload)))
            c['targets']=[dict(re.findall(r'(\w+)=([^\s]+)',s)) for s in stdout.splitlines() if s.startswith('TARGET ')]
            key=(c['payload'],c['occupancy'],c['qd'],c['mode'],c['repeat'])
            matched=True
            if a.compare and not a.seconds:
                signature=(fields.get('trace'),fields.get('reads'),fields.get('writes'),fields.get('live_objects'))
                matched=signature==traces.setdefault(key,signature) and signature[0] is not None
                c['matched_trace']=matched
            valid=matched and len(c['targets'])==len(parents) and p.returncode==0 and line and fields.get('errors')=='0' and fields.get('verified_objects')==fields.get('live_objects')
            c.update(status='PASS' if valid else 'FAIL',fields=fields,returncode=p.returncode)
        except subprocess.TimeoutExpired as e:
            stdout=e.stdout or b'';stderr=e.stderr or b''
            if isinstance(stdout,bytes):stdout=stdout.decode(errors='replace')
            if isinstance(stderr,bytes):stderr=stderr.decode(errors='replace')
            c.update(status='TIMEOUT',fields={},returncode=None)
        c['elapsed_s']=time.monotonic()-begin
        (case_dir/'stdout.txt').write_text(stdout);(case_dir/'stderr.txt').write_text(stderr)
        results.append(c);report(out,results)
        if c['status']=='PASS':
            for scratch in scratches:
                if list(scratch.iterdir()):raise RuntimeError('passing case left storage behind')
                scratch.rmdir()
        else:
            c['retained_scratch']=[str(scratch) for scratch in scratches];failed=True;report(out,results)
            print('Failure retained; stopping matrix.',flush=True);break
        print(f"  {c['status']} {c['elapsed_s']:.1f}s",flush=True)
    return 1 if failed else 0
if __name__=='__main__':
    try:raise SystemExit(main())
    except ValueError as e:raise SystemExit(str(e))
