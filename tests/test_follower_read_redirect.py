#!/usr/bin/env python3
"""A stale follower may redirect only after an authoritative remote read."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_follower_read_result(');b=s.index('static int host_read_index(',a)
code='#include "efs/common.h"\n#include <assert.h>\n#include <stdio.h>\n'+s[a:b]+r'''
int main(void){
 int hint=-1;
 assert(host_follower_read_result(EFS_ERR_BUSY,0,4,1,&hint)==EFS_ERR_NOT_PRIMARY&&hint==1);
 assert(host_follower_read_result(EFS_ERR_BUSY,0,4,1,NULL)==EFS_ERR_NOT_PRIMARY);
 for(int bad=-1;bad<=4;bad+=5){hint=23;assert(host_follower_read_result(EFS_ERR_BUSY,0,4,bad,&hint)==EFS_ERR_BUSY&&hint==23);}
 hint=23;assert(host_follower_read_result(EFS_ERR_BUSY,0,4,0,&hint)==EFS_ERR_BUSY&&hint==23);
 int errors[]={EFS_OK,EFS_ERR_IO,EFS_ERR_NET,EFS_ERR_NOMEM,EFS_ERR_STALE};
 for(unsigned i=0;i<5;i++){hint=23;assert(host_follower_read_result(errors[i],0,4,1,&hint)==errors[i]&&hint==23);}
 puts("follower read: applied timeout redirects to validated remote leader; unknown/self/out-of-range/transport and storage errors never grant a local read PASS");
}
'''
# The redirect is only reached inside the successful remote-authority branch.
branch=s[s.index('rc = host_remote_read_index(h, group, v.leader, &end, &ridx,'):]
assert 'if (rc == EFS_OK) {\n                rc = host_wait_applied' in branch
with tempfile.TemporaryDirectory(prefix='efs-read-redirect-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
