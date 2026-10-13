#!/usr/bin/env python3
"""Actual GC idle policy: idle/blocked scans retain backoff; progress is bounded."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_gc_backlog_config(')
b=s.index('static int host_expiry_clock_read(',a)
code='''#include "efs/common.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#define GC_LOOP_MS 1000
'''+s[a:b]+'''
int main(void) {
 unsigned ms;
 assert(host_gc_backlog_config(NULL,&ms)==EFS_OK && ms==1000);
 const char *invalid[]={"", "0", "19", "1001", "-100", "+100", " 100", "100x", "999999999999999999999999"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++)
  assert(host_gc_backlog_config(invalid[i],&ms)==EFS_ERR_INVAL);
 for(unsigned configured=20;configured<=1000;configured+=20) {
  char value[16];snprintf(value,sizeof(value),"%u",configured);
  assert(host_gc_backlog_config(value,&ms)==EFS_OK && ms==configured);
  assert(host_gc_backlog_idle(ms,0,1,2)==1000);
  assert(host_gc_backlog_idle(ms,1,1,1)==1000);
  assert(host_gc_backlog_idle(ms,1,2,1)==1000);
  assert(host_gc_backlog_idle(ms,1,1,2)==ms);
 }
 assert(host_gc_packed_idle(1000,0,0)==1000);
 assert(host_gc_packed_idle(1000,1,0)==1000);
 assert(host_gc_packed_idle(1000,0,1)==1000);
 assert(host_gc_packed_idle(1000,1,1)==20);
 assert(host_gc_packed_idle(40,1,0)==40);
 assert(host_gc_expiry_idle(1000,0)==1000);
 assert(host_gc_expiry_idle(1000,1)==100);
 assert(host_gc_expiry_idle(20,1)==20);
 assert(host_gc_expiry_idle(100,1)==100);
 assert(host_gc_expiry_idle(40,0)==40);
 puts("GC backlog idle: strict config, default/idle/blocked/reset backoff and bounded progress PASS");
}
'''
# Integration must keep both groups and snapshot-responsive shutdown slices.
thread=s[s.index('static void *host_gc_thread(',b):s.index('/* W92 query publication:',b)]
assert 'host_gc_reap_pass(h, h->g[g].group' in thread
assert 'backlog_capped |= __atomic_load_n' in thread
assert 'backlog_capped |= host_gc_frag_pass(' in thread
assert 'pack_capped = host_gc_pack_pass(' in thread
assert 'host_gc_packed_idle(idle_ms, packed_capped, packed_deleted)' in thread
assert 'expiry_progress |= host_gc_expiry_pass(' in thread
assert 'host_gc_expiry_idle(idle_ms, expiry_progress)' in thread
assert 'removed_before' in thread and 'removed_after' in thread
assert 'host_snap_export_pass(h)' in thread and 'usleep(20 * 1000)' in thread
with tempfile.TemporaryDirectory(prefix='efs-gc-idle-') as temp:
 p=Path(temp)/'test.c';p.write_text(code)
 command=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))]
 subprocess.run(command,check=True);subprocess.run([str(p.with_suffix(''))],check=True)
