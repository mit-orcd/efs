#!/usr/bin/env python3
"""Actual session reader: same-request group reuse, errors and fresh defaults."""
from pathlib import Path
import os,subprocess,tempfile,shlex
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_SESSION_READ_SOURCE',root/'src/server/raft_host.c')).read_text()
a=s.index('static int host_session_get_after_group(');b=s.index('/* ESTABLISH must follow',a)
c=r'''
#include "efs/session.h"
#include "efs/protocol.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define HOST_SESS_REG_LEN 26
struct efs_raft_host {struct efs_kv *kv;};
static int reads,session_gets,read_error,get_error;
static struct efs_session_rec current;
static uint32_t rd32be(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int host_read_index(struct efs_raft_host *h,uint8_t group,int *hint)
{(void)h;assert(group==0||group==2);reads++;*hint=7;return read_error;}
int efs_session_get(struct efs_kv *kv,const uint8_t uuid[EFS_OPID_UUID_LEN],struct efs_session_rec *out)
{(void)kv;assert(uuid[0]==11);session_gets++;if(get_error)return get_error;*out=current;return EFS_OK;}
int efs_session_bit_get(const uint8_t *bits,uint32_t shard){return !!(bits[shard/8]&(1u<<(shard%8)));}
'''+s[a:b]+r'''
int main(void) {
 struct efs_raft_host h={0};uint8_t query[22]={0};query[2]=11;query[21]=1;
 current.epoch=5;current.state=EFS_SESSION_ACTIVE;current.touched[0]=2;
 uint64_t salt=0;int hint=-1;
 assert(!host_session_get(&h,query,sizeof(query),2,&hint,&salt));
 assert(reads==1&&session_gets==1&&salt==((uint64_t)5|((uint64_t)EFS_SESSION_ACTIVE<<32)|(1ull<<40)));
 reads=session_gets=0;assert(!host_session_get_after_group(&h,query,sizeof(query),2,&hint,&salt,2));assert(!reads&&session_gets==1);
 /* The proof is request-local: every ordinary next request reads again. */
 assert(!host_session_get(&h,query,sizeof(query),2,&hint,&salt));assert(reads==1&&session_gets==2);
 reads=session_gets=0;read_error=EFS_ERR_BUSY;salt=99;
 assert(host_session_get_after_group(&h,query,sizeof(query),2,&hint,&salt,0)==EFS_ERR_BUSY);
 assert(reads==1&&!session_gets&&salt==99);
 read_error=0;get_error=EFS_ERR_IO;reads=session_gets=0;
 assert(host_session_get_after_group(&h,query,sizeof(query),2,&hint,&salt,2)==EFS_ERR_IO);assert(!reads&&session_gets==1&&salt==99);
 get_error=0;current.epoch=6;current.state=EFS_SESSION_FENCING;
 assert(!host_session_get_after_group(&h,query,sizeof(query),2,&hint,&salt,2));
 assert((uint32_t)salt==6&&(uint8_t)(salt>>32)==EFS_SESSION_FENCING);
 /* Bitmap queries retain their original decoding and bounds. */
 query[18]=0;query[19]=0;query[20]=16;query[21]=0;
 current.touched[0]=0xa5;assert(!host_session_get(&h,query,sizeof(query),0,&hint,&salt));assert(salt==0xa5);
 query[18]=0xff;assert(host_session_get(&h,query,sizeof(query),0,&hint,&salt)==EFS_ERR_INVAL);
 puts("session reader: matching request proof only, ordinary/foreign fresh reads, live epoch/state/bitmap and hard refusal PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-session-read-proof-') as td:
 p=Path(td)/'test.c';p.write_text(c);binary=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=30)
