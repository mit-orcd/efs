#!/usr/bin/env python3
"""A settled deferred edge owns atomic proof; probes retain independent authority."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_lease_submission_result(');body=s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/session.h"
#include "efs/meta_cmd.h"
#include "efs/raft.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdio.h>
struct efs_raft_host {int unused;};
static int unpack_rc,reads,legacy_rc;
static int host_unpack_lease_edge(const uint8_t*cmd,uint32_t n,struct efs_opid_req*q,efs_ino_t*ino,uint64_t*gen,unsigned*flags,uint64_t*token){(void)cmd;(void)q;assert(n==66);*ino=4097;*gen=1;*flags=EFS_LEASE_EDGE_CLOSE;*token=0;return unpack_rc;}
static int host_lease_edge_result(struct efs_raft_host*h,const uint8_t*c,uint32_t n,uint8_t group,int*hint,uint64_t*known){(void)h;(void)c;(void)n;(void)group;(void)hint;reads++;*known=legacy_rc==EFS_OK;return legacy_rc;}
''' + body + r'''
int main(void){struct efs_raft_host h={0};uint8_t c[66]={EFS_MD_CMD_SESSION,EFS_MD_SESS_LEASE_EDGE_DEFERRED};uint64_t known=99;int hint=-1;uint8_t group=efs_raft_shard_group(1);
 assert(!host_lease_submission_result(&h,c,66,group,&hint,&known,EFS_OK)&&known==1&&!reads);
 int errors[]={EFS_ERR_BUSY,EFS_ERR_IO,EFS_ERR_STALE,EFS_ERR_NOT_PRIMARY};
 for(unsigned i=0;i<4;i++){known=99;assert(host_lease_submission_result(&h,c,66,group,&hint,&known,errors[i])==errors[i]&&!known&&!reads);}
 unpack_rc=EFS_ERR_PROTO;assert(host_lease_submission_result(&h,c,66,group,&hint,&known,0)==EFS_ERR_PROTO&&!known&&!reads);unpack_rc=0;
 assert(host_lease_submission_result(&h,c,66,group^2,&hint,&known,0)==EFS_ERR_INVAL&&!known&&!reads);
 c[1]=EFS_MD_SESS_LEASE_EDGE;assert(!host_lease_submission_result(&h,c,66,group,&hint,&known,0)&&known&&reads==1);
 c[1]=EFS_MD_SESS_LEASE_PROBE_DEFERRED;legacy_rc=EFS_ERR_BUSY;
 assert(host_lease_submission_result(&h,c,66,group,&hint,&known,0)==EFS_ERR_BUSY&&!known&&reads==2);
 puts("lease submission: exact settled success only, refusal/ambiguity retains identity, legacy/probe quorum preserved PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-lease-proof-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),str(root/'src/kv/kv_key.c'),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
# Verify the wire handler uses this only after its exact-index waiter.
a=s.index('rc = host_propose_wait_idx(h, group, cmd, clen, &hint, &idx);');b=s.index('\n    out->salt=salt;',a)
assert 'host_lease_submission_result(h,cmd,clen,group,&hint,&salt,rc)' in s[a:b]
