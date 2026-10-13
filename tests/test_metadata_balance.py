#!/usr/bin/env python3
"""Production balancing policy: grace, lag refusal and re-election backoff."""
from pathlib import Path
import os,shlex,subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_RAFT_HOST_SOURCE',root/'src/server/raft_host.c')).read_text()
a=s.index('static void host_balance_leaders(')
fn=s[a:s.index('\n}',a)+2]
a=s.index('static int host_container_balanced_voter(');helper=s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define HOST_NGROUPS 2
struct efs_raft { int role; uint64_t term; uint32_t voters; };
struct host_group {
 uint64_t balance_term,balance_since_us,balance_attempt_us;
 uint32_t balance_voters,voters;struct efs_raft *r;int kv_incomplete;uint8_t group;
};
struct efs_server {int storage_engine;};
struct efs_raft_host {int balance_enabled,raft_id;struct efs_server *s;struct host_group g[2];};
static struct host_group *group_slot(struct efs_raft_host *h,uint8_t group){return group==0?&h->g[0]:group==2?&h->g[1]:NULL;}
static unsigned calls;static int target,transfer_rc;
int efs_raft_role(const struct efs_raft *r){return r->role;}
uint64_t efs_raft_term(const struct efs_raft *r){return r->term;}
uint32_t efs_raft_voters(const struct efs_raft *r){return r->voters;}
int efs_raft_transfer(struct efs_raft *r,int id) {
 calls++;target=id;if(transfer_rc!=EFS_ERR_BUSY)r->role=EFS_RAFT_FOLLOWER;
 return transfer_rc;
}
'''+helper+fn+r'''
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
 /* Container placement selects committed dual-group voters, including an
  * unhosted local group's committed mask; it never changes the voter set. */
 struct efs_server server={1};struct efs_raft r2={EFS_RAFT_FOLLOWER,7,0xe};
 memset(&h,0,sizeof(h));h.s=&server;h.g[0].r=&r;h.g[1].r=&r2;r.voters=7;
 assert(host_container_balanced_voter(&h,7,0)==2);
 assert(host_container_balanced_voter(&h,0xe,1)==2);
 h.g[1].r=NULL;h.g[1].voters=0xe;assert(host_container_balanced_voter(&h,7,0)==2);
 h.g[1].voters=0;assert(host_container_balanced_voter(&h,7,0)==-1);
 h.g[1].voters=4;r.voters=1;assert(host_container_balanced_voter(&h,1,0)==0);
 assert(host_container_balanced_voter(&h,4,1)==2);
 r.voters=0xf;h.g[1].voters=0xf;assert(host_container_balanced_voter(&h,0xf,0)==1);
 assert(host_container_balanced_voter(&h,0xf,1)==1);
 server.storage_engine=0;r.voters=7;h.g[1].voters=0xe;
 assert(host_container_balanced_voter(&h,7,0)==0);
 /* Transfer still honors settling, target lag refusal and backoff. */
 server.storage_engine=1;h.balance_enabled=1;h.raft_id=0;r.role=EFS_RAFT_LEADER;
 transfer_rc=EFS_ERR_BUSY;unsigned before=calls;
 host_balance_leaders(&h,60000000);host_balance_leaders(&h,62000000);
 assert(calls==before+1&&target==2&&!h.g[0].balance_attempt_us);
 transfer_rc=EFS_OK;host_balance_leaders(&h,62010000);assert(calls==before+2&&target==2);
 puts("metadata balance: opt-in, actual voter placement, grace, lag refusal and cross-term backoff PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-metadata-balance-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror',*shlex.split(os.environ.get('EFS_TEST_CFLAGS','')),'-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
