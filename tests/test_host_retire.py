#!/usr/bin/env python3
"""Exercise actual checkpoint order and KV/marker EIO guards."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
def function(name):
 a=s.index('static int '+name+'(')
 return s[a:s.index('\n}',a)+2]
code=r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include "efs/common.h"
#define HOST_NGROUPS 2
struct efs_raft_host {int fail;unsigned writes;uint64_t saved[2];};
static int persist_applied(struct efs_raft_host*h,int gi,uint64_t idx,uint64_t now,int force) {
 (void)now;(void)force;h->writes++;
 if(h->fail==gi+1)return EFS_ERR_IO;
 h->saved[gi]=idx;return EFS_OK;
}
'''+function('host_save_applied')+function('host_checkpoint_retire')+r'''
int main(void) {
 for(int failure=0;failure<4;failure++) {
  struct efs_raft_host h={0};uint64_t idx[2]={11,22};
  h.saved[0]=10;h.saved[1]=20;h.fail=failure==2?1:failure==3?2:0;
  int rc=host_checkpoint_retire(&h,idx,100,failure==1?EFS_ERR_IO:EFS_OK);
  assert(rc==(failure?EFS_ERR_IO:EFS_OK));
  if(failure==1)assert(!h.writes&&h.saved[0]==10&&h.saved[1]==20);
  if(failure==2)assert(h.writes==1&&h.saved[0]==10&&h.saved[1]==20);
  if(failure==3)assert(h.writes==2&&h.saved[0]==11&&h.saved[1]==20);
  if(!failure)assert(h.writes==2&&h.saved[0]==11&&h.saved[1]==22);
 }
 return 0;
}
'''
# The executable KV-prefix worker regression checks the asynchronous barrier
# before this production retirement helper; this test exercises its failure
# behavior without depending on source spellings.
with tempfile.TemporaryDirectory(prefix='efs-host-retire-') as d:
 p=Path(d)/'test.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-pthread','-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
print('host retirement: marker after KV release; KV/marker EIO guards PASS')
