# Gate budget re-measurement (Task BN, 2026-10-30)

**Verdict: the re-measured table does not fit the 1650 s bound.** Trimmed to measurement
the budgets add up to **1729 s** (pool 1127 s + exclusive 602 s), 79 s over; with the two
pending changes 1734 s. The bound is not moved here; the user decides (shortfall section).
The edited [tests/budgets.tsv](../../tests/budgets.tsv) is therefore a *proposal* and
`python3 tests/test_make_test_recipe.py` fails on it by design until the bound, a slot
assignment or a test changes.

- Source: devel `e2c5067d1` (clean tree, `git archive HEAD`); sums before this change: 1646 s.
- Linked item: W160 gate budgets; Task T ([gate-budgets-per-host-20261028.md](gate-budgets-per-host-20261028.md)) is the standard.

## Hosts

| Host | CPU, disk | Gate (`make test-all`, 8 jobs) |
|---|---|---|
| amd | AMD Ryzen AI Max+ 395, 32 CPUs, ext4 NVMe, Python 3.14.4, kernel 7.0 | 1060 s, 1066 s, 821 s |
| node9900 | AMD EPYC 9474F, 192 CPUs, XFS md0 under `/tmp`, RHEL 9.8, Python 3.9 | 828 s, 719 s, 926 s |

Both are faster than the xorinox/intel1/fstor007 hosts Task T used, so rows whose Task T
median is higher keep that median as evidence (column "prior"). intel2's wave 42 and 44
gates (`/home/maik/efs-artifacts/merge-wave4{2,4}-20261030/timings.tsv`, one gate each,
passing rows) are a third reference. nuc and xorinox had no `timings.tsv` in the named
directories, so their evidence is only what existing notes record.

## Method

1. `TMPDIR=<local dir> EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh` three times per host,
   in `screen -dmS taskBN`, driver `runner.sh` (below), tree shipped once with `git archive HEAD`
   (amd `~/efs-taskBN-tree`, TMPDIR `~/tmp/taskBN`; node9900 `/tmp/efs-taskBN`, TMPDIR
   `/tmp/efs-taskBN-tmp`, removed with `edelete` afterwards).
2. **What `timings.tsv` records**: per command the wall seconds from its thread start to its
   end (pool queueing excluded), its exit status, and the command's first line cut to 150
   characters, the same string `tests/budgets.tsv` is keyed on and the same pool slots
   (`make_test_summary.sh` schedules by those rows). A row's seconds are therefore directly
   comparable to its budget; only runs with rc 0 count, a failing run's time is a failure time.
3. Per host median of the passing runs; evidence = the maximum of the amd, node9900 and
   intel2 medians and the earlier recorded measurement in the note (Task T median, K53,
   W165/K48, AO, Task O, W166, W170, Task W, put_quota nuc 33.2 s); proposed budget =
   1.25 x evidence rounded up to 5 s, floor 5 s; every row (101 rows) is replaced by that
   value, with one exception: a row with fewer than two passing runs on a host keeps
   max(current, proposal) and says so in its note. Every row now carries a `BN:` note with the
   slowest median and its host.
4. Sums: `python3 tests/test_make_test_recipe.py` (list schedule at 8 slots plus EXCLUSIVE rows in series).

Raw timings (outside Git): `~/efs-artifacts/taskBN-20261030/` on the Mac (copies of
`amd/` and `node9900/` run{1,2,3}-{timings.tsv,summary.txt}, `ref/` intel2 timings) with
`SHA256SUMS` in `amd/` and `node9900/` (and the originals under the same path on amd and node9900).

## Sums

| Table | Pool | Exclusive | Total | vs 1650 s |
|---|---|---|---|---|
| devel `e2c5067d1` | 1094 | 552 | 1646 | fits (4 s) |
| devel + Task BI `test_pump_logging` 85 s + W130 row at the 60 s placeholder (Task BG's budget is unknown to me) | 1104 | 552 | 1656 | 6 s over |
| **BN proposal** | 1127 | 602 | **1729** | **79 s over** |
| BN proposal + pump_logging 85 s + W130 60 s | 1132 | 602 | **1734** | **84 s over** |

`test_w130_dir_fault.py` already has a row (devel 40 s; branch `claude/bg-k48-1cef51` makes it 50 s), so the
placeholder replaces that row rather than adding a new one; adding a separate 60 s row instead would cost
at most another 60 s. In the BN proposal the W130 row is 50 s (intel1 K48 38 s x 1.25), amd 18.7 s, node9900 17.5 s, intel2 27.7 s.

## Shortfall: why measurement does not shrink the gate

Trimming guesses saves little. Nine rows fall (k41 packer -40, k38 protection -40, k36 segment
recovery -55, k36 recovery -20, k54 fence -15, put_quota -15, b6 slow -10, conn_idle -5, kv_lsm_concurrent -5);
fifteen rise because the 8-job medians exceed their budgets: `test_d4_progress` 115 -> 195 (2 slots, +80),
`test_dir_ops_busy` 25 -> 65 (amd 48 s on all three runs), `test_b_geometry` full 65 -> 110 (+45; 81-87 s
on all three hosts), `test_k33_interrupt` 85 -> 120, and three **EXCLUSIVE** rows that add in series:
`pack_dup_candidate` 55 -> 75, `w166_authority` 30 -> 50, `opid_admission` 15 -> 25 (+50 s of the 79).
The big rows stay: report step-down 315 (intel1 median 250.8 s) and snapshot import 425 (xorinox 337.9 s).
Levers for the user, none applied:

- Exclusive set (602 s, 36% of the budgeted gate): report step-down 315, pack_dup 75, w166 50,
  opid 25 are the large rows; moving `test_pack_dup_candidate` or `test_w166_authority` out of the
  `EXCLUSIVE` regex (pool slots instead) is a runner change (the regex is in `scripts/make_test_summary.sh`) and would take 125 s out of the series time.
- Slot column: `test_d4_progress` (2 slots, 195 s) is deliberately the largest 2-slot budget so
  it starts first; it measures 135-172 s here, so its budget is real cost.
- The bound itself (1650 s) is the user's decision.

## Rows that changed (budget s, measured passing-run medians, s)

| Command | Was | Now | amd | node9900 | intel2 | prior | Note |
|---|---|---|---|---|---|---|---|
| `python3 tests/test_k33_interrupt.py` | 85 | **120** | 93.6 | 32.3 | 51.8 | - |  |
| `python3 tests/test_k33_join_retry.py` | 10 | **15** | 8.3 | 8.4 | 8.2 | - |  |
| `python3 tests/test_k33_payload.py` | 45 | **55** | 29.5 | 42.3 | 31.9 | - |  |
| `python3 tests/test_k41_packer_placement.py` | 115 | **75** | 55.9 | 55.6 | 58.8 | - |  |
| `python3 tests/test_k38_protection_now.py` | 70 | **30** | 23.7 | 21.8 | 23.1 | - |  |
| `EFS_B_PHASES=full python3 tests/test_b_geometry.py` | 65 | **110** | 81.5 | 87.4 | 86.3 | intel1 50.7 |  |
| `python3 tests/test_k42_peer_stop_open.py` | 50 | **50** | 33.5 | 32.9 | 33.9 | xorinox 36.3 | amd <2 passes |
| `python3 tests/test_k54_member_down_fence.py` | 50 | **35** | 27.6 | 27.1 | 27.2 | - |  |
| `python3 tests/test_k38_shared_chunk_pressure.py` | 60 | **70** | 30.4 | 55.2 | 37.1 | xorinox 46.4 |  |
| `python3 tests/test_stop_progress.py` | 175 | **175** | 125.3 | 11.8 | 35.8 | xorinox 137.5 | amd <2 passes |
| `EFS_B6_MODES=slow python3 tests/test_b6_unavailable.py` | 270 | **260** | 197.0 | 197.5 | 204.9 | - |  |
| `python3 tests/test_d4_progress.py` | 115 | **195** | 153.9 | 134.6 | 79.4 | - |  |
| `python3 tests/test_pack_dup_candidate.py` | 55 | **75** | 59.9 | 54.6 | 46.5 | - |  |
| `python3 tests/test_conn_idle_reap.py` | 75 | **70** | 53.1 | 54.2 | 54.5 | - |  |
| `python3 tests/test_query_first_catalog.py` | 20 | **25** | 15.0 | 15.3 | 16.5 | - |  |
| `python3 tests/test_k36_recovery.py` | 90 | **70** | 32.3 | 50.6 | 52.2 | - |  |
| `python3 tests/test_k36_segment_recovery.py` | 155 | **100** | 61.5 | 36.4 | 77.2 | - |  |
| `python3 tests/test_opid_admission.py` | 15 | **25** | 16.9 | 16.9 | 16.9 | - |  |
| `./tests/test_kv_lsm_concurrent 4` | 10 | **5** | 4.0 | 4.0 | 4.0 | - |  |
| `EFS_QUERY_DELTA_PARENTS=64 EFS_QUERY_DELTA_LANES=1 EFS_QUERY_DELTA_MULTI_LANES=1 ./tests/test_query_delta` | 10 | **15** | 9.0 | 1.7 | 2.9 | xorinox 7.3 |  |
| `python3 tests/test_w130_dir_fault.py` | 40 | **50** | 18.7 | 17.5 | 27.7 | intel1 38.0 |  |
| `python3 tests/test_w166_authority.py` | 30 | **50** | 38.1 | 6.0 | 14.4 | xorinox 28.8 |  |
| `python3 tests/test_dir_ops_busy.py` | 25 | **65** | 48.2 | 17.7 | 19.5 | intel2 19.9 |  |
| `python3 tests/test_put_quota_reclaim.py` | 60 | **45** | 7.6 | 8.5 | 13.8 | nuc 33.2 |  |
| `python3 tests/test_pump_logging.py` | 65 | **75** | 56.8 | 56.6 | 56.5 | - |  |
| `python3 tests/test_k54_member2_unmount.py` | 60 | **85** | 67.6 | 8.7 | 10.1 | - | amd <2 passes |

75 rows keep their budget after the re-measurement (`BN:` note added). Three rows in the table (unchanged or raised) have fewer than two amd passes (`test_k42_peer_stop_open`, `test_stop_progress`,
`test_k54_member2_unmount`); they are flagged and keep max(current, proposal); their amd failures were
`fusermount3 -u` refusals and `efsd n4 exited` under the pool (not a code verdict; the node9900
medians are 32.9, 11.8 and 8.7 s).

## Failures and host limits (every gate FAILed or was INCOMPLETE; none is a verdict on the code)

| Run | Failures | NOTRUN |
|---|---|---|
| amd 1 | k54_member2_unmount, d4_plateau | cluster_stop (no `~/git/cluster`), store_durability (TMPDIR not XFS: ext4) |
| amd 2 | k42_peer_stop_open, k54_member_down_fence, k54_member2_unmount, stop_progress, settings_listing | same |
| amd 3 | k42_peer_stop_open, stop_progress | same |
| node9900 1 | k37_segment_rebuild | cluster_stop, rdma_memcheck and leak_gate (valgrind not installed) |
| node9900 2 | pump_logging (`foreground failures 2`) | same |
| node9900 3 | none | same |

Rows whose command did not run to a real verdict on these hosts: `test_cluster_stop.py` (NOTRUN, needs
`~/git/cluster`), `test_rdma_memcheck.py`, `test_leak_gate.py` real-memcheck fixtures (node9900 only, no valgrind),
`./tests/test_store_durability` (amd only, ext4). Their budgets (all 5 s) are measured only as the NOTRUN skip time.
`test_pump_logging.py` passed 3/3 on amd and 2/3 on node9900; median 56.8 s, so the proposal is 75 s (Task BI asks 85 s).
Passing medians exclude the failing runs, so rows that failed under load (k54 pair, d4_plateau, k37) may be understated.

## Reproduction

Prerequisites: a Linux host with the build toolchain, `screen`, libfuse (`/data1/efs/deps/libfuse` on ORCD).
Per host: `git archive e2c5067d1 | ssh host tar -x -C <tree>`; `SCREENDIR=$HOME/.screen screen -dmS taskBN bash runner.sh <tree> <tmpdir> <artifact dir>`;
about 15-18 minutes per gate. Then `python3 med.py`/`gen.py` below in a checkout (they read
the artifact directory and write the new table), and `python3 tests/test_make_test_recipe.py` for the sums.
Cleanup: `edelete --force --delete <tree> <tmpdir>`.

`runner.sh`:

```bash
TREE=$1; TMPB=$2; ART=$3
mkdir -p "$ART"; cd "$TREE" || exit 2
for r in 1 2 3; do
  mkdir -p "$TMPB"
  TMPDIR=$TMPB EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh > "$ART/run$r.out" 2>&1
  cp timings.tsv "$ART/run$r-timings.tsv"; cp summary.txt "$ART/run$r-summary.txt"
  (cd "$ART" && sha256sum run*-timings.tsv run*-summary.txt > SHA256SUMS)
done
```

Analysis (`med.py` then `gen.py`; paths in the source: the artifact directory and `ref/` intel2 copies):

```python
import csv,statistics as st,os,sys
D=os.path.expanduser('~/efs-artifacts/taskBN-20261030')
B={}
for l in open('tests/budgets.tsv'):
    f=l.rstrip('\n').split('\t')
    if l.startswith('#') or len(f)<4 or f[0]=='command': continue
    B[f[0].strip()]=(float(f[1]),int(f[2]),f[3])
T={}
for h in('amd','node9900'):
    for r in (1,2,3):
        for row in csv.DictReader(open(f'{D}/{h}/run{r}-timings.tsv'),delimiter='\t'):
            T.setdefault((row['command'].strip(),h),[]).append((float(row['seconds']),int(row['rc'])))
out={}
for c,(b,s,n) in B.items():
    r={}
    for h in('amd','node9900'):
        v=T.get((c[:150],h),[])
        ok=[x for x,rc in v if rc==0]
        r[h]=(st.median(ok) if ok else None,len(ok),len(v))
    out[c]=r
if __name__=='__main__':
    for c,r in out.items():
        b=B[c][0]; ms=[r[h][0] for h in r if r[h][0] is not None]
        m=max(ms) if ms else None
        p=max(5,-(-int(m*1.25*1000)//5000)*5) if m else None
        print(f"{b:g}\t{p}\t{r['amd'][0] and round(r['amd'][0],1)}({r['amd'][1]})\t{r['node9900'][0] and round(r['node9900'][0],1)}({r['node9900'][1]})\t{c[:70]}")

# ---- gen.py
import re,csv,os,statistics as st,math,sys
sys.path.insert(0,'/tmp/bn')
import med
B,out=med.B,med.out
D=os.path.expanduser('~/efs-artifacts/taskBN-20261030/ref')
i2={}
for fn in('intel2-w44.tsv','intel2-w42.tsv'):
    for r in csv.DictReader(open(f'{D}/{fn}'),delimiter='\t'):
        if int(r['rc'])==0: i2.setdefault(r['command'].strip(),[]).append(float(r['seconds']))
manual={'EFS_B6_MODES=full python3 tests/test_b6_unavailable.py':(318,'nuc'),'python3 tests/test_put_quota_reclaim.py':(33.2,'nuc'),
 'python3 tests/test_w130_dir_fault.py':(38,'intel1'),'python3 tests/test_w166_authority.py':(28.8,'xorinox'),'python3 tests/test_dir_ops_busy.py':(19.9,'intel2'),
 'python3 tests/test_k48_readdir.py':(11.91,'intel1'),'python3 tests/test_ao_readdir_live.py':(20.5,'intel1'),'python3 tests/test_bench_published.py':(7.7,'gate'),
 './tests/faults/test_follower_bound --stalled-compactor':(1.2,'gate')}
ceil5=lambda x:max(5,int(math.ceil(x/5.0-1e-9))*5)
rows=[];res=[]
for l in open('tests/budgets.tsv'):
    f=l.rstrip('\n').split('\t')
    if l.startswith('#') or len(f)<4 or f[0]=='command' : rows.append(l.rstrip('\n'));continue
    c=f[0].strip();b=float(f[1]);slots=f[2];reason=f[3]
    ev={};n={}
    for h in('amd','node9900'):
        m,ok,tot=out[c][h];n[h]=(ok,tot)
        if m is not None: ev[h]=m
    v=i2.get(c[:150],[])
    if v: ev['intel2']=st.median(v)
    mt=re.search(r'Task T: slowest host median ([\d.]+) s \((\w+)\)',reason)
    if mt: ev['prior:'+mt.group(2)]=float(mt.group(1))
    if c in manual: ev['prior:'+manual[c][1]]=manual[c][0]
    slow=max(ev,key=ev.get);prop=ceil5(1.25*ev[slow])
    thin=[h for h in('amd','node9900') if n[h][0]<2]
    new=max(b,prop) if thin else prop
    rest=re.sub(r'^Task T: slowest host median.*?previous budget \d+ s;?\s*','',reason)
    rest=re.sub(r'Task T: slowest host median [\d.]+ s \(\w+\); three runs/host, 1\.25x rounded up to 5 s; previous budget \d+ s','',rest).strip(' ;')
    sh=lambda h:('%s %.1f'%(h,ev[h])) if h in ev else '%s none'%h
    note='BN: slowest median %.1f s (%s) -> %g s; %s, %s%s%s; three passing runs/host (1.25x rounded up to 5 s); previous %g s'%(
        ev[slow],slow,new,sh('amd'),sh('node9900'),', intel2 %.1f (%d gates)'%(ev['intel2'],len(v)) if 'intel2' in ev else '',
        ''.join(', %s %.1f'%(k,x) for k,x in ev.items() if k.startswith('prior')),b)
    if thin: note=note.replace('three passing runs/host (1.25x rounded up to 5 s)','FEWER THAN 2 PASSING RUNS on '+'/'.join('%s %d/3'%(h,n[h][0]) for h in thin)+', previous budget kept if higher (1.25x rounded up to 5 s)')
    if rest: note+='; '+rest
    rows.append('\t'.join([c,'%g'%new,slots,note]));res.append((c,b,new,ev,thin,slow))
open('/tmp/bn/new.tsv','w').write('\n'.join(rows)+'\n')
import pickle;pickle.dump(res,open('/tmp/bn/res.pkl','wb'))
print(sum(1 for r in res if r[2]>r[1]),'raised',sum(1 for r in res if r[2]<r[1]),'lowered',sum(1 for r in res if r[2]==r[1]),'same','thin:',[(r[0],r[4]) for r in res if r[4]])

```

Limits: two host classes, three runs each, but only the passing ones; nuc and xorinox were not rerun;
intel2 contributes one gate per tree. Remaining gates: the user's decision on the 1650 s bound.
