#!/usr/bin/env python3
"""Actual lock apply verdict and Raft advancement/retry classification."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int apply_lock_cmd(');b=s.index('\n}',a)+2
c=s.index('    ret = (cmd[0] == EFS_MD_CMD_PREPARE');d=s.index('    /* Finish the atomic apply scope',c)
def define(name):
    return re.search(r'^#define '+name+r'\s+[^\n]+',s,re.M).group(0)
code=r'''
#include "efs/meta_cmd.h"
#include "efs/lock.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
struct efs_raft_host {struct efs_kv *kv;};
static int verdict,calls;
static uint64_t rd64be(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=v<<8|p[i];return v;}
static uint32_t rd32be(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v=v<<8|p[i];return v;}
int efs_lock_grant(struct efs_kv *kv,const struct efs_lock_req *q){(void)kv;assert(q->ino==4097 && q->generation==8 && q->owner.epoch==1);calls++;return verdict;}
int efs_lock_release(struct efs_kv *kv,const struct efs_lock_req *q){return efs_lock_grant(kv,q);}
#define APPLY_LOG(...) ((void)index)
''' + '\n'.join(define(n) for n in ['HOST_LOCK_LEN','HOST_APPEND_RSV_LEN','HOST_SESS_LEASE_LEN']) + '\n' + s[a:b] + r'''
static int core_result(uint32_t clen,int rc)
{uint8_t cmd[2]={EFS_MD_CMD_LOCK,1};int ret;
''' + s[c:d] + r'''
 return ret;
}
int main(void)
{
 struct efs_raft_host h={0};uint8_t cmd[HOST_LOCK_LEN]={EFS_MD_CMD_LOCK,EFS_MD_LOCK_GRANT};
 cmd[8]=16;cmd[9]=1;cmd[17]=8;cmd[HOST_LOCK_LEN-1]=1;
 const int failures[]={EFS_ERR_STALE,EFS_ERR_BUSY,EFS_ERR_AGAIN,EFS_ERR_NOLCK,EFS_ERR_NOT_FOUND,EFS_ERR_IO,EFS_ERR_NOMEM};
 for(unsigned i=0;i<sizeof(failures)/sizeof(failures[0]);i++) {
  verdict=failures[i];assert(apply_lock_cmd(&h,cmd,sizeof(cmd),1)==verdict);
  assert(core_result(sizeof(cmd),verdict)==((verdict==EFS_ERR_IO || verdict==EFS_ERR_NOMEM)?verdict:EFS_OK));
  cmd[1]=EFS_MD_LOCK_RELEASE;assert(apply_lock_cmd(&h,cmd,sizeof(cmd),1)==verdict);cmd[1]=EFS_MD_LOCK_GRANT;
 }
 verdict=EFS_OK;assert(apply_lock_cmd(&h,cmd,sizeof(cmd),1)==EFS_OK);
 int before=calls;assert(apply_lock_cmd(&h,cmd,2,1)==EFS_ERR_PROTO && calls==before);
 puts("lock apply: audible business verdicts, advancing conflicts and retained IO/NOMEM replay PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-lock-apply-') as t:
    p=Path(t);(p/'test.c').write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
