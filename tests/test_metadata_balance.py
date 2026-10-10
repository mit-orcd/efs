#!/usr/bin/env python3
"""Production balancing policy: grace, lag refusal and re-election backoff."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static void host_balance_leaders(')
fn=s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define HOST_NGROUPS 2
struct efs_raft { int role; uint64_t term; uint32_t voters; };
struct host_group {
 uint64_t balance_term,balance_since_us,balance_attempt_us;
 uint32_t balance_voters;struct efs_raft *r;int kv_incomplete;uint8_t group;
};
struct efs_raft_host {int balance_enabled,raft_id;struct host_group g[2];};
static unsigned calls;static int target,transfer_rc;
int efs_raft_role(const struct efs_raft *r){return r->role;}
uint64_t efs_raft_term(const struct efs_raft *r){return r->term;}
uint32_t efs_raft_voters(const struct efs_raft *r){return r->voters;}
int efs_raft_transfer(struct efs_raft *r,int id) {
 calls++;target=id;if(transfer_rc!=EFS_ERR_BUSY)r->role=EFS_RAFT_FOLLOWER;
 return transfer_rc;
}
'''+fn+r'''
int main(void) {
 struct efs_raft r={EFS_RAFT_LEADER,7,0xf};struct efs_raft_host h={0};
 h.raft_id=1;h.g[0].r=&r;host_balance_leaders(&h,10000);assert(!calls);
 h.balance_enabled=1;h.g[0].kv_incomplete=1;host_balance_leaders(&h,10000);assert(!calls);
 h.g[0].kv_incomplete=0;r.role=EFS_RAFT_FOLLOWER;host_balance_leaders(&h,10000);assert(!calls);
 r.role=EFS_RAFT_LEADER;host_balance_leaders(&h,10000);assert(!calls);
 transfer_rc=EFS_ERR_BUSY;host_balance_leaders(&h,2010000);
 assert(calls==1&&target==0&&!h.g[0].balance_attempt_us&&r.role==EFS_RAFT_LEADER);
 transfer_rc=EFS_OK;host_balance_leaders(&h,2020000);
 assert(calls==2&&h.g[0].balance_attempt_us==2020000&&r.role==EFS_RAFT_FOLLOWER);
 /* The source re-elected after a lost trigger must retain its backoff. */
 r.role=EFS_RAFT_LEADER;r.term++;
 host_balance_leaders(&h,3020000);host_balance_leaders(&h,5020000);
 assert(calls==2&&r.role==EFS_RAFT_LEADER);
 host_balance_leaders(&h,32020000);assert(calls==3);
 /* Membership change resets preference/backoff, never election safety. */
 r.role=EFS_RAFT_LEADER;r.voters=0xe;h.raft_id=0;
 host_balance_leaders(&h,33020000);assert(calls==3);
 host_balance_leaders(&h,35020000);assert(calls==4&&target==1);
 /* Group ordinal picks another actual voter, not group-id arithmetic. */
 memset(&h,0,sizeof(h));h.balance_enabled=1;h.raft_id=1;
 h.g[1].r=&r;h.g[1].group=2;r.role=EFS_RAFT_LEADER;r.voters=0xe;
 host_balance_leaders(&h,40000000);host_balance_leaders(&h,42000000);
 assert(calls==5&&target==2);
 r.role=EFS_RAFT_LEADER;r.voters=0;
 host_balance_leaders(&h,45000000);host_balance_leaders(&h,48000000);assert(calls==5);
 puts("metadata balance: opt-in, actual voter placement, grace, lag refusal and cross-term backoff PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-metadata-balance-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
