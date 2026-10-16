#!/usr/bin/env python3
"""Compile the actual bounded inbox slicing and pump wake checks."""
from pathlib import Path
import os,re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
take=s.index('static int host_inbox_take(');take_end=s.index('\n}',take)+2
poll=s.index('static int host_pump_poll_timeout(');poll_end=s.index('\n}',poll)+2
limit=int(re.search(r'#define HOST_INBOX_SLICE_MAX\s+(\d+)',s).group(1))
assert 'n=host_inbox_take(h,local);' in s
assert 'poll(&pfd, 1, host_pump_poll_timeout(h))' in s

code=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define HOST_TICK_US 5000
#include "efs/raft_frame.h"
#include "efs/wire.h"
#define HOST_INBOX_MAX 256
#define HOST_NGROUPS 2
struct host_inbox_item {uint8_t *buf;uint32_t len;uint64_t queued_us;int read_noted;};
struct efs_raft_host {pthread_mutex_t inbox_mu;int inbox_n;uint64_t inbox_bytes;struct host_inbox_item inbox[HOST_INBOX_MAX];};
'''+f'#define HOST_INBOX_SLICE_MAX {limit}\n'+s[take:take_end]+s[poll:poll_end]+r'''
int main(void) {
 /* A popped FIFO prefix cannot reorder an old tail behind new arrivals or
  * sleep waiting for a drained eventfd. */
 struct efs_raft_host h={.inbox_mu=PTHREAD_MUTEX_INITIALIZER};
 struct host_inbox_item prefix[256];
 unsigned next=200,expected=0;uint64_t accounted=0;
 for(unsigned i=0;i<next;i++) {
  h.inbox[i]=(struct host_inbox_item){.buf=(void*)(uintptr_t)(i+1),.len=80+17*i,.queued_us=i,.read_noted=i%2};
  h.inbox_bytes+=h.inbox[i].len;
 }
 h.inbox_n=200;
 assert(host_pump_poll_timeout(&h)==0);
 while(h.inbox_n) {
  int n=host_inbox_take(&h,prefix);
  assert(n>0 && n<=HOST_INBOX_SLICE_MAX);
  uint64_t want=0;
  for(int i=0;i<n;i++) {
   assert(prefix[i].queued_us==expected && prefix[i].buf==(void*)(uintptr_t)(expected+1));
   assert(prefix[i].read_noted==(int)(expected%2));
   want+=80+17*expected++;
  }
  accounted+=want;
  uint64_t remaining=0;for(int i=0;i<h.inbox_n;i++)remaining+=h.inbox[i].len;
  assert(h.inbox_bytes==remaining);
  assert(host_pump_poll_timeout(&h)==(h.inbox_n?0:5));
  if(next<256) {
   /* Arrival after the pop appends behind every older retained frame. */
   pthread_mutex_lock(&h.inbox_mu);
   h.inbox[h.inbox_n++]=(struct host_inbox_item){.buf=(void*)(uintptr_t)(next+1),.len=80+17*next,.queued_us=next,.read_noted=next%2};
   h.inbox_bytes+=80+17*next++;
   pthread_mutex_unlock(&h.inbox_mu);
   assert(host_pump_poll_timeout(&h)==0);
  }
 }
 assert(expected==256 && accounted==256*80+17*(255*256/2));
 assert(!host_inbox_take(&h,prefix));
 assert(host_pump_poll_timeout(&h)==5);
 pthread_mutex_destroy(&h.inbox_mu);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-host-inbox-') as d:
 p=Path(d)/'test.c';p.write_text(code);binary=p.with_suffix('')
 flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.environ.get('EFS_TEST_SANITIZE')=='1' else []
 subprocess.run([os.environ.get('CC','cc'),*flags,'-std=c99','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
print('host inbox: bounded FIFO slices, exact byte accounting and backlog wakes PASS')
