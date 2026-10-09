/* Failed version reads must preserve both mutation and session guards. */
#include "efs/txn.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const struct efs_kv_ops *original;
static int injected;
static int fault_get(void *ctx,const uint8_t *key,uint32_t bytes,uint8_t *value,uint32_t *len)
{
 if(bytes>=3 && key[2]==EFS_KV_KIND_VER && injected)return injected;
 return original->get(ctx,key,bytes,value,len);
}
static int fault_batch(void *ctx,const struct efs_kv_item *items,uint32_t count)
{
 if(injected)return injected;
 return original->batch(ctx,items,count);
}
static void be(uint8_t out[8],uint64_t v){for(unsigned i=0;i<8;i++)out[i]=(uint8_t)(v>>(56-i*8));}
static void run(int error,int batch)
{
 struct efs_kv *kv=efs_kv_mem_create();assert(kv);uint8_t uuid[16]={81};
 struct efs_txid tx={{82}};struct efs_txn_parts parts={.n=1,.shard={1}};
 assert(!efs_session_create(kv,uuid,1));assert(!efs_session_register(kv,uuid,1,1));assert(!efs_session_establish(kv,1,uuid,1));
 struct efs_opid id={.session_epoch=1,.seq=1};memcpy(id.client_uuid,uuid,16);
 assert(!efs_session_prepare_namespace(kv,1,&id,&tx,&parts));
 uint8_t key[EFS_KV_KEY_MAX],vk[EFS_KV_KEY_MAX],old[8],fresh[8],version[8];uint32_t kl=0,vl=0;
 assert(!efs_kv_key_dseq(1,4097,0,key,&kl));assert(!efs_kv_key_ver(key,kl,vk,&vl));
 be(old,17);be(fresh,19);be(version,7);
 assert(!efs_kv_put(kv,key,kl,old,8));assert(!efs_kv_put(kv,vk,vl,version,8));
 assert(!efs_txn_prepare_excl(kv,&tx,&parts,key,kl,7,EFS_TXN_PUT,fresh,8));
 assert(!efs_session_begin_fence(kv,uuid));
 original=kv->ops;struct efs_kv_ops fault=*original;
 if(batch)fault.batch=fault_batch;else fault.get=fault_get;
 injected=error;kv->ops=&fault;
 int rc=efs_txn_resolve(kv,&tx,1,EFS_TXN_COMMIT);
 kv->ops=original;injected=0;
 if(rc!=error)fprintf(stderr,"fault=%d batch=%d resolve=%d expected=%d\n",error,batch,rc,error);
 assert(rc==error);
 uint8_t got[8];uint32_t n=8;assert(!efs_kv_get(kv,key,kl,got,&n) && !memcmp(got,old,8));
 uint64_t ver=0;assert(!efs_txn_ver_get(kv,key,kl,&ver) && ver==7);
 assert(efs_txn_key_exclusive(kv,key,kl)==EFS_ERR_BUSY);
 assert(efs_session_fence_local(kv,1,uuid,2)==EFS_ERR_BUSY);
 assert(!efs_txn_resolve(kv,&tx,1,EFS_TXN_COMMIT));
 n=8;assert(!efs_kv_get(kv,key,kl,got,&n) && !memcmp(got,fresh,8));
 assert(!efs_txn_ver_get(kv,key,kl,&ver) && ver==8);
 assert(!efs_txn_key_exclusive(kv,key,kl));assert(!efs_session_fence_local(kv,1,uuid,2));
 assert(!efs_txn_resolve(kv,&tx,1,EFS_TXN_COMMIT));
 assert(!efs_txn_ver_get(kv,key,kl,&ver) && ver==8);
 assert(efs_session_accept(kv,1,uuid,1)==EFS_ERR_STALE);
 efs_kv_mem_free(kv);
}
int main(void){int errors[]={EFS_ERR_IO,EFS_ERR_NOMEM,EFS_ERR_PROTO};
 for(unsigned i=0;i<3;i++)run(errors[i],0);
 for(unsigned i=0;i<2;i++)run(errors[i],1);
 puts("transaction resolve: version/batch failures preserve old row/version and epoch guard; exact retry commits once then permits revocation PASS");}
