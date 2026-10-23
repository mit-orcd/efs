/* Exercise buffered/main overlap through real get, ordered scan, snapshot,
 * flush/WAL reset and cold replay. No sockets or cluster required. */
#include "efs/common.h"
#include "efs/kv.h"
#include "efs/kv_lsm.h"
#include "../src/kv/kv_lsm_internal.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_tmpdir.h"
#define KEYS 8192
static uint8_t key[KEYS][8],present[KEYS],frozen_present[KEYS];
static uint64_t value[KEYS],frozen_value[KEYS];
static uint32_t rng=42;
static uint32_t next(void){rng=rng*1664525u+1013904223u;return rng;}
static void be32(uint8_t *p,uint32_t n){for(int i=3;i>=0;i--){p[i]=(uint8_t)n;n>>=8;}}
static uint32_t rd32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
struct scan {uint8_t last[8];int have;unsigned count;int frozen;};
static int check_row(void *ctx,const uint8_t *k,uint32_t kl,const uint8_t *v,uint32_t vl)
{
 struct scan *s=ctx;assert(kl==8 && vl==8);uint32_t i=rd32(k+4);assert(i<KEYS);
 uint64_t got;memcpy(&got,v,8);
 assert((s->frozen?frozen_present:present)[i] && got==(s->frozen?frozen_value:value)[i]);
 assert(!s->have || memcmp(s->last,k,8)<0);memcpy(s->last,k,8);s->have=1;s->count++;return 0;
}
static void check(struct efs_kv *kv)
{
 unsigned expected=0;
 for(unsigned i=0;i<KEYS;i++) {uint64_t got=0;uint32_t n=8;int rc=efs_kv_get(kv,key[i],8,(uint8_t *)&got,&n);
  if(present[i]){assert(!rc && n==8 && got==value[i]);expected++;}else assert(rc==EFS_ERR_NOT_FOUND);}
 struct scan s={0};assert(!efs_kv_scan(kv,check_row,&s) && s.count==expected);
 /* Seek/prefix scans must see a newer buffered tombstone or replacement. */
 for(unsigned i=0;i<KEYS;i+=257){s=(struct scan){0};
  assert(!efs_kv_scan_prefix(kv,key[i],8,check_row,&s));
  assert(s.count==(unsigned)present[i]);
  s=(struct scan){0};assert(!efs_kv_scan_from(kv,key[i],8,key[i],8,check_row,&s));
  assert(s.count==(unsigned)present[i]);}
}
static void changes(struct efs_kv *kv,unsigned rounds)
{
 for(unsigned r=0;r<rounds;r++) {
  struct efs_kv_item items[32];unsigned ids[32];uint64_t vals[32];
  for(unsigned j=0;j<32;j++){unsigned i=next()%KEYS;ids[j]=i;vals[j]=(uint64_t)r<<32|j;
   items[j]=(struct efs_kv_item){j%4?EFS_KV_PUT:EFS_KV_DEL,key[i],8,(uint8_t *)&vals[j],8};}
  assert(!efs_kv_batch(kv,items,32));
  for(unsigned j=0;j<32;j++){present[ids[j]]=items[j].op==EFS_KV_PUT;value[ids[j]]=vals[j];}
  if(r%31==0)check(kv);
 }
}
int main(void)
{
 char dir[EFS_TEST_PATH];efs_test_path(dir,sizeof(dir),"efs-kv-insertion-XXXXXX");assert(efs_test_mkdtemp(dir));
 struct efs_kv_lsm_cfg cfg={.sync_mode=EFS_KV_LSM_SYNC,.memtable_max=32u<<20,.l0_max=4};
 struct efs_kv *kv=efs_kv_lsm_open(dir,&cfg);assert(kv);
 for(unsigned i=0;i<KEYS;i++){be32(key[i],i*2654435761u);be32(key[i]+4,i);}
 for(unsigned begin=0;begin<KEYS;begin+=32){struct efs_kv_item items[32];
  for(unsigned j=0;j<32;j++){unsigned i=begin+j;value[i]=i;present[i]=1;
   items[j]=(struct efs_kv_item){EFS_KV_PUT,key[i],8,(uint8_t *)&value[i],8};}
  assert(!efs_kv_batch(kv,items,32));}
 changes(kv,161);check(kv);
 struct kv_lsm *l=kv->ctx;assert(l->mt.n && l->pending_mt.n && l->pending_mt.n<1024);
 memcpy(frozen_present,present,sizeof(present));memcpy(frozen_value,value,sizeof(value));
 struct efs_kv_lsm_snapshot *snapshot=NULL;
 assert(efs_kv_lsm_snapshot_pin(kv,1024,&snapshot)==EFS_ERR_BUSY && !snapshot);
 assert(!efs_kv_lsm_snapshot_pin(kv,4u<<20,&snapshot));
 changes(kv,41);check(kv);struct scan s={.frozen=1};
 assert(!efs_kv_lsm_snapshot_scan(snapshot,NULL,0,NULL,0,check_row,&s));
 unsigned count=0;for(unsigned i=0;i<KEYS;i++)count+=frozen_present[i];assert(count==s.count);
 efs_kv_lsm_snapshot_release(snapshot);
 assert(!efs_kv_lsm_flush(kv));assert(!l->mt.n && !l->pending_mt.n);check(kv);
 changes(kv,3);assert(l->pending_mt.n);check(kv);
 efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(dir,&cfg);assert(kv);check(kv);efs_kv_lsm_close(kv);
 DIR *d=opendir(dir);assert(d);struct dirent *e;char path[1024];
 while((e=readdir(d)))if(strcmp(e->d_name,".") && strcmp(e->d_name,"..")){
  snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);assert(!unlink(path));}
 closedir(d);assert(!rmdir(dir));
 puts("insertion buffer: mixed binary keys, tombstones, ordered scans, snapshot budget/isolation, flush and cold WAL tail PASS");
}
