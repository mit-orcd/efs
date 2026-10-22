#!/usr/bin/env python3
"""Actual UNLINK/RMDIR callbacks preserve refusal of the namespace envelope."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_FUSE_SOURCE',root/'src/client/efs_fuse.c')).read_text()
def function(name):
    m=re.search(r'^static int '+name+r'\([^;]+?\)\n\{',s,re.M)
    return s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include <efs/metadata.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <time.h>
typedef uint64_t fuse_ino_t;
static int status,stat_status,permission,mutations,sleeps;
static int efs_client_stat_ino(uint64_t ino,struct efs_inode *row){row->ino=ino;return stat_status;}
static int check_dir_wx(const struct efs_inode *row){(void)row;return permission;}
static int efs_client_unlink(uint64_t ino,const char *name,bool directory){(void)ino;(void)name;(void)directory;mutations++;return status;}
static int fixture_sleep(const struct timespec *delay,struct timespec *remaining){(void)delay;(void)remaining;sleeps++;return 0;}
#define nanosleep fixture_sleep
'''+function('fuse_stat_errno')+function('fuse_unlink_errno')+function('efs_fuse_unlink_at')+function('efs_fuse_rmdir_at')+r'''
int main(void){
 int cases[][2]={{EFS_ERR_OVERFLOW,-EOVERFLOW},{EFS_ERR_ACCES,-EACCES},{EFS_ERR_NOMEM,-ENOMEM},{EFS_ERR_NOT_PRIMARY,-EBUSY},{EFS_ERR_BUSY,-EBUSY},{EFS_ERR_AGAIN,-EBUSY},{EFS_ERR_NOT_FOUND,-ENOENT},{EFS_ERR_INVAL,-EINVAL},{EFS_ERR_IO,-EIO},{EFS_ERR_NET,-EIO},{EFS_ERR_NO_QUORUM,-EIO},{EFS_OK,0}};
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
  status=cases[i][0];int before=mutations;assert(efs_fuse_unlink_at(42,"entry")==cases[i][1]);assert(mutations==before+1);
  before=mutations;assert(efs_fuse_rmdir_at(42,"entry")==cases[i][1]);assert(mutations==before+1 && !sleeps);
 }
 status=EFS_ERR_NOT_EMPTY;int before=mutations;assert(efs_fuse_rmdir_at(42,"entry")==-ENOTEMPTY);assert(mutations==before+20 && sleeps==20);
 stat_status=EFS_ERR_NET;before=mutations;assert(efs_fuse_unlink_at(42,"entry")==-EIO);assert(efs_fuse_rmdir_at(42,"entry")==-EIO);assert(mutations==before);
 stat_status=0;permission=-EACCES;assert(efs_fuse_unlink_at(42,"entry")==-EACCES);assert(efs_fuse_rmdir_at(42,"entry")==-EACCES);assert(mutations==before);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-unlink-authority-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('UNLINK/RMDIR: namespace overflow, permission, memory and authority refusal remain distinct; bounded emptiness retry PASS')
