#!/usr/bin/env python3
"""Real session getter/admission: authority is scoped to one local RPC."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
f=''
for name in ['host_session_get_covered','host_session_get','host_session_establish_admit_covered','host_session_establish_admit']:
 a=s.index('static int '+name+'(');f+=s[a:s.index('\n}',a)+2]+'\n'
code=r'''
#include "efs/session.h"
#include "efs/protocol.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define HOST_SESS_REG_LEN 26
#define HOST_SESS_HDR_LEN 18
#define HOST_SESS_GET 0
struct efs_raft_host {struct efs_kv *kv;};
static unsigned fences,fetches;static int hosted=1,fence_rc,store_rc;static struct efs_session_rec rec;
static uint32_t rd32be(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void wr32be(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=n>>(24-i*8);}
static int host_read_index(struct efs_raft_host *h,uint8_t g,int *hint){(void)h;(void)hint;assert(g==0 || g==2);fences++;return fence_rc;}
int efs_session_get(struct efs_kv *kv,const uint8_t id[EFS_OPID_UUID_LEN],struct efs_session_rec *out){(void)kv;(void)id;fetches++;*out=rec;return store_rc;}
int efs_session_bit_get(const uint8_t *bits,uint32_t shard){return (bits[shard>>3]>>(shard&7))&1;}
uint32_t efs_kv_session_shard(const uint8_t id[EFS_OPID_UUID_LEN]){(void)id;return 1;}
static int host_hosts(struct efs_raft_host *h,uint8_t group){(void)h;assert(group==0);return hosted;}
static int host_remote_cmd(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,uint32_t len,struct efs_msg_raft_mkfs_reply *out,int prefer);
'''+f+r'''
static int host_remote_cmd(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,uint32_t len,struct efs_msg_raft_mkfs_reply *out,int prefer){assert(prefer==-1);return host_session_get(h,cmd,len,group,NULL,&out->salt);}
int main(void){
 struct efs_raft_host h={0};int hint=-1;uint64_t value=0;uint8_t q[26]={0};q[2]=1;wr32be(q+18,123);
 rec.epoch=7;rec.state=EFS_SESSION_ACTIVE;rec.touched[123>>3]|=1u<<(123&7);
 assert(!host_session_get(&h,q,22,0,&hint,&value) && fences==1 && fetches==1);
 assert(!host_session_get(&h,q,22,0,&hint,&value) && fences==2); /* independent request */
 assert(!host_session_get_covered(&h,q,22,0,&hint,&value,1) && fences==2 && fetches==3);
 assert(!host_session_get_covered(&h,q,22,2,&hint,&value,1) && fences==3); /* newly encountered group */
 fence_rc=EFS_ERR_IO;unsigned before=fetches;
 assert(host_session_get_covered(&h,q,22,2,&hint,&value,1)==EFS_ERR_IO && fetches==before);
 fence_rc=0;store_rc=EFS_ERR_IO;
 assert(host_session_get_covered(&h,q,22,0,&hint,&value,1)==EFS_ERR_IO);store_rc=0;
 wr32be(q+18,7);wr32be(q+22,123);before=fences;
 assert(!host_session_establish_admit_covered(&h,q,26,0,&hint,1) && fences==before);
 rec.state=EFS_SESSION_FENCING;
 assert(host_session_establish_admit_covered(&h,q,26,0,&hint,1)==EFS_ERR_BUSY && fences==before);
 rec.epoch=8;assert(host_session_establish_admit_covered(&h,q,26,0,&hint,1)==EFS_ERR_STALE);
 rec.epoch=7;rec.state=EFS_SESSION_ACTIVE;rec.touched[123>>3]=0;
 assert(host_session_establish_admit_covered(&h,q,26,0,&hint,1)==EFS_ERR_BUSY);
 rec.touched[123>>3]|=1u<<(123&7);hosted=0;before=fences;
 assert(!host_session_establish_admit_covered(&h,q,26,0,&hint,1) && fences==before+1); /* remote cannot inherit local authority */
 hosted=1;before=fences;
 assert(!host_session_establish_admit(&h,q,26,0,&hint));assert(!host_session_establish_admit(&h,q,26,0,&hint));assert(fences==before+2);
 before=fences;unsigned old_fetches=fetches;
 assert(host_session_get_covered(&h,q,22,255,&hint,&value,1)==EFS_ERR_INVAL);
 assert(host_session_get_covered(&h,q,17,0,&hint,&value,1)==EFS_ERR_INVAL);
 assert(fences==before && fetches==old_fetches);
 puts("namespace authority: local request reuse, independent/default/remote fresh rounds, new groups and unchanged owner state/error checks PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-namespace-authority-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
