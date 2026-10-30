#!/usr/bin/env python3
"""Production inode adoption keeps local EOF, never cached security/namespace.

Adapters model the table and RPC boundary. Live two-mount tests cover the full
permission path. A SOURCE file argument makes the same assertions a base control.
"""
from pathlib import Path
import argparse, os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--source',type=Path,default=root/'src/client/ops.c');ap.add_argument('--case',choices=('overlay','mutations','unlink','all'),default='all');args=ap.parse_args()
s=args.source.read_text()
def fn(signature):
    start=s.index(signature)
    return s[start:s.index('\n}',start)+2]
code=r'''
#include "efs/metadata.h"
#include "efs/protocol.h"
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
static struct {struct efs_export export;pthread_mutex_t idx_mu;unsigned export_id;int last_err;} g_client={.idx_mu=PTHREAD_MUTEX_INITIALIZER};
static struct efs_inode staged,reply;
static int present,dirty,pinned,rpc_rc,pulls,drops,calls;
static void efs_client_stage_touch(efs_ino_t ino){(void)ino;}
static void efs_client_lock_dir(efs_ino_t ino){(void)ino;}
static void efs_client_unlock_dir(efs_ino_t ino){(void)ino;}
int efs_export_get_inode(struct efs_export *e,efs_ino_t ino,struct efs_inode *out){(void)e;(void)ino;if(!present)return EFS_ERR_NOT_FOUND;*out=staged;return 0;}
int efs_export_upsert_inode(struct efs_export *e,const struct efs_inode *in){(void)e;staged=*in;present=1;return 0;}
static int efs_client_ino_has_unpublished_data(efs_ino_t ino){(void)ino;return dirty;}
static int efs_client_mtime_is_pinned(efs_ino_t ino){(void)ino;return pinned;}
static void pull_file_layout(const struct efs_inode *in){(void)in;pulls++;}
static void invalidate_file_layout(const struct efs_inode *in,int drop){(void)in;drops+=drop;}
static int efs_client_rpc_getattr(unsigned ex,efs_ino_t ino,struct efs_inode *out){(void)ex;(void)ino;calls++;*out=reply;return rpc_rc;}
int efs_client_stat_local(efs_ino_t ino,struct efs_inode *out);
int efs_client_stat_refresh(efs_ino_t ino,struct efs_inode *out);
void efs_client_adopt_lookup(const struct efs_inode *in,struct efs_inode *out);
static int mutations;
static int setattr_rpc_dual_apply(efs_ino_t ino,uint32_t mask,uint32_t mode,uid_t uid,gid_t gid,uint64_t size,uint64_t mt,uint32_t mn,uint64_t at,uint32_t an){(void)ino;(void)mask;(void)mode;(void)uid;(void)gid;(void)size;(void)mt;(void)mn;(void)at;(void)an;mutations++;return EFS_OK;}
static int efs_client_report_dirty_ino(efs_ino_t ino,int sync){(void)ino;(void)sync;return EFS_OK;}
static void now_ns(uint64_t *sec,uint32_t *ns){*sec=100;*ns=0;}
static uint32_t data_chunk_size(void){return 131072;}
void efs_export_drop_chunks_from(struct efs_export *e,efs_ino_t ino,uint32_t ci){(void)e;(void)ino;(void)ci;}
static void efs_rdcache_invalidate(efs_ino_t ino,uint32_t ci){(void)ino;(void)ci;}
static void efs_dcache_drop_if_clean(efs_ino_t ino,uint32_t ci){(void)ino;(void)ci;}
static int efs_client_rpc_setattr(unsigned ex,efs_ino_t ino,uint32_t mask,uint32_t mode,uid_t uid,gid_t gid,uint64_t size,uint64_t mt,uint32_t mn,uint64_t at,uint32_t an,struct efs_inode *out){(void)ex;(void)ino;(void)mask;(void)mode;(void)uid;(void)gid;(void)size;(void)mt;(void)mn;(void)at;(void)an;mutations++;*out=reply;return EFS_OK;}
static int efs_client_rpc_unlink(unsigned ex,efs_ino_t p,const char *name,bool directory){(void)ex;(void)p;(void)name;(void)directory;mutations++;return rpc_rc;}
static void efs_client_stage_pin(efs_ino_t ino){(void)ino;}
static void efs_client_stage_unpin(efs_ino_t ino){(void)ino;}
int efs_export_lookup(struct efs_export *e,efs_ino_t p,const char *name,struct efs_inode *out){(void)e;(void)p;(void)name;*out=staged;return EFS_OK;}
int efs_export_dir_empty(struct efs_export *e,efs_ino_t ino){(void)e;(void)ino;return 1;}
static int first_child_name_cb(struct efs_export *e,uint64_t slot,void *arg){(void)e;(void)slot;(void)arg;return 0;}
int efs_export_foreach_child(struct efs_export *e,efs_ino_t p,efs_child_cb cb,void *arg){(void)e;(void)p;(void)cb;(void)arg;return EFS_OK;}
static int kept_ghost;
int efs_export_unlink_name_ex(struct efs_export *e,efs_ino_t p,const char *name,int keep){(void)e;(void)p;(void)name;kept_ghost=keep;return EFS_OK;}
int efs_export_nlink_dec_ex(struct efs_export *e,efs_ino_t ino,struct efs_inode *out,int keep){(void)e;(void)ino;(void)out;(void)keep;return EFS_OK;}
'''+fn('int efs_client_unlink(')+(fn('static int local_inode(') if 'static int local_inode(' in s else '')+fn('int efs_client_chmod(')+fn('int efs_client_chown(')+fn('int efs_client_truncate(')+fn('static void adopt_rpc_inode(')+(fn('static int inode_mtime_older(') if 'static int inode_mtime_older(' in s else '')+fn('static void overlay_local_size(')+fn('void efs_client_adopt_lookup(')+fn('int efs_client_stat_local(')+fn('int efs_client_stat_open(')+fn('int efs_client_stat_refresh(')+r'''
static void reset(void){
 present=1;dirty=1;pinned=0;rpc_rc=pulls=drops=calls=0;
 staged=(struct efs_inode){.ino=42,.parent=1,.mode=S_IFREG|0777,.uid=11,.gid=12,.size=8192,.mtime=10,.nlink=1,.content_epoch=1};strcpy(staged.name,"old");
 reply=staged;reply.parent=2;strcpy(reply.name,"renamed");reply.mode=S_IFREG|0000;reply.uid=21;reply.gid=22;reply.nlink=2;reply.ctime=20;reply.ctime_nsec=8;reply.atime=30;reply.content_epoch=2;reply.size=0;reply.mtime=9;
}
static void authority(const struct efs_inode *out){
 assert(out->mode==reply.mode && out->uid==reply.uid && out->gid==reply.gid);
 assert(out->nlink==reply.nlink && out->parent==reply.parent && !strcmp(out->name,reply.name));
 assert(out->content_epoch==reply.content_epoch && out->ctime==reply.ctime && out->ctime_nsec==reply.ctime_nsec);
 assert(out->atime==reply.atime);
}
int main(int argc,char **argv){struct efs_inode out;assert(argc==2);
 if(!strcmp(argv[1],"unlink") || !strcmp(argv[1],"all")){
  reset();rpc_rc=EFS_ERR_BUSY;mutations=0;assert(efs_client_unlink(1,"replaced",true)==EFS_ERR_BUSY && mutations==1);
  reset();mutations=kept_ghost=0;assert(!efs_client_unlink(1,"replaced",true) && mutations==1 && kept_ghost);
  if(!strcmp(argv[1],"unlink")){puts("cached type never rejects namespace authority; old file ghost retained PASS");return 0;}
 }
 if(strcmp(argv[1],"overlay")){
  reset();mutations=0;assert(!efs_client_chmod(42,staged.mode));assert(mutations==1);
  reset();mutations=0;assert(!efs_client_chown(42,staged.uid,staged.gid));assert(mutations==1);
  reset();mutations=0;assert(!efs_client_truncate(42,staged.size));assert(mutations==1);
  reset();present=0;mutations=0;assert(!efs_client_truncate(42,0) && mutations==1);
  if(!strcmp(argv[1],"mutations")){puts("same cached attributes never suppress authoritative mutations PASS");return 0;}
 }
 reset();efs_client_adopt_lookup(&reply,&out);authority(&out);assert(out.size==8192 && !drops);
 reset();assert(!efs_client_stat_refresh(42,&out));authority(&out);assert(out.size==8192 && calls==1);
 reset();assert(!efs_client_stat_open(42,&out));authority(&out);assert(out.size==8192 && calls==1 && !drops);
 reset();dirty=0;reply.mtime=11;assert(!efs_client_stat_open(42,&out));assert(out.size==0 && staged.size==0 && drops==1);
 /* Open-handle data remains available on an unavailable authority; it is
  * explicitly NOT an authorization row. Fresh refresh must report errors. */
 int errors[]={EFS_ERR_BUSY,EFS_ERR_IO,EFS_ERR_NET,EFS_ERR_ACCES};
 for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++){
  reset();rpc_rc=errors[i];assert(!efs_client_stat_open(42,&out)&&out.size==8192);
  assert(efs_client_stat_refresh(42,&out)==rpc_rc);
  present=0;assert(efs_client_stat_open(42,&out)==rpc_rc);
 }
 reset();rpc_rc=EFS_ERR_NOT_FOUND;assert(!efs_client_stat_open(42,&out)&&out.nlink==0&&out.size==8192);
 reset();present=dirty=0;assert(!efs_client_stat_refresh(42,&out));authority(&out);assert(out.size==0);
 /* A late response can represent its earlier capture point. It cannot
  * authorize a later operation through the cache: that operation must fetch
  * its own authority. This runs actual adoption/refresh wrappers, not a model. */
 reset();struct efs_inode latest=reply,late=reply;late.mode=S_IFREG|0777;late.uid=11;late.gid=12;late.parent=1;strcpy(late.name,"old");late.content_epoch=1;
 efs_client_adopt_lookup(&latest,&out);efs_client_adopt_lookup(&late,&out);
 reply=latest;assert(!efs_client_stat_refresh(42,&out));authority(&out);assert(out.size==8192);
 rpc_rc=EFS_ERR_NET;assert(efs_client_stat_refresh(42,&out)==EFS_ERR_NET);
 reset();dirty=0;efs_client_adopt_lookup(&reply,&out);authority(&out);assert(out.size==0 && drops==1);
 /* Once no local data owns EOF, a newer peer shrink is adopted. */
 reset();dirty=0;reply.mtime=11;efs_client_adopt_lookup(&reply,&out);authority(&out);assert(out.size==0&&drops==1);
 puts("client metadata authority: dirty EOF isolated from fresh DAC/names/epoch; open-ghost and strict RPC failures PASS");
}
'''
d=Path(tempfile.mkdtemp(prefix='efs-bb-authority-'))
try:
    c=d/'test.c';c.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(c),'-pthread','-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test'),args.case],check=True,timeout=10)
except BaseException:
    print('kept BB authority failure',d)
    raise
else:
    import shutil
    shutil.rmtree(d)
