#!/usr/bin/env python3
"""Actual inode adoption/lookup blocks retain EOF after an inode-only REPORT.

Staging and pending-body ownership are adapters; live O_EXCL qualification
separately exercises the complete mounted publication path.
"""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];ref=os.environ.get('EFS_PENDING_EOF_SOURCE_REF')
s=subprocess.check_output(['git','show',ref+':src/client/ops.c'],cwd=root,text=True) if ref else (root/'src/client/ops.c').read_text()
def fn(signature):
    a=s.index(signature);return s[a:s.index('\n}',a)+2]
w=(root/'src/client/write.c').read_text();i=w.index('int efs_client_ino_has_unpublished_data(');ownership=w[i:w.index('\n}',i)+2]
code=r'''
#include "efs/metadata.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
static struct {struct efs_export export;pthread_mutex_t idx_mu,dirty_mu;} g_client={.idx_mu=PTHREAD_MUTEX_INITIALIZER,.dirty_mu=PTHREAD_MUTEX_INITIALIZER};
static struct efs_inode staged;
static int dirty,pinned,drops;
static void efs_client_stage_touch(efs_ino_t ino){assert(ino==42);}
static void efs_client_lock_dir(efs_ino_t ino){assert(ino==42 || ino==1);}
static void efs_client_unlock_dir(efs_ino_t ino){(void)ino;}
int efs_export_get_inode(struct efs_export*e,efs_ino_t ino,struct efs_inode*out){(void)e;assert(ino==42);*out=staged;return 0;}
int efs_export_upsert_inode(struct efs_export*e,const struct efs_inode*r){(void)e;staged=*r;return 0;}
static int __attribute__((unused)) efs_client_ino_is_dirty(efs_ino_t ino){assert(ino==42);return dirty;}
static int efs_client_ino_is_dirty_locked(efs_ino_t ino){assert(ino==42);return dirty;}
static int efs_dcache_ino_pinned(efs_ino_t ino){assert(ino==42);assert(pthread_mutex_trylock(&g_client.dirty_mu)==EBUSY);return pinned;}
static int efs_client_mtime_is_pinned(efs_ino_t ino){(void)ino;return 0;}
static void pull_file_layout(const struct efs_inode*r){(void)r;}
static void invalidate_file_layout(const struct efs_inode*r,int drop){(void)r;drops+=drop;}
'''+ownership+fn('static void adopt_rpc_inode(')+fn('static int inode_mtime_older(')+fn('static void overlay_local_size(')+fn('void efs_client_adopt_lookup(')+r'''
static struct efs_inode reset(void){staged=(struct efs_inode){.ino=42,.parent=1,.mode=S_IFREG|0600,.nlink=1,.size=1,.mtime=1};dirty=pinned=drops=0;struct efs_inode rpc=staged;rpc.size=0;rpc.mtime=2;return rpc;}
int main(void){struct efs_inode out,rpc=reset();
 /* The inode-only mark has retired; one dirty body still owns EOF1. */
 pinned=1;efs_client_adopt_lookup(&rpc,&out);assert(staged.size==1 && out.size==1 && !drops);
 /* Once body ownership retires, an actual newer peer truncate is accepted. */
 pinned=0;efs_client_adopt_lookup(&rpc,&out);assert(staged.size==0 && out.size==0 && drops==1);
 rpc=reset();dirty=1;efs_client_adopt_lookup(&rpc,&out);assert(staged.size==1 && out.size==1 && !drops);
 rpc=reset();pinned=1;rpc.size=8;efs_client_adopt_lookup(&rpc,&out);assert(staged.size==8 && out.size==8 && !drops);
 rpc=reset();pinned=1;rpc.nlink=0;efs_client_adopt_lookup(&rpc,&out);assert(staged.size==1 && out.size==1 && !out.nlink && !drops);
 rpc=reset();staged.mode=rpc.mode=S_IFDIR|0700;pinned=dirty=1;efs_client_adopt_lookup(&rpc,&out);assert(staged.size==0 && out.size==0);
 puts("pending EOF: inode-only mark retirement retains body-owned size, lookup overlay, real truncate/growth and directory authority PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-pending-eof-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-pthread','-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
