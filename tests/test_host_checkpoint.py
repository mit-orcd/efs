#!/usr/bin/env python3
"""Verify the actual pump/shutdown helper cannot save past failed KV sync."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_save_applied(');b=s.index('\n}',a)+2
code=r'''
#include <assert.h>
#include <stdint.h>
#include "efs/common.h"
#define HOST_NGROUPS 2
struct efs_raft_host {uint64_t saved[2];unsigned writes;int fail;};
static int persist_applied(struct efs_raft_host*h,int gi,uint64_t idx,uint64_t now,int force) {
 (void)now;(void)force;h->writes++;
 if(h->fail==gi+1)return EFS_ERR_IO;
 h->saved[gi]=idx;return EFS_OK;
}
'''+s[a:b]+r'''
int main(void) {
 for(int force=0;force<2;force++) {
  struct efs_raft_host h={{10,20},0,0};uint64_t next[2]={11,22};
  assert(host_save_applied(&h,next,100,force,EFS_ERR_IO)==EFS_ERR_IO);
  assert(!h.writes&&h.saved[0]==10&&h.saved[1]==20);
  assert(host_save_applied(&h,next,100,force,EFS_ERR_NOMEM)==EFS_ERR_NOMEM);
  assert(!h.writes);
  assert(host_save_applied(&h,next,100,force,EFS_OK)==EFS_OK);
  assert(h.writes==2&&h.saved[0]==11&&h.saved[1]==22);
  h.fail=1;h.writes=0;next[0]=12;next[1]=23;
  assert(host_save_applied(&h,next,200,force,EFS_OK)==EFS_ERR_IO);
  assert(h.writes==1&&h.saved[0]==11&&h.saved[1]==22);
 }
 return 0;
}
'''
# Both production paths must pass the actual release result into this helper.
assert s.count('int kv_rc = efs_kv_lsm_sync_release(h->kv);')==2
assert s.count('host_save_applied(h,')==2
with tempfile.TemporaryDirectory(prefix='efs-host-checkpoint-') as d:
 p=Path(d)/'test.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
print('host checkpoint: failed KV sync cannot advance normal/shutdown markers; save failures retained PASS')
