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
static int read_rc,write_rc,report_rc,writes,reports,setattrs,short_read;
static uint32_t data_chunk_size(void) {return 131072;}
static void now_ns(uint64_t *s,uint32_t *n) {*s=1;*n=0;}
void efs_client_lock_dir(efs_ino_t i) {(void)i;}
void efs_client_unlock_dir(efs_ino_t i) {(void)i;}
int efs_export_get_inode(struct efs_export *e,efs_ino_t i,struct efs_inode *r) {(void)e;(void)i;memset(r,0,sizeof(*r));r->size=131072;return 0;}
void *efs_buf_alloc(uint32_t n) {return malloc(n);}
void efs_buf_free(void *p,uint32_t n) {(void)n;free(p);}
int efs_client_read(efs_ino_t i,uint64_t o,size_t n,char *p,size_t *got) {(void)i;(void)o;memset(p,'x',n);*got=short_read?0:n;return read_rc;}
int efs_dcache_store_full_owned(efs_ino_t i,uint32_t ci,uint8_t *p,uint32_t n) {(void)i;assert(ci==0&&n==131072&&p[0]=='x'&&p[99]=='x'&&!p[100]);writes++;if(write_rc)return -1;free(p);return 0;}
int efs_client_report_dirty_ino(efs_ino_t i,int sync) {(void)i;assert(sync);return EFS_OK;}
int efs_dcache_flush_ino(efs_ino_t i) {(void)i;reports++;return report_rc;}
int efs_client_rpc_setattr(efs_export_id_t e,efs_ino_t i,uint32_t mask,uint32_t mode,uid_t uid,gid_t gid,uint64_t size,uint64_t mt,uint32_t mn,uint64_t at,uint32_t an,struct efs_inode *out) {(void)e;(void)i;(void)mask;(void)mode;(void)uid;(void)gid;(void)size;(void)mt;(void)mn;(void)at;(void)an;memset(out,0,sizeof(*out));setattrs++;return EFS_OK;}
void efs_export_drop_chunks_from(struct efs_export *e,efs_ino_t i,uint32_t ci) {(void)e;(void)i;(void)ci;}
int efs_export_upsert_inode(struct efs_export *e,const struct efs_inode *r) {(void)e;(void)r;return 0;}
void efs_client_stage_touch(efs_ino_t i) {(void)i;}
void efs_rdcache_invalidate(efs_ino_t i,uint32_t ci) {(void)i;(void)ci;}
void efs_dcache_drop_if_clean(efs_ino_t i,uint32_t ci) {(void)i;(void)ci;}
'''+f+r'''
int main(void) {
 pthread_mutex_init(&g_client.idx_mu,NULL);
 read_rc=EFS_ERR_NET;assert(efs_client_truncate(1,100)==EFS_ERR_NET);
 assert(!writes&&!reports&&!setattrs);
 read_rc=EFS_OK;short_read=1;assert(efs_client_truncate(1,100)==EFS_ERR_IO);
 assert(!writes&&!reports&&!setattrs);
 short_read=0;write_rc=EFS_ERR_NOMEM;assert(efs_client_truncate(1,100)==EFS_ERR_NOMEM);assert(writes==1&&!reports&&!setattrs);write_rc=0;report_rc=EFS_ERR_BUSY;assert(efs_client_truncate(1,100)==EFS_ERR_BUSY);
 assert(writes==2&&reports==1&&!setattrs);
 report_rc=EFS_OK;assert(!efs_client_truncate(1,100));
 assert(writes==3&&reports==2&&setattrs==1);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)

print("truncate prefix: failed/short read and failed publication refuse size mutation PASS")
