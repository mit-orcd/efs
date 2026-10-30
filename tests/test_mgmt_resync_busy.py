#!/usr/bin/env python3
"""Run the real CLI command over scripted pre-acceptance BUSY replies.
A single absolute budget covers connect/declaration/send/receive and pauses;
unknown transport outcomes and terminal refusals are never auto-retried.
"""
from pathlib import Path
import os,shlex,subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
s=(Path(sys.argv[1]) if len(sys.argv)>1 else root/'src/mgmt/efs_mgmt.c').read_text()
a=s.index('static int cmd_raft_resync(');b=s.index('\n}',a)+2
code=r'''
#include "efs/common.h"
#include "efs/network.h"
#include "efs/protocol.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static uint64_t now=1000,deadline;
static unsigned calls,connects,closes,busy_count;
static int terminal,broken,malformed;
static int __attribute__((unused)) fake_clock(clockid_t c,struct timespec*t){assert(c==CLOCK_MONOTONIC);t->tv_sec=now/1000;t->tv_nsec=(now%1000)*1000000;return 0;}
static int __attribute__((unused)) fake_sleep(useconds_t us){now+=(us+999)/1000;return 0;}
static int fake_close(int fd){assert(fd==73);closes++;return 0;}
#define clock_gettime fake_clock
#define usleep fake_sleep
#define close fake_close
uint64_t efs_net_deadline_ms(void){return deadline;}
void efs_net_set_deadline_ms(uint64_t d){deadline=d;}
int efs_net_remaining_ms(int cap){if(!deadline)return cap;if(now>=deadline)return 0;uint64_t r=deadline-now;return r<(uint64_t)cap?(int)r:cap;}
int efs_set_recv_timeout(int fd,int ms){assert(fd==73&&ms>0&&ms<=efs_net_remaining_ms(EFS_IO_TIMEOUT_MS));return 0;}
int efs_set_send_timeout(int fd,int ms){return efs_set_recv_timeout(fd,ms);}
int efs_connect_declared(const char*h,uint16_t p){assert(!strcmp(h,"peer")&&p==1234);connects++;return 73;}
static int parse_host_port(const char*a,char*h,size_t n,uint16_t*p){(void)a;snprintf(h,n,"peer");*p=1234;return 0;}
static int send_recv(int fd,uint8_t type,const void*body,uint32_t n,uint8_t*rt,void**rp,uint32_t*rn){
 assert(fd==73&&type==EFS_MSG_RAFT_RESYNC&&n==sizeof(struct efs_msg_raft_resync));
 assert(((const struct efs_msg_raft_resync*)body)->group==2);calls++;
 if(broken)return -1;
 struct efs_msg_raft_resync_reply*r=calloc(1,sizeof(*r));assert(r);
 r->rc=calls<=busy_count?EFS_ERR_BUSY:terminal;*rp=r;*rt=EFS_MSG_RAFT_RESYNC_REPLY;*rn=malformed?1:sizeof(*r);return 0;
}
'''+s[a:b]+r'''
static void reset(void){now=1000;deadline=0;calls=connects=closes=busy_count=0;terminal=broken=malformed=0;}
int main(void){char*a[]={"peer:1234","2"};
 reset();busy_count=3;assert(cmd_raft_resync(2,a)==0);assert(calls==4&&connects==4&&closes==4&&now==1150&&!deadline);
 reset();terminal=EFS_ERR_NOT_PRIMARY;assert(cmd_raft_resync(2,a)==1);assert(calls==1&&now==1000&&!deadline);
 reset();busy_count=10000;assert(cmd_raft_resync(2,a)==1);assert(now==1000+EFS_IO_TIMEOUT_MS&&calls<=EFS_IO_TIMEOUT_MS/50&&!deadline);
 reset();busy_count=10000;deadline=1120;assert(cmd_raft_resync(2,a)==1);assert(now==1120&&calls==3&&deadline==1120);
 reset();broken=1;assert(cmd_raft_resync(2,a)==1);assert(calls==1&&closes==1&&!deadline);
 reset();malformed=1;assert(cmd_raft_resync(2,a)==1);assert(calls==1&&closes==1&&!deadline);
 puts("resync CLI: definite BUSY retries, fixed budget, outer deadline, refusal and unknown outcome PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-mgmt-resync-') as d:
 p=Path(d)/'t.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
