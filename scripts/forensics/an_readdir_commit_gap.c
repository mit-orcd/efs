/* Diagnostic for a committed DIR migration with one participant materialized.
 * Uses production metadata/transaction APIs, no network, no original store.
 * Exit 1 means enumeration omitted a logically committed name; it is a FAIL,
 * not proof that this exact interleaving caused a historical live omission.
 * Build against the selected tree's libefs.a; all payloads are synthetic. */
#include "efs/meta_apply.h"
#include "efs/dir_layout.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/txn.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int coord(void *ctx,const struct efs_txid *t,uint32_t shard,int *decision)
{
    return efs_txn_decision_get(ctx,shard,t,decision);
}

static unsigned listing(struct efs_kv *kv,int *found)
{
    struct efs_meta_dir_cursor cursor={0};
    char names[128][EFS_MAX_NAME];unsigned count=0,pages=0;
    *found=0;
    do {
        struct efs_meta_dir_ent page[64];uint32_t n=0;
        int rc=efs_meta_apply_readdir(kv,EFS_ROOT_INO,&cursor,page,64,&n);
        assert(rc==EFS_OK);
        for(uint32_t i=0;i<n;i++) {
            unsigned j=0;
            while(j<count && strcmp(names[j],page[i].name))j++;
            if(j==count) {
                assert(count<128);
                snprintf(names[count++],EFS_MAX_NAME,"%s",page[i].name);
            }
            if(!strcmp(page[i].name,"f0064"))*found=1;
        }
        assert(++pages<1000);
    } while(!cursor.done);
    return count;
}

int main(void)
{
    struct efs_kv *kv=efs_kv_mem_create();assert(kv);
    const struct efs_meta_attrs attrs={1000,1000,1000000000000000000ull};
    assert(efs_meta_apply_init(kv,attrs.now)==EFS_OK);
    for(unsigned i=0;i<96;i++) {
        char name[16];efs_ino_t ino=0;
        if(i==65)assert(efs_meta_dir_begin_split(kv,EFS_ROOT_INO)==EFS_OK);
        snprintf(name,sizeof name,"f%04u",i);
        assert(efs_meta_apply_create_file(kv,&attrs,EFS_ROOT_INO,S_IFREG|0644,name,&ino)==EFS_OK);
    }
    for(unsigned i=0;;i++) {
        char first[EFS_MAX_NAME];uint32_t shard=0;int lane0=0;
        assert(i<96);
        assert(efs_meta_dir_migrate_peek(kv,EFS_ROOT_INO,first,sizeof first,&shard,&lane0)==EFS_OK);
        if(!strcmp(first,"f0064"))break;
        assert(efs_meta_dir_migrate_one(kv,EFS_ROOT_INO)==EFS_OK);
    }
    int found=0;assert(listing(kv,&found)==96 && found);
    uint32_t psh=efs_kv_inode_shard(EFS_ROOT_INO);
    uint32_t hsh=efs_kv_dentry_shard(EFS_ROOT_INO,"f0064",EFS_META_LAYOUT_HASHED);
    assert(psh!=hsh);
    uint8_t local[EFS_KV_KEY_MAX],hashed[EFS_KV_KEY_MAX],dseq[EFS_KV_KEY_MAX],value[EFS_META_DENT_BYTES];
    uint32_t kl=0,kh=0,ks=0,bytes=sizeof value;
    assert(efs_kv_key_dentry(psh,EFS_ROOT_INO,"f0064",local,&kl)==EFS_OK);
    assert(efs_kv_key_dentry(hsh,EFS_ROOT_INO,"f0064",hashed,&kh)==EFS_OK);
    assert(efs_kv_key_dseq(hsh,EFS_ROOT_INO,efs_kv_dir_lane("f0064"),dseq,&ks)==EFS_OK);
    assert(efs_kv_get(kv,local,kl,value,&bytes)==EFS_OK);
    uint64_t lv=0,hv=0;assert(efs_txn_ver_get(kv,local,kl,&lv)==EFS_OK);
    assert(efs_txn_ver_get(kv,hashed,kh,&hv)==EFS_OK);
    struct efs_txid t={{0x41,0x4e,0x48,0x32}};
    struct efs_txn_parts parts={.n=2,.shard={psh,hsh}};
    if(parts.shard[0]>parts.shard[1]){parts.shard[0]=hsh;parts.shard[1]=psh;}
    assert(efs_txn_prepare_excl(kv,&t,&parts,local,kl,lv,EFS_TXN_DEL,NULL,0)==EFS_OK);
    assert(efs_txn_prepare_excl(kv,&t,&parts,hashed,kh,hv,EFS_TXN_PUT,value,bytes)==EFS_OK);
    assert(efs_txn_prepare_add(kv,&t,&parts,dseq,ks,1)==EFS_OK);
    assert(efs_txn_decide(kv,efs_txn_coordinator(&t,&parts),&t,EFS_TXN_COMMIT)==EFS_OK);
    assert(efs_txn_resolve(kv,&t,psh,EFS_TXN_COMMIT)==EFS_OK);
    struct efs_meta_dentry dent;
    int lookup=efs_meta_apply_lookup_tx(kv,EFS_ROOT_INO,"f0064",coord,kv,&dent);
    unsigned partial=listing(kv,&found);
    printf("committed migration: lookup_rc=%d names=%u f0064=%d source_resolved=1 destination_resolved=0\n",lookup,partial,found);
    int omitted=lookup!=EFS_OK || partial!=96 || !found;
    assert(efs_txn_resolve(kv,&t,hsh,EFS_TXN_COMMIT)==EFS_OK);
    unsigned finished=listing(kv,&found);
    printf("both participants resolved: names=%u f0064=%d\n",finished,found);
    assert(finished==96 && found);
    efs_kv_mem_free(kv);
    return omitted?1:0;
}
