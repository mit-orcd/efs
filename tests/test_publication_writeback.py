#!/usr/bin/env python3
"""Actual retained writeback driver under lost-reply/pending/ACK faults."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include "client_internal.h"
#include "efs/publication_ack.h"
#include "efs/checksum.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
void efs_hash(const void *src,size_t n,uint8_t out[EFS_HASH_SIZE]) {
 const uint8_t *p=src;memset(out,0,EFS_HASH_SIZE);
 for(size_t i=0;i<n;i++)out[i%EFS_HASH_SIZE]=(uint8_t)(out[i%EFS_HASH_SIZE]*33u^p[i]);
}
/* Packed record encoding uses the real canonical codec; this legacy-only
 * ownership harness does not create segment bodies. */
int efs_segment_geometry(size_t n, size_t *out) { (void)n; (void)out; return EFS_ERR_INVAL; }
#define put packed_put
#define get packed_get
#include "packed_segment.c"
#undef put
#undef get
#include "publication.c"
#include "publication_ack.c"
#include "publication_writeback.c"
uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane) { (void)ino;(void)lane;return 1; }
static int allocation_fail,write_rc;static unsigned accepted,allocations,frees;
int efs_client_publication_id(uint64_t seq,struct efs_opid *out) {
 memset(out,0,sizeof(*out));out->client_uuid[0]=1;out->session_epoch=1;out->seq=seq;return EFS_OK;
}
struct efs_publication_ack_queue *efs_client_publication_ack_alloc(void) {
 allocations++;return allocation_fail?NULL:calloc(1,sizeof(struct efs_publication_ack_queue));
}
int efs_client_publication_ack_free(struct efs_publication_ack_queue *q) { frees++;free(q);return EFS_OK; }
int efs_dcache_admit_lane_write(efs_ino_t ino,uint64_t gen,uint32_t ci,const struct efs_msg_lane_writer_view_reply *view,uint32_t off,const uint8_t *src,uint32_t len) {
 (void)ino;(void)gen;(void)ci;(void)view;(void)off;(void)src;(void)len;
 if(!write_rc)accepted++;
 return write_rc;
}
static int status_rc=EFS_ERR_NET,submit_rc,retire_rc=EFS_ERR_NET;
static unsigned status_calls,submits,sessions,captures,consumes,retires;
static unsigned state=EFS_PUBLICATION_COMMITTED;
static int verdict,consume_rc,consume_exact=1;
static struct efs_msg_publication admitted;
int efs_dcache_publication_request(efs_ino_t ino,uint64_t gen,uint32_t ci,uint64_t object,uint64_t sequence,uint64_t size,struct efs_msg_publication *out) {
 captures++;memset(out,0,sizeof(*out));out->rec.ino=ino;out->rec.file_generation=gen;
 out->rec.chunk_index=ci;out->rec.chunk_generation=object;out->size=size;
 out->rec.publish_flags=EFS_CHUNK_REC_F_CAPTURED_FILEID|EFS_CHUNK_REC_F_CAPTURED_EPOCH;
 for(unsigned j=0;j<3;j++)out->rec.nodes[j]=j+1;
 out->id.client_uuid[0]=1;out->id.session_epoch=1;out->id.seq=sequence;
 admitted=*out;return EFS_OK;
}
int efs_client_rpc_publication_session(uint32_t shard,const struct efs_opid *id) {
 (void)shard;assert(!memcmp(id,&admitted.id,sizeof(*id)));sessions++;return EFS_OK;
}
int efs_client_rpc_publication(const struct efs_msg_publication *request,int mode,struct efs_msg_publication_reply *out) {
 assert(!memcmp(request,&admitted,sizeof(*request)));
 int rc;if(mode==1){status_calls++;rc=status_rc;}else if(!mode){submits++;rc=submit_rc;}else{retires++;rc=retire_rc;}
 if(rc)return rc;
 memset(out,0,sizeof(*out));
 struct efs_meta_pub p;assert(!efs_publication_from_rec(&request->rec,request->size,&p));p.publication_id=request->id;
 assert(!efs_publication_digest(&p,out->digest));out->state=mode==2?EFS_PUBLICATION_RETIRED:state;
 out->verdict=mode==2?EFS_META_PUBLICATION_RETIRED:verdict;return EFS_OK;
}
int efs_dcache_consume_publication(efs_ino_t ino,uint64_t gen,uint32_t ci,uint64_t object,uint64_t seq,const struct efs_msg_publication_reply *reply,int *consumed) {
 assert(ino==1 && gen==7 && ci==0 && object==10 && seq==9);assert(reply->state==state);
 consumes++;*consumed=consume_exact;return consume_rc;
}
int main(void) {
 struct efs_publication_ack_queue *reserved=NULL;struct efs_msg_lane_writer_view_reply view={.chunk_size=EFS_MIN_CHUNK_SIZE};uint8_t byte=1;
 allocation_fail=1;
 assert(efs_client_publication_admit_write(&reserved,1,7,0,&view,0,&byte,1)==EFS_ERR_NOMEM && !reserved && !accepted);
 allocation_fail=0;write_rc=EFS_ERR_BUSY;
 assert(efs_client_publication_admit_write(&reserved,1,7,0,&view,0,&byte,1)==EFS_ERR_BUSY && !reserved && !accepted && frees==1);
 write_rc=0;
 assert(!efs_client_publication_admit_write(&reserved,1,7,0,&view,0,&byte,1) && accepted==1 && reserved->bound);
 assert(efs_client_publication_admit_write(&reserved,2,7,0,&view,0,&byte,1)==EFS_ERR_STALE && accepted==1);
 reserved->count=EFS_META_PUBLICATION_MAX_RECEIPTS;
 assert(efs_client_publication_admit_write(&reserved,1,7,0,&view,0,&byte,1)==EFS_ERR_BUSY && accepted==1);
 reserved->count=0;assert(!efs_client_publication_ack_free(reserved));
 struct efs_publication_ack_queue q={0};
 assert(efs_client_publication_writeback(&q,1,7,0,10,9,100)==EFS_ERR_NET);
 assert(q.count==1 && captures==1 && !submits && !consumes && !q.entries[0].consumed_state);
 status_rc=EFS_ERR_BUSY;submit_rc=EFS_ERR_NET;
 assert(efs_client_publication_writeback(&q,1,7,0,10,9,999)==EFS_ERR_NET);
 assert(captures==1 && admitted.size==100 && sessions==1 && submits==1 && !consumes);
 status_rc=0;state=EFS_PUBLICATION_APPLY_PENDING;verdict=EFS_META_PUBLICATION_PENDING;
 assert(efs_client_publication_writeback(&q,1,7,0,10,9,999)==EFS_ERR_BUSY);
 assert(submits==1 && !consumes && q.count==1);
 assert(efs_client_publication_retire(&q)==EFS_ERR_BUSY && !retires);
 /* A retired floor cannot reconstruct an unconsumed outcome or authorize
  * releasing accepted bytes. Keep the local identity through reconciliation. */
 state=EFS_PUBLICATION_RETIRED;verdict=EFS_META_PUBLICATION_RETIRED;
 assert(efs_client_publication_writeback(&q,1,7,0,10,9,999)==EFS_ERR_STALE);
 assert(q.count==1 && !q.entries[0].consumed_state && !consumes && submits==1);
 status_rc=EFS_ERR_NOT_FOUND;submit_rc=0;state=EFS_PUBLICATION_PENDING;verdict=EFS_META_PUBLICATION_PENDING;
 assert(efs_client_publication_writeback(&q,1,7,0,10,9,999)==EFS_ERR_BUSY);
 assert(sessions==2 && submits==2 && !consumes && q.count==1);
 status_rc=0;
 assert(efs_client_publication_writeback(&q,1,7,0,10,9,999)==EFS_ERR_BUSY);
 assert(sessions==2 && submits==3 && !consumes && q.count==1);
 state=EFS_PUBLICATION_COMMITTED;verdict=0;consume_exact=0;consume_rc=EFS_ERR_STALE;
 assert(efs_client_publication_writeback(&q,1,7,0,10,9,999)==EFS_ERR_STALE);
 assert(!q.entries[0].consumed_state);
 /* A newer write remains owned, but exact old completion still retires only
  * its own immutable intent. Unknown ACK cannot release its metadata budget. */
 consume_exact=1;
 assert(!efs_client_publication_writeback(&q,1,7,0,10,9,999));
 assert(q.entries[0].consumed_state==EFS_PUBLICATION_COMMITTED);
 unsigned consumed_calls=consumes,old_status=status_calls;
 assert(!efs_client_publication_writeback(&q,1,7,0,10,9,5000));
 assert(consumes==consumed_calls && status_calls==old_status && captures==1);
 assert(efs_client_publication_retire(&q)==EFS_ERR_NET && q.count==1);
 retire_rc=0;assert(!efs_client_publication_retire(&q) && !q.count);
 assert(efs_client_publication_retire(&q)==EFS_ERR_NOT_FOUND);
 /* Rejection is consumed separately from transport STALE. */
 memset(&q,0,sizeof(q));status_rc=0;state=EFS_PUBLICATION_REJECTED;verdict=EFS_ERR_STALE;
 assert(efs_client_publication_writeback(&q,1,7,0,10,9,100)==EFS_ERR_STALE);
 assert(q.entries[0].consumed_state==EFS_PUBLICATION_REJECTED);
 assert(!efs_client_publication_retire(&q) && !q.count);
 puts("retained writeback: immutable lost replies, apply-pending retention, exact consumption and lost retirement ACK PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-publication-writeback-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),'-I'+str(root/'src/common'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
