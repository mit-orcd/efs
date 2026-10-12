from pathlib import Path
import json,re,collections,hashlib,datetime
base=Path('/Users/mike/Documents/Codex/2026-10-07/do-x20/work/release-v0.3.0-pre-alpha/results/measure/20261012-v030-query-stage-diagnostic/d485afbf/direct')
raw=base/'gc-direct-3j9zzdm2'
out=Path('/Users/mike/Documents/Codex/2026-10-08/efs/work/devel-orcd/results/measure/20261011-orcd-coalescing-rollout/d485-directory-query-analysis')
pat=re.compile(r'^(\S+) query-stage: stage=(\S+) rc=(-?\d+) elapsed_ms=(\d+) before=(\d+) after=(\d+)')
def seconds(a,b):return (datetime.datetime.fromisoformat(b.replace('Z','+00:00'))-datetime.datetime.fromisoformat(a.replace('Z','+00:00'))).total_seconds()
summary={'source':'d485afbffc3585d628574beaf98f190b90d469f1','scope':'read-only analysis of committed instrumented evidence, no new run','inputs':{},'nodes':{},'clients':{}}
for h in ('n2','n3'):
 p=raw/(h+'-query-stages.log');ls=p.read_text().splitlines();(out/p.name).write_bytes(p.read_bytes());summary['inputs'][str(p)]={'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
 ev=[m.groups() for l in ls if (m:=pat.match(l))];root=collections.defaultdict(list);nested=collections.Counter();inner=collections.Counter()
 for t,st,rc,ms,a,b in ev:
  if st=='request-root-authority':root[(a,b)].append(t);inner[(rc,a,b)]+=1
  if st=='page-consume' and rc=='-13':nested[(rc,a,b)]+=1
 assert inner==nested,(h,'root and outer refusal mismatch')
 cuts=[(t,int(a),int(b)) for t,st,rc,ms,a,b in ev if st=='transaction-cut' and rc=='-13'];assert all(a!=b for t,a,b in cuts)
 pubs=[l for l in ls if 'query-publish:' in l];times={int(re.search(r'generation=(\d+)',l)[1]):l.split()[0] for l in pubs}
 summary['nodes'][h]={'transaction_mismatch_count':len(cuts),'capture_BUSY':sum(st=='capture' and rc=='-13' for t,st,rc,ms,a,b in ev),'root_authority_count':sum(map(len,root.values())),'nested_page_consume_counts_match_per_root_generation':True,'root_retries':[{'root':int(k[0]),'generation':int(k[1]),'count':len(v),'first':v[0],'last':v[-1],'span_seconds':seconds(v[0],v[-1])} for k,v in sorted(root.items())],'generation2_to3_publication_gap_seconds':seconds(times[2],times[3]),'publication_records':pubs,'pending_record_count':sum(st=='pending-record' for t,st,rc,ms,a,b in ev)}
for p in sorted(raw.glob('posix-client-*.tsv')):
 vals=[l.split('\t') for l in p.read_text().splitlines() if not l.startswith('#') and '\t' in l];vals=[v for v in vals if v[1] in ('PASS','FAIL','SKIP','NOTRUN')]
 summary['clients'][p.name]={'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'counts':dict(collections.Counter(v[1] for v in vals)),'failures':[v for v in vals if v[1]=='FAIL']}
assert len(summary['clients'])==7
assert {r['root'] for r in summary['nodes']['n2']['root_retries']}=={r['root'] for r in summary['nodes']['n3']['root_retries']}
(out/'analysis.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps({h:{k:v for k,v in r.items() if k not in ('root_retries','publication_records')} for h,r in summary['nodes'].items()},indent=2))
