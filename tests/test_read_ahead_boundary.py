#!/usr/bin/env python3
"""Layout lookahead must pull the final representable range, never a wrapped hole."""
from pathlib import Path
import subprocess,tempfile
r=Path(__file__).resolve().parents[1];s=(r/'src/client/ops.c').read_text();a=s.index('int efs_client_pull_layout_miss(');f=s[a:s.index('\n}',a)+2]
code=r'''
#include "client_internal.h"
#include <assert.h>
static unsigned calls;static uint32_t start,end;static int reply;
static int pull_groups_parallel(efs_ino_t ino,uint32_t a,uint32_t b,void*p){(void)ino;(void)p;calls++;start=a;end=b;return reply;}
'''+f+r'''
int main(void){
 assert(!efs_client_pull_layout_miss(42,UINT32_MAX-2,UINT32_MAX-1));
 assert(calls==1&&start==UINT32_MAX-2&&end==UINT32_MAX);
 assert(!efs_client_pull_layout_miss(42,UINT32_MAX-1,UINT32_MAX));assert(calls==1);
 reply=EFS_ERR_IO;assert(efs_client_pull_layout_miss(43,UINT32_MAX-3,UINT32_MAX)==EFS_ERR_IO&&calls==2);
 reply=EFS_OK;assert(!efs_client_pull_layout_miss(43,UINT32_MAX-3,UINT32_MAX)&&calls==3&&start==UINT32_MAX-3&&end==UINT32_MAX);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-ahead-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(r/'include'),'-I'+str(r/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('Read lookahead: final uint32 range, covered extension and failed pull retry PASS')
