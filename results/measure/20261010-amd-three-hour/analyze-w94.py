#!/usr/bin/env python3
"""Summarize completed finite CREATE cases; never mix oracle time into rate."""
import argparse,hashlib,json,statistics
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('root',type=Path)
p.add_argument('--partial',action='store_true')
a=p.parse_args();cases=[];missing=[]
inventories={entry['case']:entry for entry in json.loads((a.root/'physical-inventory-proof.json').read_text())}
for repeat in range(1,5):
    for mode in ('direct','buffered'):
        for variant in ('separate','combined'):
            folder=a.root/f'r{repeat}-{mode}-{variant}'
            exit_path=folder/'exit-code'
            if not exit_path.exists():missing.append(str(folder));continue
            assert exit_path.read_text().strip()=='0',folder
            paths=list(folder.glob('gc-*/tiny-mixed.json'));assert len(paths)==1,paths
            raw=paths[0].read_bytes();record=json.loads(raw)
            inventory=inventories[folder.name]
            assert inventory['result_sha256']==hashlib.sha256(raw).hexdigest()
            assert len(inventory['inventories'])==4 and all(entry['physical_files']==0 for entry in inventory['inventories'])
            manifest_raw=(paths[0].parent/'manifest.json').read_bytes()
            assert inventory['manifest_sha256']==hashlib.sha256(manifest_raw).hexdigest()
            manifest=json.loads(manifest_raw)
            assert manifest['real_roots']==[entry['root'] for entry in inventory['inventories']]
            assert len(set(manifest['real_roots']))==4
            assert manifest['mode']==mode and manifest['gates']['transport']=='tcp'
            assert manifest['gates']['tiny_create_only'] and manifest['gates']['tiny_mixed_files']==8192
            assert manifest['gates']['storage_roots']==1 and manifest['port']==24190
            assert record['tiny_files']==8192 and record['tiny_bytes']==512 and record['large_files']==0,record
            assert record['large_written_bytes']==[] and record['wall_s']>0
            # The first baseline ran the original serial oracle; subsequent
            # cases explicitly record eight cold verification workers.
            assert record.get('cold_verification_workers',1)==(1 if repeat==1 and mode=='direct' and variant=='separate' else 8)
            assert 'exact cold bytes and reclaim PASS' in (folder/'run.log').read_text()
            cases.append(dict(repeat=repeat,mode=mode,variant=variant,files_s=8192/record['wall_s'],
                result=str(paths[0]),sha256=hashlib.sha256(raw).hexdigest(),binary_sha256=manifest['binary_sha256'],**record))
if not a.partial:
    assert not missing,missing
    for name in ('round.exit','restore.exit'):assert (a.root/name).read_text().strip()=='0',name
# Each variant must retain its frozen executables through every repeat.
for variant in ('separate','combined'):
    identities={json.dumps(row['binary_sha256'],sort_keys=True) for row in cases if row['variant']==variant}
    assert len(identities)<=1,('executable changed during A/B',variant,identities)
summary=[]
for mode in ('direct','buffered'):
    for variant in ('separate','combined'):
        rows=[r for r in cases if r['mode']==mode and r['variant']==variant]
        if not rows:continue
        rates=[r['files_s'] for r in rows]
        summary.append(dict(mode=mode,variant=variant,repeats=len(rows),files_s_median=statistics.median(rates),
            files_s_min=min(rates),files_s_max=max(rates),files_s_cv_percent=100*statistics.pstdev(rates)/statistics.mean(rates),
            open_p99_us_median=statistics.median(r['open']['p99_us'] for r in rows),
            close_p99_us_median=statistics.median(r['close']['p99_us'] for r in rows),
            max_single_process_rss_kib=max(max(r['peak_rss_kib'].values()) for r in rows)))
paired={}
for mode in ('direct','buffered'):
    deltas=[]
    for repeat in range(1,5):
        pair={r['variant']:r for r in cases if r['mode']==mode and r['repeat']==repeat}
        if len(pair)==2:deltas.append(100*(pair['combined']['files_s']/pair['separate']['files_s']-1))
    if deltas:paired[mode]=dict(repeat_percent=deltas,median_percent=statistics.median(deltas))
result=dict(complete=not missing,cases=cases,summary=summary,paired=paired,missing=missing)
(a.root/'comparison.json').write_text(json.dumps(result,indent=2)+'\n')
print('| Mode | Adoption | Repeats | Files/s median (min–max) | Open p99 ms* | Close p99 ms* |')
print('|---|---|---:|---:|---:|---:|')
for r in summary:
    print(f"| {r['mode']} | {r['variant']} | {r['repeats']} | {r['files_s_median']:.1f} ({r['files_s_min']:.1f}–{r['files_s_max']:.1f}) | {r['open_p99_us_median']/1000:.2f} | {r['close_p99_us_median']/1000:.2f} |")
print('*Median of per-case percentiles; not pooled latency percentiles.')
print('Paired rate changes:',json.dumps(paired))
print('Completed cases:',len(cases),'/ 16')
