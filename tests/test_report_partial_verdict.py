#!/usr/bin/env python3
"""Execute the production REPORT verdict after a known CAS loser + BUSY.

K38_HOST_SOURCE selects the unchanged server source for the negative control.
No record is acknowledged by STALE; the client must repull/rebuild. BUSY with
no definite loser, permanent failures and other ambiguous errors stand.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
s = Path(os.environ.get('K38_HOST_SOURCE', root/'src/server/raft_host.c')).read_text()
m = re.search(r'^static int host_report_stale_verdict\([^;]+?\)\n\{', s, re.M)
if m:
    body = s[m.start():s.index('\n}', m.end())+2]
else:
    # Execute the original source's terminal classification, not a model of it.
    a=s.index('        if (rc == EFS_OK && had_stale)')
    b=s.index('        host_report_note.phase',a)
    body='static int host_report_stale_verdict(int rc,int had_stale){\n'+s[a:b]+'return rc;\n}'
code = '#include "efs/common.h"\n#include <assert.h>\n#include <stdio.h>\n' + body + r'''
int main(void){
 assert(host_report_stale_verdict(EFS_ERR_BUSY,1)==EFS_ERR_STALE);
 assert(host_report_stale_verdict(EFS_OK,1)==EFS_ERR_STALE);
 assert(host_report_stale_verdict(EFS_ERR_BUSY,0)==EFS_ERR_BUSY);
 assert(host_report_stale_verdict(EFS_OK,0)==EFS_OK);
 int unchanged[]={EFS_ERR_IO,EFS_ERR_INVAL,EFS_ERR_PROTO,EFS_ERR_NET,
                  EFS_ERR_NOT_PRIMARY,EFS_ERR_NO_QUORUM,EFS_ERR_FENCED};
 for(unsigned i=0;i<sizeof(unchanged)/sizeof(*unchanged);i++)
  assert(host_report_stale_verdict(unchanged[i],1)==unchanged[i]);
 puts("REPORT verdict: known CAS loser plus BUSY stays STALE; no success or unknown-outcome promotion PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-partial-verdict-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),str(p),'-o',str(Path(d)/'test')],check=True)
    subprocess.run([str(Path(d)/'test')],check=True)
