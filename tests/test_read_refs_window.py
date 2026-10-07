#!/usr/bin/env python3
"""Actual zero-copy read cannot expose cached bytes after a failed PUT-window wait."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/read.c').read_text();a=s.index('int efs_client_read_refs(');b=s.index('\n}',a)+2
code=r'''
#include "client_internal.h"
#include "efs/write_extent.h"
int efs_chunk_size_valid(uint32_t cs){return cs==131072;}
#include <assert.h>
#include <string.h>
struct efs_client g_client;
static int window_rc,pins,unpins;
int efs_client_rpc_past_deadline(void){return 0;}static unsigned char data[131072];
static __thread efs_ino_t t_seq_ino;static __thread uint64_t t_seq_next;static __thread int t_seq_run;
static uint32_t data_chunk_size(void){return 131072;}
int efs_export_get_inode(struct efs_export*e,efs_ino_t n,struct efs_inode*r){(void)e;(void)n;memset(r,0,sizeof(*r));r->size=262144;return 0;}
int efs_client_pull_chunks_range(efs_ino_t n,uint32_t a,uint32_t b){(void)n;(void)a;(void)b;return EFS_OK;}
int efs_dcache_put_win_wait(efs_ino_t n){(void)n;return window_rc;}
int efs_dcache_has(efs_ino_t n,uint32_t ci){(void)n;(void)ci;return 0;}
int efs_export_get_chunk(struct efs_export*e,efs_ino_t n,uint32_t ci,struct efs_chunk_entry*r){(void)e;(void)n;(void)ci;memset(r,0,sizeof(*r));return 0;}
void *efs_rdcache_pin(efs_ino_t n,uint32_t ci,uint32_t len,const uint8_t **p){(void)n;(void)ci;(void)len;pins++;*p=data;return data;}
void efs_rdcache_unpin(void*p){assert(p==data);unpins++;}
void efs_client_stage_touch(efs_ino_t n){(void)n;}
static void maybe_prefetch(int pf,efs_ino_t n,uint64_t off,uint64_t end){(void)pf;(void)n;(void)off;(void)end;}
'''+s[a:b]+r'''
int main(void){struct efs_read_ref refs[2];pthread_mutex_init(&g_client.idx_mu,NULL);memset(refs,0x5a,sizeof(refs));struct efs_read_ref saved[2];memcpy(saved,refs,sizeof(refs));
 assert(efs_client_read_refs(42,0,131072,NULL,2)==EFS_ERR_INVAL&&!pins);
 assert(efs_client_read_refs(42,0,131072,refs,-1)==EFS_ERR_INVAL&&!pins);
 assert(!efs_client_read_refs(42,UINT64_MAX-131071,131072,refs,2)&&!pins);
 assert(!efs_client_read_refs(42,(uint64_t)UINT32_MAX*131072,131072,refs,2)&&!pins);
 window_rc=EFS_ERR_BUSY;assert(efs_client_read_refs(42,0,131072,refs,2)==EFS_ERR_BUSY&&!pins&&!memcmp(saved,refs,sizeof(refs)));
 window_rc=EFS_ERR_IO;assert(efs_client_read_refs(42,0,131072,refs,2)==EFS_ERR_IO&&!pins);
 window_rc=1;assert(efs_client_read_refs(42,0,262144,refs,2)==2&&pins==2&&!unpins&&refs[0].data==data&&refs[1].len==131072);for(int i=0;i<2;i++)efs_rdcache_unpin(refs[i].pin);assert(unpins==2);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-refs-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('read refs: failed PUT-window waits reject cache pins and preserve outputs; successful pins released PASS')
