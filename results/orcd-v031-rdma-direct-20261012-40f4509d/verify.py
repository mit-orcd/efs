from pathlib import Path
import json,csv,collections,hashlib
p=Path('work/fix-orcd-valgrind/results/orcd-v031-rdma-direct-20261012-40f4509d')
manifest=json.loads((p/'artifact-sha256.json').read_text())
for name,sha in manifest.items():assert hashlib.sha256((p/name).read_bytes()).hexdigest()==sha,name
meta=json.loads((p/'profile-stage.json').read_text());runtime=json.loads((p/'profile-runtime.json').read_text());expected={r['host']:dict((line.split()[1],line.split()[0]) for line in r['sha256'].splitlines()) for r in meta['hosts']}
for row in runtime:
 assert row['source']==meta['revision']
 for proc in row['processes']:assert proc['exe_sha256']==expected[row['host']][proc['comm']]
for tag in [meta['revision'][:12],meta['revision'][:12]+'-repeat1']:
 total=collections.Counter()
 for file in sorted((p/'remote').glob('fcstor*/profile-posix-'+tag+'.tsv')):
  rows=csv.DictReader([x for x in file.read_text().splitlines() if not x.startswith('#')],delimiter='\t');counts=collections.Counter(r['result'] for r in rows);total.update(counts);print(file.parent.name,tag,dict(counts))
 assert total=={'PASS':1512},total
 file=p/'remote/fcstor007'/('profile-peer-'+tag+'.tsv');rows=csv.DictReader([x for x in file.read_text().splitlines() if not x.startswith('#')],delimiter='\t');assert collections.Counter(r['result'] for r in rows)=={'PASS':67}
prereq=json.loads((p/'remote/fcstor006'/('v031-'+meta['revision'][:12]+'-prerequisites.json')).read_text());assert all(x['exit']==0 for x in prereq)
proof=json.loads((p/'source-proof-installed.json').read_text());assert len(proof)==11 and all(not r['mismatches'] for r in proof)
gc=json.loads((p/'profile-ordinary-gc.json').read_text());assert all(not files for row in gc['after'] for files in row['files'].values())
print('PASS: exact source/binaries, two concurrent core+peer rounds, native fault/sanitizer prerequisites, ordinary physical GC,',len(manifest),'artifact hashes')
