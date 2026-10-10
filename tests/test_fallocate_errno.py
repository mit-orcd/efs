#!/usr/bin/env python3
"""Actual FALLOCATE callback must reply with positive errno and preserve authority."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_FUSE_SOURCE',root/'src/client/efs_fuse.c')).read_text()
def function(name):
    m=re.search(r'^static (?:int|void) '+name+r'\([^;]+?\)\n\{',s,re.M)
    return s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include <efs/metadata.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
typedef uint64_t fuse_ino_t;
typedef void *fuse_req_t;
struct fuse_file_info {uint64_t fh;};
#define FALLOC_FL_KEEP_SIZE 1
static fuse_req_t t_req;
static int flush_rc,stat_rc,truncate_rc,mutations,stat_calls,depth,replied=-1;
static uint64_t row_size=100;
static int directory;
static int virt_kind(uint64_t ino){(void)ino;return 0;}
static void lookup_memo_mutation_begin(void){assert(!depth);depth++;}
static void lookup_memo_mutation_end(void){assert(depth==1);depth--;}
static int efs_append_flush_report(struct fuse_file_info *fi,uint64_t ino){(void)fi;(void)ino;return flush_rc;}
static int efs_client_stat_ino(uint64_t ino,struct efs_inode *row){stat_calls++;row->ino=ino;row->mode=directory?S_IFDIR:S_IFREG;row->size=row_size;return stat_rc;}
static int efs_fuse_truncate_ino(uint64_t ino,off_t size,struct fuse_file_info *fi){(void)ino;(void)size;(void)fi;mutations++;return truncate_rc;}
static void fuse_reply_err(fuse_req_t req,int err){(void)req;assert(err>=0);assert(!depth && !t_req);replied=err;}
'''+function('fuse_stat_errno')+function('efs_rc_to_errno')+function('ll_fallocate_run')+r'''
int main(void){
 struct fuse_file_info fi={0};fuse_req_t request=(void *)(uintptr_t)1;
 int cases[][2]={{EFS_ERR_QUOTA,ENOSPC},{EFS_ERR_IO,EIO},{EFS_ERR_NET,EIO},{EFS_ERR_BUSY,EBUSY},{EFS_ERR_AGAIN,EAGAIN},{EFS_ERR_NOMEM,ENOMEM},{EFS_ERR_ACCES,EACCES},{EFS_ERR_OVERFLOW,EOVERFLOW}};
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
  flush_rc=cases[i][0];ll_fallocate_run(request,42,0,0,200,&fi);assert(replied==cases[i][1]);assert(!mutations && !stat_calls);
 }
 flush_rc=0;
 int stat_cases[][2]={{EFS_ERR_IO,EIO},{EFS_ERR_NET,EIO},{EFS_ERR_NO_QUORUM,EIO},{EFS_ERR_BUSY,EBUSY},{EFS_ERR_NOT_PRIMARY,EBUSY},{EFS_ERR_NOMEM,ENOMEM},{EFS_ERR_ACCES,EACCES},{EFS_ERR_NOT_FOUND,ENOENT}};
 for(unsigned i=0;i<sizeof(stat_cases)/sizeof(stat_cases[0]);i++){
  stat_rc=stat_cases[i][0];ll_fallocate_run(request,42,0,0,200,&fi);assert(replied==stat_cases[i][1]);assert(!mutations);
 }
 stat_rc=0;ll_fallocate_run(request,42,0,0,50,&fi);assert(replied==0 && !mutations);
 ll_fallocate_run(request,42,FALLOC_FL_KEEP_SIZE,0,200,&fi);assert(replied==EOPNOTSUPP && !mutations);
 truncate_rc=-ENOSPC;ll_fallocate_run(request,42,0,0,200,&fi);assert(replied==ENOSPC && mutations==1);
 truncate_rc=0;ll_fallocate_run(request,42,0,0,200,&fi);assert(!replied && mutations==2);
 directory=1;ll_fallocate_run(request,42,0,0,200,&fi);assert(replied==EISDIR && mutations==2);directory=0;
 ll_fallocate_run(request,42,0,-1,200,&fi);assert(replied==EINVAL && mutations==2);
 ll_fallocate_run(request,42,2,0,200,&fi);assert(replied==EOPNOTSUPP && mutations==2);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-fallocate-errno-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('FALLOCATE: positive errno replies, flush/stat failure preserved, no mutation on refusal and balanced request cleanup PASS')
