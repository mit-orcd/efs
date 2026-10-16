import control as c
import sys,json,shlex,collections
label=sys.argv[1]
code='''import pathlib,json,csv,collections
base=pathlib.Path(BASE)
result={}
for d in sorted(base.iterdir()):
 if not d.is_dir():continue
 r={}
 for name in ('workload.exit','posix.exit'):
  if (d/name).exists():r[name]=(d/name).read_text().strip()
 if (d/'workload.json').exists():r['workload']=json.loads((d/'workload.json').read_text())
 if (d/'posix.tsv').exists():
  rows=list(csv.DictReader((x for x in (d/'posix.tsv').open() if not x.startswith('#')),delimiter='\\t'));r['posix_rows']=rows
 if (d/'resource.identity.json').exists():r['identity']=json.loads((d/'resource.identity.json').read_text())
 if (d/'resource.jsonl').exists():r['resource']=[json.loads(x) for x in (d/'resource.jsonl').read_text().splitlines()]
 result[d.name]=r
print(json.dumps(result))
'''.replace('BASE',repr(c.BASE+'/'+label))
r=c.run('fcstor007','python3 -c '+shlex.quote(code))
data=json.loads(r);(c.P/(label+'-results.json')).write_text(json.dumps(data))
merged=collections.defaultdict(list);counts=collections.Counter()
for h,v in data.items():
 if 'workload' in v:
  for row in v['workload']['rows']:merged[row['op']].append(row)
 if 'posix_rows' in v:
  for row in v['posix_rows']:counts.update([row.get('result',row.get('status','?'))])
 print(h,{k:v[k] for k in ('workload.exit','posix.exit') if k in v},'posix',len(v.get('posix_rows',[])))
print('POSIX',dict(counts))
for op,rows in merged.items():
 vals=sorted(r['ms'] for r in rows);print(op,len(vals),'errors',sum(r['error'] is not None for r in rows),'p50/p95/p99/max',*[round(vals[int(f*(len(vals)-1))],2) for f in (.5,.95,.99,1)])
 if any(r['error'] for r in rows):print('errors',collections.Counter(r['error'] for r in rows if r['error']).most_common(3))
