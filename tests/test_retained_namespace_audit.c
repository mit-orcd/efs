/* Isolated real-LSM audit fixtures; never opens an installed store. */
#include "retained_namespace_audit.h"
#include "efs/txn.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "test_tmpdir.h"
static int cancel(void *ctx){(void)ctx;return 1;}
static struct retained_audit audit(struct efs_kv *kv,efs_ino_t ino) {
    struct efs_kv_lsm_snapshot *s=NULL;assert(!efs_kv_lsm_snapshot_pin(kv,8<<20,&s));
    struct retained_audit r;int rc=retained_namespace_audit(s,ino,10000,NULL,NULL,&r); if(rc) fprintf(stderr,"audit ino=%llu rc=%d records=%llu\n",(unsigned long long)ino,rc,(unsigned long long)r.records); assert(!rc);
    assert(r.local_scan_complete && r.ownership_inconclusive && !r.repair_eligible && !r.distributed_coherence_proven);
    efs_kv_lsm_snapshot_release(s);return r;
}
/* Lower-level transaction retirement model, not the full POSIX host path.
 * A half-resolved committed transaction can look like the retained graph.
 * Pending ownership must therefore remain inconclusive across cold reopen. */
static struct efs_kv *partial_replacement(struct efs_kv *kv,const char *path,
    const struct efs_kv_lsm_cfg *cfg,const struct efs_meta_attrs *at) {
    efs_ino_t parent,source,victim;
    assert(!efs_meta_apply_mkdir(kv,at,1,S_IFDIR|0755,"txn-parent",&parent));
    assert(!efs_meta_apply_mkdir(kv,at,parent,S_IFDIR|0755,"source",&source));
    assert(!efs_meta_apply_mkdir(kv,at,parent,S_IFDIR|0755,"target",&victim));
    uint8_t src[EFS_KV_KEY_MAX],dst[EFS_KV_KEY_MAX],dead[EFS_KV_KEY_MAX],pk[EFS_KV_KEY_MAX];
    uint32_t sn,dn,kn,pn;uint32_t ps=efs_kv_inode_shard(parent),vs=efs_kv_inode_shard(victim);
    assert(ps!=vs);assert(!efs_kv_key_dentry(ps,parent,"source",src,&sn));
    assert(!efs_kv_key_dentry(ps,parent,"target",dst,&dn));
    assert(!efs_kv_key_inode(vs,victim,dead,&kn));assert(!efs_kv_key_inode(ps,parent,pk,&pn));
    struct efs_meta_dentry dent;assert(!efs_meta_apply_lookup(kv,parent,"source",&dent));
    uint8_t value[EFS_META_DENT_BYTES];assert(!efs_meta_pack_dentry(&dent,value,sizeof(value)));
    struct efs_txid tx={{2}};struct efs_txn_parts parts={.n=2,.shard={ps,vs}};
    uint64_t ver;assert(!efs_txn_ver_get(kv,src,sn,&ver));
    assert(!efs_txn_prepare_excl(kv,&tx,&parts,src,sn,ver,EFS_TXN_DEL,NULL,0));
    assert(!efs_txn_ver_get(kv,dst,dn,&ver));
    assert(!efs_txn_prepare_excl(kv,&tx,&parts,dst,dn,ver,EFS_TXN_PUT,value,sizeof(value)));
    assert(!efs_txn_ver_get(kv,dead,kn,&ver));
    assert(!efs_txn_prepare_excl(kv,&tx,&parts,dead,kn,ver,EFS_TXN_DEL,NULL,0));
    struct efs_txn_ino_delta delta={.d_nlink=-1,.d_nents=-1};
    assert(!efs_txn_prepare_ino_delta(kv,&tx,&parts,pk,pn,&delta));
    assert(audit(kv,victim).binding_inconclusive);
    assert(!efs_txn_decide(kv,efs_txn_coordinator(&tx,&parts),&tx,EFS_TXN_COMMIT));
    assert(!efs_txn_resolve(kv,&tx,ps,EFS_TXN_COMMIT));
    struct retained_audit r=audit(kv,victim);
    assert(r.inode.nlink==2 && r.binding_inconsistent && r.binding_inconclusive && !r.authoritative_backlinks);
    efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(path,cfg);assert(kv);
    r=audit(kv,victim);assert(r.binding_inconsistent && r.binding_inconclusive && r.kinds[EFS_KV_KIND_INTENT]);
    assert(!efs_txn_resolve(kv,&tx,vs,EFS_TXN_COMMIT));
    struct efs_meta_row row;assert(efs_meta_apply_get_inode(kv,victim,&row)==EFS_ERR_NOT_FOUND);
    r=audit(kv,source);assert(!r.binding_inconsistent && !r.binding_inconclusive && r.authoritative_backlinks==1);
    puts("partial committed replacement: live orphan shape stays inconclusive across cold reopen, final victim retirement restores graph PASS");
    return kv;
}
int main(void) {
    char path[EFS_TEST_PATH];efs_test_path(path,sizeof(path),"efs-retained-audit-XXXXXX");assert(efs_test_mkdtemp(path));
    struct efs_kv_lsm_cfg cfg={0,0,4,0};struct efs_kv *kv=efs_kv_lsm_open(path,&cfg);assert(kv);
    struct efs_meta_attrs at={1000,1000,100};efs_ino_t parent,child,replacement;
    assert(!efs_meta_apply_init(kv,100));
    assert(!efs_meta_apply_mkdir(kv,&at,1,S_IFDIR|0755,"parent",&parent));
    assert(!efs_meta_apply_mkdir(kv,&at,parent,S_IFDIR|0755,"level_4",&child));
    struct efs_meta_row original; assert(!efs_meta_apply_get_inode(kv,child,&original));
    struct retained_audit r=audit(kv,child);assert(r.authoritative_backlinks==1 && !r.binding_inconsistent && !r.children);
    efs_ino_t nested; assert(!efs_meta_apply_mkdir(kv,&at,child,S_IFDIR|0755,"nested",&nested));
    r=audit(kv,child); assert(r.children==1 && r.directory_children==1 && r.inode.nlink==3 && !r.binding_inconsistent);
    assert(!efs_meta_apply_rmdir(kv,child,"nested",100));
    struct efs_kv_lsm_snapshot *old=NULL;assert(!efs_kv_lsm_snapshot_pin(kv,8<<20,&old));
    assert(!efs_meta_apply_mkdir(kv,&at,parent,S_IFDIR|0755,"replacement",&replacement));
    /* This direct apply API deliberately refuses replacement; don't confuse
     * it with the host's POSIX replacement transaction path. */
    assert(efs_meta_apply_rename(kv,parent,"replacement",parent,"level_4",101)==EFS_ERR_EXIST);
    assert(!audit(kv,child).binding_inconsistent);
    assert(!efs_meta_apply_rmdir(kv,parent,"level_4",102));
    assert(!efs_meta_apply_rename(kv,parent,"replacement",parent,"level_4",103));
    struct efs_meta_row retired; assert(efs_meta_apply_get_inode(kv,child,&retired)==EFS_ERR_NOT_FOUND);
    assert(!audit(kv,replacement).binding_inconsistent);
    struct retained_audit historical;assert(!retained_namespace_audit(old,child,10000,NULL,NULL,&historical));
    assert(historical.authoritative_backlinks==1 && historical.inode.nlink==2);
    assert(retained_namespace_audit(old,child,1,NULL,NULL,&historical)==EFS_ERR_OVERFLOW && !historical.local_scan_complete);
    assert(retained_namespace_audit(old,child,10000,cancel,NULL,&historical)==EFS_ERR_BUSY && !historical.local_scan_complete);
    efs_kv_lsm_snapshot_release(old);
    efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(path,&cfg);assert(kv);
    r=audit(kv,replacement);assert(!r.binding_inconsistent && r.authoritative_backlinks==1);
    /* Synthetic corruption exactly separates wrong-name live replacement
     * from a retired row. This is a negative fixture, not a writer cause. */
    struct efs_meta_row row=original;row.nlink=2;
    uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_INO_BYTES];uint32_t kn;
    assert(!efs_kv_key_inode(efs_kv_inode_shard(child),child,key,&kn));
    assert(!efs_meta_pack_inode(&row,value,sizeof(value)));
    assert(!efs_kv_put(kv,key,kn,value,sizeof(value)));
    r=audit(kv,child);assert(r.binding_inconsistent && !r.raw_backlinks && !r.authoritative_backlinks);
    struct efs_txid tx={{1}};struct efs_txn_parts parts={.n=1,.shard={efs_kv_inode_shard(child)}};
    uint64_t ver;assert(!efs_txn_ver_get(kv,key,kn,&ver));assert(!efs_txn_prepare_guard(kv,&tx,&parts,key,kn,ver));
    r=audit(kv,child);assert(r.binding_inconclusive && r.kinds[EFS_KV_KIND_GUARD]);
    assert(!efs_txn_resolve(kv,&tx,parts.shard[0],EFS_TXN_ABORT));
    efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(path,&cfg);assert(kv);
    r=audit(kv,child);assert(r.binding_inconsistent && !r.binding_inconclusive);
    printf("readonly real-LSM audit: parent=%llu child=%llu replacement=%llu records=%llu lineage=%llu/%llu; cold graph/ownership inconclusive and no repair PASS\n",
      (unsigned long long)parent,(unsigned long long)child,(unsigned long long)replacement,(unsigned long long)r.records,
      (unsigned long long)r.lineage[0],(unsigned long long)r.lineage[1]);
    kv=partial_replacement(kv,path,&cfg,&at);
    efs_kv_lsm_close(kv);return 0;
}
