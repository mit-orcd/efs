from pathlib import Path
import json,hashlib
p=Path('work/fix-orcd-valgrind/results/measure/20261012-orcd-standalone-fault-fix');r=p/'remote';gates={}
for name in ('fault-native','normal-native','rdma','tcp'):
 rc=int((r/('efs-qualified-v3-'+name+'.exit')).read_text());assert rc==0;gates[name]={'status':'PASS','exit':rc,'log':'remote/efs-qualified-v3-'+name+'.log'}
for transport in ('rdma','tcp'):
 root=r/('efs-qualified-v3-'+transport+'-ed749cab')
 rows=[json.loads(x) for x in (root/'results.jsonl').read_text().splitlines()];assert len(rows)==23 and all(x['status']=='PASS' for x in rows)
 repairs=[x for x in rows if x['layer']=='integration' and x['case']=='repair_restores_protection'];assert {x['role'] for x in repairs}=={0,1,2}
 assert all(x['repaired'] and x['protected'] and x['expected_sha256']==x['actual_sha256'] for x in repairs)
 proofs=[json.loads(x) for x in (root/'cluster/journal.jsonl').read_text().splitlines() if json.loads(x)['op']=='transport']
 assert proofs and all(x['transport']==transport for x in proofs)
 assert all(bool(x['uverbs'])==(transport=='rdma') for x in proofs)
 gates[transport].update(pass_rows=len(rows),repair_roles=[0,1,2],transport_proofs=len(proofs),mode='direct',results=str((root/'results.jsonl').relative_to(p)))
asan=(r/'efs-qualified-v3-worker-asan.log').read_text();assert 'PASS' in asan and 'ERROR: AddressSanitizer' not in asan and 'runtime error:' not in asan
wire=json.loads((r/'efs-qualified-v3-wire-ed749cab/result.json').read_text());assert wire['status']=='PASS'
for f in Path('operations/orcd-fault-fix').glob('*'):
 if f.is_file() and f.suffix in ('.py','.json','.log'):(p/'controller'/f.name).write_bytes(f.read_bytes())
summary=dict(commit='ed749cab212768137a32ee8d3a2b80fa79744215',branch='fix-orcd-fault-repair-rdma',host='fcstor006',gates=gates,worker_asan_ubsan='PASS',wire_policy='PASS',source_proof=json.loads(Path('operations/orcd-fault-fix/source-proof.json').read_text()),cleanup=json.loads(Path('operations/orcd-fault-fix/cleanup.json').read_text()),installed_preservation=json.loads(Path('operations/orcd-fault-fix/main-preservation.json').read_text()),scope='default-off experimental missing ordinary base role, same target/incarnation; not automatic or comprehensive recovery',remaining=['packed objects','delta spans','containers','spare relocation','empty replacement','existing damaged-role replacement','automatic repair','interrupted-job leader takeover','comprehensive pending GC/late writer overlap'],prior_failures_preserved=['short-write delta refused','old shared-pool TCP source connection unavailable','1970 archive timestamps rejected by ZIP fixtures','omitted historical evidence broke documentation links'])
(p/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
files={str(f.relative_to(p)):hashlib.sha256(f.read_bytes()).hexdigest() for f in p.rglob('*') if f.is_file() and f.name!='artifact-sha256.json'};(p/'artifact-sha256.json').write_text(json.dumps(files,indent=2)+'\n');print(str(len(files))+' hashed evidence files; final normal/fault/native TCP/RDMA/direct repair qualification PASS')
