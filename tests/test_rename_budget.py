#!/usr/bin/env python3
"""Execute production nested rename retries under one monotonic deadline."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/ops.c').read_text()
a=s.index('int efs_client_rename_at(');a=s.index('    struct timespec ts;',a)
b=s.index('    efs_client_rpc_set_deadline_ms(previous);',a)+len('    efs_client_rpc_set_deadline_ms(previous);')
code=r'''
#include <efs/common.h>
#include <assert.h>
#include <stdint.h>
#include <time.h>
static uint64_t stamp,deadline;static int calls,succeed;
static struct {int export_id;} g_client;
static int fake_clock(int clock,struct timespec *ts){(void)clock;ts->tv_sec=stamp/1000;ts->tv_nsec=stamp%1000*1000000;return 0;}
#define clock_gettime fake_clock
static uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
static void efs_client_rpc_set_deadline_ms(uint64_t d){deadline=d;}
static int efs_client_rpc_past_deadline(void){return deadline&&stamp>=deadline;}
static int efs_client_rpc_rename_at(int e,efs_ino_t o,const char *n,efs_ino_t p,const char *m,void *out){
 (void)e;(void)o;(void)n;(void)p;(void)m;(void)out;calls++;stamp+=2000;return succeed&&calls==2?EFS_OK:EFS_ERR_BUSY;
}
static int usleep(unsigned n){stamp+=n/1000;return 0;}
static int run(void){int rc=EFS_ERR_BUSY,t,out;efs_ino_t old_parent=1,new_parent=2;const char *old_name="a",*new_name="b";
'''+s[a:b]+r'''
 return rc;
}
int main(void){
 stamp=1000;deadline=0;calls=0;assert(run()==EFS_ERR_BUSY&&calls==4&&deadline==0&&stamp<10000);
 stamp=1000;deadline=3500;calls=0;assert(run()==EFS_ERR_BUSY&&calls==2&&deadline==3500);
 stamp=1000;deadline=500;calls=0;assert(run()==EFS_ERR_BUSY&&calls==0&&deadline==500);
 stamp=1000;deadline=0;calls=0;succeed=1;assert(run()==EFS_OK&&calls==2&&deadline==0);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-rename-budget-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-O2','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('rename budget: nested BUSY bounded, tighter/expired caller deadline preserved, transient success PASS')
