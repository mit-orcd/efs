#!/usr/bin/env python3
"""Actual replay helper must distinguish a missing and materializing receipt."""
from pathlib import Path
import os,shlex,subprocess,tempfile
import csrc
root=Path(__file__).resolve().parents[1]
s=csrc.module('raft_host','NAMESPACE_REPLAY_SOURCE');a=s.index('static int host_opid_replay(');b=s.index('\n}',a)+2
source=r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
void efs_telemetry_log(int critical, const char *format, ...) {
    (void)critical; va_list ap; va_start(ap, format); vfprintf(stderr, format, ap); va_end(ap);
}
#define EFS_OK 0
#define EFS_ERR_BUSY -13
#define EFS_ERR_IO -1
#define EFS_KV_KEY_MAX 512
struct efs_opid {uint8_t client_uuid[16];uint32_t session_epoch;uint64_t seq;};
struct efs_opid_req {struct efs_opid id;uint64_t ack;};
struct efs_opid_reply {int rc;uint64_t ino;uint64_t seq;};
struct efs_raft_host {void *telemetry,*kv;uint64_t obs_opid_replay;};
static unsigned probes,checks,keys,telemetry_calls;
static void efs_telemetry_printf(void *p,const char *fmt,...){(void)p;assert(strstr(fmt,"opid-receipt: replay"));telemetry_calls++;}
static int result,pending,key_error,resolve_during_check,committed_pending;
struct host_txn_read_context {struct efs_raft_host *host;uint8_t covered;int unhosted_coordinator;};
static int host_txn_read_coord(void *,void *,uint32_t,int *);
static int efs_opid_req_valid(const struct efs_opid_req *q){return q && q->id.seq && q->id.client_uuid[0];}
static int efs_meta_apply_opid_probe(void *kv,uint32_t shard,const struct efs_opid *id,struct efs_opid_reply *rep){
 (void)kv;assert(shard==1 && id->client_uuid[0]==17);probes++;
 if(result==1){rep->rc=0;rep->ino=4242;}return result;
}
static int efs_kv_key_opid(uint32_t shard,const uint8_t *uuid,uint32_t epoch,uint8_t *key,uint32_t *bytes){
 assert(shard==1 && uuid[0]==17 && epoch==1);keys++;*bytes=3;memset(key,19,3);return key_error;
}
static int efs_txn_opid_busy(void *kv,const uint8_t *key,uint32_t bytes,uint64_t seq){
 (void)kv;assert(bytes==3 && key[0]==19 && seq==1);checks++;
 if(resolve_during_check)result=1;
 return pending;
}
static int efs_txn_opid_replay(void *kv,const uint8_t *key,uint32_t bytes,uint64_t seq,
 int (*coord)(void *,void *,uint32_t,int *),void *ctx,struct efs_opid_reply *rep){
 assert(coord==host_txn_read_coord && ((struct host_txn_read_context*)ctx)->host->kv==kv);
 int rc=efs_txn_opid_busy(kv,key,bytes,seq);
 if(committed_pending){rep->rc=0;rep->ino=4242;return 1;}
 return rc;
}
''' + s[a:b] + r'''
static void reset(void){probes=checks=keys=0;result=pending=key_error=resolve_during_check=committed_pending=0;}
int main(void){
 struct efs_raft_host h={0};struct efs_opid_req q={.id={.client_uuid={17},.session_epoch=1,.seq=1}};struct efs_opid_reply rep={0};
 reset();pending=EFS_ERR_BUSY;assert(host_opid_replay(&h,1,&q,&rep)==EFS_ERR_BUSY);assert(probes==1 && checks==1 && !h.obs_opid_replay);
 reset();pending=EFS_ERR_IO;assert(host_opid_replay(&h,1,&q,&rep)==EFS_ERR_IO);assert(probes==1 && checks==1);
 reset();key_error=EFS_ERR_IO;assert(host_opid_replay(&h,1,&q,&rep)==EFS_ERR_IO);assert(probes==1 && !checks);
 reset();resolve_during_check=1;assert(host_opid_replay(&h,1,&q,&rep)==1 && rep.ino==4242);assert(probes==2 && checks==1 && h.obs_opid_replay==1);
 reset();result=1;pending=EFS_ERR_BUSY;assert(host_opid_replay(&h,1,&q,&rep)==1);assert(probes==1 && !keys && h.obs_opid_replay==2);
 reset();assert(!host_opid_replay(&h,1,&q,&rep));assert(probes==2 && checks==1);
 reset();result=EFS_ERR_IO;assert(host_opid_replay(&h,1,&q,&rep)==EFS_ERR_IO);assert(probes==1 && !keys);
 reset();assert(!host_opid_replay(&h,1,NULL,&rep));assert(!probes && !keys);
 reset();q.id.session_epoch=0;assert(!host_opid_replay(&h,1,&q,&rep));assert(probes==1 && !keys);
 puts("committed pending receipt regression");fflush(stdout);
 reset();q.id.session_epoch=1;pending=EFS_ERR_BUSY;committed_pending=1;
 uint64_t previous=h.obs_opid_replay;
 assert(host_opid_replay(&h,1,&q,&rep)==1 && rep.ino==4242 && probes==1 && checks==1 && h.obs_opid_replay==previous+1);
 puts("Namespace replay: committed pending verdict without background materialization, pending receipt refusal, concurrent materialization re-probe, existing verdict and hard-error propagation PASS");
}
'''
c=s.index('static void host_opid_reply_ino(');d=s.index('\n}',c)+2
source=source.replace('int main(void)', 'static int receipt_probe_main(void)')
last=source.rindex('\n}');source=source[:last]+'\n return 0;'+source[last:]
source += r'''
typedef uint64_t efs_ino_t;
#define EFS_ERR_NOT_FOUND -3
#define EFS_INODE_RPC_OK 0
#define EFS_INODE_RPC_NOT_FOUND 1
#define EFS_INODE_RPC_BUSY 4
#define EFS_INODE_RPC_ERROR 255
#define EFS_MAX_NAME 256
struct efs_meta_row {uint64_t ino;};
struct efs_meta_stat {uint64_t ino;};
struct efs_inode {uint64_t ino,parent;char name[EFS_MAX_NAME];};
struct efs_msg_inode_reply {unsigned status;struct efs_inode inode;};

static int read_rc,row_rc,remote_status,hosted=1,unhosted_coord;
static unsigned reads,rows,forwards;
static uint32_t efs_kv_inode_shard(uint64_t ino){assert(ino==4242);return 2;}
static uint8_t efs_raft_shard_group(uint32_t shard){assert(shard==2);return 2;}
static int host_hosts(struct efs_raft_host *h,uint8_t group){(void)h;assert(group==2);return hosted;}
static void set_inode_rc(struct efs_msg_inode_reply *r,int rc,int hint){(void)hint;memset(r,0,sizeof(*r));r->status=rc==EFS_OK?EFS_INODE_RPC_OK:rc==EFS_ERR_NOT_FOUND?EFS_INODE_RPC_NOT_FOUND:rc==EFS_ERR_BUSY?EFS_INODE_RPC_BUSY:EFS_INODE_RPC_ERROR;}
static int host_read_index(struct efs_raft_host *h,uint8_t group,int *hint){(void)h;(void)hint;assert(group==2);reads++;return read_rc;}
static int host_txn_read_coord(void *arg,void *id,uint32_t sh,int *decision){(void)arg;(void)id;(void)sh;(void)decision;return 0;}
static int efs_meta_apply_get_inode_tx(void *kv,uint64_t ino,int (*coord)(void *,void *,uint32_t,int *),void *ctx,struct efs_meta_row *row){(void)kv;assert(ino==4242&&coord==host_txn_read_coord);struct host_txn_read_context *t=ctx;assert(reads&&t->covered==4);rows++;t->unhosted_coordinator=unhosted_coord;row->ino=ino;return row_rc;}
/* The exact committed-but-unmaterialized fixture: plain read is absent. */
static int efs_meta_apply_get_inode(void *kv,uint64_t ino,struct efs_meta_row *row){(void)kv;(void)ino;(void)row;return EFS_ERR_NOT_FOUND;}
static void host_stat_from_row(const struct efs_meta_row *row,struct efs_meta_stat *st){st->ino=row->ino;}
static void stat_to_inode(const struct efs_meta_stat *st,struct efs_inode *ino){ino->ino=st->ino;}
static void host_need_both(uint8_t *need){need[0]=0;need[1]=2;}
static void host_fwd_getattr(struct efs_raft_host *h,uint64_t ino,struct efs_msg_inode_reply *out,const uint8_t *need,int n){(void)h;assert(ino==4242&&((n==1&&need[0]==2)||(n==2&&need[0]==0&&need[1]==2)));forwards++;memset(out,0,sizeof(*out));out->status=remote_status;out->inode.ino=ino;}
''' + s[c:d] + r'''
int main(void) {
 assert(receipt_probe_main()==0);
 struct efs_raft_host h={0};struct efs_msg_inode_reply out;
 struct efs_opid_reply committed={.rc=EFS_OK,.ino=4242,.seq=9};
 read_rc=row_rc=0;host_opid_reply_ino(&h,1,"child",&committed,&out);
 assert(out.status==EFS_INODE_RPC_OK&&out.inode.ino==4242&&reads==1&&rows==1&&!strcmp(out.inode.name,"child"));
 read_rc=EFS_ERR_BUSY;host_opid_reply_ino(&h,1,"child",&committed,&out);
 assert(out.status==EFS_INODE_RPC_BUSY&&rows==1);
 read_rc=EFS_ERR_IO;host_opid_reply_ino(&h,1,"child",&committed,&out);assert(out.status==EFS_INODE_RPC_ERROR&&rows==1);
 read_rc=0;row_rc=EFS_ERR_NOT_FOUND;host_opid_reply_ino(&h,1,"child",&committed,&out);
 assert(out.status==EFS_INODE_RPC_OK&&out.inode.ino==4242&&rows==2);
 row_rc=EFS_ERR_BUSY;host_opid_reply_ino(&h,1,"child",&committed,&out);assert(out.status==EFS_INODE_RPC_BUSY);
 hosted=0;remote_status=EFS_INODE_RPC_BUSY;host_opid_reply_ino(&h,1,"child",&committed,&out);assert(out.status==EFS_INODE_RPC_BUSY&&forwards==1);
 remote_status=EFS_INODE_RPC_OK;host_opid_reply_ino(&h,1,"child",&committed,&out);assert(!out.status&&out.inode.ino==4242&&forwards==2);
 hosted=1;row_rc=EFS_ERR_IO;unhosted_coord=1;host_opid_reply_ino(&h,1,"child",&committed,&out);assert(!out.status&&forwards==3);
 committed.rc=EFS_ERR_NOT_FOUND;unsigned old=reads;host_opid_reply_ino(&h,1,"child",&committed,&out);assert(out.status==EFS_INODE_RPC_NOT_FOUND&&reads==old);
 puts("Namespace replay inode: fresh child proof and committed intents, BUSY/error refusal, remote authority and proven deletion PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-namespace-receipt-') as td:
 p=Path(td)/'test.c';p.write_text(source);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c99','-Wall','-Wextra','-Werror','-Wno-unused-function',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
