#!/usr/bin/env python3
"""Execute FUSE abandoned CREATE cleanup with ambiguous proof and lease edges."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/efs_fuse.c').read_text()
a=s.index('#define EFS_PENDING_CREATES_MAX');b=s.index('/* A lost last-close reply must not pin',a)
code=r'''
#include "client_internal.h"
#include "efs/session.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static pthread_mutex_t stripe=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *open_edge_mu(efs_ino_t ino){assert(ino==42);return &stripe;}
static int lock_busy;
static int wb_lock_budget(pthread_mutex_t *mu){if(lock_busy)return EFS_ERR_BUSY;return pthread_mutex_lock(mu)?EFS_ERR_IO:EFS_OK;}
static int release_rc,create_known,create_rc,lease_rc,begins,submits,cancels;
static int efs_open_release_pending(efs_ino_t ino){assert(ino==42);return release_rc;}
'''
# Forward declarations from the production header suffice for mock RPCs below.
code+=s[a:b]+r'''
int efs_client_create_cancel(struct efs_client_create_request *r){assert(!r->attempted);cancels++;memset(r,0,sizeof(*r));return EFS_OK;}
int efs_client_create_reset(struct efs_client_create_request *r){assert(r->complete);memset(r,0,sizeof(*r));return EFS_OK;}
int efs_client_create_resume(struct efs_client_create_request *r,int probe) {
 assert(!probe&&r->wire.q.id.seq==3);
 assert(!pthread_mutex_trylock(&g_pending_create_mu));pthread_mutex_unlock(&g_pending_create_mu);
 if(r->complete)return r->result;
 if(create_known){r->complete=1;r->result=create_rc;r->inode.ino=42;r->generation=99;}
 return create_rc;
}
int efs_client_lease_begin(struct efs_client_lease_request *r,efs_ino_t ino,uint64_t gen,unsigned flags,uint64_t token) {
 assert(!r->active&&ino==42&&gen==99&&flags==EFS_LEASE_EDGE_CLEAR_PENDING&&token==3);
 begins++;*r=(struct efs_client_lease_request){.ino=ino,.generation=gen,.flags=flags,.pending_token=token,.active=1};r->q.id.seq=88;return EFS_OK;
}
int efs_client_lease_resume(struct efs_client_lease_request *r,int probe) {
 assert(!probe&&r->active&&r->q.id.seq==88&&r->pending_token==3);submits++;
 assert(!pthread_mutex_trylock(&g_pending_create_mu));pthread_mutex_unlock(&g_pending_create_mu);
 if(!lease_rc){r->complete=1;r->result=0;}return lease_rc;
}
int efs_client_lease_reset(struct efs_client_lease_request *r){assert(!r->active || r->complete);memset(r,0,sizeof(*r));return EFS_OK;}
static struct efs_pending_create *unknown(void) {
 struct efs_pending_create *entry=efs_pending_create_alloc();assert(entry);
 entry->request.active=1;entry->request.attempted=1;entry->request.wire.q.id.seq=3;
 efs_pending_create_queue(entry);return entry;
}
int main(void) {
 struct efs_pending_create *all[EFS_PENDING_CREATES_MAX];
 for(unsigned i=0;i<EFS_PENDING_CREATES_MAX;i++){all[i]=efs_pending_create_alloc();assert(all[i]);}
 assert(!efs_pending_create_alloc()&&g_pending_create_count==EFS_PENDING_CREATES_MAX);
 assert(efs_pending_create_drain()==EFS_ERR_BUSY); /* local callers still own reserved entries */
 for(unsigned i=0;i<EFS_PENDING_CREATES_MAX;i++)efs_pending_create_queue(all[i]);
 for(unsigned i=0;i<EFS_PENDING_CREATES_MAX;i++)assert(efs_pending_create_drain()==EFS_ERR_BUSY);
 assert(!efs_pending_create_drain()&&cancels==EFS_PENDING_CREATES_MAX);
 struct efs_pending_create *entry=unknown();create_rc=EFS_ERR_NET;
 assert(efs_pending_create_drain()==EFS_ERR_NET&&g_pending_create_count==1&&!begins&&g_pending_creates==entry);
 create_known=1;create_rc=0;release_rc=EFS_ERR_BUSY;
 assert(efs_pending_create_drain()==EFS_ERR_BUSY&&!begins&&entry->request.complete);
 release_rc=0;lock_busy=1;
 assert(efs_pending_create_drain()==EFS_ERR_BUSY&&entry->request.complete&&!begins&&g_pending_creates==entry);
 lock_busy=0;lease_rc=EFS_ERR_NET;
 assert(efs_pending_create_drain()==EFS_ERR_NET&&begins==1&&entry->cleanup.active&&g_pending_create_count==1);
 lease_rc=0;assert(efs_pending_create_drain()==EFS_ERR_BUSY&&begins==1&&submits==2&&!g_pending_create_count);
 entry=unknown();create_rc=EFS_ERR_EXIST;
 assert(efs_pending_create_drain()==EFS_ERR_BUSY&&begins==1&&!g_pending_create_count);
 assert(!efs_pending_create_drain());
 puts("FUSE pending CREATE cleanup: bounded admission, unattempted cancel, unknown proof retention, old-close barrier, immutable token retry and negative proof PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-pending-create-cleanup-') as directory:
 p=Path(directory)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
