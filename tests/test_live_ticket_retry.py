#!/usr/bin/env python3
"""W137: the live ticket probe's retry path against the real PUT reply shape.

fragment_probe is extracted from tests/live/client_put_ticket.c and driven
by a fake member whose replies are built by efs_put_reply_encode, the same
encoder storage uses (handler.c), and decoded by efs_put_reply_decode. BUSY
is retried with the same ticket and bytes inside one budget; STALE and IO are
terminal; a lost reply is followed by an exact retry. Negative controls: a
retry path that also retries STALE, and a member answering the retired
5-byte staged shape, must both fail."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'tests/live/client_put_ticket.c').read_text();a=s.index('static void fragment_probe(');b=s.index('\n/* A full chunk image',a)
probe=s[a:b]
code=r'''
#include "efs/protocol.h"
#include "efs/put_upload.h"
#include "efs/network.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static uint64_t budget,now_ms=1;
static unsigned fi,calls[3],mode,sleeps,old_shape,lost[3];
static const struct efs_put_ticket *original;
uint64_t efs_net_deadline_ms(void){return budget;}
void efs_net_set_deadline_ms(uint64_t d){if(d)assert(d<=now_ms+15000);budget=d;}
static int fake_clock(clockid_t id,struct timespec *out){assert(id==CLOCK_MONOTONIC);out->tv_sec=now_ms/1000;out->tv_nsec=(now_ms%1000)*1000000;return 0;}
static int fake_sleep(useconds_t us){assert(us==100000);sleeps++;now_ms+=100;return 0;}
#define clock_gettime fake_clock
#define usleep fake_sleep
int efs_connect_declared(const char *host,uint16_t port){assert(!strcmp(host,"127.0.0.1"));fi=port-27110;assert(fi<3);return 100+fi;}
int efs_set_recv_timeout(int fd,int ms){assert(fd>=100&&ms==15000);return 0;}
int efs_set_send_timeout(int fd,int ms){assert(fd>=100&&ms==15000);return 0;}
int efs_put_ticket_encode(const struct efs_put_ticket *ticket,uint8_t out[EFS_PUT_TICKET_WIRE_LEN]){assert(ticket==original);memset(out,91,EFS_PUT_TICKET_WIRE_LEN);return 0;}
int efs_send_msg_parts(int fd,uint8_t type,const void *header,uint32_t size,const void *body,uint32_t bytes){
 assert(fd==100+(int)fi&&type==EFS_MSG_PUT_CHUNK&&size==EFS_PUT_UPLOAD_HEADER_LEN&&size==EFS_PUT_HEADER_LEN);
 assert(efs_put_upload_rd32(header)==1&&efs_put_upload_rd32((const uint8_t *)header+4)==fi);
 for(unsigned i=8;i<size;i++)assert(((const uint8_t *)header)[i]==91);
 assert(bytes==32);for(unsigned i=0;i<bytes;i++)assert(((const uint8_t *)body)[i]==(uint8_t)(91+fi));
 calls[fi]++;return 0;
}
int efs_send_msg(int fd,uint8_t type,const void *body,uint32_t bytes){(void)fd;(void)type;(void)body;(void)bytes;assert(0);return -1;}
int efs_recv_msg(int fd,uint8_t *type,void **reply,uint32_t *bytes){
 assert(fd==100+(int)fi);
 int rc=calls[fi]==1?EFS_ERR_NOT_PRIMARY:calls[fi]==2?EFS_ERR_BUSY:EFS_OK;
 if(mode==1)rc=calls[fi]==1?EFS_ERR_NOT_PRIMARY:EFS_ERR_STALE;
 if(mode==2)rc=EFS_ERR_IO;
 if(mode>=3 && mode<=4)rc=EFS_ERR_BUSY;
 if(mode==4)now_ms=20000;
 if(mode==6)rc=EFS_OK;
 *type=EFS_MSG_PUT_CHUNK_REPLY;
 if(old_shape){
  /* The retired staged PUT_TICKETED reply: 4-byte rc + path. */
  uint8_t *p=calloc(1,5);assert(p);efs_put_upload_wr32(p,(uint32_t)rc);*reply=p;*bytes=5;return 0;
 }
 uint8_t *p=calloc(1,EFS_PUT_REPLY_LEN);assert(p);
 efs_put_reply_encode(rc,rc==EFS_OK?2:0xff,123,p);
 *reply=p;*bytes=EFS_PUT_REPLY_LEN;return 0;
}
static int fake_close(int fd){assert(fd==100+(int)fi);if(mode==6&&calls[fi]==1)lost[fi]++;return 0;}
#define close fake_close
''' + probe + r'''
int main(int argc,char **argv){
 (void)argc;mode=atoi(argv[1]);old_shape=getenv("OLD_SHAPE")!=NULL;
 struct efs_put_ticket t={.ino=4097,.ci=3,.fragment_len=32,.nodes={1,2,3}};original=&t;
 uint8_t f0[32],f1[32],f2[32];memset(f0,91,32);memset(f1,92,32);memset(f2,93,32);
 uint8_t *const frags[3]={f0,f1,f2};
 if(mode==5){mode=0;budget=5000;}
 uint64_t saved=budget;
 fragment_probe(27110,&t,frags,0,mode==6,mode==1?EFS_ERR_STALE:EFS_OK);
 assert(budget==saved);
 for(unsigned i=0;i<3;i++)assert(calls[i]==(mode==1?2u:mode==6?2u:3u));
 if(mode==6)for(unsigned i=0;i<3;i++)assert(lost[i]==1);
 assert(sleeps==(mode==1?3u:mode==6?0u:6u));return 0;
}
'''
def build(d,name,src):
 c=Path(d)/(name+'.c');exe=Path(d)/name;c.write_text(src)
 subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(c),'-o',str(exe)],check=True)
 return exe
def run(exe,mode,**env):
 return subprocess.run([str(exe),str(mode)],env=dict(os.environ,**env),stdout=subprocess.PIPE,stderr=subprocess.PIPE).returncode
with tempfile.TemporaryDirectory(prefix='efs-live-ticket-retry-') as d:
 exe=build(d,'probe',code)
 for mode in (0,1,5,6):assert run(exe,mode)==0,('accepted path failed',mode)
 for mode in (2,3,4):assert run(exe,mode)!=0,(mode,'refusal incorrectly accepted')
 # Negative control 1: the retired staged reply shape is never decoded.
 for mode in (0,1):assert run(exe,mode,OLD_SHAPE='1')!=0,('5-byte reply accepted',mode)
 # Negative control 2: a retry path that treats STALE as retryable.
 broken=code.replace('   if(rc!=EFS_ERR_BUSY)break;','   if(rc!=EFS_ERR_BUSY && rc!=EFS_ERR_STALE)break;')
 assert broken!=code,'negative-control mutation did not apply'
 bexe=build(d,'broken',broken)
 assert run(bexe,1)!=0,'broken retry path (STALE retried) passed'
 assert run(bexe,0)==0,'mutation must only change STALE handling'
print('W137 live ticket probe: real 10-byte PUT reply (storage encoder/decoder), BUSY retried with the same ticket and bytes, STALE/IO terminal, lost reply then exact retry, attempt/deadline bounds and inherited budget PASS; negative controls (retired 5-byte shape, STALE-retrying path) FAIL as required')
