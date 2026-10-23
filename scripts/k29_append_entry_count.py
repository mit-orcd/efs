#!/usr/bin/env python3
"""K29: Raft entries per O_APPEND record by active-lane count, before/after.

Runs the real metadata code (efs_meta_capture_append,
efs_meta_execute_append_owned, efs_meta_apply_append_catchup,
efs_meta_retire_append_barriers) on an in-memory KV with the first L writer
lanes of one file active. Transaction callbacks apply the real txn layer and
count, per Raft group, every PREPARE, DECIDE and RESOLVE. A record is one
reservation, its completion and, when it was the only open reservation, the
barrier retirement (the sequential-writer case; with several reservations in
flight the retirement runs only when the count of open ones reaches zero).

The counts are mapped onto host entries as src/server/raft_host.c proposes
them. Before K29: one entry per PREPARE and per RESOLVE, one DECIDE, one
finisher RETIRE; a single-shard reservation is one APPEND_ATOMIC entry; the
completion is one entry on the inode group. After K29: the two synchronous
guards (session owner, inode), then one TXN_BATCH entry per group for the
other PREPAREs and one per group for the RESOLVEs (EFS_MD_TXN_BATCH_MAX
commands each). Data publication, PUT tickets and session work are not
counted; they do not depend on the lane count.

Usage: make libefs.a && python3 scripts/k29_append_entry_count.py
Prints one row per lane count: entries per record for group 0 and group 2.
"""
from pathlib import Path
import os, shlex, subprocess, sys, tempfile
root = Path(__file__).resolve().parents[1]
if not (root / 'libefs.a').exists():
    sys.exit('build libefs.a first (make libefs.a)')
code = r'''
#include "efs/kv.h"
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include "efs/raft.h"
#include "efs/session.h"
#include "efs/txn.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
struct count {unsigned prep[3],decide[3],resolve[3],calls;uint32_t coord;};
static struct count *now;
static uint32_t shard_of(const uint8_t *key){return (uint32_t)key[0]<<8|key[1];}
static int coord_fn(void *ctx,const struct efs_txid *t,uint32_t sh,int *d){return efs_txn_decision_get(ctx,sh,t,d);}
static int prepare(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,const struct efs_meta_append_image *q){
    now->prep[efs_raft_shard_group(shard_of(q->key))]++;now->calls++;
    return efs_txn_prepare_excl_value(ctx,t,p,q->key,q->klen,q->expected,q->expected_len,q->op,q->value,q->new_len);
}
static int decide(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,int d){
    now->coord=efs_txn_coordinator(t,p);now->decide[efs_raft_shard_group(now->coord)]++;
    return efs_txn_decide(ctx,now->coord,t,d);
}
static int resolve(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,int d){
    for(unsigned i=0;i<p->n;i++){now->resolve[efs_raft_shard_group(p->shard[i])]++;
        int rc=efs_txn_resolve(ctx,t,p->shard[i],d);if(rc)return rc;}
    return EFS_OK;
}
static const struct efs_meta_append_ops ops={prepare,decide,resolve};
static unsigned batches(unsigned n){return (n+EFS_MD_TXN_BATCH_MAX-1)/EFS_MD_TXN_BATCH_MAX;}
/* Host entries of one counted transaction; inode_group holds the two guards. */
static void entries(const struct count *c,unsigned inode_group,unsigned before[3],unsigned after[3]){
    if(!c->calls)return;
    for(unsigned g=0;g<=2;g+=2){
        unsigned guards=g==inode_group?2:0,rest=c->prep[g]-guards;
        assert(c->prep[g]>=guards);
        before[g]+=c->prep[g]+c->decide[g]+c->resolve[g];
        after[g]+=guards+batches(rest)+c->decide[g]+batches(c->resolve[g]);
    }
    unsigned rg=efs_raft_shard_group(c->coord);before[rg]++;after[rg]++; /* finisher RETIRE */
}
int main(void){
    printf("lanes  group0_before  group0_after  group2_before  group2_after  total_before  total_after\n");
    unsigned lanes[]={1,2,4,8,16,32,48,64};
    for(unsigned li=0;li<sizeof(lanes)/sizeof(lanes[0]);li++){
        struct efs_kv *kv=efs_kv_mem_create();assert(kv);
        struct efs_meta_attrs a={1,1,1};efs_ino_t ino;struct efs_meta_row row;
        assert(!efs_meta_apply_init(kv,1));
        assert(!efs_meta_apply_create_file(kv,&a,EFS_ROOT_INO,S_IFREG|0644,"f",&ino));
        uint64_t mask=lanes[li]==64?UINT64_MAX:((1ull<<lanes[li])-1);
        assert(!efs_meta_apply_activate_lanes(kv,ino,mask));
        struct efs_opid_req q={0};q.id.client_uuid[0]=42;q.id.session_epoch=1;
        assert(!efs_session_establish(kv,efs_kv_inode_shard(ino),q.id.client_uuid,1));
        unsigned ig=efs_raft_shard_group(efs_kv_inode_shard(ino)),before[3]={0},after[3]={0};
        struct efs_meta_append_plan *plan=malloc(sizeof(*plan));assert(plan);
        uint64_t tx=0;
        for(unsigned record=0;record<3;record++){
            struct count reserve={0},retire={0};struct efs_txid t={{0}};
            q.id.seq=record+1;q.ack=record;
            assert(!efs_meta_capture_append(kv,ino,4096,&q,coord_fn,kv,plan));
            assert(!efs_meta_apply_get_inode(kv,ino,&row));
            memcpy(t.bytes,&(uint64_t){++tx},8);now=&reserve;
            assert(!efs_meta_execute_append_owned(kv,plan,&q,&t,&ops,kv));
            /* REPORT's completion: APPEND_CATCHUP up to the published EOF. */
            assert(!efs_meta_apply_append_catchup(kv,ino,row.generation,row.content_epoch,
                                                  plan->offset+4096,1,&q.id));
            memcpy(t.bytes,&(uint64_t){++tx},8);now=&retire;
            assert(!efs_meta_retire_append_barriers(kv,ino,&q.id,&t,&ops,kv));
            if(record!=2)continue; /* steady state: the third record */
            if(plan->parts.n==1){before[ig]++;after[ig]++;} /* APPEND_ATOMIC */
            else entries(&reserve,ig,before,after);
            before[ig]++;after[ig]++; /* completion entry */
            entries(&retire,ig,before,after);
        }
        printf("%5u  %13u  %12u  %13u  %12u  %12u  %11u\n",lanes[li],before[0],after[0],before[2],after[2],
               before[0]+before[2],after[0]+after[2]);
        free(plan);efs_kv_mem_free(kv);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='k29-entry-count-') as d:
    p = Path(d) / 'count.c'; p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(root / 'include'), str(p), str(root / 'libefs.a'), '-pthread', '-lm', '-ldl',
                   '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True)
