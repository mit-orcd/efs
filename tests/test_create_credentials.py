#!/usr/bin/env python3
"""Execute FUSE CREATE/MKDIR/SYMLINK with differing creator and parent groups."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
s = Path(os.environ.get('EFS_FUSE_SOURCE', root / 'src/client/efs_fuse.c')).read_text()
def fn(name):
    m = re.search(r'^static [^\n;]*\b' + name + r'\([^;]+?\)\n\{', s, re.M)
    return s[m.start():s.index('\n}', m.end()) + 2] if m else ''
code = r'''
#include <efs/metadata.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
typedef uint64_t fuse_ino_t;
struct fuse_ctx {uid_t uid;gid_t gid;};
struct fuse_file_info {int flags;};
static struct fuse_ctx creator={1001,2001};
static int has_context=1;
static int create_rc, lookup_rc=EFS_OK;
static const struct fuse_ctx *ll_ctx(void){return has_context?&creator:NULL;}
static mode_t parent_mode,captured_mode;
static uid_t captured_uid;
static gid_t captured_gid;
static struct {uint64_t export_id;} g_client={1};
struct request {struct efs_inode inode;int complete,result,attempted;uint64_t generation;struct {struct {struct {uint64_t seq;} id;} q;} wire;};
struct efs_pending_create {struct request request;};
static int name_is_reserved(const char *name){(void)name;return 0;}
static int check_dir_wx(const struct efs_inode *p){(void)p;return 0;}
static int efs_client_stat_ino(efs_ino_t ino,struct efs_inode *p){memset(p,0,sizeof(*p));p->ino=ino;p->mode=parent_mode;p->gid=3001;return EFS_OK;}
static struct efs_pending_create *efs_pending_create_alloc(void){return calloc(1,sizeof(struct efs_pending_create));}
static void efs_pending_create_free(struct efs_pending_create *p){free(p);}
static void efs_pending_create_queue(struct efs_pending_create *p){free(p);}
static void capture(mode_t mode,uid_t uid,gid_t gid){captured_mode=mode;captured_uid=uid;captured_gid=gid;}
static int efs_fuse_create_begin_wait(struct request *r,efs_ino_t p,const char *name,mode_t mode,uid_t uid,gid_t gid){(void)p;(void)name;capture(mode,uid,gid);r->complete=1;r->generation=1;r->inode.ino=42;return EFS_OK;}
static int efs_fuse_create_resume_wait(struct request *r){(void)r;return EFS_OK;}
static int efs_client_create_stage_result(struct request *r){(void)r;return EFS_OK;}
static int efs_client_create_reset(struct request *r){(void)r;return EFS_OK;}
static int efs_client_create_cancel(struct request *r){(void)r;return EFS_OK;}
static int efs_client_rpc_lookup(uint64_t ex,efs_ino_t p,const char*n,struct efs_inode*out){(void)ex;(void)p;(void)n;if(out){out->ino=42;out->mode=S_IFDIR|0755;}return lookup_rc;}
static int efs_client_truncate(efs_ino_t ino,uint64_t size){(void)ino;(void)size;return EFS_OK;}
static int efs_file_open(struct fuse_file_info *fi,efs_ino_t ino){(void)fi;(void)ino;return 0;}
static void efs_file_close(struct fuse_file_info *fi){(void)fi;}
static void fuse_fi_direct_io(struct fuse_file_info *fi){(void)fi;}
static int efs_open_acquire_fileid(efs_ino_t ino,uint64_t gen){(void)ino;(void)gen;return EFS_OK;}
static int efs_open_adopt(efs_ino_t ino,uint64_t gen,uint64_t token,int *handoff){(void)ino;(void)gen;(void)token;*handoff=1;return EFS_OK;}
static int efs_fuse_release_ino(efs_ino_t ino,struct fuse_file_info *fi){(void)ino;(void)fi;return 0;}
static int efs_client_create_result(efs_ino_t p,const char *name,mode_t mode,uid_t uid,gid_t gid,int flags,efs_ino_t *out){(void)p;(void)name;(void)flags;capture(mode,uid,gid);if(create_rc)return create_rc;*out=42;return EFS_OK;}
static int efs_client_write(efs_ino_t ino,uint64_t off,size_t n,const void *data){(void)ino;(void)off;(void)n;(void)data;return EFS_OK;}
static int efs_client_report_dirty_ino(efs_ino_t ino,int all){(void)ino;(void)all;return EFS_OK;}
''' + '\n'.join(fn(n) for n in ['efs_create_credentials','fuse_stat_errno','fuse_create_errno','efs_fuse_create_at','efs_fuse_mkdir_at','efs_fuse_symlink_at']) + r'''
static void check_all(int setgid,int context){
 parent_mode=S_IFDIR|0777|(setgid?S_ISGID:0);has_context=context;
 gid_t expected_gid=setgid?3001:(context?2001:0);uid_t expected_uid=context?1001:0;
 struct fuse_file_info fi={0};efs_ino_t ino=0;
 assert(!efs_fuse_create_at(1,"file",0640,&fi,&ino,NULL));
 assert(captured_gid==expected_gid && captured_uid==expected_uid && captured_mode==(S_IFREG|0640));
 assert(!efs_fuse_mkdir_at(1,"dir",0750,&ino));
 assert(captured_gid==expected_gid && captured_uid==expected_uid && captured_mode==(S_IFDIR|0750|(setgid?S_ISGID:0)));
 assert(!efs_fuse_symlink_at("file",1,"link",&ino));
 assert(captured_gid==expected_gid && captured_uid==expected_uid && captured_mode==(S_IFLNK|0777));
}
int main(void){
 check_all(1,1);check_all(0,1);check_all(1,0);check_all(0,0);
 efs_ino_t ino=999;create_rc=EFS_ERR_EXIST;
 assert(efs_fuse_mkdir_at(1,"raced",0755,&ino)==-EEXIST && ino==999);
 create_rc=EFS_ERR_BUSY;
 assert(efs_fuse_mkdir_at(1,"raced",0755,&ino)==-EBUSY && ino==999);
 create_rc=EFS_ERR_QUOTA;
 assert(efs_fuse_mkdir_at(1,"raced",0755,&ino)==-ENOSPC && ino==999);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-create-credentials-') as d:
    p = Path(d) / 'test.c'
    p.write_text(code)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-D_GNU_SOURCE','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('FUSE creation credentials: parent setgid inheritance, directory-only bit, distinct caller groups internal context and refused mkdir verdicts PASS')
