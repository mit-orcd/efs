from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/ops.c').read_text();a=s.index('int efs_client_truncate(');f=s[a:s.index('\n}',a)+2]
code=r'''
#include "client_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
struct efs_client g_client;
static int report_rc,setattr_rc,reports,setattrs,dropped_from=-1,invalidated;
static uint32_t data_chunk_size(void) {return 131072;}
static void now_ns(uint64_t *s,uint32_t *n) {*s=1;*n=0;}
void efs_client_lock_dir(efs_ino_t i) {(void)i;}
void efs_client_unlock_dir(efs_ino_t i) {(void)i;}
int efs_export_get_inode(struct efs_export *e,efs_ino_t i,struct efs_inode *r) {(void)e;(void)i;memset(r,0,sizeof(*r));r->size=3*131072;return 0;}
int efs_client_report_dirty_ino(efs_ino_t i,int sync) {(void)i;assert(sync);reports++;return report_rc;}
int efs_client_rpc_setattr(efs_export_id_t e,efs_ino_t i,uint32_t mask,uint32_t mode,uid_t uid,gid_t gid,uint64_t size,uint64_t mt,uint32_t mn,uint64_t at,uint32_t an,struct efs_inode *out) {(void)e;(void)i;(void)mask;(void)mode;(void)uid;(void)gid;(void)size;(void)mt;(void)mn;(void)at;(void)an;memset(out,0,sizeof(*out));setattrs++;return setattr_rc;}
void efs_export_drop_chunks_from(struct efs_export *e,efs_ino_t i,uint32_t ci) {(void)e;(void)i;dropped_from=(int)ci;}
int efs_export_upsert_inode(struct efs_export *e,const struct efs_inode *r) {(void)e;(void)r;return 0;}
void efs_client_stage_touch(efs_ino_t i) {(void)i;}
void efs_rdcache_invalidate(efs_ino_t i,uint32_t ci) {(void)i;(void)ci;invalidated++;}
void efs_dcache_drop_if_clean(efs_ino_t i,uint32_t ci) {(void)i;(void)ci;}
'''+f+r'''
int main(void) {
 pthread_mutex_init(&g_client.idx_mu,NULL);
 /* D25: no tail rewrite (the server's history masks the tail). A failed
  * REPORT of earlier writes sends no SETATTR; a refused SETATTR changes
  * nothing locally. */
 report_rc=EFS_ERR_BUSY;assert(efs_client_truncate(1,100)==EFS_ERR_BUSY);
 assert(reports==1&&!setattrs&&dropped_from<0);
 report_rc=EFS_OK;setattr_rc=EFS_ERR_NET;assert(efs_client_truncate(1,100)==EFS_ERR_NET);
 assert(reports==2&&setattrs==1&&dropped_from<0);
 /* Success: only the SETATTR; the boundary chunk (0) is dropped locally
  * and every chunk from it on is invalidated. */
 setattr_rc=EFS_OK;assert(!efs_client_truncate(1,100));
 assert(setattrs==2&&dropped_from==0&&invalidated==3);
 /* Aligned shrink: chunks from size/cs on. */
 invalidated=0;assert(!efs_client_truncate(1,131072));
 assert(dropped_from==1&&invalidated==2);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)

print("truncate prefix: no tail rewrite; failed report or SETATTR changes nothing PASS")
