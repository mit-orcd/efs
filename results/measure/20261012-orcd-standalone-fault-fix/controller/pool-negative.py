import importlib.util,subprocess
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
root='/tmp/efs-repair-pool-negative-20261012'
body=f'''test ! -e {root}
cp -a /tmp/efs-repair-final-normal-20261012 {root}
cd {root}
python3 - <<'INNER'
from pathlib import Path
p=Path('src/server/peer_pool.c');s=p.read_text();s=s.replace('    if (p->down_until_ms > now) {{','    if (p->down_until_ms > now) {{\\n        fprintf(stderr,"Repair negative pool cooldown: host=%s port=%u remaining_ms=%llu\\\\n",host,port,(unsigned long long)(p->down_until_ms-now));');p.write_text(s)
p=Path('src/server/repair_worker.c');s=p.read_text().replace('#include <stdlib.h>','#include <stdlib.h>\\n#include <stdio.h>');s=s.replace('    if(!port)return EFS_ERR_IO;','    if(!port){{fprintf(stderr,"Repair negative missing route: node=%u\\\\n",node);return EFS_ERR_IO;}}');s=s.replace('struct efs_conn *c=server_peer_conn_get(addr,port);if(!c)return EFS_ERR_IO;','struct efs_conn *c=server_peer_conn_get(addr,port);if(!c){{fprintf(stderr,"Repair negative pool refused: node=%u host=%s port=%u\\\\n",node,addr,port);return EFS_ERR_IO;}}');p.write_text(s)
INNER
export PKG_CONFIG_PATH={m.BULK}/deps/libfuse/lib/pkgconfig
export LD_LIBRARY_PATH={m.BULK}/deps/libfuse/lib${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}
make -j12 EFS_GIT_ID=orcd-repair-pool-negative EFS_FAULTS=1 efsd efs-fuse efs-mgmt tests/faults/fault_sim tests/faults/repair_request > /tmp/efs-repair-pool-negative-build.log 2>&1
export EFS_FAULT_BIN={root}
set +e
bash tests/faults/run_faults.sh /tmp/efs-repair-pool-negative-results-20261012 --integrate --mode direct --transport tcp --repair-experiment > /tmp/efs-repair-pool-negative.log 2>&1
rc=$?;echo POOL_NEGATIVE_EXIT=$rc
grep -h 'Repair negative' /tmp/efs-repair-pool-negative-results-20261012/cluster/s*/efsd.log || true
'''
print(m.shell('fcstor006',body,1800),flush=True)
