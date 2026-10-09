#!/usr/bin/env python3
"""Compile the actual bounded inbox scheduler and check ordering fences."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static void host_inbox_order(');b=s.index('\n}',a)+2
code=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "efs/raft_flow.h"
#include "efs/wire.h"
#define HOST_INBOX_MAX 256
#define HOST_NGROUPS 2
struct host_drain_view { uint64_t applied, term; int leader, ready; };
static struct host_drain_view view[2]={{0,0,0,1},{0,0,0,1}};
struct host_inbox_item {uint8_t *buf;uint32_t len;uint64_t queued_us;};
'''+s[a:b]+r'''
static struct host_inbox_item frame(unsigned group,unsigned bytes) {
 struct host_inbox_item m={0};m.len=92+bytes;m.buf=calloc(1,m.len);assert(m.buf);
 m.buf[0]=3;m.buf[1]=group;m.buf[11]=1;m.buf[79]=1;
 m.buf[88]=(bytes>>24)&255;m.buf[89]=(bytes>>16)&255;
 m.buf[90]=(bytes>>8)&255;m.buf[91]=bytes&255;
 if(bytes)m.buf[92]=1;return m;
}
static void check(struct host_inbox_item *m,unsigned n) {
 unsigned o[256],seen[256]={0};int last[2]={-1,-1};host_inbox_order(m,n,view,o);
 for(unsigned i=0;i<n;i++) {
  assert(o[i]<n&&!seen[o[i]]++);int g=m[o[i]].buf[1]/2;
  assert((int)o[i]>last[g]);last[g]=o[i];
 }
}
int main(void) {
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
  for(unsigned i=3;i<8;i++)assert(o[i]>2);
  memcpy(m[2].buf,saved,93);m[2].len=93;
 }
 check(m,256);host_inbox_order(m,256,view,o);
 for(unsigned i=0;i<16;i++)assert(o[i]==2*i);
 assert(o[16]==1); /* other group cannot wait beyond one 16-frame chunk */
 for(unsigned i=0;i<256;i++)free(m[i].buf);
 for(unsigned i=0;i<6;i++)m[i]=frame(i%2?2:0,600*1024);
 check(m,6);host_inbox_order(m,6,view,o);assert(o[0]==0&&o[1]==1);
 for(unsigned i=0;i<6;i++)free(m[i].buf);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-host-inbox-') as d:
 p=Path(d)/'test.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
print('host inbox: stable group order, global read/control/config fences, frame/byte bounds PASS')
