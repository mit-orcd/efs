#!/usr/bin/env python3
"""Exercise the production defaults against the real libfuse parser."""
from pathlib import Path
import os,shlex,subprocess,tempfile,sys
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/efs_fuse.c').read_text()
a=s.index('static uint32_t fuse_io_limit(');limit=s[a:s.index('\n}',a)+2]
a=s.index('static int efs_fuse_parse_cmdline(');f=s[a:s.index('\n}',a)+2]
try:
 flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','fuse3'],text=True,stderr=subprocess.DEVNULL))
except (OSError,subprocess.CalledProcessError):
 print('real FUSE worker parser: unavailable on this platform');sys.exit(0)
code=r'''
#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#define EFS_WRITE_PIPELINE 8
static uint32_t fuse_chunk_size(void) {return 131072;}
'''+limit+'\n'+f+r'''
static void check(int argc,char **argv,unsigned max,unsigned idle){
 struct fuse_args a=FUSE_ARGS_INIT(argc,argv);struct fuse_cmdline_opts o;
 assert(!efs_fuse_parse_cmdline(&a,&o));
 assert(o.max_threads==max && o.max_idle_threads==idle);
 free(o.mountpoint);fuse_opt_free_args(&a);
}
int main(void){
 unsigned page=(unsigned)sysconf(_SC_PAGESIZE);
 unsigned limit=fuse_io_limit();
 assert(limit==1048576+page);
 for(unsigned off=0;off<page;off++)
   assert((1048576+off+page-1)/page <= (limit+page-1)/page);
 char *plain[]={"efs-fuse","/tmp"};check(2,plain,32,8);
 char *explicit[]={"efs-fuse","-o","max_threads=4,max_idle_threads=1","/tmp"};check(4,explicit,4,1);
 char *zero[]={"efs-fuse","-omax_threads=0,max_idle_threads=0","/tmp"};check(3,zero,0,0);
 char *help[]={"efs-fuse","--help"};check(2,help,32,8);
 puts("real FUSE parser: bounded defaults, explicit/zero precedence and help PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-fuse-options-') as d:
 p=Path(d)/'t.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror',str(p),'-o',str(binary)]+flags,check=True)
 subprocess.run([str(binary)],check=True)
