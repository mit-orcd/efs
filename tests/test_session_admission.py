#!/usr/bin/env python3
"""Production ESTABLISH registration/barrier admission, local and remote owners."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_session_establish_admit(')
f=s[a:s.index('\n}',a)+2]
source=r'''
#include "efs/session.h"
#include "efs/protocol.h"
#include "efs/meta_cmd.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define HOST_SESS_REG_LEN 26
#define HOST_SESS_HDR_LEN 18
#define HOST_SESS_GET 0
struct efs_raft_host {int unused;};
static uint64_t salt;
static int hosted=1,read_rc;
static unsigned local_reads,remote_reads;
static uint32_t rd32be(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void wr32be(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=n>>(24-8*i);}
static int host_hosts(struct efs_raft_host *h,uint8_t group){(void)h;(void)group;return hosted;}
static int host_session_get(struct efs_raft_host *h,const uint8_t *cmd,uint32_t len,
 uint8_t group,int *hint,uint64_t *out) {
 (void)h;(void)hint;assert(len==22 && cmd[0]==EFS_MD_CMD_SESSION && cmd[1]==0 && rd32be(cmd+18)==123);
 assert(group==efs_raft_shard_group(efs_kv_session_shard(cmd+2)));local_reads++;*out=salt;return read_rc;
}
static int host_remote_cmd(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,
 uint32_t len,struct efs_msg_raft_mkfs_reply *out,int prefer) {
 assert(prefer==-1);uint64_t result;int hint;
 int rc=host_session_get(h,cmd,len,group,&hint,&result);local_reads--;remote_reads++;out->salt=result;return rc;
}
''' + f + r'''
int main(void) {
 struct efs_raft_host host;uint8_t cmd[26]={EFS_MD_CMD_SESSION,EFS_MD_SESS_ESTABLISH};
 cmd[2]=1;wr32be(cmd+18,2);wr32be(cmd+22,123);uint8_t group=efs_raft_shard_group(123);int hint;
 for(hosted=0;hosted<=1;hosted++) {
  salt=2|((uint64_t)EFS_SESSION_ACTIVE<<32)|(1ull<<40);
  assert(host_session_establish_admit(&host,cmd,26,group,&hint)==EFS_OK);
  salt&=~(1ull<<40);assert(host_session_establish_admit(&host,cmd,26,group,&hint)==EFS_ERR_BUSY);
  salt=2|((uint64_t)EFS_SESSION_FENCING<<32)|(1ull<<40);
  assert(host_session_establish_admit(&host,cmd,26,group,&hint)==EFS_ERR_BUSY);
  wr32be(cmd+18,1);assert(host_session_establish_admit(&host,cmd,26,group,&hint)==EFS_ERR_STALE);
  salt=2|((uint64_t)EFS_SESSION_ACTIVE<<32)|(1ull<<40);
  assert(host_session_establish_admit(&host,cmd,26,group,&hint)==EFS_ERR_STALE);wr32be(cmd+18,2);
  read_rc=EFS_ERR_BUSY;assert(host_session_establish_admit(&host,cmd,26,group,&hint)==EFS_ERR_BUSY);read_rc=0;
 }
 assert(local_reads && remote_reads);
 assert(host_session_establish_admit(&host,cmd,25,group,&hint)==EFS_ERR_INVAL);
 assert(host_session_establish_admit(&host,cmd,26,group^1,&hint)==EFS_ERR_INVAL);
 puts("session ESTABLISH: authoritative ACTIVE/REGISTER, fencing, routes and failed reads PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-session-admission-') as d:
 p=Path(d)/'test.c';p.write_text(source)
 exe=p.with_suffix('')
 cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include')]
 subprocess.run(cmd+[str(p),str(root/'src/kv/kv_key.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
submit=s[s.index('void server_raft_host_submit('):s.index('void server_raft_host_status(')]
assert submit.index('host_session_establish_admit(')<submit.index('host_propose_wait_idx(')
