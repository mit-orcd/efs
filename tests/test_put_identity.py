#!/usr/bin/env python3
"""Production PUT allocator: mount-wide unique identities under concurrency."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/inode_rpc.c').read_text();a=s.index('int efs_client_put_id(');fn=s[a:s.index('\n}',a)+2]
code=r'''
#include "client_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
struct efs_client g_client;
static pthread_mutex_t opid_mu=PTHREAD_MUTEX_INITIALIZER;
static unsigned seeded;
static void opid_seed_locked(void){if(!g_client.opid_next){seeded++;g_client.opid_next=1;g_client.opid_epoch=3;g_client.opid_uuid[0]=19;}}
'''+fn+r'''
static uint64_t seqs[8192];
static void *worker(void *arg){size_t offset=(size_t)arg;for(unsigned i=0;i<1024;i++){struct efs_opid id;assert(!efs_client_put_id(&id));assert(id.client_uuid[0]==19 && id.session_epoch==3);seqs[offset+i]=id.seq;}return NULL;}
static int compare(const void *a,const void *b){uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return x>y?1:x<y?-1:0;}
int main(void){pthread_t threads[8];for(size_t i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,worker,(void *)(i*1024)));for(unsigned i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));qsort(seqs,8192,sizeof(*seqs),compare);for(unsigned i=0;i<8192;i++)assert(seqs[i]==i+1);assert(seeded==1 && g_client.opid_next==1);g_client.put_next=UINT64_MAX-1;struct efs_opid id;assert(!efs_client_put_id(&id)&&id.seq==UINT64_MAX-1);assert(efs_client_put_id(&id)==EFS_ERR_BUSY && !id.seq && !id.session_epoch && !id.client_uuid[0]);assert(efs_client_put_id(&id)==EFS_ERR_BUSY && !id.seq);assert(efs_client_put_id(NULL)==EFS_ERR_INVAL);puts("PUT identities: 8192 concurrent mount-wide unique sequences, namespace ACK isolation and no-wrap exhaustion PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-put-id-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-pthread','-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
