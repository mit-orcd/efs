#!/usr/bin/env python3
"""Actual ticket result proof: exact settlement, fresh queries and local fences."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text();a=s.index('        int settled=cmd[0]!=EFS_MD_REQ_PUT_TICKET_STATUS');b=s.index('        out->rc=rc;out->salt=',a)
body=s[a:b]
code=r'''
#include "efs/common.h"
#include "efs/meta_cmd.h"
#include "efs/put_ticket.h"
#include <assert.h>
#include <stdio.h>
struct efs_raft_host {struct efs_kv *kv;};
static unsigned reads,floors,ticket_reads,retire_checks;
static int read_rc,floor_rc,ticket_rc,retire_rc,retired=1;
static uint32_t ticket_state=EFS_PUT_TICKET_ADMITTED;
static int host_read_index(struct efs_raft_host*h,uint8_t g,int*hint){(void)h;(void)g;(void)hint;reads++;return read_rc;}
int efs_session_accept(struct efs_kv*k,uint32_t shard,const uint8_t uuid[16],uint32_t epoch){(void)k;assert(shard==17&&uuid[0]==9&&epoch==1);floors++;return floor_rc;}
int efs_put_ticket_get(struct efs_kv*k,const struct efs_put_ticket*t,uint32_t*out){(void)k;assert(t->id.seq==3);ticket_reads++;*out=ticket_state;return ticket_rc;}
int efs_put_ticket_retired(struct efs_kv*k,const struct efs_put_ticket*t,int*out){(void)k;assert(t->id.seq==3);retire_checks++;*out=retired;return retire_rc;}
static int run(uint8_t tag,int rc,uint64_t idx,uint64_t *reply_salt){
 struct efs_raft_host host={0},*h=&host;uint8_t cmd[1]={tag},group=0;int hint=-1;uint32_t shard=17;uint64_t salt=0;
 struct efs_put_ticket ticket={.id={.client_uuid={9},.session_epoch=1,.seq=3}};
''' + body + r'''
 *reply_salt=rc==EFS_OK?salt:0;return rc;
}
static void reset(void){reads=floors=ticket_reads=retire_checks=0;read_rc=floor_rc=ticket_rc=retire_rc=0;retired=1;ticket_state=EFS_PUT_TICKET_ADMITTED;}
int main(void){uint64_t salt=99;
 reset();assert(!run(EFS_MD_CMD_PUT_TICKET_ADMIT,EFS_OK,7,&salt)&&salt==EFS_PUT_TICKET_ADMITTED&&!reads&&floors==1&&ticket_reads==1);
 reset();assert(!run(EFS_MD_CMD_PUT_TICKET_RETIRE_PUBLISHED,EFS_OK,7,&salt)&&salt==EFS_PUT_TICKET_RETIRED&&!reads&&floors==1&&retire_checks==1);
 reset();assert(!run(EFS_MD_REQ_PUT_TICKET_STATUS,EFS_OK,7,&salt)&&reads==1&&ticket_reads==1);
 reset();assert(!run(EFS_MD_CMD_PUT_TICKET_ADMIT,EFS_OK,0,&salt)&&reads==1&&ticket_reads==1);
 int ambiguous[]={EFS_ERR_BUSY,EFS_ERR_STALE};
 for(unsigned i=0;i<2;i++) {
  reset();assert(!run(EFS_MD_CMD_PUT_TICKET_ADMIT,ambiguous[i],7,&salt)&&reads==1&&floors==1&&ticket_reads==1);
  reset();read_rc=EFS_ERR_BUSY;assert(run(EFS_MD_CMD_PUT_TICKET_ADMIT,ambiguous[i],7,&salt)==EFS_ERR_BUSY&&!salt&&reads==1&&!floors&&!ticket_reads);
 }
 reset();floor_rc=EFS_ERR_STALE;assert(run(EFS_MD_CMD_PUT_TICKET_ADMIT,EFS_OK,7,&salt)==EFS_ERR_STALE&&!salt&&!reads&&floors==1&&!ticket_reads);
 reset();ticket_rc=EFS_ERR_PROTO;assert(run(EFS_MD_CMD_PUT_TICKET_ADMIT,EFS_OK,7,&salt)==EFS_ERR_PROTO&&!salt&&ticket_reads==1);
 reset();retired=0;assert(run(EFS_MD_CMD_PUT_TICKET_RETIRE_PUBLISHED,EFS_OK,7,&salt)==EFS_ERR_BUSY&&!salt&&retire_checks==1);
 reset();ticket_rc=EFS_ERR_NOT_FOUND;assert(run(EFS_MD_CMD_PUT_TICKET_ADMIT,EFS_ERR_STALE,7,&salt)==EFS_ERR_STALE&&!salt&&reads==1);
 reset();assert(run(EFS_MD_CMD_PUT_TICKET_ADMIT,EFS_ERR_IO,7,&salt)==EFS_ERR_IO&&!salt&&!reads&&!ticket_reads);
 puts("PUT submission proof: exact success only, STATUS/ambiguity/zero-index fresh fences, immutable body, epoch rejection and retirement evidence PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-put-submit-proof-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
