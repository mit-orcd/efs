#!/usr/bin/env python3
"""K52 four-host diagnostic controller. Requires explicit host-use approval.

Run from a source-only tree on a login/controller host. Config describes
fcstor012-015 only; source/store/mount roots must be a private task subdirectory.
Every stage gets fresh stores. Checked teardown keeps stores for investigation;
separate evidence-preserving cleanup removes them after the run.
"""
import argparse
import json
from pathlib import Path
import shlex
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts'))
import qualify as Q


class RetainedCluster(Q.PrivateCluster):
    def owned_teardown(self, g):
        errors=[]
        for m in self.q.cluster['mounts']:
            path=shlex.quote(m['path'])
            rc,text=g.run('teardown-unmount',m['host'],f'mountpoint -q {path} || exit 0; '
                f'cd {shlex.quote(self.q.source(m["host"]))} && scripts/client.sh stop {path} && ! mountpoint -q {path}',300)
            if rc:errors.append(f'unmount {m["host"]}: {rc}: {text[-200:]}')
        if errors:return 1,'; '.join(errors)
        rc,text=self.stop_cluster(g)
        if rc:errors.append(f'stop {rc}: {text[-200:]}')
        for n in self.nodes:
            p=Path(n['store'])
            g.fetch(n['host'],str(p.parent/(p.name+'.log')),f'closed-{n["host"]}.log')
        for m in self.q.cluster['mounts']:
            p=Path(m['path'])
            g.fetch(m['host'],str(p.parent/('efs-fuse-'+p.name+'.log')),f'closed-client-{m["host"]}.log')
        return (1,'; '.join(errors)) if errors else (0,'clean stop; stores retained until evidence-preserving cleanup')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cluster',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--candidate',required=True)
    a=p.parse_args()
    base=json.loads(a.cluster.read_text())
    assert {n['host'] for n in base['nodes']}=={'fcstor012','fcstor013','fcstor014','fcstor015'}
    root=base['private_cluster']['root']
    assert root.startswith('/data1/01/efs-taskX4-') and '..' not in Path(root).parts
    a.out.mkdir(parents=True,exist_ok=False)
    Q.PrivateCluster=RetainedCluster
    results=[]
    for label,reader in [('original','in-process'),('spawn-reader','spawn'),('corrected',None)]:
        c=json.loads(json.dumps(base))
        stage_root=root+'/'+label
        for n in c['nodes']:n['store']=n['store'].replace(root,stage_root,1)
        for m in c['mounts']:m['path']=m['path'].replace(root,stage_root,1)
        c['private_cluster']['root']=stage_root
        c['fixture']=dict(Q.DEFAULT_FIXTURE,**c.get('fixture',{}))
        gates='identity,topology,d3-efs-load'
        if reader:
            c['plugins']={'k52-readback':dict(host='fcstor012',deadline_s=2400,items='K52/D3',
                ledger='Enabled `.efs` under load',command='cd {source} && python3 tests/live/k52_readback.py '
                '{mount} '+label+' --reader '+reader+' --evidence '+shlex.quote(str(a.out/(label+'-observations'))))}
            gates='identity,topology,k52-readback'
        config=a.out/(label+'-cluster.json');config.write_text(json.dumps(c,indent=2)+'\n')
        rc=Q.main(['run','--cluster',str(config),'--candidate',a.candidate,'--gates',gates,
            '--out',str(a.out/label),'--work',str(a.out.parent/(a.out.name+'-work-'+label))])
        results.append(dict(stage=label,rc=rc))
        (a.out/'stages.json').write_text(json.dumps(results,indent=2)+'\n')
        teardown=json.loads((a.out/label/'gates/_teardown/result.json').read_text())
        if teardown['verdict']!='PASS':
            raise RuntimeError('unsafe teardown: further stages refused')
    return int(any(r['rc'] for r in results))


if __name__=='__main__':
    raise SystemExit(main())
