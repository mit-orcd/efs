#!/usr/bin/env python3
"""Run the real host adapter with lost apply verdicts and delayed outcomes."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text();a=s.index('void server_raft_host_publication(')
fn=s[a:s.index('\n}',a)+2]
pack_a=s.index('static int pack_publish_cmd(uint8_t *out');pack=s[pack_a:s.index('\n}',pack_a)+2]
apply_a=s.index('static int apply_one_publish(');apply=s[apply_a:s.index('\n}',apply_a)+2]
source=r'''
#include "efs/publication.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include "efs/meta_cmd.h"
#include "efs/session.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define HOST_PUBLISH_LEN 202
#define HOST_PUBLICATION_LEN 230
#define HOST_PUB_F_LANE_LOCAL 1
#define HOST_PUB_F_FRESH_OBJECT 2
#define APPLY_LOG(...) ((void)0)
static int raft_dbg_on(void){return 0;}
static void wr32be(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(24-8*i));}
static void wr64be(uint8_t *p,uint64_t n){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(n>>(56-8*i));}
static uint32_t rd32be(const uint8_t *p){uint32_t n=0;for(unsigned i=0;i<4;i++)n=(n<<8)|p[i];return n;}
static uint64_t rd64be(const uint8_t *p){uint64_t n=0;for(unsigned i=0;i<8;i++)n=(n<<8)|p[i];return n;}
struct efs_raft_host {int running,n;struct efs_kv *kv;};
static struct efs_raft_host host={.running=1,.n=3},*g_host=&host;
static int owns=1,stored,stored_verdict,wait_rc,read_rc,store_on_propose=1;
static int session_rc, fence_after_propose;
static unsigned gates;
static unsigned proposals,reads,forwards;static uint8_t group;
int efs_chunk_size_valid(uint32_t s){return s==EFS_MIN_CHUNK_SIZE;}
void efs_hash(const void *p,size_t n,uint8_t out[EFS_HASH_SIZE])
{(void)p;(void)n;memset(out,0x55,EFS_HASH_SIZE);}
static uint64_t now_ns(void){return 1234;}
static int host_hosts(struct efs_raft_host *h,uint8_t g){(void)h;assert(g==group);return owns;}
static int host_pick_peer(struct efs_raft_host *h,const uint8_t *g,int n,int skip)
{(void)h;(void)skip;assert(n==1 && *g==group);return 1;}
static int host_read_index(struct efs_raft_host *h,uint8_t g,int *hint)
{(void)h;assert(g==group);*hint=1;reads++;return read_rc;}
int efs_session_accept(struct efs_kv *kv,uint32_t shard,const uint8_t uuid[EFS_OPID_UUID_LEN],uint32_t epoch)
{(void)kv;assert(shard==efs_kv_lane_shard(123,17) && uuid[0]==1 && epoch==1);gates++;return session_rc;}
static void set_inode_rc(struct efs_msg_inode_reply *r,int rc,int hint)
{r->status=rc==EFS_OK?EFS_INODE_RPC_OK:rc==EFS_ERR_NOT_FOUND?EFS_INODE_RPC_NOT_FOUND:rc==EFS_ERR_STALE?EFS_INODE_RPC_STALE:EFS_INODE_RPC_BUSY;
r->primary_id=hint>=0?(uint32_t)(hint+1):0;}
static int host_inode_rpc_peer(struct efs_raft_host *h,int rid,uint8_t t,const void *q,
 uint32_t n,uint8_t rt,void *out,uint32_t on)
{
 (void)h;(void)q;assert(rid==1 && n==sizeof(struct efs_msg_publication) && on==sizeof(struct efs_msg_publication_reply));
 assert((t==EFS_MSG_PUBLICATION && rt==EFS_MSG_PUBLICATION_REPLY) ||
 (t==EFS_MSG_PUBLICATION_STATUS && rt==EFS_MSG_PUBLICATION_STATUS_REPLY) ||
 (t==EFS_MSG_PUBLICATION_RETIRE && rt==EFS_MSG_PUBLICATION_RETIRE_REPLY));
 ((struct efs_msg_publication_reply *)out)->rpc.status=EFS_INODE_RPC_BUSY;forwards++;return 0;
}
int efs_meta_publication_result(struct efs_kv *kv,const struct efs_meta_pub *p,int *v)
{(void)kv;assert(p->ino==123 && p->inode_gen==456 && p->chunk_index==17 && p->publication_id.seq==77);if(!stored)return EFS_ERR_NOT_FOUND;*v=stored_verdict;return EFS_OK;}
int efs_meta_apply_publish(struct efs_kv *kv,const struct efs_meta_pub *p)
{(void)kv;assert(p->durable_result && p->lane_local && p->candidate_gen==789 &&
 p->inode_gen==456 && p->new_size==100 && p->publication_id.seq==77 &&
 p->publication_id.client_uuid[0]==1 && p->publication_id.session_epoch==1);return EFS_OK;}
int efs_meta_apply_publication_retire(struct efs_kv *kv,const struct efs_meta_pub *p)
{assert(efs_meta_apply_publish(kv,p)==EFS_OK);stored_verdict=EFS_META_PUBLICATION_RETIRED;return EFS_OK;}
int efs_meta_apply_publish_stale_why(void){return 0;}
''' + pack + '\n' + apply + r'''
static int host_propose_wait(struct efs_raft_host *h,uint8_t g,const uint8_t *cmd,uint32_t n,int *hint)
{(void)h;(void)hint;assert(g==group && (cmd[0]==EFS_MD_CMD_PUBLICATION || cmd[0]==EFS_MD_CMD_PUBLICATION_RETIRE) && n==HOST_PUBLICATION_LEN);proposals++;if(store_on_propose)assert(apply_one_publish(h,cmd,1)==EFS_OK);stored=store_on_propose;if(fence_after_propose)session_rc=EFS_ERR_STALE;return wait_rc;}
''' +fn+r'''
int main(void)
{
 struct efs_msg_publication req={0};req.rec.ino=123;req.rec.file_generation=456;req.rec.chunk_index=17;
 req.rec.chunk_generation=789;req.rec.publish_flags=3;req.size=100;req.id.client_uuid[0]=1;req.id.session_epoch=1;req.id.seq=77;
 group=efs_raft_shard_group(efs_kv_lane_shard(123,17));
 assert(group!=efs_raft_shard_group(efs_kv_inode_shard(123)));
 struct efs_msg_publication_reply out;
 server_raft_host_publication(&req,1,&out);
 assert(out.rpc.status==EFS_INODE_RPC_NOT_FOUND && out.state==EFS_PUBLICATION_UNKNOWN && !proposals);
 wait_rc=EFS_ERR_STALE;server_raft_host_publication(&req,0,&out);
 assert(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_COMMITTED && proposals==1);
 /* Apply-ring verdict lost: exact durable result wins, never re-propose. */
 server_raft_host_publication(&req,0,&out);assert(proposals==1 && out.state==EFS_PUBLICATION_COMMITTED);
 stored=0;stored_verdict=EFS_ERR_STALE;wait_rc=EFS_ERR_BUSY;
 server_raft_host_publication(&req,0,&out);assert(proposals==2 && out.state==EFS_PUBLICATION_REJECTED && out.verdict==EFS_ERR_STALE);
 stored=0;store_on_propose=0;server_raft_host_publication(&req,0,&out);
 assert(proposals==3 && out.rpc.status==EFS_INODE_RPC_NOT_FOUND && out.state==EFS_PUBLICATION_UNKNOWN);
 /* The missing outcome later appears: a status query recovers it without I/O. */
 stored=1;stored_verdict=EFS_OK;server_raft_host_publication(&req,1,&out);
 assert(proposals==3 && out.state==EFS_PUBLICATION_COMMITTED);
 read_rc=EFS_ERR_BUSY;server_raft_host_publication(&req,1,&out);
 assert(out.rpc.status==EFS_INODE_RPC_BUSY && out.state==EFS_PUBLICATION_UNKNOWN && proposals==3);
 owns=0;server_raft_host_publication(&req,1,&out);assert(forwards==1 && proposals==3);
 server_raft_host_publication(&req,2,&out);assert(forwards==2 && proposals==3);
 owns=1;read_rc=EFS_OK;stored=1;stored_verdict=EFS_OK;store_on_propose=0;
 server_raft_host_publication(&req,2,&out);assert(out.rpc.status==EFS_INODE_RPC_NOT_FOUND && out.state==EFS_PUBLICATION_UNKNOWN);
 stored=1;store_on_propose=1;
 server_raft_host_publication(&req,2,&out);assert(out.state==EFS_PUBLICATION_RETIRED && out.verdict==EFS_META_PUBLICATION_RETIRED);
 unsigned saved=proposals;server_raft_host_publication(&req,0,&out);assert(out.state==EFS_PUBLICATION_RETIRED && proposals==saved);
 /* Fenced clients cannot recover/submit/retire even existing receipts. */
 saved=proposals;session_rc=EFS_ERR_STALE;
 for(int mode=0;mode<3;mode++) {
  server_raft_host_publication(&req,mode,&out);
  assert(out.rpc.status==EFS_INODE_RPC_STALE && out.state==EFS_PUBLICATION_UNKNOWN && proposals==saved);
 }
 session_rc=EFS_ERR_BUSY;server_raft_host_publication(&req,0,&out);
 assert(out.rpc.status==EFS_INODE_RPC_BUSY && out.state==EFS_PUBLICATION_UNKNOWN && proposals==saved);
 session_rc=EFS_OK;stored=0;fence_after_propose=1;
 server_raft_host_publication(&req,0,&out);
 assert(out.rpc.status==EFS_INODE_RPC_STALE && out.state==EFS_PUBLICATION_UNKNOWN && proposals==saved+1 && gates);
 puts("publication host: lane-only authority, apply-ring loss, terminal recovery and unknown PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-publication-host-') as d:
 p=Path(d)/'test.c';p.write_text(source)
 cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include')]
 cmd+=shlex.split(os.environ.get('WRITER_RPC_TEST_CFLAGS',''))
 subprocess.run(cmd+[str(p),str(root/'src/kv/kv_key.c'),str(root/'src/common/publication.c'),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
