import ast,json,hashlib
from pathlib import Path
original=Path('/tmp/amd-all-follow-approved.py').read_text();tree=ast.parse(original)
loops=[n for n in tree.body if isinstance(n,ast.For) and isinstance(n.iter,ast.Tuple) and [getattr(x,'value',None) for x in n.iter.elts]==['direct','buffered']]
assert len(loops)==1
loop=loops[0];loop.iter=ast.Tuple(elts=[ast.Constant(value='buffered')],ctx=ast.Load());ast.fix_missing_locations(loop)
base=Path('/data1/efs/intel-qual-28b2089b');src=base/'normal';fault=base/'fault';out=base/'evidence';cases=[]
def gate(name,cmd,env=None,timeout=1800,cwd=src):
 cmd=list(map(str,cmd));variant='fault' if str(cwd)==str(fault) else 'normal';host='intel2_efs' if variant=='fault' or name.startswith(('fault-integration','valgrind')) else 'intel1_efs'
 temp=Path('/tmp/efs-intel-28b2089b-'+host)
 if 'query_publication.py' in ' '.join(cmd):cmd+=['--mount',str(temp/'mnt'),'--peer-mount',str(temp/'peer')]
 if name.startswith('fault-integration'):cmd[2]=str(temp/'fault-integration')
 if name.startswith('valgrind'):cmd[2]=str(temp/'valgrind')
 cases.append(dict(name=name,command=cmd,environment=env or {},timeout=timeout,cwd=str(cwd),variant=variant,host=host));return 0
def gc(name,args,mode,variant='normal',timeout=1800):
 s=fault if variant=='fault' else src;host='intel2_efs' if variant=='fault' else 'intel1_efs';temp=Path('/tmp/efs-intel-28b2089b-'+host)
 work=out/'live'/name
 cmd=['python3','-u',s/'tests/live/gc_reclamation.py','--mode',mode,'--transport','tcp','--root',work,'--root',work,'--port','34210','--mount',temp/'mnt','--peer-mount',temp/'peer',*args]
 if variant=='fault':cmd+=['--server-binary',s/'efsd','--client-binary',s/'efs-fuse']
 return gate(name,cmd,timeout=timeout,cwd=s)
exec(compile(ast.Module(body=[loop],type_ignores=[]),'<approved-buffered-mode-loop>','exec'))
assert len(cases)==48,len(cases)
for c in cases:
 if c['name'].startswith(('query-','large-delta-')):
  c['command'][3]='/tmp/efs-intel-28b2089b-'+c['host']+'/'+c['name']
 if c['name'].startswith('valgrind'):c['environment']['EFS_VG_PORT']='34320'
for host in ('intel1_efs','intel2_efs'):
 records=[c for c in cases if c['host']==host]
 Path('work/intel-28b2089b-'+host+'-cases.json').write_text(json.dumps(records,indent=2));print(host,len(records))
Path('outputs/intel-28b2089b-inventory.json').write_text(json.dumps({'source':'28b2089be6a5df835cc7bb46b9c2a9e2f10df190','approved_runner_sha256':hashlib.sha256(original.encode()).hexdigest(),'cases':cases},indent=2))
Path('work/intel-28b2089b-approved-original.py').write_text(original)
