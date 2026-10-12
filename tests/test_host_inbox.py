#!/usr/bin/env python3
"""Compile the actual bounded inbox scheduler and check ordering fences."""
from pathlib import Path
import os,re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static void host_inbox_order(');b=s.index('\n}',a)+2
take=s.index('static int host_inbox_take(');take_end=s.index('\n}',take)+2
poll=s.index('static int host_pump_poll_timeout(');poll_end=s.index('\n}',poll)+2
limit=int(re.search(r'#define HOST_INBOX_SLICE_MAX\s+(\d+)',s).group(1))
assert 'n=host_inbox_take(h,local,done_n,done_bytes);' in s
assert 'poll(&pfd, 1, host_pump_poll_timeout(h))' in s

code=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define HOST_TICK_US 5000
#include "efs/raft_flow.h"
#include "efs/wire.h"
#define HOST_INBOX_MAX 256
#define HOST_NGROUPS 2
struct host_drain_view { uint64_t applied, term; int leader, ready; };
static struct host_drain_view view[2]={{0,0,0,1},{0,0,0,1}};
struct host_inbox_item {uint8_t *buf;uint32_t len;uint64_t queued_us;int read_noted;};
struct efs_raft_host {pthread_mutex_t inbox_mu;int inbox_n;uint64_t inbox_bytes,inbox_work_n,inbox_work_bytes;struct host_inbox_item inbox[HOST_INBOX_MAX];};
'''+f'#define HOST_INBOX_SLICE_MAX {limit}\n'+s[a:b]+s[take:take_end]+s[poll:poll_end]+r'''
static struct host_inbox_item frame(unsigned group,unsigned bytes) {
 struct host_inbox_item m={0};m.len=92+bytes;m.buf=calloc(1,m.len);assert(m.buf);
 m.buf[0]=3;m.buf[1]=group;m.buf[11]=1;m.buf[79]=1;
 m.buf[88]=(bytes>>24)&255;m.buf[89]=(bytes>>16)&255;
 m.buf[90]=(bytes>>8)&255;m.buf[91]=bytes&255;
 if(bytes)m.buf[92]=1;
 return m;
}
static void check(struct host_inbox_item *m,unsigned n) {
 unsigned o[256],seen[256]={0};int last[2]={-1,-1};host_inbox_order(m,n,view,o);
 for(unsigned i=0;i<n;i++) {
  assert(o[i]<n&&!seen[o[i]]++);int g=m[o[i]].buf[1]/2;
  assert((int)o[i]>last[g]);last[g]=o[i];
 }
}
int main(void) {
 /* A popped FIFO prefix cannot release durability credits, reorder an old
  * tail behind new arrivals, or sleep waiting for a drained eventfd. */
 struct efs_raft_host h={.inbox_mu=PTHREAD_MUTEX_INITIALIZER};
 struct host_inbox_item prefix[256];unsigned done;uint64_t bytes;
 unsigned next=200,expected=0;uint64_t accounted=0;
 for(unsigned i=0;i<next;i++) {
  h.inbox[i]=(struct host_inbox_item){.buf=(void*)(uintptr_t)(i+1),.len=80+17*i,.queued_us=i,.read_noted=i%2};
  h.inbox_bytes+=h.inbox[i].len;
 }
 h.inbox_n=200;h.inbox_work_n=64;h.inbox_work_bytes=999999;
 assert(host_pump_poll_timeout(&h)==0);
 while(h.inbox_n) {
  int n=host_inbox_take(&h,prefix,&done,&bytes);
  assert(n>0 && n<=HOST_INBOX_SLICE_MAX && done==(unsigned)n);
  uint64_t want=0;
  for(int i=0;i<n;i++) {
   assert(prefix[i].queued_us==expected && prefix[i].buf==(void*)(uintptr_t)(expected+1));
   assert(prefix[i].read_noted==(int)(expected%2));
   want+=80+17*expected++;
  }
  assert(bytes==want);accounted+=bytes;
  assert(h.inbox_work_n==64 && h.inbox_work_bytes==999999);
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
 assert(!host_inbox_take(&h,prefix,&done,&bytes) && !done && !bytes);
 assert(host_pump_poll_timeout(&h)==5);
 pthread_mutex_destroy(&h.inbox_mu);
 struct host_inbox_item m[256];unsigned o[256];
 for(unsigned i=0;i<256;i++)m[i]=frame(i%2?2:0,1);
 host_inbox_order(m,4,view,o);assert(o[0]==0&&o[1]==2&&o[2]==1&&o[3]==3);
 for(unsigned fence=0;fence<9;fence++) {
  unsigned char saved[93];memcpy(saved,m[2].buf,93);
  if(fence==0)m[2].buf[35]=9; /* fresh read */
  if(fence==1)m[2].buf[0]=5; /* snapshot */
  if(fence==2)m[2].buf[92]=EFS_RAFT_CMD_JOINT;
  if(fence==3)m[2].buf[92]=EFS_RAFT_CMD_COLD;
  if(fence==4)m[2].buf[91]=0; /* malformed length */
  if(fence==5){m[2].buf[79]=0;m[2].len=80;} /* empty heartbeat */
  if(fence==6)m[2].buf[67]=1; /* commit would apply */
  if(fence==7)m[2].buf[19]=1; /* changed term */
  if(fence==8)m[2].buf[7]=2; /* changed leader */
  host_inbox_order(m,8,view,o);assert(o[2]==2);
  for(unsigned i=0;i<2;i++)assert(o[i]<2);
  for(unsigned i=3;i<8;i++)assert(o[i]==i); /* captured view invalid after fence */
  memcpy(m[2].buf,saved,93);m[2].len=93;
 }
 check(m,256);host_inbox_order(m,256,view,o);
 for(unsigned i=0;i<16;i++)assert(o[i]==2*i);
 assert(o[16]==1); /* other group cannot wait beyond one 16-frame chunk */
 for(unsigned i=0;i<256;i++)free(m[i].buf);
 /* Broad permutation/FIFO/fence coverage over the full hard inbox bound. */
 unsigned random=971;
 for(unsigned trial=0;trial<128;trial++) {
  unsigned n=1+trial*2;unsigned fence=n;
  for(unsigned i=0;i<n;i++) {
   random=random*1664525u+1013904223u;
   m[i]=frame((random>>31)?2:0,1);
  }
  if(trial%2){fence=trial%n;m[fence].buf[67]=1;}
  check(m,n);host_inbox_order(m,n,view,o);
  for(unsigned i=fence;i<n;i++)assert(o[i]==i);
  for(unsigned i=0;i<n;i++)free(m[i].buf);
 }
 for(unsigned i=0;i<8;i++)m[i]=frame(i%2?2:0,1);
 view[0].ready=0;host_inbox_order(m,8,view,o);
 for(unsigned i=0;i<8;i++)assert(o[i]==i); /* stalled apply cannot be regrouped */
 view[0].ready=1;view[0].leader=2;host_inbox_order(m,8,view,o);
 for(unsigned i=0;i<8;i++)assert(o[i]==i); /* changed leader cannot be regrouped */
 view[0].leader=0;
 for(unsigned i=0;i<8;i++)free(m[i].buf);
 for(unsigned i=0;i<6;i++)m[i]=frame(i%2?2:0,600*1024);
 check(m,6);host_inbox_order(m,6,view,o);assert(o[0]==0&&o[1]==1);
 for(unsigned i=0;i<6;i++)free(m[i].buf);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-host-inbox-') as d:
 p=Path(d)/'test.c';p.write_text(code);binary=p.with_suffix('')
 flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.environ.get('EFS_TEST_SANITIZE')=='1' else []
 subprocess.run([os.environ.get('CC','cc'),*flags,'-std=c99','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
print('host inbox: stable group order, global read/control/config fences, frame/byte bounds, bounded FIFO slices, deferred credits and backlog wakes PASS')
