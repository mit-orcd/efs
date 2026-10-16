#!/usr/bin/env python3
"""Actual CHOWN/LINK must preserve unavailable authority rather than report absence."""
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
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t fuse_ino_t;
static int stat_rc,mutation_rc,permission_rc,stat_calls,mutations,source_dir;
static int virt_kind(fuse_ino_t n){(void)n;return 0;}
static int name_is_reserved(const char *name){(void)name;return 0;}
static int efs_client_stat_ino(uint64_t ino,struct efs_inode *row){stat_calls++;row->ino=ino;row->mode=source_dir?S_IFDIR:S_IFREG;return stat_rc;}
static int check_chown_perm(const struct efs_inode *row,uid_t uid,gid_t gid){(void)row;(void)uid;(void)gid;return permission_rc;}
static int check_dir_wx(const struct efs_inode *row){(void)row;return permission_rc;}
static int efs_client_chown(uint64_t ino,uid_t uid,gid_t gid){(void)ino;(void)uid;(void)gid;mutations++;return mutation_rc;}
static int efs_client_link(uint64_t ino,uint64_t parent,const char *name){(void)ino;(void)parent;(void)name;mutations++;return mutation_rc;}
struct fuse_ctx {uid_t uid;gid_t gid;};
struct fuse_file_info {uint64_t fh;int flags,direct_io,keep_cache;};
static struct fuse_ctx context={1001,2001};
static const struct fuse_ctx *ll_ctx(void){return &context;}
static int virtual_access(uint64_t ino){(void)ino;return 0;}
static uint64_t virt_parent(uint64_t ino){return ino;}
static int feature_enabled(unsigned feature){(void)feature;return 0;}
static int commands_enabled(void){return 0;}
struct stats_ent {char text[1];size_t len;};
static int stats_snapshot(uint64_t ino,struct stats_ent *e){(void)ino;(void)e;return -EIO;}
static int virtual_file_open(struct fuse_file_info *fi,uint64_t ino,const char *text,size_t len){(void)fi;(void)ino;(void)text;(void)len;return -EIO;}
static int vq_lookup(uint64_t ino,uint64_t *dir,char *term,size_t size){(void)ino;(void)dir;(void)term;(void)size;return -1;}
static int command_script(const char *term){(void)term;return 0;}
static const char efs_command_source[]="fixture";
#define EFS_FIND_NAME ".find"
#define EFS_FIND_RES_N 1
static pthread_mutex_t g_find_res_mu=PTHREAD_MUTEX_INITIALIZER;
static struct {int valid;uint64_t dir_ino;char term[256];char *text;size_t len;} g_find_res[EFS_FIND_RES_N];
static int find_query_ensure(uint64_t ino,const char *path,const char *term){(void)ino;(void)path;(void)term;return -EIO;}
static int find_res_caller(int i){(void)i;return 0;}
static int vq_version_matches(uint64_t ino,const char *text,size_t len){(void)ino;(void)text;(void)len;return 0;}
static int check_access(const struct efs_inode *row,uid_t uid,gid_t gid,int mask){(void)row;(void)uid;(void)gid;(void)mask;return permission_rc;}
static int efs_fuse_truncate_ino(uint64_t ino,off_t size,struct fuse_file_info *fi){(void)ino;(void)size;(void)fi;mutations++;return 0;}
static int file_opens,lease_opens,file_closes,lease_rc;
static int efs_file_open(struct fuse_file_info *fi,uint64_t ino){fi->fh=ino;file_opens++;return 0;}
static __thread uint64_t t_open_read_size;
static int efs_open_acquire(uint64_t ino){(void)ino;lease_opens++;return lease_rc;}
static void efs_file_close(struct fuse_file_info *fi){fi->fh=0;file_closes++;}
'''+function('fuse_stat_errno')+function('efs_rc_to_errno')+function('fuse_fi_direct_io')+function('efs_fuse_open_ino')+function('efs_fuse_chown_ino')+function('efs_fuse_link_at')+r'''
int main(void){
 int cases[][2]={{EFS_ERR_IO,-EIO},{EFS_ERR_NET,-EIO},{EFS_ERR_NO_QUORUM,-EIO},{EFS_ERR_BUSY,-EBUSY},{EFS_ERR_NOT_PRIMARY,-EBUSY},{EFS_ERR_NOMEM,-ENOMEM},{EFS_ERR_ACCES,-EACCES},{EFS_ERR_NOT_FOUND,-ENOENT}};
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
  struct fuse_file_info fi={.fh=999,.flags=O_RDONLY};
  stat_rc=cases[i][0];assert(efs_fuse_open_ino(42,&fi)==cases[i][1]);assert(fi.fh==999 && !file_opens && !lease_opens);assert(efs_fuse_chown_ino(42,1,2)==cases[i][1]);assert(efs_fuse_link_at(42,43,"target")==cases[i][1]);assert(!mutations);
 }
 stat_rc=0;permission_rc=-EPERM;assert(efs_fuse_chown_ino(42,1,2)==-EPERM);assert(efs_fuse_link_at(42,43,"target")==-EPERM);assert(!mutations);
 permission_rc=0;source_dir=1;assert(efs_fuse_link_at(42,43,"target")==-EPERM);assert(!mutations);source_dir=0;
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
  mutation_rc=cases[i][0];assert(efs_fuse_link_at(42,43,"target")==cases[i][1]);
 }
 int mutation_cases[][2]={{EFS_ERR_QUOTA,-ENOSPC},{EFS_ERR_OVERFLOW,-EOVERFLOW},{EFS_ERR_AGAIN,-EAGAIN},{EFS_ERR_INVAL,-EINVAL},{EFS_ERR_NAMETOOLONG,-ENAMETOOLONG}};
 for(unsigned i=0;i<sizeof(mutation_cases)/sizeof(mutation_cases[0]);i++){mutation_rc=mutation_cases[i][0];assert(efs_fuse_link_at(42,43,"target")==mutation_cases[i][1]);}
 mutation_rc=EFS_ERR_EXIST;assert(efs_fuse_link_at(42,43,"target")==-EEXIST);
 mutation_rc=0;struct fuse_file_info fi={.flags=O_RDONLY};
 assert(!efs_fuse_open_ino(42,&fi));assert(fi.fh==42 && fi.direct_io && file_opens==1 && lease_opens==1);
 lease_rc=EFS_ERR_BUSY;assert(efs_fuse_open_ino(42,&fi)==-EBUSY);assert(!fi.fh && file_closes==1);
 assert(!efs_fuse_chown_ino(42,1,2));assert(!efs_fuse_link_at(42,43,"target"));
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-attribute-authority-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix('')),'-lpthread'],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('OPEN/CHOWN/LINK: only actual absence ENOENT; unavailable authority, permissions and mutation errors preserved PASS')
