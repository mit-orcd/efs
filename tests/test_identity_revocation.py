#!/usr/bin/env python3
"""Execute authoritative revocation proof validation, including negative evidence."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/inode_rpc.c').read_text()
a=s.index('int efs_client_rpc_identity_revoked(')
b=s.index('static int publication_session_command(',a)
code=r'''
#include "client_internal.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <string.h>
static uint64_t salt;static int result;
static int session_command_reply(uint32_t shard,const uint8_t *q,uint32_t bytes,uint64_t *out,int mode) {
 assert(bytes==22 && mode==2 && q[0]==EFS_MD_CMD_SESSION && !q[1]);
 assert(shard==efs_kv_session_shard(q+2));
 assert(!memcmp(q+18,(uint8_t[4]){0},4));*out=salt;return result;
}
'''+s[a:b]+r'''
int main(void) {
 struct efs_opid id={.session_epoch=7,.seq=1};id.client_uuid[15]=33;int revoked=1;
 salt=8ull|((uint64_t)EFS_SESSION_ACTIVE<<32);
 assert(!efs_client_rpc_identity_revoked(&id,&revoked) && revoked);
 salt|=1ull<<40;assert(!efs_client_rpc_identity_revoked(&id,&revoked) && revoked);
 salt=8ull|((uint64_t)EFS_SESSION_FENCING<<32);
 assert(!efs_client_rpc_identity_revoked(&id,&revoked) && !revoked);
 salt=7ull|((uint64_t)EFS_SESSION_ACTIVE<<32);
 assert(!efs_client_rpc_identity_revoked(&id,&revoked) && !revoked);
 salt=6ull|((uint64_t)EFS_SESSION_ACTIVE<<32);
 assert(!efs_client_rpc_identity_revoked(&id,&revoked) && !revoked);
 salt|=1ull<<41;assert(efs_client_rpc_identity_revoked(&id,&revoked)==EFS_ERR_PROTO && !revoked);
 salt=(uint64_t)EFS_SESSION_ACTIVE<<32;
 assert(efs_client_rpc_identity_revoked(&id,&revoked)==EFS_ERR_PROTO && !revoked);
 result=EFS_ERR_NOT_FOUND;assert(efs_client_rpc_identity_revoked(&id,&revoked)==result && !revoked);
 result=EFS_ERR_NET;assert(efs_client_rpc_identity_revoked(&id,&revoked)==result && !revoked);
 memset(id.client_uuid,0,16);assert(efs_client_rpc_identity_revoked(&id,&revoked)==EFS_ERR_INVAL && !revoked);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p/'test.c'),str(root/'src/kv/kv_key.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('authoritative revocation proof: newer ACTIVE only; FENCING, missing, transport and malformed views remain unresolved PASS')
