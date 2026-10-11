#!/usr/bin/env python3
"""Actual storage coordinator entry: fresh authority and durable ACK ordering."""
from pathlib import Path
import subprocess,tempfile,os,shlex
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/packed_upload.c').read_text();a=s.index('int server_packed_gc(')
code=r'''
#include "efs/packed_upload.h"
#include "efs/meta_cmd.h"
#include "efs/protocol.h"
#include <assert.h>
#include <stdio.h>
struct efsd_server {unsigned id;};
static struct efs_pack_object expected;
static int auth_rc,fence_rc,delete_rc,fences,deletes;
static uint32_t phase,acks;
int efs_pack_object_decode(const uint8_t *b,size_t n,struct efs_pack_object *o){(void)b;if(n!=1)return EFS_ERR_PROTO;*o=expected;return 0;}
void server_raft_host_submit(const uint8_t *q,uint32_t n,struct efs_msg_raft_mkfs_reply *r){assert(n==3 && q[0]==2 && q[1]==EFS_MD_REQ_PACK_GC_VIEW);*r=(struct efs_msg_raft_mkfs_reply){.rc=auth_rc,.salt=phase|((uint64_t)acks<<32)};}
int server_fence_packed_object(struct efsd_server *s,uint32_t e,uint64_t id,uint64_t g){assert(s->id && e==1 && id==31 && g==41);fences++;return fence_rc;}
int server_delete_packed_fragment(struct efsd_server *s,uint32_t e,uint64_t id,uint64_t g,uint32_t fi,uint32_t len,const uint8_t *sum){assert(fences && !fence_rc && acks==15 && phase>=EFS_PACK_RECLAIMING);assert(e==1 && id==31 && g==41 && fi<3 && expected.nodes[fi]==s->id && len==4096 && !memcmp(sum,expected.checksums[fi],EFS_HASH_SIZE));deletes++;return delete_rc;}
''' + s[a:] + r'''
int main(void){
 struct efsd_server s={.id=1};uint8_t req[]={0,0};
 expected.ledger=(struct efs_pack_ledger){.publication_group=2,.segment_id=31,.segment_generation=41,.member_mask=15,.geometry=8192};
 expected.nodes[0]=1;expected.nodes[1]=2;expected.nodes[2]=3;
 phase=EFS_PACK_ACTIVE;assert(server_packed_gc(&s,req,2)==EFS_ERR_BUSY && !fences && !deletes);
 phase=EFS_PACK_RECLAIMING;auth_rc=EFS_ERR_IO;assert(server_packed_gc(&s,req,2)==EFS_ERR_IO && !fences);
 auth_rc=0;fence_rc=EFS_ERR_IO;assert(server_packed_gc(&s,req,2)==EFS_ERR_IO && fences==1 && !deletes);
 fence_rc=0;assert(!server_packed_gc(&s,req,2) && fences==2 && !deletes);
 req[0]=1;acks=7;assert(server_packed_gc(&s,req,2)==EFS_ERR_BUSY && fences==2 && !deletes);
 acks=15;delete_rc=EFS_ERR_IO;assert(server_packed_gc(&s,req,2)==EFS_ERR_IO && deletes==1);
 delete_rc=0;assert(!server_packed_gc(&s,req,2) && deletes==2);
 assert(!server_packed_gc(&s,req,2) && deletes==3); /* lost ACK retry */
 s.id=4;assert(!server_packed_gc(&s,req,2) && deletes==3); /* captured non-placement member */
 s.id=5;assert(server_packed_gc(&s,req,2)==EFS_ERR_INVAL && deletes==3);
 puts("packed GC storage: fresh exact authority, all-member fencing, hard failures, deletion replay and non-placement fence PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-packed-gc-') as td:
 p=Path(td)/'test.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c99','-D_DEFAULT_SOURCE','-D_DARWIN_C_SOURCE','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
