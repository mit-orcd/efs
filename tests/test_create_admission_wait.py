#!/usr/bin/env python3
"""FUSE waits for receipt capacity without issuing or replacing an identity."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/efs_fuse.c').read_text()
a=s.index('static int efs_fuse_create_begin_wait(');b=s.index('\n}',a)+2
code=r'''
#include "client_internal.h"
#include <assert.h>
#include <stdio.h>
static uint64_t now,deadline;
static unsigned calls,failures;
static int result,retain;
static uint64_t stats_now_ms(void){return now;}
uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
void efs_client_rpc_set_deadline_ms(uint64_t d){deadline=d;}
int efs_client_rpc_past_deadline(void){return deadline && now>=deadline;}
static int fake_sleep(unsigned n){assert(n==1000);now+=1000;return 0;}
#define usleep fake_sleep
int efs_client_create_begin(struct efs_client_create_request *r,efs_ino_t p,
    const char *name,uint32_t mode,uid_t uid,gid_t gid){
 assert(p==1 && name[0]=='f' && mode==0600 && uid==1 && gid==2);calls++;
 if(calls<=failures){if(retain)r->active=1;return EFS_ERR_BUSY;}
 if(!result)r->active=1;
 return result;
}
'''+s[a:b]+r'''
int main(void){
 struct efs_client_create_request r={0};failures=3;
 assert(!efs_fuse_create_begin_wait(&r,1,"f",0600,1,2));
 assert(calls==4 && r.active && !deadline);
 r.active=0;calls=0;failures=100;now=0;deadline=2500;
 assert(efs_fuse_create_begin_wait(&r,1,"f",0600,1,2)==EFS_ERR_BUSY);
 assert(calls==3 && !r.active && deadline==2500);
 calls=0;now=0;deadline=0;retain=1;
 assert(efs_fuse_create_begin_wait(&r,1,"f",0600,1,2)==EFS_ERR_BUSY);
 assert(calls==1 && r.active && !deadline);
 r.active=0;retain=0;calls=0;failures=0;result=EFS_ERR_NOMEM;
 assert(efs_fuse_create_begin_wait(&r,1,"f",0600,1,2)==EFS_ERR_NOMEM && calls==1);
 calls=0;now=10;deadline=now;
 assert(efs_fuse_create_begin_wait(&r,1,"f",0600,1,2)==EFS_ERR_BUSY && !calls);
 puts("CREATE admission: capacity recovery, inherited deadline, retained identity and terminal errors PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-create-admission-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
