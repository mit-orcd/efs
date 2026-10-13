#!/usr/bin/env python3
"""Actual client registration cache: immutable identity, failures never cache."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/inode_rpc.c').read_text();a=s.index('int efs_client_session_admit(');f=s[a:s.index('\n}',a)+2]
c=r'''
#include "efs/kv_key.h"
#include "efs/opid.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
static struct {uint8_t append_session_uuid[16];uint32_t append_session_epoch;uint8_t append_session_shards[512];} g_client;
static pthread_mutex_t opid_mu=PTHREAD_MUTEX_INITIALIZER;
static unsigned calls;static int result;
static struct efs_opid last;
static int efs_client_rpc_publication_session(uint32_t shard,const struct efs_opid *id)
{assert(shard==1 || shard==2);calls++;last=*id;return result;}
'''+f+r'''
int main(void) {
 struct efs_opid_req q={.id={.client_uuid={42},.session_epoch=1,.seq=1}};
 result=EFS_ERR_IO;assert(efs_client_session_admit(1,&q.id)==EFS_ERR_IO && calls==1 && !g_client.append_session_epoch);
 assert(!memcmp(last.client_uuid,q.id.client_uuid,16) && last.session_epoch==1 && last.seq==1);
 result=EFS_OK;assert(efs_client_session_admit(1,&q.id)==EFS_OK && calls==2);
 q.id.seq=2;assert(efs_client_session_admit(1,&q.id)==EFS_OK && calls==2);
 assert(efs_client_session_admit(2,&q.id)==EFS_OK && calls==3);
 q.id.session_epoch=2;result=EFS_ERR_STALE;
 assert(efs_client_session_admit(1,&q.id)==EFS_ERR_STALE && calls==4 && g_client.append_session_epoch==1);
 result=EFS_OK;assert(efs_client_session_admit(1,&q.id)==EFS_OK && calls==5);
 assert(g_client.append_session_shards[0]==2);
 q.id.client_uuid[0]=43;assert(efs_client_session_admit(2,&q.id)==EFS_OK && calls==6 && g_client.append_session_shards[0]==4);
 unsigned before=calls;
 assert(efs_client_session_admit(4096,&q.id)==EFS_ERR_INVAL && calls==before);
 assert(efs_client_session_admit(1,NULL)==EFS_ERR_INVAL && calls==before);
 q.id.session_epoch=0;assert(efs_client_session_admit(1,&q.id)==EFS_ERR_INVAL && calls==before);
 q.id.session_epoch=1;memset(q.id.client_uuid,0,16);
 assert(efs_client_session_admit(1,&q.id)==EFS_ERR_INVAL && calls==before);
 puts("append session: failed registration retained, immutable UUID/epoch, cached successful shard admission and identity reset PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-append-session-') as t:
 p=Path(t);(p/'test.c').write_text(c)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p/'test.c'),'-pthread','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
