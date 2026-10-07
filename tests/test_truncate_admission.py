#!/usr/bin/env python3
"""Exercise production FUSE truncate with failed flush and nested deadlines."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/efs_fuse.c').read_text()
def fn(name):
 m=re.search(r'^static int '+name+r'\([^;]+?\)\n\{',s,re.M)
 return s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include "efs/metadata.h"
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
typedef uint64_t fuse_ino_t;
struct fuse_file_info {uint64_t fh;};
struct fuse_ctx {uid_t uid;gid_t gid;};
static uint64_t deadline,now=1000,seen;
static int flush_rc,stat_rc,flushes,truncates;
static uint64_t stats_now_ms(void) {return now;}
static uint64_t efs_client_rpc_deadline_ms(void) {return deadline;}
static void efs_client_rpc_set_deadline_ms(uint64_t d) {deadline=d;}
static int efs_client_rpc_past_deadline(void) {return deadline&&now>=deadline;}
static int virt_kind(fuse_ino_t ino) {(void)ino;return 0;}
static const struct fuse_ctx *ll_ctx(void) {return NULL;}
static int check_access(const struct efs_inode *p,uid_t u,gid_t g,int m) {(void)p;(void)u;(void)g;(void)m;return 0;}
static int efs_client_stat_ino(efs_ino_t ino,struct efs_inode *p) {(void)ino;p->mode=0100644;seen=deadline;return stat_rc;}
static int efs_rc_to_errno(int rc) {return rc==EFS_OK?0:rc==EFS_ERR_BUSY?-EAGAIN:-EIO;}
static int efs_dcache_flush_ino(efs_ino_t ino) {(void)ino;flushes++;seen=deadline;return flush_rc;}
static int efs_client_truncate(efs_ino_t ino,uint64_t size) {(void)ino;(void)size;truncates++;return EFS_OK;}
'''+fn('efs_fuse_truncate_run')+fn('efs_fuse_truncate_ino')+r'''
int main(void) {
 struct fuse_file_info fi={.fh=1};
 flush_rc=EFS_ERR_IO;deadline=1200;
 assert(efs_fuse_truncate_ino(1,0,&fi)==-EIO);
 assert(!truncates&&seen==1200&&deadline==1200);
 flush_rc=EFS_OK;deadline=0;
 assert(!efs_fuse_truncate_ino(1,0,&fi));
 assert(truncates==1&&seen==9000&&!deadline);
 int before=flushes;deadline=1000;
 assert(efs_fuse_truncate_ino(1,0,&fi)==-EAGAIN);
 assert(flushes==before&&deadline==1000);
 deadline=0;stat_rc=EFS_ERR_IO;
 assert(efs_fuse_truncate_ino(1,0,NULL)==-EIO);
 assert(flushes==before&&seen==9000&&!deadline);
 assert(efs_fuse_truncate_ino(1,-1,&fi)==-EINVAL);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-truncate-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
print('truncate admission: failed flush, access error, nested budget and expired refusal PASS')
