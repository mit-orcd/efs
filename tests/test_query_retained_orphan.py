#!/usr/bin/env python3
"""Execute retained orphan preflight without changing installed metadata."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_QUERY_ORPHAN_SOURCE',root/'src/meta/query_disk.c')).read_text()
a=s.index('struct backlink_probe {')
# Include the orphan helper and its backlink helpers, independently of later
# unrelated builder helpers or their declaration order.
b=s.index('\n}\n',s.index('static int orphan_preflight(',a))+3
code=r'''
#include "efs/query_catalog.h"
#include "efs/meta_apply.h"
#include "efs/kv_lsm.h"
#include "efs/kv_key.h"
#include "efs/txn.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
struct efs_kv_lsm_snapshot {int repaired,elsewhere,guarded,missing;};
static const struct efs_kv_ops source_ops={0};
static unsigned reads,scans;
int efs_meta_apply_get_inode(struct efs_kv *kv,efs_ino_t ino,struct efs_meta_row *row){
 struct efs_kv_lsm_snapshot *s=kv->ctx;reads++;memset(row,0,sizeof(*row));
 if(ino==4091 && s->missing)return EFS_ERR_NOT_FOUND;
 assert(ino==4091 || ino==2098);row->ino=ino;row->parent=ino==4091?2098:1;
 row->mode=S_IFDIR|0755;row->nlink=ino==4091?2:4;return EFS_OK;
}
int efs_txn_key_exclusive(struct efs_kv *kv,const uint8_t *key,uint32_t n){
 (void)key;(void)n;return ((struct efs_kv_lsm_snapshot *)kv->ctx)->guarded?EFS_ERR_BUSY:EFS_OK;
}
int efs_txn_key_guarded(struct efs_kv *kv,const uint8_t *key,uint32_t n){
 return efs_txn_key_exclusive(kv,key,n);
}
int efs_meta_apply_readdir(struct efs_kv *kv,efs_ino_t dir,struct efs_meta_dir_cursor *cursor,
 struct efs_meta_dir_ent *out,uint32_t max,uint32_t *n){
 assert(dir==2098 && max==64);reads++;cursor->done=1;*n=1;
 memset(out,0,sizeof(*out));out[0].d.ino=((struct efs_kv_lsm_snapshot *)kv->ctx)->repaired?4091:8187;
 return EFS_OK;
}
int efs_meta_unpack_dentry(const uint8_t *value,uint32_t n,struct efs_meta_dentry *d){
 assert(n==sizeof(*d));memcpy(d,value,n);return EFS_OK;
}
int efs_kv_lsm_snapshot_scan(struct efs_kv_lsm_snapshot *s,const uint8_t *prefix,uint32_t pn,
 const uint8_t *start,uint32_t sn,int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *ctx){
 assert(!prefix && !pn && !start && !sn);scans++;
 uint8_t key[]={0,1,EFS_KV_KIND_DENTRY};
 struct efs_meta_dentry d={.ino=s->elsewhere?4091:8187};
 return cb(ctx,key,sizeof(key),(const uint8_t *)&d,sizeof(d));
}
''' + s[a:b] + r'''
static int cancelled(void *ctx){(void)ctx;return 1;}
int main(void){
 struct efs_kv_lsm_snapshot retained={0},repaired={.repaired=1},moved={.elsewhere=1},guarded={.guarded=1},missing={.missing=1};
 struct efs_query_build_failure failure={.directory=4091};
 assert(orphan_preflight(&retained,&failure,NULL,NULL)==EFS_ERR_IO && failure.directory==4091);
 assert(reads==3 && scans==1);
 assert(orphan_preflight(&retained,&failure,NULL,NULL)==EFS_ERR_IO && failure.directory==4091);
 unsigned before=reads;assert(orphan_preflight(&guarded,&failure,NULL,NULL)==EFS_ERR_BUSY && reads==before+2 && failure.directory==4091);
 assert(orphan_preflight(&retained,&failure,cancelled,NULL)==EFS_ERR_BUSY && failure.directory==4091);
 assert(!orphan_preflight(&repaired,&failure,NULL,NULL) && !failure.directory);
 failure.directory=4091;assert(orphan_preflight(&retained,&failure,NULL,NULL)==EFS_ERR_IO && failure.directory==4091);
 assert(!orphan_preflight(&moved,&failure,NULL,NULL) && !failure.directory);
 failure.directory=4091;assert(!orphan_preflight(&missing,&failure,NULL,NULL) && !failure.directory);
 puts("retained4091/parent2098/replaced8187: actual preflight refuses, pending/cancel inconclusive, immutable old snapshot and new repair/backlink controls PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-retained-orphan-') as tmp:
 p=Path(tmp)/'probe.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),str(root/'src/kv/kv_key.c'),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=10)
