#!/usr/bin/env python3
"""Committed memtable merge: byte-model equivalence and every allocation refusal."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'src/kv/kv_lsm.c').read_text()
section = source.split('/* --- memtable ', 1)[1].split('/* --- live set ', 1)[0]
section = section[section.index('static void ent_free'):]
pre = r'''
#include "kv_lsm_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static long refuse=-1, allocations;
static size_t moved;
static int denied(void) { return refuse>=0 && allocations++==refuse; }
static void *probe_malloc(size_t n) {return denied()?NULL:malloc(n);}
static void *probe_calloc(size_t n,size_t s) {return denied()?NULL:calloc(n,s);}
static void *probe_realloc(void *p,size_t n) {return denied()?NULL:realloc(p,n);}
static void *probe_move(void *d,const void *s,size_t n) {moved+=n;return memmove(d,s,n);}
#define malloc probe_malloc
#define calloc probe_calloc
#define realloc probe_realloc
#undef memmove
#define memmove probe_move
'''
post = r'''
static void equal(const struct kv_mtab *a,const struct kv_mtab *b) {
 assert(a->n==b->n && a->bytes==b->bytes);
 for(uint32_t i=0;i<a->n;i++) {
  const struct kv_ent *x=a->e[i],*y=b->e[i];
  assert(x->op==y->op && x->klen==y->klen && x->vlen==y->vlen);
  assert(!memcmp(x->key,y->key,x->klen));
  if(x->vlen)assert(!memcmp(x->val,y->val,x->vlen));
  assert(a->pfx[i]==b->pfx[i]);
  int found;assert(kv_mtab_pos(a,x->key,x->klen,&found)==i && found);
  if(i)assert(kv_key_cmp(a->e[i-1]->key,a->e[i-1]->klen,x->key,x->klen)<0);
 }
}
static uint32_t rnd=42;
static uint32_t next(void) {rnd=rnd*1664525u+1013904223u;return rnd;}
int main(void) {
 struct kv_mtab a={0},b={0};
 uint8_t keys[16][10],values[16][8];struct efs_kv_item items[16];
 for(unsigned round=0;round<2000;round++) {
  unsigned n=round%17;
  for(unsigned i=0;i<n;i++) {
   unsigned k=next()%256;memset(keys[i],0,sizeof(keys[i]));
   keys[i][8]=(uint8_t)k;keys[i][9]=(uint8_t)(k>>8);
   memset(values[i],(uint8_t)next(),sizeof(values[i]));
   items[i]=(struct efs_kv_item){next()%3?EFS_KV_PUT:EFS_KV_DEL,
      keys[i],round%5?10u:9u,values[i],next()%9};
   assert(!kv_mtab_set(&b,items[i].op==EFS_KV_PUT?KV_OP_PUT:KV_OP_DEL,
      items[i].key,items[i].klen,items[i].val,items[i].vlen));
  }
  assert(!kv_mtab_batch(&a,items,n));equal(&a,&b);
 }
 kv_mtab_clear(&a);kv_mtab_clear(&b);
 /* Force capacity growth plus duplicates, tombstones and existing updates.
  * Refuse each allocation in turn; failure must preserve the entire model. */
 unsigned failures=0;
 for(long fail=0;fail<100;fail++) {
  for(unsigned i=0;i<64;i++) {
   uint8_t k=(uint8_t)(i*2);
   assert(!kv_mtab_set(&a,KV_OP_PUT,&k,1,(uint8_t *)"old",3));
   assert(!kv_mtab_set(&b,KV_OP_PUT,&k,1,(uint8_t *)"old",3));
  }
  for(unsigned i=0;i<16;i++) {
   keys[i][0]=(uint8_t)(i<12?i:i-12);
   items[i]=(struct efs_kv_item){i%3?EFS_KV_PUT:EFS_KV_DEL,keys[i],1,(uint8_t *)"new",3};
  }
  allocations=0;refuse=fail;int rc=kv_mtab_batch(&a,items,16);refuse=-1;
  if(rc) {assert(rc==EFS_ERR_NOMEM);equal(&a,&b);failures++;}
  else {
   for(unsigned i=0;i<16;i++)assert(!kv_mtab_set(&b,items[i].op==EFS_KV_PUT?KV_OP_PUT:KV_OP_DEL,
      items[i].key,1,items[i].val,3));
   equal(&a,&b);
  }
  kv_mtab_clear(&a);kv_mtab_clear(&b);if(!rc)break;
 }
 assert(failures>20);

 /* Owned insertion buffers retain both tables on capacity refusal. */
 for(long fail=0;fail<3;fail++) {
  struct kv_mtab pending={0},saved={0};
  for(unsigned i=0;i<64;i++) {uint8_t k=(uint8_t)(i*2);
   assert(!kv_mtab_set(&a,KV_OP_PUT,&k,1,(uint8_t *)"old",3));
   assert(!kv_mtab_set(&b,KV_OP_PUT,&k,1,(uint8_t *)"old",3));}
  for(unsigned i=0;i<12;i++) {uint8_t k=(uint8_t)i;
   assert(!kv_mtab_set(&pending,KV_OP_PUT,&k,1,(uint8_t *)"new",3));
   assert(!kv_mtab_set(&saved,KV_OP_PUT,&k,1,(uint8_t *)"new",3));}
  allocations=0;refuse=fail;int rc=kv_mtab_merge(&a,&pending);refuse=-1;
  if(rc){assert(rc==EFS_ERR_NOMEM);equal(&a,&b);equal(&pending,&saved);}
  else {for(unsigned i=0;i<saved.n;i++) {struct kv_ent *e=saved.e[i];
   assert(!kv_mtab_set(&b,e->op,e->key,e->klen,e->val,e->vlen));}
   equal(&a,&b);assert(!pending.n && !pending.bytes && !pending.e && !pending.pfx);}
  kv_mtab_clear(&a);kv_mtab_clear(&b);kv_mtab_clear(&pending);kv_mtab_clear(&saved);
 }
 /* A batch at the front must not shift the large suffix once per key. */
 for(unsigned i=0;i<4096;i++) {
  uint8_t k[4]={1,(uint8_t)(i>>8),(uint8_t)i,0};
  assert(!kv_mtab_set(&a,KV_OP_PUT,k,4,NULL,0));
  assert(!kv_mtab_set(&b,KV_OP_PUT,k,4,NULL,0));
 }
 for(unsigned i=0;i<16;i++) {
  keys[i][0]=0;keys[i][1]=(uint8_t)i;
  items[i]=(struct efs_kv_item){EFS_KV_PUT,keys[i],2,NULL,0};
 }
 moved=0;assert(!kv_mtab_batch(&a,items,16));size_t batch=moved;
 moved=0;
 for(unsigned i=0;i<16;i++)assert(!kv_mtab_set(&b,KV_OP_PUT,keys[i],2,NULL,0));
 assert(batch<=moved/16);equal(&a,&b);
 kv_mtab_clear(&a);kv_mtab_clear(&b);
 /* Broadly distributed insertions, comparing per-entry and buffered merges. */
 struct kv_mtab delta={0};uint8_t broad[32][4];struct efs_kv_item inserts[32];
 for(unsigned i=0;i<8192;i++){unsigned k=i*2;uint8_t kb[4]={0,0,(uint8_t)(k>>8),(uint8_t)k};
  assert(!kv_mtab_set(&a,KV_OP_PUT,kb,4,NULL,0));
  assert(!kv_mtab_set(&b,KV_OP_PUT,kb,4,NULL,0));}
 size_t ordinary=0,buffered=0;
 for(unsigned r=0;r<128;r++) {
  for(unsigned j=0;j<32;j++){unsigned k=(next()%8192)*2+1;
   broad[j][0]=broad[j][1]=0;broad[j][2]=(uint8_t)(k>>8);broad[j][3]=(uint8_t)k;
   inserts[j]=(struct efs_kv_item){EFS_KV_PUT,broad[j],4,NULL,0};}
  moved=0;assert(!kv_mtab_batch(&a,inserts,32));ordinary+=moved;
  moved=0;assert(!kv_mtab_batch(&delta,inserts,32));
  if(delta.n>=1024)assert(!kv_mtab_merge(&b,&delta));
  buffered+=moved;
 }
 moved=0;assert(!kv_mtab_merge(&b,&delta));buffered+=moved;
 equal(&a,&b);assert(buffered<ordinary/4);
 printf("distributed insertions: pointer/prefix bytes moved %zu -> %zu PASS\n",ordinary,buffered);
 kv_mtab_clear(&a);kv_mtab_clear(&b);
 printf("memtable batch: 2000 mixed binary-key model batches, %u allocation refusals, bounded suffix movement PASS\n",failures);
}
'''
with tempfile.TemporaryDirectory(prefix='efs-mtab-batch-') as d:
    c=Path(d)/'test.c'; binary=Path(d)/'test'; c.write_text(pre+section+post)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-O2','-I'+str(root/'include'),'-I'+str(root/'src/kv'),str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
