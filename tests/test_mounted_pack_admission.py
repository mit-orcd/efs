#!/usr/bin/env python3
"""Actual fresh-write admission: metadata-only CREATE, default geometry, budgets."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/pack_write.c').read_text()
code=s[:s.index('static uint64_t pack_now(')]+r'''
#include <assert.h>
#include <stdio.h>
#include "efs/publication_ack.h"
struct efs_client g_client={.idx_mu=PTHREAD_MUTEX_INITIALIZER};
static int pinned,cap_error,admit_error,alloc_fail,abort_calls,free_calls,size_notes,owner_session_error;
static int allocations,base_error,base_count;static uint64_t inode_size,lane_epoch,resolved_generation=3;
static uint64_t pack_now(void){return 100;}
static int pack_start(void){return 0;}
static void cache_unlocked(void){assert(!pthread_mutex_trylock(&pack_mu));assert(pack_groups[0]->busy);pthread_mutex_unlock(&pack_mu);}
static int pack_capabilities(uint32_t *n){*n=15;return cap_error;}
static int pack_flush_group(unsigned gi,int force){(void)gi;assert(force);return EFS_ERR_BUSY;}
static int pack_retire_group(unsigned gi){assert(gi<2);return EFS_ERR_BUSY;}
static int pack_pause(void){return EFS_ERR_BUSY;}
int efs_export_get_inode(struct efs_export *e,efs_ino_t ino,struct efs_inode *out){(void)e;*out=(struct efs_inode){.ino=ino,.size=inode_size,.mode=S_IFREG|0600};return 0;}
int efs_dcache_ino_pinned(efs_ino_t ino){assert(ino==101);return pinned;}

int efs_client_rpc_lane_writer_view(efs_ino_t ino,uint64_t g,uint32_t ci,uint32_t cs,struct efs_msg_lane_writer_view_reply *out){assert(ino==101&&g==3&&!ci&&cs==EFS_MIN_CHUNK_SIZE);memset(out,0,sizeof(*out));out->view.authority_epoch=lane_epoch;return 0;}
int efs_client_rpc_lane_bootstrap(efs_export_id_t eid,efs_ino_t ino,uint64_t g,uint32_t ci,uint32_t cs,struct efs_msg_lane_writer_view_reply *out){(void)eid;return efs_client_rpc_lane_writer_view(ino,g,ci,cs,out);}
int efs_client_rpc_getchunks_fileid(efs_export_id_t eid,efs_ino_t ino,uint64_t g,uint32_t ci,struct efs_chunk_rec *r,uint32_t *n,uint64_t *resolved){(void)r;assert(eid==1&&ino==101&&!g&&!ci);*n=base_count;*resolved=resolved_generation;return base_error;}
int efs_client_publication_id(uint64_t seq,struct efs_opid *id){*id=(struct efs_opid){.client_uuid={19},.session_epoch=7,.seq=seq};return 0;}
int efs_client_put_id(struct efs_opid *id){return efs_client_publication_id(17,id);}
int efs_client_rpc_publication_session(uint32_t shard,const struct efs_opid *id){assert(shard&&id->session_epoch==7);assert(!pthread_mutex_trylock(&pack_mu));pthread_mutex_unlock(&pack_mu);if(id->seq==17){assert(pack_groups[0]->busy);if(owner_session_error){owner_session_error=0;return EFS_ERR_NET;}}return 0;}
void *efs_buf_metadata_alloc(size_t n){allocations++;if(alloc_fail==allocations)return NULL;return calloc(1,n);}
void efs_buf_metadata_free(void *p,size_t n){assert(n);free(p);}
int efs_client_publication_admit_write(struct efs_publication_ack_queue **q,efs_ino_t ino,uint64_t g,uint32_t ci,const struct efs_msg_lane_writer_view_reply *v,uint32_t off,const uint8_t *b,uint32_t n){(void)v;cache_unlocked();assert(ino==101&&g==3&&!ci&&!off&&b&&n==512);if(admit_error)return admit_error;*q=(struct efs_publication_ack_queue *)(uintptr_t)1;return 0;}
int efs_dcache_snapshot_publication(efs_ino_t ino,uint64_t g,uint32_t ci,const struct efs_msg_lane_writer_view_reply *v,const struct efs_msg_inode_getchunks_reply *base,struct efs_writer_plan *plan,uint8_t *body,uint32_t n){(void)v;cache_unlocked();assert(ino==101&&g==3&&!ci&&base->generation==3&&n==EFS_MIN_CHUNK_SIZE);*plan=(struct efs_writer_plan){.ino=ino,.generation=g,.cache_sequence=1};memset(body,59,512);return 0;}
int efs_dcache_begin_publication(efs_ino_t ino,uint64_t g,uint32_t ci,const struct efs_writer_plan *p,uint64_t obj){cache_unlocked();assert(ino==101&&g==3&&!ci&&p->cache_sequence&&obj);return 0;}
int efs_dcache_abort_pack_admission(efs_ino_t ino,uint64_t g){cache_unlocked();assert(ino==101&&g==3);abort_calls++;return 0;}
int efs_client_publication_ack_free(struct efs_publication_ack_queue *q){cache_unlocked();assert(q);free_calls++;return 0;}
void efs_dcache_note_packed_size(efs_ino_t ino,uint64_t n){assert(ino==101&&n==512);assert(!pthread_mutex_trylock(&pack_mu));assert(pack_groups[0]->busy&&pack_groups[0]->cohort.count==1);pthread_mutex_unlock(&pack_mu);size_notes++;}
int efs_client_pack_enabled(void){return 1;}
'''+s[s.index('int efs_client_pack_write('):s.index('static int pack_flush_run(')]+r'''
int main(void){
 g_client.export_id=1;uint8_t bytes[512];memset(bytes,59,sizeof(bytes));
 assert(efs_client_pack_write(101,1,512,bytes)==EFS_ERR_NOT_FOUND&&!allocations);
 g_client.export.chunk_size=2*EFS_MIN_CHUNK_SIZE;
 assert(efs_client_pack_write(101,0,512,bytes)==EFS_ERR_NOT_FOUND&&!allocations);
 g_client.export.chunk_size=0;inode_size=1;
 assert(efs_client_pack_write(101,0,512,bytes)==EFS_ERR_NOT_FOUND&&!allocations);inode_size=0;
 pinned=1;assert(efs_client_pack_write(101,0,512,bytes)==EFS_ERR_NOT_FOUND&&!allocations);pinned=0;
 cap_error=EFS_ERR_PROTO;assert(efs_client_pack_write(101,0,512,bytes)==EFS_ERR_PROTO&&!allocations);cap_error=0;
 base_error=EFS_ERR_STALE;assert(efs_client_pack_write(101,0,512,bytes)==EFS_ERR_STALE&&!allocations);base_error=0;
 resolved_generation=0;assert(efs_client_pack_write(101,0,512,bytes)==EFS_ERR_PROTO&&!allocations);resolved_generation=3;
 base_count=1;assert(efs_client_pack_write(101,0,512,bytes)==EFS_ERR_NOT_FOUND&&!allocations);base_count=0;
 lane_epoch=1;assert(efs_client_pack_write(101,0,512,bytes)==EFS_ERR_NOT_FOUND&&!allocations);lane_epoch=0;
 alloc_fail=1;assert(efs_client_pack_write(101,0,512,bytes)==EFS_ERR_NOMEM&&!pack_groups[0]&&!abort_calls&&!size_notes);
 alloc_fail=0;owner_session_error=1;assert(efs_client_pack_write(101,0,512,bytes)==EFS_ERR_NET&&!pack_groups[0]->cohort.count&&!pack_groups[0]->busy&&!abort_calls&&!size_notes);
 admit_error=EFS_ERR_BUSY;
 assert(efs_client_pack_write(101,0,512,bytes)==EFS_ERR_BUSY && pack_groups[0] && !pack_groups[0]->cohort.count && !abort_calls&&!free_calls);admit_error=0;
 alloc_fail=allocations+1;
 assert(efs_client_pack_write(101,0,512,bytes)==EFS_ERR_NOMEM && !pack_groups[0]->cohort.count && abort_calls==1&&free_calls==1&&!size_notes);
 alloc_fail=0;
 /* Fresh CREATE's metadata staging mark is intentionally no exclusion: only
  * actual cached/queued data ownership and fresh remote authority block it. */
 assert(!efs_client_pack_write(101,0,512,bytes)&&size_notes==1&&pack_groups[0]->cohort.count==1);
 memset(bytes,99,sizeof(bytes));assert(pack_groups[0]->cohort.records[0].data[0]==59);
 pack_groups[0]->busy=1;
 assert(efs_client_pack_write(101,0,512,bytes)==EFS_ERR_BUSY&&pack_groups[0]->cohort.count==1&&abort_calls==1);
 puts("mounted pack admission: default geometry, CREATE eligibility, real data exclusion, capability refusal, budget-before-accept and immutable retained bytes PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-pack-admit-') as d:
 p=Path(d)/'t.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),str(root/'libefs.a'),'-lm','-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
