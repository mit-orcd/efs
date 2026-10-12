#!/usr/bin/env python3
"""Exercise the production OPENDIR callback's metadata failure boundary."""
import os,re,subprocess,tempfile
from pathlib import Path
root=Path(os.environ.get('EFS_TEST_ROOT',Path(__file__).resolve().parents[1]))
s=Path(os.environ.get('EFS_FUSE_SOURCE',root/'src/client/efs_fuse.c')).read_text()
def function(name):
 m=re.search(r'^static (?:int|void) '+name+r'\([^;]+?\)\n\{',s,re.M)
 return s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include <efs/metadata.h>
#include <efs/wire.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
typedef uint64_t fuse_ino_t;
typedef void *fuse_req_t;
struct fuse_file_info {uint64_t fh;};
struct efs_dirh {uint64_t ino;};
static fuse_req_t t_req;
static int stat_rc,kind,enabled=1,is_dir=1,allocation_fail,error=-1,opens,allocations;
static int virt_kind(uint64_t ino){(void)ino;return kind;}
static int commands_enabled(void){return enabled;}
static int efs_client_stat_ino(uint64_t ino,struct efs_inode *row){row->ino=ino;row->mode=is_dir?S_IFDIR:S_IFREG;return stat_rc;}
static struct efs_dirh *dirh_new(uint64_t ino){if(allocation_fail)return NULL;struct efs_dirh *d=malloc(sizeof(*d));assert(d);d->ino=ino;allocations++;return d;}
static void dirh_free(struct efs_dirh *d){free(d);allocations--;}
static void fuse_reply_err(fuse_req_t req,int e){(void)req;error=e;assert(!t_req);}
static void fuse_reply_open(fuse_req_t req,struct fuse_file_info *fi){(void)req;(void)fi;opens++;assert(!t_req);}
static int readdir_rc,readdir_calls,collect_calls;
struct readdir_ent {char name[EFS_MAX_NAME];struct stat st;};
struct readdir_collect_arg {struct readdir_ent *ents;size_t count,cap;};
struct fuse_ctx {unsigned uid,gid;};
static struct {uint64_t export_id;} g_client;
static uint64_t virt_parent(uint64_t ino){return ino;}
static int virtual_access(uint64_t ino){(void)ino;return 0;}
static const struct fuse_ctx *ll_ctx(void){return NULL;}
static int check_access(const struct efs_inode *row,unsigned uid,unsigned gid,int mode){(void)row;(void)uid;(void)gid;(void)mode;return 0;}
static int readdir_collect_page(struct readdir_collect_arg *col,struct efs_inode *ents,unsigned n){(void)col;(void)ents;(void)n;collect_calls++;return 0;}
static int efs_client_rpc_readdir_cur(uint64_t export_id,uint64_t parent,struct efs_inode *ents,uint32_t *n,uint32_t *src,char *name,uint32_t *done){(void)export_id;(void)parent;(void)ents;(void)src;(void)name;readdir_calls++;*n=0;*done=1;return readdir_rc;}
'''+function('fuse_stat_errno')+function('ll_opendir')+function('efs_fuse_readdir_ino')+r'''
int main(void){
 assert(fuse_stat_errno(EFS_ERR_NOT_FOUND)==-ENOENT);
 int cases[][2]={{EFS_ERR_IO,EIO},{EFS_ERR_NET,EIO},{EFS_ERR_NO_QUORUM,EIO},{EFS_ERR_BUSY,EBUSY},{EFS_ERR_NOT_PRIMARY,EBUSY},{EFS_ERR_NOMEM,ENOMEM},{EFS_ERR_ACCES,EACCES},{EFS_ERR_NOT_FOUND,ENOENT}};
 struct fuse_file_info fi={.fh=999};
 for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++){stat_rc=cases[i][0];error=-1;ll_opendir((void*)1,42,&fi);assert(error==cases[i][1]);assert(fi.fh==999&&!opens&&!allocations);}
 stat_rc=0;is_dir=0;ll_opendir((void*)1,42,&fi);assert(error==ENOTDIR&&!allocations);
 is_dir=1;allocation_fail=1;ll_opendir((void*)1,42,&fi);assert(error==ENOMEM&&!allocations);
 allocation_fail=0;ll_opendir((void*)1,42,&fi);assert(opens==1&&allocations==1);dirh_free((struct efs_dirh*)(uintptr_t)fi.fh);
 kind=2;enabled=0;ll_opendir((void*)1,42,&fi);assert(error==ENOENT&&!allocations);
 kind=1;ll_opendir((void*)1,42,&fi);assert(error==ENOTDIR&&!allocations);
 kind=0;ll_opendir((void*)1,42,NULL);assert(opens==2&&!allocations);
 struct readdir_collect_arg col={0};stat_rc=0;is_dir=1;kind=0;
 for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++){
  readdir_rc=cases[i][0];assert(efs_fuse_readdir_ino(42,&col,NULL)==-cases[i][1]);
  assert(!collect_calls&&!col.count); /* no partial listing after refusal */
 }
 unsigned before=readdir_calls;stat_rc=EFS_ERR_BUSY;readdir_rc=0;
 assert(efs_fuse_readdir_ino(42,&col,NULL)==-EBUSY && (unsigned)readdir_calls==before);
 stat_rc=0;assert(!efs_fuse_readdir_ino(42,&col,NULL)&&collect_calls==1);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-opendir-authority-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.environ.get('EFS_TEST_SANITIZE')=='1' else []
 subprocess.run([os.environ.get('CC','cc'),*flags,'-D_GNU_SOURCE','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('OPENDIR/READDIR preserve authority errno, collect no refused page, and balance successful handles PASS')
