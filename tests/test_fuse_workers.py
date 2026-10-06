#!/usr/bin/env python3
"""Check production active-worker configuration, including allocation failure."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/efs_fuse.c').read_text()
m=re.search(r'^static struct fuse_loop_config \*efs_fuse_worker_config\([^;]+?\)\n\{',s,re.M)
f=s[m.start():s.index('\n}',m.end())+2]
source=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
struct fuse_cmdline_opts { unsigned max_threads,max_idle_threads,clone_fd; };
struct fuse_loop_config { unsigned max,idle,clone; };
static int fail;
static struct fuse_loop_config *fuse_loop_cfg_create(void) {
    return fail ? NULL : calloc(1,sizeof(struct fuse_loop_config));
}
static void fuse_loop_cfg_set_max_threads(struct fuse_loop_config *c,unsigned n) {c->max=n;}
static void fuse_loop_cfg_set_idle_threads(struct fuse_loop_config *c,unsigned n) {c->idle=n;}
static void fuse_loop_cfg_set_clone_fd(struct fuse_loop_config *c,unsigned n) {c->clone=n;}
'''+f+r'''
int main(void) {
    struct fuse_cmdline_opts o={0};
    struct fuse_loop_config *c=efs_fuse_worker_config(&o);
    assert(c&&c->max==32&&c->idle==8&&!c->clone);free(c);
    o=(struct fuse_cmdline_opts){4,64,1};c=efs_fuse_worker_config(&o);
    assert(c&&c->max==4&&c->idle==4&&c->clone);free(c);
    o=(struct fuse_cmdline_opts){128,16,0};c=efs_fuse_worker_config(&o);
    assert(c&&c->max==128&&c->idle==16);free(c);
    fail=1;assert(!efs_fuse_worker_config(&o));
    puts("FUSE workers: active bound, option handling, idle clamp and allocation failure PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-workers-') as directory:
    p=Path(directory)/'test.c';p.write_text(source)
    cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror']
    cmd+=shlex.split(os.environ.get('FUSE_WORKER_CFLAGS',''))
    subprocess.run(cmd+[str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
