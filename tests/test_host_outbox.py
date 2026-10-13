#!/usr/bin/env python3
"""Exercise production priority replacement with interleaved Raft groups."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int outbox_pri_replace(');b=s.index('\n}',a)+2
helper=s.index('static int outbox_pure_read_control(');helper_end=s.index('\n}',helper)+2
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define HOST_OUTBOX_BYTES 1024
#define EFS_WIRE_RAFT_HDR_LEN 80
#define EFS_RAFT_MSG_AE_REQ 3
#define EFS_RAFT_MSG_AE_REP 4
struct host_outbox_item {uint8_t *buf;uint32_t len;};
struct host_outbox {int preserve_read,npri;uint64_t queued_bytes,queued_bytes_hi;struct host_outbox_item pri[4];};
static uint64_t rd64be(const uint8_t*p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=(v<<8)|p[i];return v;}
static int reply(const uint8_t*b,uint32_t n){return b&&n>=1&&b[0]==4;}
static int heartbeat(const uint8_t*b,uint32_t n){return b&&n>=1&&b[0]==3;}
static uint8_t *frame(unsigned type,unsigned group,unsigned mark,unsigned len){
 uint8_t*b=calloc(1,len);assert(b);b[0]=type;b[1]=group;b[4]=mark;return b;}
'''+s[helper:helper_end]+s[a:b]+r'''
int main(void){
 struct host_outbox tx={.npri=2,.queued_bytes=160,.queued_bytes_hi=160};
 tx.pri[0]=(struct host_outbox_item){frame(4,0,10,80),80};
 tx.pri[1]=(struct host_outbox_item){frame(4,2,20,80),80};
 uint8_t *g0=tx.pri[0].buf,*g2=tx.pri[1].buf;
 uint8_t *new=frame(4,2,21,96);
 assert(outbox_pri_replace(&tx,reply,new,96)==1);
 assert(tx.pri[0].buf==g0&&g0[4]==10); /* other group's ACK retained */
 assert(tx.pri[1].buf==new&&new[4]==21);
 assert(tx.queued_bytes==176&&tx.queued_bytes_hi==176);
 new=frame(4,1,30,80);g2=tx.pri[1].buf;
 assert(outbox_pri_replace(&tx,reply,new,80)==0);
 assert(tx.pri[0].buf==g0&&tx.pri[1].buf==g2&&tx.queued_bytes==176);free(new);
 free(tx.pri[0].buf);free(tx.pri[1].buf);
 tx.pri[0]=(struct host_outbox_item){frame(3,0,10,80),80};
 tx.pri[1]=(struct host_outbox_item){frame(3,2,20,96),96};
 g0=tx.pri[0].buf;new=frame(3,2,22,80);
 assert(outbox_pri_replace(&tx,heartbeat,new,80)==1);
 assert(tx.pri[0].buf==g0&&tx.pri[1].buf==new&&tx.queued_bytes==160);
 /* Capacity refusal retains the old buffer and caller ownership. */
 g2=tx.pri[1].buf;new=frame(3,0,31,1024);
 assert(!outbox_pri_replace(&tx,heartbeat,new,1024));
 assert(tx.pri[0].buf==g0&&tx.pri[1].buf==g2&&tx.queued_bytes==160);free(new);
 /* Preserve existing vote-at-capacity eviction of any heartbeat. */
 new=frame(1,1,30,80);assert(outbox_pri_replace(&tx,heartbeat,new,80));
 assert(tx.pri[0].buf==new&&tx.pri[1].buf==g2&&tx.queued_bytes==160);
 new=frame(1,2,30,80);assert(outbox_pri_replace(&tx,heartbeat,new,80));
 assert(tx.pri[1].buf==new&&tx.queued_bytes==160);
 free(tx.pri[0].buf);free(tx.pri[1].buf);
 tx=(struct host_outbox){.preserve_read=1,.npri=1,.queued_bytes=80,.queued_bytes_hi=80};
 uint8_t *progress=frame(4,0,1,80);progress[3]=1;progress[19]=2;progress[27]=7;progress[35]=20;progress[75]=9;
 tx.pri[0]=(struct host_outbox_item){progress,80};
 /* Probe votes cannot erase entry-batch retirement, or the converse. */
 uint8_t *probe=malloc(80);memcpy(probe,progress,80);probe[2]=1;
 assert(!outbox_pri_replace(&tx,reply,probe,80));
 tx.pri[1]=(struct host_outbox_item){probe,80};tx.npri++;tx.queued_bytes+=80;
 uint8_t *fresh=malloc(80);memcpy(fresh,progress,80);fresh[35]=21;
 assert(!outbox_pri_replace(&tx,reply,fresh,80));
 tx.pri[2]=(struct host_outbox_item){fresh,80};tx.npri++;tx.queued_bytes+=80;
 uint8_t *late=malloc(80);memcpy(late,probe,80);late[35]=19;
 assert(!outbox_pri_replace(&tx,reply,late,80));
 tx.pri[3]=(struct host_outbox_item){late,80};tx.npri++;tx.queued_bytes+=80;
 new=malloc(80);memcpy(new,probe,80);new[4]=99;
 assert(outbox_pri_replace(&tx,reply,new,80));
 assert(tx.pri[0].buf==progress && tx.pri[1].buf==new && tx.pri[2].buf==fresh && tx.pri[3].buf==late);
 assert(tx.queued_bytes==320);probe=new;
 /* Different durable prefix, failure, boot or term remains separately owed. */
 for(unsigned field=0;field<4;field++){
  new=malloc(80);memcpy(new,fresh,80);new[field==0?75:field==1?3:field==2?27:19]++;
  assert(!outbox_pri_replace(&tx,reply,new,80));free(new);
 }
 new=malloc(80);memcpy(new,progress,80);
 assert(outbox_pri_replace(&tx,reply,new,80));assert(tx.pri[0].buf==new && tx.pri[1].buf==probe);
 for(unsigned i=0;i<4;i++)free(tx.pri[i].buf);
 tx=(struct host_outbox){.preserve_read=1,.npri=1,.queued_bytes=80,.queued_bytes_hi=80};
 uint8_t *ordinary=frame(3,0,1,80);ordinary[19]=2;ordinary[27]=7;ordinary[35]=20;
 tx.pri[0]=(struct host_outbox_item){ordinary,80};
 probe=malloc(80);memcpy(probe,ordinary,80);probe[2]=1;
 assert(!outbox_pri_replace(&tx,heartbeat,probe,80));
 tx.pri[1]=(struct host_outbox_item){probe,80};tx.npri++;tx.queued_bytes+=80;
 fresh=malloc(80);memcpy(fresh,probe,80);fresh[35]=21;
 assert(outbox_pri_replace(&tx,heartbeat,fresh,80) && tx.pri[1].buf==fresh);probe=fresh;
 /* A matched-tail heartbeat must not overwrite the prefix-zero escape. */
 for(unsigned field=0;field<2;field++) {
  new=malloc(80);memcpy(new,probe,80);new[field?59:51]=9;
  assert(!outbox_pri_replace(&tx,heartbeat,new,80));free(new);
 }
 new=frame(1,1,30,80);
 assert(outbox_pri_replace(&tx,heartbeat,new,80));
 assert(tx.pri[0].buf==new && tx.pri[1].buf==probe && tx.queued_bytes==160);
 free(new);free(probe);
 /* Only canonical successful prefix-zero read votes may supersede an
  * older context. A regular replication ACK retains its independent slot. */
 tx=(struct host_outbox){.preserve_read=1,.npri=2,.queued_bytes=160,.queued_bytes_hi=160};
 progress=frame(4,0,1,80);progress[3]=1;progress[19]=2;progress[27]=7;progress[35]=20;progress[75]=9;
 uint8_t canonical[80]={0};memcpy(canonical,progress,80);canonical[2]=1;canonical[75]=0;
 assert(outbox_pure_read_control(canonical,80));
 probe=malloc(80);memcpy(probe,canonical,80);
 tx.pri[0]=(struct host_outbox_item){progress,80};tx.pri[1]=(struct host_outbox_item){probe,80};
 for(uint64_t round=21;round<=1024;round++){
  new=malloc(80);memcpy(new,canonical,80);
  for(unsigned j=0;j<8;j++)new[28+j]=(uint8_t)(round>>(56-8*j));
  assert(outbox_pri_replace(&tx,reply,new,80));
  assert(tx.npri==2 && tx.queued_bytes==160 && tx.queued_bytes_hi==160 && tx.pri[0].buf==progress);
  assert(rd64be(tx.pri[1].buf+28)==round);
 }
 probe=tx.pri[1].buf;
 new=malloc(80);memcpy(new,canonical,80); /* delayed old vote is consumed, never replaces latest */
 assert(outbox_pri_replace(&tx,reply,new,80) && tx.pri[1].buf==probe && rd64be(probe+28)==1024);
 /* Every nonzero progress field, class/status, group, peer, term and boot
  * stays separately owed; zero context grants no read authority. */
 for(unsigned field=0;field<80;field++){
  if(field==0 || (field>=28 && field<36))continue;
  new=malloc(80);memcpy(new,canonical,80);new[field]++;
  assert(!outbox_pure_read_control(new,80) || (field>=4 && field<28) || field==1);
  assert(!outbox_pri_replace(&tx,reply,new,80));free(new);
 }
 new=malloc(80);memcpy(new,canonical,80);memset(new+28,0,8);
 assert(!outbox_pri_replace(&tx,reply,new,80));free(new);
 new=malloc(80);memcpy(new,canonical,80);memset(new+28,255,8);
 assert(outbox_pri_replace(&tx,reply,new,80) && rd64be(tx.pri[1].buf+28)==UINT64_MAX);
 probe=tx.pri[1].buf;new=malloc(80);memcpy(new,canonical,80);memset(new+28,255,8);new[35]--;
 assert(outbox_pri_replace(&tx,reply,new,80) && tx.pri[1].buf==probe && tx.queued_bytes==160);
 /* Files retain their prior replacement policy. */
 tx.preserve_read=0;new=malloc(80);memcpy(new,canonical,80);
 assert(outbox_pri_replace(&tx,reply,new,80) && tx.pri[0].buf==new && tx.pri[1].buf==probe);
 free(tx.pri[0].buf);free(tx.pri[1].buf);
 /* Requests have their own canonical queue bound, separate from ordinary
  * progress-bearing AE and from votes. Original entries remain owed. */
 tx=(struct host_outbox){.preserve_read=1,.npri=2,.queued_bytes=160,.queued_bytes_hi=160};
 uint8_t request[80]={0};memcpy(request,canonical,80);request[0]=3;request[3]=0;
 assert(outbox_pure_read_control(request,80));
 ordinary=malloc(80);memcpy(ordinary,request,80);ordinary[2]=0;ordinary[59]=7;
 tx.pri[0]=(struct host_outbox_item){ordinary,80};
 uint8_t *queued=malloc(80);memcpy(queued,request,80);tx.pri[1]=(struct host_outbox_item){queued,80};
 for(unsigned r=21;r<=1024;r++) {
  new=malloc(80);memcpy(new,request,80);new[34]=(uint8_t)(r>>8);new[35]=(uint8_t)r;
  assert(outbox_pri_replace(&tx,heartbeat,new,80));
  assert(tx.pri[0].buf==ordinary && tx.pri[0].buf[59]==7 && tx.npri==2 && tx.queued_bytes==160 && tx.queued_bytes_hi==160);
 }
 probe=tx.pri[1].buf;new=malloc(80);memcpy(new,request,80);
 assert(outbox_pri_replace(&tx,heartbeat,new,80) && tx.pri[1].buf==probe && rd64be(probe+28)==1024);
 for(unsigned field=1;field<80;field++) {
  if(field>=28 && field<36)continue;
  new=malloc(80);memcpy(new,request,80);new[field]++;
  assert(!outbox_pri_replace(&tx,heartbeat,new,80));free(new);
 }
 new=malloc(80);memcpy(new,request,80);memset(new+28,0,8);
 assert(!outbox_pri_replace(&tx,heartbeat,new,80));free(new);
 for(int i=0;i<tx.npri;i++)free(tx.pri[i].buf);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-host-outbox-') as d:
 p=Path(d)/'test.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
 sanitized=Path(d)/'sanitized'
 subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p),'-o',str(sanitized)],check=True)
 subprocess.run([str(sanitized)],check=True)
 p.write_text(code.replace('if(read_vote && rd64be(tx->pri[i].buf+28)>rd64be(buf+28))', 'if(0 && read_vote && rd64be(tx->pri[i].buf+28)>rd64be(buf+28))'))
 negative=Path(d)/'late-context-negative'
 subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',str(p),'-o',str(negative)],check=True)
 result=subprocess.run([str(negative)],capture_output=True)
 assert result.returncode!=0,'late context negative unexpectedly passed'
 p.write_text(code.replace('(buf[0]==EFS_RAFT_MSG_AE_REQ && buf[3]==0)', '(0 && buf[0]==EFS_RAFT_MSG_AE_REQ && buf[3]==0)'))
 subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',str(p),'-o',str(negative)],check=True)
 assert subprocess.run([str(negative)],capture_output=True).returncode!=0,'request-bound negative unexpectedly passed'
 print('host outbox: canonical request/vote bounded across1000rounds, delayed context refusal, all header guards, ASan/UBSan and late-context negative PASS')
print('host outbox: group/class/read-context/prefix retention, vote eviction and bounded replacement PASS')
