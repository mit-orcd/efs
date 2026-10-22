#!/usr/bin/env python3
"""Exercise the production defaults against the real libfuse parser."""
from pathlib import Path
import os,shlex,subprocess,tempfile,sys
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/efs_fuse.c').read_text()
a=s.index('static int efs_fuse_parse_cmdline(');f=s[a:s.index('\n}',a)+2]
try:
 flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','fuse3'],text=True,stderr=subprocess.DEVNULL))
except (OSError,subprocess.CalledProcessError):
 print('NOTRUN: real FUSE worker parser: fuse3 pkg-config unavailable on this platform');sys.exit(0)
code=r'''
#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
'''+f+r'''
static void check(int argc,char **argv,unsigned max,unsigned idle){
 struct fuse_args a=FUSE_ARGS_INIT(argc,argv);struct fuse_cmdline_opts o;
 assert(!efs_fuse_parse_cmdline(&a,&o));
 assert(o.max_threads==max && o.max_idle_threads==idle);
 struct fuse_lowlevel_ops ops={0};
 struct fuse_session *se=fuse_session_new(&a,&ops,sizeof(ops),NULL);
 assert(se);fuse_session_destroy(se);
 free(o.mountpoint);fuse_opt_free_args(&a);
}
int main(void){
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
