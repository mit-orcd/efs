#!/usr/bin/env python3
"""Execute all-storage acknowledgements, including offline and changing rosters."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_storage_fence_members(');b=s.index('\n}',a)+2
code=r'''
#include "src/server/server_internal.h"
#include "efs/session.h"
#include <assert.h>
#include <string.h>
struct efs_raft_host {struct efsd_server *s;};
static int calls,local_calls,bad_uuid,bad_floor,fail_id,change_members,mode;
static struct efsd_server server;
void server_raft_host_storage_session_fence(const struct efs_msg_storage_session_fence *q,struct efs_msg_storage_session_fence_reply *r) {
 assert(q->uuid[0]==17&&q->floor==2&&q->check_only==(unsigned)mode);
 local_calls++;memcpy(r->uuid,q->uuid,16);r->floor=q->floor;r->rc=0;
}
static int host_inode_rpc_peer(struct efs_raft_host *h,int rid,uint8_t type,const void *body,uint32_t bytes,uint8_t expected,void *out,uint32_t out_bytes) {
 assert(h->s==&server&&rid>=1&&rid<=3&&type==EFS_MSG_STORAGE_SESSION_FENCE&&expected==EFS_MSG_STORAGE_SESSION_FENCE_REPLY);
 assert(bytes==sizeof(struct efs_msg_storage_session_fence)&&out_bytes==sizeof(struct efs_msg_storage_session_fence_reply));
 const struct efs_msg_storage_session_fence *q=body;struct efs_msg_storage_session_fence_reply *r=out;
 assert(q->uuid[0]==17&&q->floor==2&&q->check_only==(unsigned)mode);calls++;
 if(fail_id==rid+1)return -1;
 memcpy(r->uuid,q->uuid,16);r->floor=q->floor;r->rc=0;
 if(bad_uuid)r->uuid[0]^=1;
 if(bad_floor)r->floor++;
 if(change_members&&rid==3)h->s->node_count=3;
 return 0;
}
'''+s[a:b]+r'''
int main(void) {
 pthread_mutex_init(&server.lock,NULL);server.id=1;server.node_count=4;
 for(unsigned i=0;i<4;i++)server.nodes[i].id=i+1;
 struct efs_raft_host host={&server};uint8_t uuid[16]={17};
 assert(!host_storage_fence_members(&host,uuid,2,0)&&calls==3&&local_calls==1);
 calls=local_calls=0;mode=1;
 assert(!host_storage_fence_members(&host,uuid,2,1)&&calls==3&&local_calls==1);
 /* Never infer an unavailable member's seal from the other three. */
 fail_id=4;assert(host_storage_fence_members(&host,uuid,2,1)==EFS_ERR_BUSY);fail_id=0;
 bad_uuid=1;assert(host_storage_fence_members(&host,uuid,2,1)==EFS_ERR_PROTO);bad_uuid=0;
 bad_floor=1;assert(host_storage_fence_members(&host,uuid,2,1)==EFS_ERR_PROTO);bad_floor=0;
 change_members=1;assert(host_storage_fence_members(&host,uuid,2,1)==EFS_ERR_BUSY);
 server.node_count=4;change_members=0;server.nodes[3].id=3;
 assert(host_storage_fence_members(&host,uuid,2,1)==EFS_ERR_INVAL);
 server.node_count=2;assert(host_storage_fence_members(&host,uuid,2,1)==EFS_ERR_BUSY);
}
'''
with tempfile.TemporaryDirectory(prefix='efs-storage-members-') as directory:
 p=Path(directory)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root),'-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
print('storage revocation: every member, exact UUID/floor, offline refusal, changed roster and duplicate identity rejection PASS')
