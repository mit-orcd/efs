#!/usr/bin/env python3
"""Production listing checks fresh permissions before any enumeration RPC."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_FUSE_SOURCE',root/'src/client/efs_fuse.c')).read_text()
m=re.search(r'static int efs_fuse_readdir_ino\([^;]+?\n\{',s)
a=m.start();b=s.index('\n}',a)+2
c=re.search(r'static int check_access\([^;]+?\n\{',s).start();d=s.index('\n}',c)+2
code=r'''
#include "efs/metadata.h"
#include "efs/protocol.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
typedef uint64_t fuse_ino_t;
struct fuse_ctx {uid_t uid;gid_t gid;};
struct readdir_ent {char name[EFS_MAX_NAME];struct stat st;};
struct readdir_collect_arg {struct readdir_ent *ents;size_t count,cap;};
static struct {efs_export_id_t export_id;} g_client;
static struct fuse_ctx context={1001,1001};
static struct efs_inode fresh,stale;
static int fresh_calls,local_calls,lists,result,empty_pages;
static int virt_kind(fuse_ino_t ino){(void)ino;return 0;}
static int commands_enabled(void){return 0;}
static int command_script(const char *term){(void)term;return 0;}
static efs_ino_t virt_parent(fuse_ino_t ino){return ino;}
static int virtual_access(efs_ino_t ino){(void)ino;return 0;}
static const struct fuse_ctx *ll_ctx(void){return &context;}
static int fuse_stat_errno(int rc){return rc==EFS_ERR_BUSY?-EBUSY:-EIO;}
static int caller_in_group(gid_t gid){return gid==context.gid;}
'''+s[c:d]+r'''
static int efs_client_stat_ino(efs_ino_t ino,struct efs_inode *p) __attribute__((unused));
static int efs_client_stat_ino(efs_ino_t ino,struct efs_inode *p){(void)ino;local_calls++;*p=stale;return 0;}
static int efs_client_stat_refresh(efs_ino_t ino,struct efs_inode *p) __attribute__((unused));
static int efs_client_stat_refresh(efs_ino_t ino,struct efs_inode *p){(void)ino;fresh_calls++;*p=fresh;return result;}
static int efs_client_rpc_readdir_cur(efs_export_id_t id,efs_ino_t ino,struct efs_inode *ents,uint32_t*n,uint32_t*src,char*cur,uint32_t*done){
 (void)id;(void)ino;(void)ents;(void)src;(void)cur;lists++;*n=0;*done=lists>empty_pages;return 0;
}
static int vq_lookup(uint64_t ino,efs_ino_t *parent,char *term,size_t n){(void)ino;(void)parent;(void)term;(void)n;return -1;}
static int readdir_collect_page(struct readdir_collect_arg*c,const struct efs_inode*p,uint32_t n){(void)c;(void)p;(void)n;return 0;}
'''+s[a:b]+s[s.index('static int efs_fuse_access_ino('):s.index('\n}',s.index('static int efs_fuse_access_ino('))+2]+r'''
static void reset(void){fresh_calls=local_calls=lists=result=empty_pages=0;context.uid=1001;fresh=(struct efs_inode){.ino=99,.uid=1001,.gid=1001,.mode=S_IFDIR|0300};stale=(struct efs_inode){.ino=99,.uid=1001,.gid=1001,.mode=S_IFDIR|0755};}
int main(void){struct readdir_collect_arg col={0};struct efs_inode row;
 reset();assert(efs_fuse_readdir_ino(99,&col,&row)==-EACCES);assert(fresh_calls==1&&!local_calls&&!lists);
 reset();fresh.mode=S_IFDIR|0400;stale.mode=S_IFDIR|0300;assert(!efs_fuse_readdir_ino(99,&col,&row));assert(fresh_calls==1&&lists==1&&row.mode==fresh.mode);
 reset();fresh.mode=S_IFDIR|0400;empty_pages=3;assert(!efs_fuse_readdir_ino(99,&col,&row)&&lists==4);
 reset();context.uid=0;assert(!efs_fuse_readdir_ino(99,&col,&row)&&lists==1);
 reset();fresh.ino=EFS_ROOT_INO;assert(!efs_fuse_readdir_ino(EFS_ROOT_INO,&col,&row)&&lists==1);
 reset();result=EFS_ERR_BUSY;assert(efs_fuse_readdir_ino(99,&col,&row)==-EBUSY&&!lists);
 reset();result=EFS_ERR_ACCES;assert(efs_fuse_readdir_ino(99,&col,&row)==-EACCES&&!lists);
 reset();fresh.mode=S_IFREG|0400;assert(efs_fuse_readdir_ino(99,&col,&row)==-ENOTDIR&&!lists);
 reset();fresh.mode=S_IFREG|0400;stale.mode=S_IFREG|0600;
 assert(efs_fuse_access_ino(99,W_OK)==-EACCES && fresh_calls==1 && !local_calls);
 reset();result=EFS_ERR_BUSY;assert(efs_fuse_access_ino(99,F_OK)==-EBUSY && !local_calls);
 reset();result=EFS_ERR_IO;assert(efs_fuse_access_ino(99,F_OK)==-EIO && !local_calls);
 puts("readdir/access: fresh DAC before enumeration, stale grants/refusals, root/export-root and authority failures PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-readdir-authority-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include')]+shlex.split(os.environ.get('EFS_TEST_CFLAGS',''))+[str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
