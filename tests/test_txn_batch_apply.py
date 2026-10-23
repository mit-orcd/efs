#!/usr/bin/env python3
"""K29: a TXN_BATCH entry applies exactly as its commands would one by one.

Compiles the production apply_txn_cmd, txn_batch_shard and apply_txn_batch_cmd
with the production PREPARE/DECIDE/RESOLVE encoders over the real metadata
library and an in-memory KV. Two KVs receive the same transaction: one as
single-command entries, one as TXN_BATCH entries. Checked: identical KV
contents after PREPARE, after a conflicting second transaction (first non-OK
command verdict, the other commands still applied) and after DECIDE plus the
RESOLVE batch; a storage failure returns IO at once and no later command is
attempted (the host then drops the entry's scope and retries it); malformed
frames are UNSUPPORTED and a command for another group's shard is refused,
both with nothing applied. Not covered here: the apply scope and the Raft
core (test_apply_unsupported.py, test_txn_apply_retry.py).
"""
from pathlib import Path
import os, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = (root/'src/server/raft_host.c').read_text()
def function(head):
    a = s.index(head)
    while s.find(';', a) < s.find('{', a):
        a = s.index(head, a + 1)
    return s[a:s.index('\n}', a) + 2]
code = r'''
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include "efs/raft.h"
#include "efs/txn.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HOST_RETIRE_LEN 29u
#define HOST_DECIDE_MAX (23u + 4u * EFS_TXN_MAX_PART)
struct efs_raft_host {struct efs_kv *kv;};
static unsigned marks;
static void host_rec_mark(struct efs_raft_host *h,uint32_t shard){(void)h;(void)shard;marks++;}
static uint32_t rd32be(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint64_t rd64be(const uint8_t *p){uint64_t v=0;for(int i=0;i<8;i++)v=v<<8|p[i];return v;}
static void wr32be(uint8_t *p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
''' + function('static uint32_t pack_prep_raw(') + function('static uint32_t pack_decide(') \
    + function('static void pack_resolve(') + function('static int apply_txn_cmd(') \
    + function('static uint32_t txn_batch_shard(') + function('static int apply_txn_batch_cmd(') + r'''
/* KV image: every key/value, in scan order. */
struct dump {uint32_t n,len;uint8_t buf[1<<16];};
static int dump_cb(void *u,const uint8_t *k,uint32_t kl,const uint8_t *v,uint32_t vl){
    struct dump *d=u;assert(d->len+8+kl+vl<=sizeof(d->buf));
    wr32be(d->buf+d->len,kl);memcpy(d->buf+d->len+4,k,kl);d->len+=4+kl;
    wr32be(d->buf+d->len,vl);memcpy(d->buf+d->len+4,v,vl);d->len+=4+vl;d->n++;return 0;
}
static void same(struct efs_kv *a,struct efs_kv *b){
    static struct dump x,y;memset(&x,0,sizeof(x));memset(&y,0,sizeof(y));
    assert(!efs_kv_scan(a,dump_cb,&x) && !efs_kv_scan(b,dump_cb,&y));
    assert(x.n && x.n==y.n && x.len==y.len && !memcmp(x.buf,y.buf,x.len));
}
static uint32_t snapshot(struct efs_kv *kv){
    static struct dump d;memset(&d,0,sizeof(d));assert(!efs_kv_scan(kv,dump_cb,&d));return d.len^d.n<<20;
}
/* A lane row (opaque to RESOLVE) of inode 4200+id on shard. */
static uint32_t key_for(uint32_t shard,unsigned id,uint8_t key[EFS_KV_KEY_MAX]){
    uint32_t kl=0;assert(!efs_kv_key_lane(shard,4200+id,1,0,key,&kl));return kl;
}
/* PREPARE of an exact-image PUT on key id of shard, as the host encodes it. */
static uint32_t prepare(uint8_t *cmd,const struct efs_txid *t,const struct efs_txn_parts *p,
                        uint32_t shard,unsigned id,const uint8_t *expected,uint32_t en,uint8_t value){
    uint8_t key[EFS_KV_KEY_MAX],pay[64];uint32_t pn,kl=key_for(shard,id,key);
    assert(!efs_txn_encode_excl_value(pay,sizeof(pay),&pn,expected,en,EFS_TXN_PUT,&value,1));
    return pack_prep_raw(cmd,EFS_TXN_EXCL_VALUE,t,p,key,kl,pay,pn);
}
/* Storage failure at the Nth write; later writes are counted. */
static struct {const struct efs_kv_ops *base;struct efs_kv_ops ops;int countdown;unsigned writes,after;} io;
static int io_fail(void){io.writes++;if(io.countdown<0){io.after++;return 0;}return --io.countdown<0;}
static int io_put(void *c,const uint8_t *k,uint32_t kl,const uint8_t *v,uint32_t vl){
    return io_fail()?EFS_ERR_IO:io.base->put(c,k,kl,v,vl);}
static int io_del(void *c,const uint8_t *k,uint32_t kl){return io_fail()?EFS_ERR_IO:io.base->del(c,k,kl);}
static int io_batch(void *c,const struct efs_kv_item *it,uint32_t n){return io_fail()?EFS_ERR_IO:io.base->batch(c,it,n);}
static void io_arm(struct efs_kv *kv,int countdown){
    io.base=kv->ops;io.ops=*kv->ops;io.ops.put=io_put;io.ops.del=io_del;io.ops.batch=io_batch;
    io.countdown=countdown;io.writes=io.after=0;kv->ops=&io.ops;
}
static void io_disarm(struct efs_kv *kv){kv->ops=io.base;}
struct batch {uint8_t cmd[8192];uint32_t len,n;};
static void batch_begin(struct batch *b,uint8_t sub){b->cmd[0]=EFS_MD_CMD_TXN_BATCH;b->cmd[1]=sub;b->len=4;b->n=0;}
static void batch_add(struct batch *b,const uint8_t *cmd,uint32_t len){
    b->cmd[b->len]=len>>8;b->cmd[b->len+1]=len;memcpy(b->cmd+b->len+2,cmd,len);b->len+=2+len;b->n++;
    b->cmd[2]=b->n>>8;b->cmd[3]=b->n;
}
int main(void){
    struct efs_kv *one=efs_kv_mem_create(),*many=efs_kv_mem_create();assert(one&&many);
    struct efs_raft_host h1={one},hb={many};
    /* Shards 1, 3, 5 are group 0; shard 2 is group 2. */
    struct efs_txn_parts parts={0};parts.n=3;parts.shard[0]=1;parts.shard[1]=3;parts.shard[2]=5;
    struct efs_txid t={{1}};uint8_t cmd[EFS_TXN_PREPARE_MAX];uint32_t len;struct batch b;
    batch_begin(&b,EFS_MD_CMD_PREPARE);
    for(unsigned i=0;i<6;i++){
        len=prepare(cmd,&t,&parts,parts.shard[i%3],i,NULL,EFS_TXN_ABSENT,(uint8_t)(10+i));
        assert(apply_txn_cmd(&h1,cmd,len)==EFS_OK);batch_add(&b,cmd,len);
    }
    assert(apply_txn_batch_cmd(&hb,b.cmd,b.len,0)==EFS_OK && marks==12);same(one,many);
    /* A second transaction: command 2 meets the first one's intent. Its
     * verdict is the batch verdict; commands 1 and 3 still apply. */
    struct efs_txid u={{2}};int verdicts[3],first=EFS_OK;batch_begin(&b,EFS_MD_CMD_PREPARE);
    unsigned ids[3]={20,1,21}; /* id 1 on shard 3 is the first transaction's */
    for(unsigned i=0;i<3;i++){
        len=prepare(cmd,&u,&parts,parts.shard[i],ids[i],NULL,EFS_TXN_ABSENT,(uint8_t)(40+i));
        verdicts[i]=apply_txn_cmd(&h1,cmd,len);batch_add(&b,cmd,len);
        if(verdicts[i]!=EFS_OK && first==EFS_OK)first=verdicts[i];
    }
    assert(verdicts[0]==EFS_OK && verdicts[1]!=EFS_OK && verdicts[2]==EFS_OK);
    assert(apply_txn_batch_cmd(&hb,b.cmd,b.len,0)==first);same(one,many);
    /* DECIDE COMMIT, then RESOLVE every part as one entry. */
    len=pack_decide(cmd,&t,efs_txn_coordinator(&t,&parts),EFS_TXN_COMMIT,&parts);
    assert(apply_txn_cmd(&h1,cmd,len)==EFS_OK && apply_txn_cmd(&hb,cmd,len)==EFS_OK);
    batch_begin(&b,EFS_MD_CMD_RESOLVE);
    for(unsigned i=0;i<3;i++){
        pack_resolve(cmd,&t,parts.shard[i],EFS_TXN_COMMIT);
        assert(apply_txn_cmd(&h1,cmd,22)==EFS_OK);batch_add(&b,cmd,22);
    }
    assert(apply_txn_batch_cmd(&hb,b.cmd,b.len,0)==EFS_OK);same(one,many);
    for(unsigned i=0;i<6;i++){
        uint8_t key[EFS_KV_KEY_MAX],v[8];uint32_t vl=sizeof(v),kl=key_for(parts.shard[i%3],i,key);
        assert(efs_kv_get(many,key,kl,v,&vl)==EFS_OK && vl==1 && v[0]==10+i);
    }
    /* Storage failure: IO at once, no later command is attempted (the host
     * drops the entry's scope and the whole entry retries). */
    struct efs_txid w={{3}};batch_begin(&b,EFS_MD_CMD_PREPARE);
    for(unsigned i=0;i<3;i++){len=prepare(cmd,&w,&parts,parts.shard[i],30+i,NULL,EFS_TXN_ABSENT,9);batch_add(&b,cmd,len);}
    uint32_t before=snapshot(many);
    io_arm(many,0);assert(apply_txn_batch_cmd(&hb,b.cmd,b.len,0)==EFS_ERR_IO);io_disarm(many);
    assert(io.writes==1 && snapshot(many)==before);
    io_arm(many,-1);assert(apply_txn_batch_cmd(&hb,b.cmd,b.len,0)==EFS_OK);io_disarm(many);
    unsigned per_command=io.after/3;assert(per_command && io.after==3*per_command);
    struct efs_kv *spare=efs_kv_mem_create();struct efs_raft_host hs={spare};assert(spare);
    io_arm(spare,(int)per_command);assert(apply_txn_batch_cmd(&hs,b.cmd,b.len,0)==EFS_ERR_IO);io_disarm(spare);
    assert(io.writes==per_command+1 && !io.after);
    efs_kv_mem_free(spare);
    before=snapshot(many);
    /* Malformed frames halt (UNSUPPORTED) and apply nothing. */
    struct batch bad=b;bad.cmd[3]=1;bad.cmd[2]=0; /* count 1, trailing commands */
    assert(apply_txn_batch_cmd(&hb,bad.cmd,bad.len,0)==EFS_ERR_UNSUPPORTED);
    bad=b;bad.cmd[1]=EFS_MD_CMD_DECIDE;assert(apply_txn_batch_cmd(&hb,bad.cmd,bad.len,0)==EFS_ERR_UNSUPPORTED);
    bad=b;bad.cmd[6]=EFS_MD_CMD_RESOLVE;assert(apply_txn_batch_cmd(&hb,bad.cmd,bad.len,0)==EFS_ERR_UNSUPPORTED);
    bad=b;assert(apply_txn_batch_cmd(&hb,bad.cmd,bad.len-1,0)==EFS_ERR_UNSUPPORTED);
    bad=b;bad.cmd[bad.len]=0;assert(apply_txn_batch_cmd(&hb,bad.cmd,bad.len+1,0)==EFS_ERR_UNSUPPORTED);
    bad=b;bad.cmd[4]=0xff;assert(apply_txn_batch_cmd(&hb,bad.cmd,bad.len,0)==EFS_ERR_UNSUPPORTED);
    assert(apply_txn_batch_cmd(&hb,b.cmd,3,0)==EFS_ERR_UNSUPPORTED);
    assert(snapshot(many)==before);
    /* A command for another group's shard: refused, nothing applied. */
    struct efs_txn_parts mixed={0};mixed.n=2;mixed.shard[0]=1;mixed.shard[1]=2;
    batch_begin(&b,EFS_MD_CMD_PREPARE);
    for(unsigned i=0;i<2;i++){len=prepare(cmd,&w,&mixed,mixed.shard[i],50+i,NULL,EFS_TXN_ABSENT,9);batch_add(&b,cmd,len);}
    assert(efs_raft_shard_group(2)!=0);
    assert(apply_txn_batch_cmd(&hb,b.cmd,b.len,0)==EFS_ERR_INVAL && snapshot(many)==before);
    efs_kv_mem_free(one);efs_kv_mem_free(many);
    puts("txn batch apply: same KV as single entries for PREPARE, conflicting PREPARE (first verdict, rest applied) and RESOLVE; IO stops at the failing command; malformed frames UNSUPPORTED; foreign shard refused PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-txn-batch-apply-') as d:
    p = Path(d)/'t.c'; p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(root/'include'), str(p), str(root/'libefs.a'), '-pthread', '-lm', '-ldl',
                   '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True)
