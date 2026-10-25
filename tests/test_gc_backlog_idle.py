#!/usr/bin/env python3
"""Actual GC idle policy: idle/blocked scans retain backoff; progress is bounded."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_gc_backlog_config(')
b=s.index('/* D43: the cluster expiry policy decides',a)
code='''#include "efs/common.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#define GC_LOOP_MS 1000
#define GC_BACKLOG_IDLE_DEFAULT_MS 20u
'''+s[a:b]+'''
int main(void) {
 unsigned ms;
 assert(host_gc_backlog_config(NULL,&ms)==EFS_OK && ms==20); /* W124: capped progress reruns fast */
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
 puts("GC backlog idle: strict config, default/idle/blocked/reset backoff and bounded progress PASS");
}
'''
# Integration must keep both groups and snapshot-responsive shutdown slices.
thread=s[s.index('static void *host_gc_thread(',b):s.index('/* W92 query publication:',b)]
assert 'host_gc_reap_pass(h, h->g[g].group' in thread
assert 'backlog_capped |= __atomic_load_n' in thread
assert 'backlog_capped |= host_gc_frag_pass(' in thread
# Expiry pacing is in every build (D43); one policy read per pass gates it.
for line in ('expiry_progress |= host_gc_expiry_pass(', 'idle_ms = host_gc_expiry_idle(idle_ms, expiry_progress)',
             'expiry_progress = host_gc_expiry_groups(h)', 'host_expiry_refresh(h);'):
    assert line in thread, line
assert 'EFS_BUILD_EXPIRY' not in s
assert 'efs_iostats_now_us() < idle_end' in thread
yield_code=thread[thread.index('uint64_t idle_end ='):]
assert 'now_us_()' not in yield_code
assert 'maintenance_at = efs_iostats_now_us() + (uint64_t)GC_LOOP_MS * 1000' in thread
assert 'if (maintenance) host_dir_spread_pass(h)' in thread
assert 'attempts<2 && h->gc_running' in s
assert 'expiry_at = efs_iostats_now_us() +\n                            20000' in thread
assert 'removed_before' in thread and 'removed_after' in thread
assert thread.count('host_reap_completed')==2 # reap completions count as progress
assert 'host_snap_export_pass(h)' in thread and 'if (sleep_us > 20000) sleep_us = 20000' in thread
with tempfile.TemporaryDirectory(prefix='efs-gc-idle-') as temp:
 p=Path(temp)/'test.c';p.write_text(code)
 command=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))]
 subprocess.run(command,check=True);subprocess.run([str(p.with_suffix(''))],check=True)
