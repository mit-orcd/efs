#!/usr/bin/env python3
"""Cross-group migration keeps every directory name during partial RESOLVE.

Build libefs.a from the same source first. The actual host migration function
runs with synchronous PREPARE adapters backed by the real transaction layer.
The transport adapter pauses after either participant resolves, exercises
COMMIT/ABORT and replay, and injects stale source/tombstone captures. No synthetic read filters or alternate READDIR are used.

Default expects all96 names at that cut and FAILS on the demonstrated defect.
--expect-gap is a negative control: success means the95-name gap reproduced,
not that the product passed. Never run this against a preserved store.
"""
import argparse
import os
from pathlib import Path
import resource
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
parser.add_argument('--expect-gap', action='store_true')
args = parser.parse_args()
# The unchanged-source negative control deliberately trips a C assertion.
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = args.root.resolve()
source = (root / 'src/server/raft_host.c').read_text()
start = source.index('static int host_dir_migrate_txn(')
end = source.index('\nstatic int host_dir_migrate(', start)
head = r'''

#include <efs/meta_apply.h>
#include <efs/dir_layout.h>
#include <efs/txn.h>
#include <efs/kv_key.h>
#include <efs/metadata.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
struct efs_raft_host {struct efs_kv *kv;};
static unsigned seen_count;static char target[32];static uint32_t source_shard;static int dest_first,decision,tombstone,mutate,cleanup;
static unsigned wanted(void){return cleanup==2?95:96;}
static unsigned listing(struct efs_kv *kv){struct efs_meta_dir_cursor cur={0};struct efs_meta_dir_ent out[64];char seen[128][EFS_MAX_NAME];unsigned total=0,pages=0;
 do{uint32_t got=0;assert(!efs_meta_apply_readdir(kv,EFS_ROOT_INO,&cur,out,64,&got));for(unsigned i=0;i<got;i++){unsigned j;for(j=0;j<total;j++)if(!strcmp(seen[j],out[i].name))break;if(j==total){assert(total<128);strcpy(seen[total++],out[i].name);}}assert(++pages<100);}while(!cur.done);return total;}
static int coord(void *kv,const struct efs_txid*t,uint32_t sh,int*d){return efs_txn_decision_get(kv,sh,t,d);}
/* Match FUSE collection: source/name streams can duplicate a migrated name,
 * and the existing collector deduplicates names across every page. */
static unsigned listing_tx(struct efs_kv *kv){struct efs_meta_dir_cursor cur={0};struct efs_meta_dir_ent out[64];char seen[128][EFS_MAX_NAME];unsigned total=0,pages=0;
 do{uint32_t got=0,left=32768;assert(!efs_meta_apply_readdir_tx_bounded(kv,EFS_ROOT_INO,&cur,out,64,&got,&left,coord,kv));for(unsigned i=0;i<got;i++){unsigned j;for(j=0;j<total;j++)if(!strcmp(seen[j],out[i].name))break;if(j==total){assert(total<128);strcpy(seen[total++],out[i].name);}}assert(++pages<100);}while(!cur.done);return total;}
static int host_parts_add(struct efs_txn_parts *p,uint32_t sh){p->shard[p->n++]=sh;return EFS_OK;}
static void fill_txid(struct efs_raft_host*h,struct efs_txid*t){static unsigned next=1;(void)h;memset(t,0,sizeof(*t));t->bytes[0]=0x7f;unsigned id=next++;memcpy(t->bytes+8,&id,sizeof id);}
static int host_prep(struct efs_raft_host*h,uint32_t sh,int kind,const struct efs_txid*t,const struct efs_txn_parts*p,const uint8_t*k,uint32_t kl,uint64_t ver,int op,const uint8_t*v,uint32_t vl,int*hint){(void)sh;(void)hint;assert(kind==EFS_TXN_EXCL);return efs_txn_prepare_excl(h->kv,t,p,k,kl,ver,op,v,vl);}
static int host_prep_value(struct efs_raft_host*h,uint32_t sh,const struct efs_txid*t,const struct efs_txn_parts*p,const uint8_t*k,uint32_t kl,const uint8_t*expected,uint32_t en,int op,const uint8_t*v,uint32_t vl,int*hint){
 (void)hint;
 if(mutate==1&&sh==source_shard)assert(!efs_kv_del(h->kv,k,kl));
 if(mutate==2&&sh!=source_shard){uint8_t raw[EFS_META_DENT_BYTES];struct efs_meta_dentry dead={.ino=0,.type=EFS_META_DENT_TOMBSTONE};assert(!efs_meta_pack_dentry(&dead,raw,sizeof raw));assert(!efs_kv_put(h->kv,k,kl,raw,sizeof raw));}
 return efs_txn_prepare_excl_value(h->kv,t,p,k,kl,expected,en,op,v,vl);}
static int host_prep_ino_delta(struct efs_raft_host*h,uint32_t sh,const struct efs_txid*t,const struct efs_txn_parts*p,const uint8_t*k,uint32_t kl,const struct efs_txn_ino_delta*d,void*out,int*hint){(void)sh;(void)out;(void)hint;return efs_txn_prepare_ino_delta(h->kv,t,p,k,kl,d);}
static int host_prep_dseq_bump(struct efs_raft_host*h,uint32_t sh,const struct efs_txid*t,const struct efs_txn_parts*p,const uint8_t*k,uint32_t kl,void*out,int*hint){(void)sh;(void)out;(void)hint;return efs_txn_prepare_add(h->kv,t,p,k,kl,1);}
static int host_drop_parts(struct efs_raft_host*h,const struct efs_txid*t,const struct efs_txn_parts*p,int*hint){(void)hint;for(unsigned i=0;i<p->n;i++)assert(!efs_txn_resolve(h->kv,t,p->shard[i],EFS_TXN_ABORT));return EFS_OK;}
static int host_txn_commit(struct efs_raft_host*h,const struct efs_txid*t,const struct efs_txn_parts*p,uint32_t coord,int*hint){
 (void)hint;assert(listing(h->kv)==wanted());
 if(!cleanup&&EXPECTED==96)assert(efs_meta_dir_migrate_one(h->kv,EFS_ROOT_INO)==EFS_ERR_BUSY);
 assert(!efs_txn_decide_parts(h->kv,coord,t,p,decision));assert(listing_tx(h->kv)==wanted());
 uint32_t first=dest_first?p->shard[1]:source_shard,second=dest_first?source_shard:p->shard[1];
 assert(!efs_txn_resolve(h->kv,t,first,decision));seen_count=listing(h->kv);
 printf("decision=%d first=%s partial=%u/96 target=%s\n",decision,dest_first?"destination":"source",seen_count,target);fflush(stdout);
 assert(listing_tx(h->kv)==wanted());assert(seen_count==(cleanup?wanted():EXPECTED) || (EXPECTED==95&&decision==EFS_TXN_ABORT));
 if(!cleanup&&EXPECTED==96)assert(efs_meta_dir_migrate_one(h->kv,EFS_ROOT_INO)==EFS_ERR_BUSY);
 assert(!efs_txn_resolve(h->kv,t,second,decision));assert(listing(h->kv)==wanted());
 assert(!efs_txn_resolve(h->kv,t,first,decision));assert(listing(h->kv)==wanted());
 return EFS_OK;}


'''
tail = r'''

static void one_case(void){struct efs_kv *kv=efs_kv_mem_create();struct efs_raft_host h={kv};struct efs_meta_attrs at={1000,1000,1000000000000000000ull};efs_ino_t ino;assert(kv);assert(!efs_meta_apply_init(kv,at.now));source_shard=efs_kv_inode_shard(EFS_ROOT_INO);
 for(unsigned i=0;;i++){snprintf(target,sizeof(target),"a%05u",i);uint32_t sh=efs_kv_dentry_shard(EFS_ROOT_INO,target,EFS_META_LAYOUT_HASHED);if(efs_kv_dir_lane(target)==1&&(sh&1)!=(source_shard&1))break;}
 assert(!efs_meta_apply_create_file(kv,&at,EFS_ROOT_INO,S_IFREG|0644,target,&ino));
 for(unsigned i=0,made=0;made<95;i++){char nm[32];snprintf(nm,sizeof(nm),"z%05u",i);if(efs_kv_dir_lane(nm)!=63)continue;if(made==63)assert(!efs_meta_dir_begin_split(kv,EFS_ROOT_INO));assert(!efs_meta_apply_create_file(kv,&at,EFS_ROOT_INO,S_IFREG|0644,nm,&ino));made++;}
 assert(listing(kv)==96);uint32_t dest=efs_kv_dentry_shard(EFS_ROOT_INO,target,EFS_META_LAYOUT_HASHED);
 if(cleanup){uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_DENT_BYTES];uint32_t kl=0,vl=sizeof value;struct efs_meta_dentry row;
  assert(!efs_kv_key_dentry(source_shard,EFS_ROOT_INO,target,key,&kl));assert(!efs_kv_get(kv,key,kl,value,&vl));assert(!efs_meta_unpack_dentry(value,vl,&row));
  if(cleanup==2){row.ino=0;row.type=EFS_META_DENT_TOMBSTONE;}assert(!efs_meta_pack_dentry(&row,value,sizeof value));
  assert(!efs_kv_key_dentry(dest,EFS_ROOT_INO,target,key,&kl));assert(!efs_kv_put(kv,key,kl,value,sizeof value));
  if(cleanup<3){struct efs_meta_row row;uint8_t val[EFS_META_INO_BYTES];assert(!efs_meta_apply_get_inode(kv,EFS_ROOT_INO,&row));row.used_shards|=1ull<<efs_kv_dir_lane(target);assert(!efs_kv_key_inode(source_shard,EFS_ROOT_INO,key,&kl));assert(!efs_meta_pack_inode(&row,val,sizeof val));assert(!efs_kv_put(kv,key,kl,val,sizeof val));}
  assert(listing(kv)==wanted());}
 int hint=-1;int rc=host_dir_migrate_txn(&h,EFS_ROOT_INO,target,dest,&hint);
 if(mutate){assert(rc==EFS_ERR_STALE);if(mutate==1)assert(listing(kv)==95);else{assert(listing(kv)==95);uint8_t key[EFS_KV_KEY_MAX],raw[EFS_META_DENT_BYTES];uint32_t kl=0,vl=sizeof raw;struct efs_meta_dentry dead;assert(!efs_kv_key_dentry(dest,EFS_ROOT_INO,target,key,&kl));assert(!efs_kv_get(kv,key,kl,raw,&vl));assert(!efs_meta_unpack_dentry(raw,vl,&dead));assert(dead.type==EFS_META_DENT_TOMBSTONE);}printf("stale capture=%s refused without resurrection\n",mutate==1?"source deletion":"hashed tombstone");efs_kv_mem_free(kv);return;}
 assert(!rc);printf("after both resolves:96/96\n");
 if(!cleanup&&decision==EFS_TXN_COMMIT&&EXPECTED==96){
  if(tombstone){uint8_t key[EFS_KV_KEY_MAX],val[EFS_META_DENT_BYTES];uint32_t kl=0;struct efs_meta_dentry dead={.ino=0,.type=EFS_META_DENT_TOMBSTONE};assert(!efs_kv_key_dentry(dest,EFS_ROOT_INO,target,key,&kl));assert(!efs_meta_pack_dentry(&dead,val,sizeof val));assert(!efs_kv_put(kv,key,kl,val,sizeof val));}
  assert(!efs_meta_dir_migrate_one(kv,EFS_ROOT_INO));assert(listing(kv)==(tombstone?95:96));
  uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_DENT_BYTES];uint32_t kl=0,vl=sizeof value;assert(!efs_kv_key_dentry(source_shard,EFS_ROOT_INO,target,key,&kl));assert(efs_kv_get(kv,key,kl,value,&vl)==EFS_ERR_NOT_FOUND);
 }
 if(cleanup){uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_DENT_BYTES];uint32_t kl=0,vl=sizeof value;
  assert(!efs_kv_key_dentry(source_shard,EFS_ROOT_INO,target,key,&kl));int rc=efs_kv_get(kv,key,kl,value,&vl);
  assert(rc==(decision==EFS_TXN_COMMIT?EFS_ERR_NOT_FOUND:EFS_OK));assert(listing(kv)==wanted());assert(listing_tx(kv)==wanted());}
 efs_kv_mem_free(kv);}
int main(void){for(dest_first=0;dest_first<2;dest_first++)for(decision=EFS_TXN_COMMIT;decision<=EFS_TXN_ABORT;decision++)for(tombstone=0;tombstone<=(decision==EFS_TXN_COMMIT);tombstone++){if(EXPECTED==95&&(dest_first||decision!=EFS_TXN_COMMIT||tombstone))continue;one_case();}if(EXPECTED==96){decision=EFS_TXN_COMMIT;dest_first=tombstone=0;for(mutate=1;mutate<=2;mutate++)one_case();mutate=tombstone=0;
 for(cleanup=1;cleanup<=3;cleanup++)for(dest_first=0;dest_first<2;dest_first++)for(decision=EFS_TXN_COMMIT;decision<=EFS_TXN_ABORT;decision++)one_case();
 cleanup=1;dest_first=0;decision=EFS_TXN_COMMIT;mutate=2;one_case();}return 0;}

'''
with tempfile.TemporaryDirectory(prefix='efs-dir-migrate-gap-') as tmp:
    path = Path(tmp) / 'probe.c'
    path.write_text(head + source[start:end] + tail)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-O2', '-DEXPECTED=' + ('95' if args.expect_gap else '96'),
                    '-I' + str(root / 'include'), str(path), str(root / 'libefs.a'),
                    '-pthread', '-lm', '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True, timeout=30)
print('REPRODUCED GAP (negative control)' if args.expect_gap else
      'PASS: directory remains complete across participant resolution')
