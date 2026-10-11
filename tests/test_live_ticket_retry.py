#!/usr/bin/env python3
"""Actual cold-ticket probe: immutable retry, terminal refusals and one budget."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'tests/live/client_put_ticket.c').read_text();a=s.index('static void fragment_probe(');b=s.index('\nint main(',a)
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
static unsigned fi,calls[3],mode,sleeps;
static const struct efs_put_ticket *original;
uint64_t efs_net_deadline_ms(void){return budget;}
void efs_net_set_deadline_ms(uint64_t d){if(d)assert(d<=now_ms+15000);budget=d;}
static int fake_clock(clockid_t id,struct timespec *out){assert(id==CLOCK_MONOTONIC);out->tv_sec=now_ms/1000;out->tv_nsec=(now_ms%1000)*1000000;return 0;}
static int fake_sleep(useconds_t us){assert(us==100000);sleeps++;now_ms+=100;return 0;}
#define clock_gettime fake_clock
#define usleep fake_sleep
int efs_connect_tcp(const char *host,uint16_t port){assert(!strcmp(host,"127.0.0.1"));fi=port-27110;assert(fi<3);return 100+fi;}
int efs_set_recv_timeout(int fd,int ms){assert(fd>=100&&ms==15000);return 0;}
int efs_set_send_timeout(int fd,int ms){assert(fd>=100&&ms==15000);return 0;}
int efs_put_ticket_encode(const struct efs_put_ticket *ticket,uint8_t out[EFS_PUT_TICKET_WIRE_LEN]){assert(ticket==original);memset(out,91,EFS_PUT_TICKET_WIRE_LEN);return 0;}
int efs_send_msg_parts(int fd,uint8_t type,const void *header,uint32_t size,const void *body,uint32_t bytes){
 assert(fd==100+(int)fi&&type==EFS_MSG_PUT_TICKETED&&size==EFS_PUT_UPLOAD_HEADER_LEN);
 assert(efs_put_upload_rd32(header)==1&&efs_put_upload_rd32((const uint8_t *)header+4)==fi);
 for(unsigned i=8;i<size;i++)assert(((const uint8_t *)header)[i]==91);
 assert(bytes==32);for(unsigned i=0;i<bytes;i++)assert(((const uint8_t *)body)[i]==91);
 calls[fi]++;return 0;
}
int efs_send_msg(int fd,uint8_t type,const void *body,uint32_t bytes){(void)fd;(void)type;(void)body;(void)bytes;assert(0);return -1;}
int efs_recv_msg(int fd,uint8_t *type,void **reply,uint32_t *bytes){
 assert(fd==100+(int)fi);uint8_t *p=calloc(1,5);assert(p);
 int rc=calls[fi]==1?EFS_ERR_NOT_PRIMARY:calls[fi]==2?EFS_ERR_BUSY:EFS_OK;
 if(mode==1)rc=calls[fi]==1?EFS_ERR_NOT_PRIMARY:EFS_ERR_STALE;
 if(mode==2)rc=EFS_ERR_IO;
 if(mode>=3)rc=EFS_ERR_NOT_PRIMARY;
 if(mode==4)now_ms=20000;
 efs_put_upload_wr32(p,(uint32_t)rc);*type=EFS_MSG_PUT_TICKETED_REPLY;*reply=p;*bytes=5;return 0;
}
static int fake_close(int fd){assert(fd==100+(int)fi);return 0;}
#define close fake_close
''' + s[a:b] + r'''
int main(int argc,char **argv){
 (void)argc;mode=atoi(argv[1]);struct efs_put_ticket t={.ino=4097,.ci=3,.fragment_len=32,.nodes={1,2,3}};original=&t;
 if(mode==5){mode=0;budget=5000;}
 uint64_t saved=budget;fragment_probe(27110,&t,0,mode==1?EFS_ERR_STALE:EFS_OK);
 assert(budget==saved);
 for(unsigned i=0;i<3;i++)assert(calls[i]==(mode==1?2u:3u));
 assert(sleeps==(mode==1?3u:6u));return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-live-ticket-retry-') as d:
 c=Path(d)/'probe.c';exe=Path(d)/'probe';c.write_text(code)
 subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(c),'-o',str(exe)],check=True)
 for mode in (0,1,5):subprocess.run([str(exe),str(mode)],check=True)
 for mode in (2,3,4):
  r=subprocess.run([str(exe),str(mode)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  assert r.returncode!=0,(mode,'refusal incorrectly accepted')
print('Actual ticket probe: identical body/ticket retries, exact stale refusal, hard IO refusal, attempt/deadline bounds and inherited budget restoration PASS')
