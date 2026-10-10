#!/usr/bin/env python3
"""Execute FUSE DAC checks across permission classes and privileged execute access."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=Path(os.environ.get('EFS_FUSE_SOURCE',root/'src/client/efs_fuse.c')).read_text()
m=re.search(r'^static int check_access\([^;]+?\)\n\{',source,re.M)
function=source[m.start():source.index('\n}',m.end())+2]
code=r'''
#include <efs/metadata.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
static int supplementary;
static int caller_in_group(gid_t gid){return supplementary && gid==2002;}
'''+function+r'''
int main(void){
 struct efs_inode row={0};row.uid=1001;row.gid=2002;
 /* Exhaust every permission value and requested mask for all DAC classes. */
 for(unsigned mode=0;mode<512;mode++){
  row.mode=S_IFREG|mode;
  for(unsigned mask=0;mask<8;mask++){
   supplementary=0;
   unsigned owner=(mode>>6)&7,group=(mode>>3)&7,other=mode&7;
   assert(check_access(&row,1001,9001,mask)==(((owner&mask)==mask)?0:-EACCES));
   assert(check_access(&row,3001,2002,mask)==(((group&mask)==mask)?0:-EACCES));
   assert(check_access(&row,3001,9001,mask)==(((other&mask)==mask)?0:-EACCES));
   supplementary=1;
   assert(check_access(&row,3001,9001,mask)==(((group&mask)==mask)?0:-EACCES));
   /* Owner class does not fall through to a more permissive group/other. */
   assert(check_access(&row,1001,2002,mask)==(((owner&mask)==mask)?0:-EACCES));
   assert(check_access(&row,0,9001,mask)==(((mask&X_OK) && !(mode&0111))?-EACCES:0));
   row.mode=S_IFDIR|mode;assert(!check_access(&row,0,9001,mask));row.mode=S_IFREG|mode;
  }
 }
 row.mode=S_IFREG|S_ISUID|S_ISGID;assert(check_access(&row,0,0,X_OK)==-EACCES);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-access-permissions-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    flags=shlex.split(os.environ.get('ACCESS_TEST_CFLAGS',''))
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-O2','-Wall','-Wextra','-Werror']+flags+['-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('Access permissions: all 512 modes, all masks, owner/primary/supplementary/other/root classes PASS')
