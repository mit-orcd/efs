#!/usr/bin/env python3
"""Actual FUSE LOOKUP callback must not report unavailable authority as absence."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/efs_fuse.c').read_text()
def fn(name):
 m=re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{',s,re.M)
 return s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include <efs/metadata.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <string.h>
typedef uint64_t fuse_ino_t;
struct fuse_entry_param {uint64_t ino;};
static struct {uint64_t export_id;} g_client={1};
#define FUSE_ROOT_ID 1
#define EFS_STATS_NAME ".stats"
#define EFS_FIND_NAME ".find"
static int failure,adopted;
static int virt_kind(fuse_ino_t n){(void)n;return 0;}
static int feature_enabled(int n){(void)n;return 1;}
static fuse_ino_t virt_parent(fuse_ino_t n){return n;}
static fuse_ino_t vq_intern(fuse_ino_t n,const char*s){(void)s;return n;}
static fuse_ino_t virt_stats_ino(fuse_ino_t n){return n;}
static fuse_ino_t virt_find_ino(fuse_ino_t n){return n;}
static int lookup_fill(fuse_ino_t n,struct fuse_entry_param*e,void*p){(void)n;(void)e;(void)p;return 0;}
static int efs_client_stat_local(efs_ino_t n,struct efs_inode*out){out->ino=n;out->mode=S_IFDIR|0755;return 0;}
static int efs_client_stat_ino(efs_ino_t n,struct efs_inode*out){return efs_client_stat_local(n,out);}
static int check_dir_x(struct efs_inode*n){(void)n;return 0;}
static int efs_client_lookup_local(efs_ino_t p,const char*n,struct efs_inode*out){(void)p;(void)n;(void)out;return EFS_ERR_NOT_FOUND;}
static int efs_client_rpc_lookup(uint64_t x,efs_ino_t p,const char*n,struct efs_inode*out){(void)x;(void)p;(void)n;out->ino=42;out->mode=S_IFREG|0644;return failure;}
static void efs_client_adopt_lookup(struct efs_inode*a,struct efs_inode*b){(void)a;(void)b;adopted++;}
static void fill_entry_row(fuse_ino_t n,struct fuse_entry_param*e,struct efs_inode*r){(void)r;e->ino=n;}
'''+fn('fuse_stat_errno')+fn('efs_fuse_lookup_at')+r'''
int main(void){
 struct fuse_entry_param e={0};
 int cases[][2]={{EFS_ERR_IO,-EIO},{EFS_ERR_NET,-EIO},{EFS_ERR_NO_QUORUM,-EIO},{EFS_ERR_BUSY,-EBUSY},{EFS_ERR_NOT_PRIMARY,-EBUSY},{EFS_ERR_NOMEM,-ENOMEM},{EFS_ERR_ACCES,-EACCES},{EFS_ERR_NOT_FOUND,-ENOENT}};
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){failure=cases[i][0];assert(efs_fuse_lookup_at(1,"name",&e)==cases[i][1]);assert(!adopted);}
 failure=0;assert(!efs_fuse_lookup_at(1,"name",&e)&&adopted==1&&e.ino==42);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-lookup-errno-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-D_GNU_SOURCE','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('FUSE LOOKUP: IO/network/quorum errors stay IO, BUSY/memory/access preserved, only actual absence ENOENT PASS')
