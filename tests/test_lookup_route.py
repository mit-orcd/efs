#!/usr/bin/env python3
"""Placement hints select a route; only fresh RPC replies supply lookup results."""
from pathlib import Path
import os, shlex, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/client/inode_rpc.c').read_text()
start = source.index('int efs_client_rpc_lookup(')
end = source.index('\nint efs_client_rpc_create(', start)
code = r'''
#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/raft.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <string.h>
static struct { unsigned node_count; } g_client = {4};
static int local_rc, wire_rc, status, owner_calls, dual_calls, local_calls, learned;
static efs_ino_t cached_ino, reply_ino;
static int efs_client_lookup_local(efs_ino_t p,const char *name,struct efs_inode *out)
{ assert(p==1 && !strcmp(name,"child"));local_calls++;out->ino=cached_ino;return local_rc; }
static int respond(uint8_t type,const void *req,uint32_t len,uint8_t expect,void *reply,uint32_t size)
{
 const struct efs_msg_inode_lookup *q=req;
 assert(type==EFS_MSG_INODE_LOOKUP && expect==EFS_MSG_INODE_LOOKUP_REPLY);
 assert(len==sizeof(*q) && size==sizeof(struct efs_msg_inode_reply));
 assert(q->parent==1 && !strcmp(q->name,"child"));
 struct efs_msg_inode_reply *r=reply;memset(r,0,sizeof(*r));r->status=status;r->inode.ino=reply_ino;
 return wire_rc;
}
static int rpc_send_recv_owner(efs_ino_t p,uint8_t t,const void *q,uint32_t n,uint8_t e,void *r,uint32_t z)
{assert(p==1);owner_calls++;return respond(t,q,n,e,r,z);}
static int rpc_send_recv_dual(uint8_t t,const void *q,uint32_t n,uint8_t e,void *r,uint32_t z,int ms)
{assert(ms==0);dual_calls++;return respond(t,q,n,e,r,z);}
static int rpc_status_to_efs(int s){return s==EFS_INODE_RPC_NOT_FOUND?EFS_ERR_NOT_FOUND:EFS_ERR_BUSY;}
static void rpc_learn_leader(const struct efs_msg_inode_reply *r){assert(r->inode.ino==reply_ino);learned++;}
''' + source[start:end] + r'''
static void reset(void)
{local_rc=wire_rc=0;status=EFS_INODE_RPC_OK;owner_calls=dual_calls=local_calls=learned=0;reply_ino=77;g_client.node_count=4;}
int main(void)
{
 struct efs_inode out;
 reset();cached_ino=2;assert(efs_client_rpc_lookup(1,1,"child",&out)==0);
 assert(dual_calls==1 && !owner_calls && out.ino==77 && learned==1); /* stale binding never returned */
 reset();cached_ino=3;assert(efs_client_rpc_lookup(1,1,"child",&out)==0 && owner_calls==1 && !dual_calls);
 reset();cached_ino=2;local_rc=EFS_ERR_NOT_FOUND;assert(efs_client_rpc_lookup(1,1,"child",&out)==0 && owner_calls==1);
 reset();g_client.node_count=3;cached_ino=2;assert(efs_client_rpc_lookup(1,1,"child",&out)==0 && owner_calls==1 && !local_calls);
 reset();cached_ino=2;status=EFS_INODE_RPC_NOT_FOUND;out.ino=99;
 assert(efs_client_rpc_lookup(1,1,"child",&out)==EFS_ERR_NOT_FOUND && out.ino==99 && !learned);
 reset();cached_ino=2;wire_rc=EFS_ERR_NET;out.ino=99;
 assert(efs_client_rpc_lookup(1,1,"child",&out)==EFS_ERR_NET && out.ino==99 && !learned);
 reset();cached_ino=2;status=EFS_INODE_RPC_BUSY;
 assert(efs_client_rpc_lookup(1,1,"child",&out)==EFS_ERR_BUSY && !learned);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-lookup-route-') as tmp:
    path = Path(tmp) / 'test.c'
    path.write_text(code)
    command = shlex.split(os.environ.get('CC', 'cc'))
    subprocess.run(command + ['-std=gnu11', '-Wall', '-Wextra', '-Werror',
                             '-I' + str(root / 'include'), str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True)
print('LOOKUP routing: stale/missing hints, fresh verdict, errors and three-node fallback PASS')
