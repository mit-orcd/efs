#!/usr/bin/env python3
"""Exercise the actual management cleanup loop with definite/ambiguous results."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/mgmt/efs_mgmt.c').read_text()
a=s.index('static int sess_cleanup_bounded(')
f=s[a:s.index('\n}',a)+2]
c=r'''
#include "efs/protocol.h"
#include "efs/meta_cmd.h"
#include "efs/session.h"
#include <assert.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
static unsigned calls, mode, append_mode;
static int tick;
static int fake_clock(clockid_t id,struct timespec *t)
{assert(id==CLOCK_MONOTONIC);t->tv_sec=tick;t->tv_nsec=0;return 0;}
#define clock_gettime fake_clock
static unsigned efs_raft_shard_group(unsigned s){assert(s==7);return 2;}
static void wr32be_mgmt(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=n>>(24-8*i);}
static void sess_pack_hdr(uint8_t *p,uint8_t g,uint8_t sub,const uint8_t *uuid)
{p[0]=g;p[1]=EFS_MD_CMD_SESSION;p[2]=sub;memcpy(p+3,uuid,16);}
static int sess_rpc(const char *host,uint16_t port,const uint8_t *p,uint32_t n,
                    struct efs_msg_raft_mkfs_reply *r)
{
 assert(!strcmp(host,"owned") && port==123 && n==(append_mode?26:27));
 if(append_mode) {
   assert(p[0]==2 && p[1]==EFS_MD_REQ_SESSION_APPEND_NEXT && p[18]==0 && p[21]==1 && p[22]==0 && p[25]==7);
   for(unsigned i=2;i<18;i++)assert(p[i]==42);
 } else {
 assert(p[0]==2 && p[1]==EFS_MD_CMD_SESSION && p[2]==EFS_MD_SESS_LEASE_LOCK_NEXT);
 assert(p[19]==0 && p[22]==1 && p[23]==0 && p[26]==7);
 for(unsigned i=3;i<19;i++)assert(p[i]==42);
 }
 calls++;
 if(mode==1)return EFS_ERR_IO; /* ambiguous submission: do not reselect */
 if(mode==2)return EFS_ERR_BUSY; /* active or incomplete global barrier */
 r->salt=mode==3?2:mode==4?0:calls==4?1:0;
 if(mode==5)tick=30;
 return EFS_OK;
}
'''+f+r'''
int main(void) {
 uint8_t uuid[16];memset(uuid,42,16);struct efs_msg_raft_mkfs_reply r;
 assert(sess_cleanup_bounded("owned",123,uuid,1,7,append_mode,&r)==EFS_OK && calls==4);
 for(mode=1;mode<=3;mode++) {
   calls=0;assert(sess_cleanup_bounded("owned",123,uuid,1,7,append_mode,&r)==
      (mode==1?EFS_ERR_IO:mode==2?EFS_ERR_BUSY:EFS_ERR_PROTO) && calls==1);
 }
 mode=4;calls=0;assert(sess_cleanup_bounded("owned",123,uuid,1,7,append_mode,&r)==EFS_ERR_BUSY && calls==4096);
 mode=5;calls=0;assert(sess_cleanup_bounded("owned",123,uuid,1,7,append_mode,&r)==EFS_ERR_BUSY && calls==1);
 append_mode=1;mode=0;calls=0;tick=0;
 assert(sess_cleanup_bounded("owned",123,uuid,1,7,append_mode,&r)==EFS_OK && calls==4);
 puts("session CLI: bounded batches, explicit empty proof, ambiguity stops, deadline and work budget PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-cleanup-cli-') as tmp:
    p=Path(tmp);(p/'test.c').write_text(c)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
