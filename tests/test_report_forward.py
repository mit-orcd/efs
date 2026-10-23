#!/usr/bin/env python3
"""Exercise production REPORT forwarding with unequal delta chains."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
s = (root / 'src/server/raft_host.c').read_text()
a = s.index('static void host_fwd_report(')
b = s.index('\nstatic int pack_create_cmd(', a)
code = r'''
#include "efs/protocol.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
struct efs_raft_host { int n; };
static struct efs_chunk_rec expected[3];
static struct efs_ino_size_rec inode;
static unsigned calls; static struct efs_opid session; static int with_pubid;
static int host_pick_peer(struct efs_raft_host *h,const uint8_t *g,int n,int skip)
{(void)h;assert(g && n==1);return skip<0?1:2;}
static int host_inode_rpc_peer(struct efs_raft_host *h,int rid,uint8_t type,
 const void *buf,uint32_t len,uint8_t reply,void *out,size_t olen)
{
 (void)h;assert(type==EFS_MSG_REPORT_CHUNKS && reply==EFS_MSG_REPORT_CHUNKS_REPLY);
 assert(olen==sizeof(struct efs_msg_inode_reply));
 const struct efs_msg_report_chunks *hdr=buf;
 assert(hdr->count==3 && hdr->ino_count==1);
 struct efs_chunk_rec got[3];size_t used=0;
 assert(!efs_chunk_recs_unpack((const uint8_t *)buf+sizeof(*hdr),len-sizeof(*hdr),3,got,&used));
 for(unsigned i=0;i<3;i++)assert(!memcmp(&got[i],&expected[i],efs_chunk_rec_wire_size(&expected[i])));
 size_t pid=with_pubid?EFS_REPORT_PUBID_LEN:0;
 assert(len==sizeof(*hdr)+used+sizeof(inode)+pid+EFS_REPORT_SESSION_LEN);
 assert(hdr->sync==(EFS_REPORT_F_SYNC|EFS_REPORT_F_SESSION|(with_pubid?EFS_REPORT_F_PUBID:0)));
 if(with_pubid){struct efs_report_pubid p;assert(efs_report_pubid_unpack(&p,(const uint8_t*)buf+len-EFS_REPORT_SESSION_LEN-pid)&&p.mint_ns==9&&p.seq==3);}
 struct efs_opid id;assert(efs_report_session_unpack(&id,(const uint8_t*)buf+len-EFS_REPORT_SESSION_LEN));
 assert(id.session_epoch==session.session_epoch&&!memcmp(id.client_uuid,session.client_uuid,16));
 assert(!memcmp((const uint8_t *)buf+sizeof(*hdr)+used,&inode,sizeof(inode)));
 calls++;((struct efs_msg_inode_reply *)out)->status=rid==1?EFS_INODE_RPC_NOT_PRIMARY:EFS_INODE_RPC_OK;
 return 0;
}
''' + s[a:b] + r'''
int main(void)
{
 struct efs_raft_host h={2};struct efs_msg_inode_reply out;uint8_t groups[]={0};
 for(unsigned i=0;i<3;i++){expected[i].ino=100+i;expected[i].chunk_index=i;expected[i].delta_base_n=i;memset(expected[i].deltas,7+i,i*sizeof(expected[i].deltas[0]));}
 session.client_uuid[0]=7;session.session_epoch=42;session.seq=1;
 inode.ino=100;inode.size=123456;
 host_fwd_report(&h,expected,3,&inode,1,EFS_REPORT_F_SYNC|EFS_REPORT_F_SESSION,&session,NULL,&out,groups,1);
 assert(out.status==EFS_INODE_RPC_OK && calls==2);
 host_fwd_report(&h,expected,UINT32_MAX,&inode,1,EFS_REPORT_F_SYNC|EFS_REPORT_F_SESSION,&session,NULL,&out,groups,1);
 assert(out.status==EFS_INODE_RPC_INVAL && calls==2);
 host_fwd_report(&h,expected,3,NULL,1,EFS_REPORT_F_SYNC|EFS_REPORT_F_SESSION,&session,NULL,&out,groups,1);
 assert(out.status==EFS_INODE_RPC_INVAL && calls==2);
 /* W77: the publication id is forwarded verbatim before the session. */
 struct efs_report_pubid pubid={9,3};with_pubid=1;
 host_fwd_report(&h,expected,3,&inode,1,EFS_REPORT_F_SYNC|EFS_REPORT_F_SESSION|EFS_REPORT_F_PUBID,&session,&pubid,&out,groups,1);
 assert(out.status==EFS_INODE_RPC_OK && calls==4);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-report-forward-') as d:
    p = Path(d) / 'test.c'
    p.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I'+str(root/'include'), str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True, timeout=5)
print('REPORT forwarding: unequal chains, inode boundary, peer retry, impossible counts and publication id PASS')
