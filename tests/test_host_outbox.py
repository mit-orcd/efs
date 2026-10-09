#!/usr/bin/env python3
"""Exercise production priority replacement with interleaved Raft groups."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int outbox_pri_replace(');b=s.index('\n}',a)+2
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define HOST_OUTBOX_BYTES 1024
#define EFS_WIRE_RAFT_HDR_LEN 80
struct host_outbox_item {uint8_t *buf;uint32_t len;};
struct host_outbox {int npri;uint64_t queued_bytes,queued_bytes_hi;struct host_outbox_item pri[4];};
static int reply(const uint8_t*b,uint32_t n){return b&&n>=1&&b[0]==4;}
static int heartbeat(const uint8_t*b,uint32_t n){return b&&n>=1&&b[0]==3;}
static uint8_t *frame(unsigned type,unsigned group,unsigned mark,unsigned len){
 uint8_t*b=calloc(1,len);assert(b);b[0]=type;b[1]=group;b[2]=mark;return b;}
'''+s[a:b]+r'''
int main(void){
 struct host_outbox tx={.npri=2,.queued_bytes=160,.queued_bytes_hi=160};
 tx.pri[0]=(struct host_outbox_item){frame(4,0,10,80),80};
 tx.pri[1]=(struct host_outbox_item){frame(4,2,20,80),80};
 uint8_t *g0=tx.pri[0].buf,*g2=tx.pri[1].buf;
 uint8_t *new=frame(4,2,21,96);
 assert(outbox_pri_replace(&tx,reply,new,96)==1);
 assert(tx.pri[0].buf==g0&&g0[2]==10); /* other group's ACK retained */
 assert(tx.pri[1].buf==new&&new[2]==21);
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
 free(tx.pri[0].buf);free(tx.pri[1].buf);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-host-outbox-') as d:
 p=Path(d)/'test.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
print('host outbox: independent group ACKs/heartbeats, vote eviction and bounded replacement PASS')
