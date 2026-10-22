#!/usr/bin/env python3
"""Execute actual FUSE rename dispatch; unsupported flags must never mutate names."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_FUSE_SOURCE',root/'src/client/efs_fuse.c')).read_text()
m=re.search(r'^static int efs_fuse_rename_at\([^;]+?\)\n\{',s,re.M)
function=s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include <efs/metadata.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
typedef uint64_t fuse_ino_t;
static struct {uint64_t export_id;} g_client={1};
static int calls,renames,rename_rc,stat_rc,lookup_rc,wx_rc;
static int virt_kind(fuse_ino_t ino){(void)ino;return 0;}
static int fuse_stat_errno(int rc){return rc==EFS_ERR_NOT_FOUND?-ENOENT:-EIO;}
static int efs_rc_to_errno(int rc){return -rc;}
static int efs_client_stat_ino(uint64_t ino,struct efs_inode *row){calls++;row->ino=ino;return stat_rc;}
static int check_dir_wx(const struct efs_inode *row){(void)row;calls++;return wx_rc;}
static int efs_client_rpc_lookup(uint64_t export_id,uint64_t parent,const char *name,struct efs_inode *row){(void)export_id;(void)parent;(void)name;calls++;row->ino=3001;return lookup_rc;}
static int efs_client_rename_at(uint64_t ino,uint64_t parent,const char *name,uint64_t newparent,const char *newname){(void)ino;(void)parent;(void)name;(void)newparent;(void)newname;calls++;renames++;return rename_rc;}
'''+function+r'''
int main(void){
 const unsigned flags[]={1,2,4,3,7,8,UINT32_MAX};
 for(unsigned i=0;i<sizeof(flags)/sizeof(flags[0]);i++){
  efs_ino_t out=999;
  assert(efs_fuse_rename_at(1001,"source",1002,"target",flags[i],&out)==-EOPNOTSUPP);
  assert(!calls && !renames);assert(out==999);
 }
 efs_ino_t out=999;
 assert(!efs_fuse_rename_at(1001,"source",1002,"target",0,&out));assert(renames==1);assert(out==3001);
 rename_rc=EACCES;out=999;assert(efs_fuse_rename_at(1001,"source",1002,"target",0,&out)==-EACCES);assert(out==999);
 rename_rc=0;wx_rc=-EACCES;int before=renames;assert(efs_fuse_rename_at(1001,"source",1002,"target",0,&out)==-EACCES);assert(renames==before);
 wx_rc=0;lookup_rc=EFS_ERR_NOT_FOUND;assert(efs_fuse_rename_at(1001,"source",1002,"target",0,&out)==-ENOENT);assert(renames==before);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-rename-flags-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('Rename flags: unsupported flags refused before any mutation; ordinary success/errors preserved PASS')
