#!/usr/bin/env python3
"""Production read admission must not turn metadata/PUT-wait failures into EOF."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/read.c').read_text();a=s.index('int efs_client_read(');b=s.index('    uint64_t file_size = inode.size;',a)
# Admission is compiled from production; data decoding is tested separately.
f=s[a:b]+'    return EFS_OK;\n}'
code=r'''
#include "client_internal.h"
#include "efs/write_extent.h"
int efs_chunk_size_valid(uint32_t cs){return cs==131072;}
#include <assert.h>
#include <string.h>
struct efs_client g_client;
static int have_row,stat_rc,window_rc,kept,expired;
static uint32_t data_chunk_size(void) {return 131072;}
int efs_client_rpc_past_deadline(void) {return expired;}
void efs_client_lock_dir(efs_ino_t i) {(void)i;}
void efs_client_unlock_dir(efs_ino_t i) {(void)i;}
int efs_export_get_inode(struct efs_export *e,efs_ino_t i,struct efs_inode *r) {(void)e;(void)i;memset(r,0,sizeof(*r));r->size=100;return have_row?0:-1;}
int efs_client_stat_ino(efs_ino_t i,struct efs_inode *r) {(void)i;r->size=100;return stat_rc;}
int efs_dcache_copy_unpub(efs_ino_t i,uint32_t ci,uint32_t o,uint8_t *p,uint32_t n) {(void)i;(void)ci;(void)o;if(!kept)return -1;memset(p,'x',n);return 0;}
int efs_dcache_copy_kept(efs_ino_t i,uint32_t ci,uint32_t o,uint8_t *p,uint32_t n) {(void)i;(void)ci;(void)o;(void)p;(void)n;return -1;}
void efs_client_stage_touch(efs_ino_t i) {(void)i;}
int efs_dcache_put_win_wait(efs_ino_t i) {(void)i;return window_rc;}
'''+f+r'''
int main(void) {
 char buf[4];size_t got=77;pthread_mutex_init(&g_client.idx_mu,NULL);
 stat_rc=EFS_ERR_NET;assert(efs_client_read(1,0,4,buf,&got)==EFS_ERR_NET&&!got);
 stat_rc=EFS_ERR_NOT_FOUND;kept=1;assert(!efs_client_read(1,0,4,buf,&got)&&got==4&&buf[0]=='x');
 have_row=1;window_rc=EFS_ERR_BUSY;got=77;assert(efs_client_read(1,0,4,buf,&got)==EFS_ERR_BUSY&&!got);
 window_rc=1;assert(!efs_client_read(1,0,4,buf,&got));
 expired=1;got=77;assert(efs_client_read(1,0,4,buf,&got)==EFS_ERR_BUSY&&!got);
 assert(efs_client_read(1,0,4,buf,NULL)==EFS_ERR_INVAL);
 assert(efs_client_read(1,0,4,NULL,&got)==EFS_ERR_INVAL);
 expired=0;got=77;assert(efs_client_read(1,UINT64_MAX-1,4,buf,&got)==EFS_ERR_INVAL&&!got);
 uint64_t limit=(uint64_t)UINT32_MAX*131072;assert(efs_client_read(1,limit,4,buf,&got)==EFS_ERR_INVAL&&!got);
 assert(!efs_client_read(1,0,0,NULL,&got)&&!got);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-failure-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Wno-unused-variable','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
print('read admission: metadata and window failures, retained ghost bytes, expired request and defined output PASS')
