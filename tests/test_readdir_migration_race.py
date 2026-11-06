#!/usr/bin/env python3
"""Execute production READDIR with migration scheduled at KV read boundaries."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import csrc

root = Path(__file__).resolve().parents[1]
source = csrc.module('meta_apply')
start = source.index('#define DENTRY_KEY_PREFIX')
end = source.index('\nstatic int dir_has_live(', start)
code = r'''
#include <efs/meta_apply.h>
#include <efs/kv_key.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
static struct efs_meta_row row;
static struct efs_meta_dentry local, hashed;
static int local_exists, hashed_exists, schedule, fired, refresh_error, row_reads;
static unsigned lane;
static const char *name="f0057";
static void migrate(void) {
 fired=1; row.layout=EFS_META_LAYOUT_SPLITTING;
 row.used_shards|=1ULL<<lane; hashed_exists=1; local_exists=0;
}
int efs_meta_apply_get_inode(struct efs_kv *kv,efs_ino_t ino,struct efs_meta_row *out) {
 (void)kv;assert(ino==800);row_reads++;
 if(row_reads==2 && refresh_error)return refresh_error;
 *out=row;return EFS_OK;
}
static int unpack_dentry(const uint8_t *v,uint32_t n,struct efs_meta_dentry *d) {
 assert(n==sizeof(*d));memcpy(d,v,n);return EFS_OK;
}
static int dent_get(struct efs_kv *kv,uint32_t sh,efs_ino_t ino,const char *nm,struct efs_meta_dentry *out) {
 (void)kv;assert(ino==800&&!strcmp(nm,name));assert(sh==efs_kv_lane_shard(800,lane));
 if(schedule==3&&!fired)migrate();
 if(!hashed_exists)return EFS_ERR_NOT_FOUND;
 *out=hashed;return EFS_OK;
}
static int dent_get_tx(struct efs_kv *kv,uint32_t sh,efs_ino_t ino,const char *nm,efs_txn_coord_fn coord,void *ctx,struct efs_meta_dentry *out) {
 (void)coord;(void)ctx;return dent_get(kv,sh,ino,nm,out);
}
static int scan_from(void *kv,const uint8_t *prefix,uint32_t plen,
 const uint8_t *start,uint32_t slen,int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *arg) {
 (void)kv;assert(plen==11);
 unsigned sh=((unsigned)prefix[0]<<8)|prefix[1];
 int islocal=sh==efs_kv_inode_shard(800);
 if(islocal&&schedule==1&&!fired)migrate();
 uint8_t key[EFS_KV_KEY_MAX];uint32_t kl=0;
 assert(!efs_kv_key_dentry(sh,800,name,key,&kl));
 int exists=islocal?local_exists:sh==efs_kv_lane_shard(800,lane)&&hashed_exists;
 int rc=0;
 if(exists&&(!slen||memcmp(key,start,kl<slen?kl:slen)>0))
  rc=cb(arg,key,kl,(const uint8_t *)(islocal?&local:&hashed),sizeof(local));
 if(islocal&&schedule==2&&!fired)migrate();
 return rc;
}
static const struct efs_kv_ops ops={.scan_from=scan_from};
''' + source[start:end] + r'''
static void reset(unsigned layout,int when) {
 memset(&row,0,sizeof(row));row.ino=800;row.generation=1;row.mode=S_IFDIR|0755;row.layout=layout;
 row.used_shards=0;local=(struct efs_meta_dentry){.ino=42,.generation=1,.type=S_IFREG};hashed=local;
 local_exists=1;hashed_exists=0;schedule=when;fired=refresh_error=row_reads=0;
 lane=efs_kv_dir_lane(name);assert(lane==17);
}
static unsigned collect(unsigned max,efs_ino_t *ino) {
 struct efs_kv kv={.ops=&ops};struct efs_meta_dir_cursor c={0};struct efs_meta_dir_ent out[64];unsigned unique=0,calls=0;
 do {uint32_t n=0;assert(!efs_meta_apply_readdir(&kv,800,&c,out,max,&n));
  for(unsigned i=0;i<n;i++){assert(!strcmp(out[i].name,name));unique=1;*ino=out[i].d.ino;}
  assert(++calls<130);
 }while(!c.done);
 return unique;
}
int main(void) {
 efs_ino_t ino=0;
 /* Cross-group migration publishes the destination before the parent-group
  * bitmap/DEL. The inode layout can stay unchanged for the entire listing.
  * An end-of-page bitmap recheck alone therefore cannot close this window. */
 reset(EFS_META_LAYOUT_SPLITTING,0);hashed_exists=1;
 assert(collect(64,&ino)==1&&ino==42);
#ifdef ONLY_LATE_DEST
 return 0;
#endif
 /* Move before local scan, after it, or during the authoritative hash lookup.
  * No writer creates or deletes a name: migration alone must never lose it. */
 for(unsigned layout=EFS_META_LAYOUT_LOCAL;layout<=EFS_META_LAYOUT_SPLITTING;layout++) {
  if(layout==EFS_META_LAYOUT_HASHED)continue;
  for(int when=1;when<=3;when++)for(unsigned max=1;max<=64;max+=63) {
   reset(layout,when);
   /* A LOCAL row has no hashed-side query until it observes the transition. */
   if(layout==EFS_META_LAYOUT_LOCAL&&when==3)continue;
   assert(collect(max,&ino)==1&&ino==42&&fired);
  }
 }
 reset(EFS_META_LAYOUT_SPLITTING,3);hashed.type=EFS_META_DENT_TOMBSTONE;assert(!collect(64,&ino));
 reset(EFS_META_LAYOUT_SPLITTING,3);hashed.ino=99;hashed.generation=2;assert(collect(64,&ino)==1&&ino==99);
 reset(EFS_META_LAYOUT_SPLITTING,0);row.used_shards=1ULL<<lane;hashed_exists=1;local_exists=0;assert(collect(64,&ino)==1);
 reset(EFS_META_LAYOUT_LOCAL,0);refresh_error=EFS_ERR_IO;
 struct efs_kv kv={.ops=&ops};struct efs_meta_dir_cursor c={0};struct efs_meta_dir_ent out[64];uint32_t n=99;
 assert(efs_meta_apply_readdir(&kv,800,&c,out,64,&n)==EFS_ERR_IO&&n==0);
 puts("READDIR migration: first-use lanes, layout transition, pagination, hashed replacement/tombstone, refresh error PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-readdir-migration-') as directory:
    path = Path(directory) / 'test.c'
    path.write_text(code)
    cmd = shlex.split(os.environ.get('CC', 'cc'))
    cmd += ['-std=gnu11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror']
    cmd += shlex.split(os.environ.get('READDIR_TEST_CFLAGS', ''))
    cmd += ['-I'+str(root/'include'), str(path), str(root/'src/kv/kv_key.c'), '-o', str(path.with_suffix(''))]
    subprocess.run(cmd, check=True)
    subprocess.run([str(path.with_suffix(''))], check=True, timeout=15)
