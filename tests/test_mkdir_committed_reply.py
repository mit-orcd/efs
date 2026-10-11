#!/usr/bin/env python3
"""Execute the actual MKDIR path with exact committed rows and refused creates."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=Path(os.environ.get('EFS_MKDIR_REPLY_SOURCE',str(root/'src/client/efs_fuse.c'))).read_text()
def function(name):
 m=re.search(r'^static (?:int|void) '+name+r'\([^;]+?\)\n\{',s,re.M);assert m,name
 return s[m.start():s.index('\n}',m.end())+2]
compat = '' if 'struct efs_inode *created)' in function('efs_fuse_mkdir_at') else r'''
static int efs_client_create_result(efs_ino_t p,const char *n,uint32_t mode,uid_t uid,gid_t gid,uint32_t flags,efs_ino_t *ino) {struct efs_inode row;return efs_client_create_result_inode(p,n,mode,uid,gid,flags,ino,&row);}
static int lookup_fill_committed(fuse_ino_t ino,struct fuse_entry_param *e,const char *op,fuse_ino_t p,const char *n) {(void)op;(void)p;(void)n;stats++;struct efs_inode row={.ino=42,.parent=7,.mode=S_IFDIR|02750,.uid=17,.gid=91,.nlink=2,.mtime=123};fill_entry_row(ino,e,&row);return 0;}
'''
code=r''' 
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "efs/metadata.h"
typedef uint64_t fuse_ino_t;
typedef void *fuse_req_t;
struct fuse_entry_param {struct efs_inode row;};
static void *t_req;
static int create_rc,stat_rc,deny,malformed,stats,creates,errors,replies,last_error,held;
static uint64_t deadline,observed_deadline;
static int name_is_reserved(const char *n) {return !strcmp(n,".efs");}
static int efs_client_stat_ino(efs_ino_t ino,struct efs_inode *row) {assert(ino==7);stats++;*row=(struct efs_inode){.ino=7,.mode=S_IFDIR|02770,.gid=91};return stat_rc;}
static int fuse_stat_errno(int rc) {(void)rc;return -EIO;}
static int check_dir_wx(const struct efs_inode *p) {assert(p->ino==7);return deny?-EACCES:0;}
static void efs_create_credentials(const struct efs_inode *p,mode_t *m,uid_t *uid,gid_t *gid) {assert(p->ino==7);*m|=S_ISGID;*uid=17;*gid=p->gid;}
static int efs_client_create_result_inode(efs_ino_t p,const char *n,uint32_t mode,uid_t uid,gid_t gid,uint32_t flags,efs_ino_t *ino,struct efs_inode *row) {
 assert(p==7&&!strcmp(n,"child")&&flags==0&&uid==17&&gid==91&&mode==(S_IFDIR|02750));creates++;
 observed_deadline=deadline;*ino=0;
 if(!create_rc){*ino=42;*row=(struct efs_inode){.ino=malformed?99:42,.parent=7,.mode=mode,.uid=uid,.gid=gid,.nlink=2,.mtime=123};}
 return create_rc;
}
static int fuse_create_errno(int rc,fuse_ino_t p,const char *n) {assert(p==7&&!strcmp(n,"child"));return rc==EFS_ERR_BUSY?-EBUSY:rc==EFS_ERR_EXIST?-EEXIST:-EIO;}
static void lookup_memo_mutation_begin(void) {assert(!held);held=1;}
static void lookup_memo_mutation_end(void) {assert(held);held=0;}
static uint64_t efs_client_rpc_deadline_ms(void) {return deadline;}
static void efs_client_rpc_set_deadline_ms(uint64_t d) {deadline=d;}
static uint64_t stats_now_ms(void) {return 1000;}
static void fill_entry_row(fuse_ino_t ino,struct fuse_entry_param *e,const struct efs_inode *row) {assert(ino==42&&row->ino==42);e->row=*row;}
static void fuse_reply_err(fuse_req_t r,int err) {(void)r;assert(err>0);errors++;last_error=err;}
static void fuse_reply_entry(fuse_req_t r,const struct fuse_entry_param *e) {(void)r;assert(e->row.ino==42&&e->row.parent==7&&e->row.uid==17&&e->row.gid==91&&e->row.mode==(S_IFDIR|02750)&&e->row.nlink==2&&e->row.mtime==123);replies++;}
''' +compat+function('efs_fuse_mkdir_at')+'\n'+function('ll_mkdir_run')+r'''
static void reset(void) {create_rc=stat_rc=deny=malformed=stats=creates=errors=replies=last_error=held=0;deadline=0;}
int main(void) {
 reset();ll_mkdir_run(NULL,7,"child",0750);assert(stats==1&&creates==1&&replies==1&&!errors&&!held&&!t_req&&observed_deadline==31000&&deadline==0);
 reset();deadline=1200;ll_mkdir_run(NULL,7,"child",0750);assert(replies==1&&observed_deadline==1200&&deadline==1200);
 for(int rc=EFS_ERR_BUSY;rc<=EFS_ERR_IO;rc++) {if(rc!=EFS_ERR_BUSY&&rc!=EFS_ERR_EXIST&&rc!=EFS_ERR_IO)continue;
  reset();create_rc=rc;ll_mkdir_run(NULL,7,"child",0750);assert(errors==1&&!replies&&creates==1&&stats==1&&!held&&!t_req&&deadline==0);
 }
 reset();deny=1;ll_mkdir_run(NULL,7,"child",0750);assert(last_error==EACCES&&!creates&&!replies);
 reset();stat_rc=EFS_ERR_IO;ll_mkdir_run(NULL,7,"child",0750);assert(last_error==EIO&&!creates&&!replies);
 reset();malformed=1;ll_mkdir_run(NULL,7,"child",0750);assert(last_error==EIO&&creates==1&&!replies);
 reset();ll_mkdir_run(NULL,7,".efs",0750);assert(last_error==EEXIST&&!stats&&!creates&&!replies);
 puts("MKDIR committed reply: exact identity/setgid/attrs, one parent check, no post-create stat, error and deadline preservation PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-mkdir-reply-') as td:
 p=Path(td)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
