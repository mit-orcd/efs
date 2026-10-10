#!/usr/bin/env python3
"""Exercise the production OPENDIR callback's metadata failure boundary."""
import os,re,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_FUSE_SOURCE',root/'src/client/efs_fuse.c')).read_text()
def function(name):
 m=re.search(r'^static (?:int|void) '+name+r'\([^;]+?\)\n\{',s,re.M)
 return s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include <efs/metadata.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdlib.h>
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
'''+function('fuse_stat_errno')+function('ll_opendir')+r'''
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
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-opendir-authority-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('OPENDIR preserves authority errno, allocates no handle on failure, and balances successful handles PASS')
