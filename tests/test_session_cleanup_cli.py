#!/usr/bin/env python3
"""Exercise the actual management cleanup loop with definite/ambiguous results."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_CLEANUP_CLIENT_SOURCE',root/'src/common/session_revoke.c')).read_text()
a=s.index('int efs_session_cleanup_bounded(')
f=s[a:s.index('\n}',a)+2]
server=Path(os.environ.get('EFS_CLEANUP_SERVER_SOURCE',root/'src/server/raft_host.c')).read_text()
start=server.index('    if (clen>=2 && cmd[0]==EFS_MD_CMD_SESSION && cmd[1]==EFS_MD_SESS_LEASE_LOCK_NEXT) {')
end=server.index('    } else if (clen>=2 && cmd[0]==EFS_MD_CMD_SESSION && cmd[1]==EFS_MD_SESS_LEASE_LOCK_BATCH)',start)
route=server[start:end]+'    }\n'
c=r'''
#include "efs/protocol.h"
#include "efs/meta_cmd.h"
#include "efs/session.h"
#include "efs/session_revoke.h"
#include <assert.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
static unsigned calls, mode, append_mode;
static int tick;
static uint64_t deadline;
static unsigned waits;
static uint64_t efs_net_deadline_ms(void){return deadline;}
static void efs_net_set_deadline_ms(uint64_t value){deadline=value;}
static int efs_net_remaining_ms(int cap){int remaining=(int)deadline-tick*1000;return remaining<cap?remaining:cap;}
static int usleep(unsigned n){assert(n==50000);waits++;tick++;return 0;}
static int fake_clock(clockid_t id,struct timespec *t)
{assert(id==CLOCK_MONOTONIC);t->tv_sec=tick;t->tv_nsec=0;return 0;}
#define clock_gettime fake_clock
static unsigned efs_raft_shard_group(unsigned s){assert(s==7);return 2;}
static void wr32be_sess(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=n>>(24-8*i);}
static void sess_pack_hdr(uint8_t *p,uint8_t g,uint8_t sub,const uint8_t *uuid)
{p[0]=g;p[1]=EFS_MD_CMD_SESSION;p[2]=sub;memcpy(p+3,uuid,16);}
static int owned;
static int sess_rpc(const struct efs_session_rpc *rpc,const uint8_t *p,uint32_t n,
                    struct efs_msg_raft_mkfs_reply *r)
{
 assert(rpc->ctx==&owned && n==(append_mode?26:27));
 if(append_mode) {
   assert(p[0]==2 && p[1]==EFS_MD_REQ_SESSION_APPEND_NEXT && p[18]==0 && p[21]==1 && p[22]==0 && p[25]==7);
   for(unsigned i=2;i<18;i++)assert(p[i]==42);
 } else {
 assert(p[0]==2 && p[1]==EFS_MD_CMD_SESSION && p[2]==EFS_MD_SESS_LEASE_LOCK_NEXT);
 assert(p[19]==0 && p[22]==1 && p[23]==0 && p[26]==7);
 for(unsigned i=3;i<19;i++)assert(p[i]==42);
 }
 assert(deadline && deadline<=30000);calls++;memset(r,0,sizeof(*r));
 if(mode==6 && calls<13)return EFS_ERR_NOT_PRIMARY;
 if(mode==7)return EFS_ERR_NOT_PRIMARY;
 if(mode==8)return EFS_ERR_STALE;
 if(mode==9)return EFS_ERR_INVAL;
 if(mode>=10 && mode<=13 && calls<13) {
   r->salt=EFS_MD_SESSION_CLEANUP_RETRY_SALT;
   if(mode==11)r->index=99;
   if(mode==12)r->term=7;
   if(mode==13)r->salt=0;
   return EFS_ERR_BUSY;
 }
 if(mode>=14 && mode<=18 && (calls<13 || mode==18)) {
   r->salt=EFS_MD_SESSION_APPEND_RETRY_SALT;
   if(mode==15)r->index=99;
   if(mode==16)r->term=7;
   if(mode==17)r->salt=EFS_MD_SESSION_CLEANUP_RETRY_SALT;
   return EFS_ERR_BUSY;
 }
 if(mode==1)return EFS_ERR_IO; /* ambiguous submission: do not reselect */
 if(mode==2)return EFS_ERR_BUSY; /* active or incomplete global barrier */
 r->salt=mode==19?EFS_MD_SESSION_APPEND_RETRY_SALT:mode==3?2:mode==4?0:((mode==6 || mode==10 || mode==14)?calls==16:calls==4)?1:0;
 if(mode==5)tick=30;
 return EFS_OK;
}
'''+f+r'''
int main(void) {
 uint8_t uuid[16];memset(uuid,42,16);struct efs_msg_raft_mkfs_reply r;
 const struct efs_session_rpc rpc={&owned,NULL};
 assert(efs_session_cleanup_bounded(&rpc,uuid,1,7,append_mode,&r)==EFS_OK && calls==4);
 for(mode=1;mode<=3;mode++) {
   calls=0;assert(efs_session_cleanup_bounded(&rpc,uuid,1,7,append_mode,&r)==
      (mode==1?EFS_ERR_IO:mode==2?EFS_ERR_BUSY:EFS_ERR_PROTO) && calls==1);
 }
 mode=4;calls=0;assert(efs_session_cleanup_bounded(&rpc,uuid,1,7,append_mode,&r)==EFS_ERR_BUSY && calls==4096);
 mode=5;calls=0;assert(efs_session_cleanup_bounded(&rpc,uuid,1,7,append_mode,&r)==EFS_ERR_BUSY && calls==1);
 append_mode=1;mode=0;calls=0;tick=0;
 assert(efs_session_cleanup_bounded(&rpc,uuid,1,7,append_mode,&r)==EFS_OK && calls==4);
 assert(!deadline);
 for(append_mode=0;append_mode<2;append_mode++) {
   mode=6;calls=waits=0;tick=0;
   assert(efs_session_cleanup_bounded(&rpc,uuid,1,7,append_mode,&r)==EFS_OK && calls==16 && waits==12 && !deadline);
   mode=7;calls=waits=0;tick=0;
   assert(efs_session_cleanup_bounded(&rpc,uuid,1,7,append_mode,&r)==EFS_ERR_BUSY && calls==30 && waits==30 && !deadline);
   for(mode=8;mode<=9;mode++) {
     calls=waits=0;tick=0;
     assert(efs_session_cleanup_bounded(&rpc,uuid,1,7,append_mode,&r)==(mode==8?EFS_ERR_STALE:EFS_ERR_INVAL) && calls==1 && !waits && !deadline);
   }
   mode=7;calls=waits=0;tick=0;deadline=3000;
   assert(efs_session_cleanup_bounded(&rpc,uuid,1,7,append_mode,&r)==EFS_ERR_BUSY && calls==3 && waits==3 && deadline==3000);
   deadline=0;
 }
 append_mode=0;mode=10;calls=waits=0;tick=0;
 assert(efs_session_cleanup_bounded(&rpc,uuid,1,7,append_mode,&r)==EFS_OK && calls==16 && waits==12 && !deadline);
 for(mode=11;mode<=13;mode++) {
   calls=waits=0;tick=0;
   assert(efs_session_cleanup_bounded(&rpc,uuid,1,7,append_mode,&r)==EFS_ERR_BUSY && calls==1 && !waits && !deadline);
 }
 append_mode=1;mode=10;calls=waits=0;tick=0;
 assert(efs_session_cleanup_bounded(&rpc,uuid,1,7,append_mode,&r)==EFS_ERR_BUSY && calls==1 && !waits && !deadline);
 mode=14;calls=waits=0;tick=0;
 assert(efs_session_cleanup_bounded(&rpc,uuid,1,7,append_mode,&r)==EFS_OK && calls==16 && waits==12 && !deadline);
 for(mode=15;mode<=17;mode++) {
   calls=waits=0;tick=0;
   assert(efs_session_cleanup_bounded(&rpc,uuid,1,7,append_mode,&r)==EFS_ERR_BUSY && calls==1 && !waits && !deadline);
 }
 append_mode=0;mode=14;calls=waits=0;tick=0;
 assert(efs_session_cleanup_bounded(&rpc,uuid,1,7,append_mode,&r)==EFS_ERR_BUSY && calls==1 && !waits && !deadline);
 append_mode=1;mode=18;calls=waits=0;tick=0;deadline=3000;
 assert(efs_session_cleanup_bounded(&rpc,uuid,1,7,append_mode,&r)==EFS_ERR_BUSY && calls==3 && waits==3 && deadline==3000);
 deadline=0;mode=19;calls=waits=0;tick=0;
 assert(efs_session_cleanup_bounded(&rpc,uuid,1,7,append_mode,&r)==EFS_ERR_PROTO && calls==1 && !waits && !deadline);
 puts("session CLI: bounded batches, explicit empty proof, ambiguity stops, deadline and work budget PASS");
}
'''
server_c=r'''
#include "efs/protocol.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define HOST_SESS_REG_LEN 26
struct host_view {uint64_t applied;};
static int verdict, nonempty, selections, proposals;
static int host_pack_session_cleanup(void *h,const uint8_t *cmd,uint32_t len,
 uint8_t group,int *hint,uint8_t *packed,uint32_t *size)
{assert(!h && cmd[0]==EFS_MD_CMD_SESSION && cmd[1]==EFS_MD_SESS_LEASE_LOCK_NEXT && len==26 && group==0);selections++;*hint=2;*size=nonempty?26:0;memcpy(packed,cmd,26);packed[1]=EFS_MD_SESS_LEASE_LOCK_BATCH;return verdict;}
static void host_view_get(void *h,uint8_t group,struct host_view *v)
{assert(!h && !group);v->applied=17;}
static void dispatch(const uint8_t *cmd,uint32_t clen,struct efs_msg_raft_mkfs_reply *out)
{void *h=NULL;uint8_t group=0,cleanup_cmd[128];int hint=-1,rc;memset(out,0,sizeof(*out));
'''+route+r'''
 assert(cmd==cleanup_cmd && clen==26 && cmd[1]==EFS_MD_SESS_LEASE_LOCK_BATCH);
 proposals++;
}
int main(void) {
 uint8_t cmd[26]={EFS_MD_CMD_SESSION,EFS_MD_SESS_LEASE_LOCK_NEXT};struct efs_msg_raft_mkfs_reply out;
 int errors[]={EFS_ERR_BUSY,EFS_ERR_NOT_PRIMARY,EFS_ERR_IO,EFS_ERR_STALE,EFS_ERR_INVAL};
 for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
   verdict=errors[i];selections=proposals=0;nonempty=1;dispatch(cmd,26,&out);
   assert(selections==1 && !proposals && out.rc==verdict && !out.index && !out.term && out.leader_hint==2);
   assert(out.salt==(verdict==EFS_ERR_BUSY?EFS_MD_SESSION_CLEANUP_RETRY_SALT:0));
 }
 verdict=EFS_OK;nonempty=0;selections=proposals=0;dispatch(cmd,26,&out);
 assert(selections==1 && !proposals && out.rc==EFS_OK && out.salt==1 && out.index==17);
 nonempty=1;selections=proposals=0;dispatch(cmd,26,&out);
 assert(selections==1 && proposals==1 && !out.salt && !out.index && !out.term);
 puts("session server: admission-only retry certificate; no submitted-batch certificate PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-cleanup-cli-') as tmp:
    p=Path(tmp);(p/'server.c').write_text(server_c)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p/'server.c'),'-o',str(p/'server')],check=True)
    subprocess.run([str(p/'server')],check=True)
    (p/'test.c').write_text(c)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
