#include "efs/expiry.h"
#include "efs/meta_apply.h"
#include "efs/session.h"
#include "efs/txn.h"
#include "efs/kv_lsm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include "test_tmpdir.h"
#define NS 1000000000ull
static void apply(struct efs_kv *kv,uint64_t ino,uint64_t revision,struct efs_expiry_record *r)
{struct efs_expiry_change c;assert(!efs_expiry_plan(kv,ino,1,revision,r,&c));assert(c.count<=3);assert(!efs_kv_batch(kv,c.items,c.count));}
struct failure {struct efs_kv *base;};
static int fault_get(void *ctx,const uint8_t *k,uint32_t n,uint8_t *v,uint32_t *vn)
{return efs_kv_get(((struct failure *)ctx)->base,k,n,v,vn);}
static int fault_scan(void *ctx,const uint8_t *k,uint32_t n,
                       int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *u)
{return efs_kv_scan_prefix(((struct failure *)ctx)->base,k,n,cb,u);}
static int fault_batch(void *ctx,const struct efs_kv_item *it,uint32_t n)
{(void)ctx;(void)it;(void)n;return EFS_ERR_IO;}
static void inheritance_gates(void)
{
 struct efs_kv *kv=efs_kv_mem_create();assert(kv);assert(!efs_meta_apply_init(kv,NS));
 struct efs_meta_attrs at={123,123,NS};efs_ino_t no_expiry,first,second,child=0;
 assert(!efs_meta_apply_create_expiring(kv,&at,1,S_IFREG|0600,"default-off",&no_expiry));
 struct efs_expiry_record record;assert(!efs_expiry_get(kv,no_expiry,1,&record));assert(!record.revision);
 struct efs_expiry_policy p={.ttl_ns=2*NS,.max_ns=20*NS,.enabled=1};
 assert(!efs_meta_apply_expiry_policy(kv,1,1,0,0,&p));
 assert(!efs_meta_apply_create_expiring(kv,&at,1,S_IFREG|0600,"first",&first));
 assert(!efs_expiry_get(kv,first,1,&record));assert(record.deadline_ns==3*NS && record.policy_revision==1);
 p.ttl_ns=4*NS;assert(!efs_meta_apply_expiry_policy(kv,1,1,0,1,&p));
 assert(!efs_meta_apply_create_expiring(kv,&at,1,S_IFREG|0600,"second",&second));
 assert(!efs_expiry_get(kv,second,1,&record));assert(record.deadline_ns==5*NS && record.policy_revision==2);
 assert(!efs_expiry_get(kv,first,1,&record));assert(record.deadline_ns==3*NS);
 for(unsigned i=0;i<100 && !child;i++) {
   char name[32];snprintf(name,sizeof(name),"child-%u",i);efs_ino_t ino;
   assert(!efs_meta_apply_mkdir(kv,&at,1,S_IFDIR|0700,name,&ino));if(ino&1)child=ino;
 }
 assert(child);efs_ino_t inherited;
 assert(!efs_meta_apply_create_expiring(kv,&at,child,S_IFREG|0600,"inherited",&inherited));
 assert(!efs_expiry_get(kv,inherited,1,&record));assert(record.root==1 && record.deadline_ns==5*NS);
 p.enabled=0;assert(!efs_meta_apply_expiry_policy(kv,child,1,0,0,&p));
 assert(!efs_meta_apply_create_expiring(kv,&at,child,S_IFREG|0600,"disabled-subtree",&no_expiry));
 assert(!efs_expiry_get(kv,no_expiry,1,&record));assert(!record.revision);
 struct failure f={kv};const struct efs_kv_ops ops={.get=fault_get,.scan_prefix=fault_scan,.batch=fault_batch};struct efs_kv failed={&ops,&f};
 assert(efs_meta_apply_create_expiring(&failed,&at,1,S_IFREG|0600,"failed",&no_expiry)==EFS_ERR_IO);
 struct efs_meta_dentry dent;assert(efs_meta_apply_lookup(kv,1,"failed",&dent)==EFS_ERR_NOT_FOUND);
 assert(!efs_meta_apply_rename(kv,1,"first",1,"renamed",2*NS));
 assert(!efs_expiry_get(kv,first,1,&record));assert(record.deadline_ns==3*NS && record.revision==2 && !strcmp(record.name,"renamed"));
 struct efs_expiry_due due={.deadline_ns=3*NS,.ino=first,.generation=1,.revision=1,.parent=1,.name="first"};
 assert(efs_meta_apply_expire(kv,1,"renamed",4*NS,&due)==EFS_ERR_STALE);
 due.revision=2;assert(!efs_meta_apply_expire(kv,1,"renamed",4*NS,&due));
 assert(!efs_meta_apply_reap_done(kv,first,1));assert(!efs_expiry_get(kv,first,1,&record));assert(!record.revision);
 assert(!efs_expiry_get(kv,second,1,&record));record.deadline_ns=0;apply(kv,second,record.revision,&record);
 assert(!efs_meta_apply_link(kv,1,"second",1,"cleared-alias",2*NS));
 assert(!efs_meta_apply_unlink(kv,1,"second",2*NS));
 assert(!efs_meta_apply_rename(kv,1,"cleared-alias",1,"cleared-renamed",2*NS));
 assert(!efs_meta_apply_check(kv));efs_kv_mem_free(kv);
}
static void namespace_gates(void)
{
 struct efs_kv *kv=efs_kv_mem_create();assert(kv);assert(!efs_meta_apply_init(kv,NS));
 struct efs_meta_attrs at={123,123,NS};efs_ino_t ino;
 assert(!efs_meta_apply_create_file(kv,&at,1,S_IFREG|0600,"a",&ino));
 struct efs_expiry_policy policy={.ttl_ns=NS,.max_ns=20*NS,.enabled=1};
 assert(efs_meta_apply_expiry_policy(kv,1,1,123,0,&policy)==EFS_ERR_ACCES);
 assert(!efs_meta_apply_expiry_policy(kv,1,1,0,0,&policy));
 assert(efs_meta_apply_expiry_policy(kv,ino,1,0,0,&policy)==EFS_ERR_INVAL);
 struct efs_expiry_record r={.deadline_ns=2*NS,.root=1,.root_generation=1,.policy_revision=1,.max_ns=20*NS,.parent=1,.name="a"};
 apply(kv,ino,0,&r);
 assert(efs_meta_apply_expiry_renew(kv,ino,1,456,1,NS,3*NS)==EFS_ERR_ACCES);
 assert(efs_meta_apply_expiry_renew(kv,ino,2,123,1,NS,3*NS)==EFS_ERR_STALE);

 struct efs_expiry_due due={.deadline_ns=2*NS,.ino=ino,.generation=1,.revision=1,.parent=1,.name="a"};
 assert(efs_meta_apply_expire(kv,1,"a",NS,&due)==EFS_ERR_STALE);
 assert(efs_meta_apply_link(kv,1,"a",1,"forbidden-link",NS)==EFS_ERR_INVAL);
 uint8_t record_key[19];efs_expiry_record_key(ino,1,record_key);
 uint8_t record_value[EFS_EXPIRY_RECORD_BYTES];uint32_t record_length=sizeof(record_value);
 assert(!efs_kv_get(kv,record_key,19,record_value,&record_length));
 struct efs_txid tx={{1}};struct efs_txn_parts parts={.n=1,.shard={1}};
 assert(!efs_txn_prepare_excl_value(kv,&tx,&parts,record_key,19,record_value,sizeof(record_value),EFS_TXN_PUT,record_value,sizeof(record_value)));
 struct efs_expiry_change blocked;
 assert(efs_expiry_plan(kv,ino,1,1,&r,&blocked)==EFS_ERR_BUSY);
 assert(efs_meta_apply_expire(kv,1,"a",4*NS,&due)==EFS_ERR_BUSY);
 assert(!efs_txn_drop(kv,&tx,1));

 r.deadline_ns=3*NS;apply(kv,ino,1,&r);
 assert(efs_meta_apply_expire(kv,1,"a",4*NS,&due)==EFS_ERR_STALE);
 due.deadline_ns=3*NS;due.revision=2;
 uint8_t uuid[16]={1};assert(!efs_session_establish(kv,efs_kv_inode_shard(ino),uuid,1)); /* K5 */
 assert(!efs_lease_open(kv,ino,1,uuid,1));
 struct failure f={kv};const struct efs_kv_ops ops={.get=fault_get,.scan_prefix=fault_scan,.batch=fault_batch};
 struct efs_kv failed={&ops,&f};
 assert(efs_meta_apply_expire(&failed,1,"a",4*NS,&due)==EFS_ERR_IO);
 struct efs_meta_dentry before;assert(!efs_meta_apply_lookup(kv,1,"a",&before));assert(before.ino==ino);
 assert(!efs_expiry_get(kv,ino,1,&r));assert(r.revision==2 && r.deadline_ns==3*NS);
 assert(!efs_meta_apply_expire(kv,1,"a",4*NS,&due));
 struct efs_meta_dentry dent;struct efs_meta_row row;
 assert(efs_meta_apply_lookup(kv,1,"a",&dent)==EFS_ERR_NOT_FOUND);
 assert(!efs_meta_apply_get_inode(kv,ino,&row));assert(!row.nlink);
 assert(!efs_expiry_get(kv,ino,1,&r));assert(r.revision==3 && !r.deadline_ns);
 assert(efs_meta_apply_expiry_renew(kv,ino,1,123,3,4*NS,5*NS)==EFS_ERR_NOT_FOUND);

 struct efs_expiry_page page;assert(!efs_expiry_due_page(kv,1,4*NS,NULL,&page));assert(!page.count);
 efs_ino_t replacement;assert(!efs_meta_apply_create_file(kv,&at,1,S_IFREG|0600,"a",&replacement));assert(replacement!=ino);
 assert(efs_meta_apply_expire(kv,1,"a",4*NS,&due)==EFS_ERR_STALE);
 assert(!efs_meta_apply_lookup(kv,1,"a",&dent));assert(dent.ino==replacement);
 r=(struct efs_expiry_record){.deadline_ns=5*NS,.root=1,.root_generation=1,.policy_revision=1,.max_ns=20*NS};
 apply(kv,replacement,0,&r);
 assert(!efs_meta_apply_expiry_renew(kv,replacement,1,123,1,4*NS,6*NS));
 assert(!efs_expiry_get(kv,replacement,1,&r));assert(r.revision==2 && r.deadline_ns==6*NS);
 assert(efs_meta_apply_expiry_renew(kv,replacement,1,123,1,4*NS,7*NS)==EFS_ERR_STALE);
 policy.enabled=0;assert(!efs_meta_apply_expiry_policy(kv,1,1,0,1,&policy));
 assert(efs_meta_apply_expiry_renew(kv,replacement,1,123,2,4*NS,7*NS)==EFS_ERR_ACCES);

 assert(!efs_meta_apply_check(kv));efs_kv_mem_free(kv);
}
/* D43: the cluster policy row (versioned CAS, off when absent) and the
 * coverage rule: a deadline acts only while its source folder policy is
 * enabled and the folder is live with the recorded generation. */
static void policy_gates(void)
{
 struct efs_kv *kv=efs_kv_mem_create();assert(kv);assert(!efs_meta_apply_init(kv,NS));
 uint64_t version=7;uint8_t on=7;
 assert(!efs_expiry_switch_get(kv,&version,&on));assert(version==0 && on==0);
 assert(efs_expiry_switch_apply(kv,0,2)==EFS_ERR_INVAL);
 assert(efs_expiry_switch_apply(kv,1,1)==EFS_ERR_STALE);
 assert(!efs_expiry_switch_apply(kv,0,1));
 assert(!efs_expiry_switch_get(kv,&version,&on));assert(version==1 && on==1);
 assert(efs_expiry_switch_apply(kv,0,0)==EFS_ERR_STALE);
 assert(!efs_expiry_switch_apply(kv,1,0));
 assert(!efs_expiry_switch_get(kv,&version,&on));assert(version==2 && on==0);
 assert(!efs_expiry_covered(NULL,0) && !efs_expiry_covered((const uint8_t *)"",EFS_TXN_ABSENT));
 struct efs_meta_attrs at={123,123,NS};efs_ino_t ino,dir;
 assert(!efs_meta_apply_mkdir(kv,&at,1,S_IFDIR|0700,"scratch",&dir));
 struct efs_expiry_policy policy={.ttl_ns=NS,.max_ns=20*NS,.enabled=1};
 assert(!efs_meta_apply_expiry_policy(kv,1,1,0,0,&policy));
 uint8_t key[EFS_EXPIRY_KEY_BYTES],value[EFS_EXPIRY_POLICY_BYTES];uint32_t n=sizeof(value);
 efs_expiry_policy_key(1,1,key);assert(!efs_kv_get(kv,key,sizeof(key),value,&n));assert(efs_expiry_covered(value,n));
 assert(!efs_meta_apply_create_file(kv,&at,1,S_IFREG|0600,"c",&ino));
 struct efs_expiry_record r={.deadline_ns=2*NS,.root=1,.root_generation=1,.policy_revision=1,.max_ns=20*NS,.parent=1,.name="c"};
 apply(kv,ino,0,&r);
 struct efs_expiry_due due={.deadline_ns=2*NS,.ino=ino,.generation=1,.revision=1,.parent=1,.name="c"};
 struct efs_expiry_page page;assert(!efs_expiry_due_page(kv,efs_kv_anchor_shard(efs_kv_inode_shard(ino)),3*NS,NULL,&page));
 assert(page.count==1 && page.rows[0].root==1 && page.rows[0].root_generation==1);
 /* Disabled source: the due deadline never removes the file. */
 policy.enabled=0;assert(!efs_meta_apply_expiry_policy(kv,1,1,0,1,&policy));
 n=sizeof(value);assert(!efs_kv_get(kv,key,sizeof(key),value,&n));assert(!efs_expiry_covered(value,n));
 assert(efs_meta_apply_expire(kv,1,"c",3*NS,&due)==EFS_ERR_STALE);
 struct efs_meta_dentry dent;assert(!efs_meta_apply_lookup(kv,1,"c",&dent));assert(dent.ino==ino);
 assert(!efs_expiry_get(kv,ino,1,&r));assert(r.revision==1 && r.deadline_ns==2*NS);
 /* A record naming another generation of its source folder is stale. */
 efs_ino_t other;assert(!efs_meta_apply_create_file(kv,&at,1,S_IFREG|0600,"g",&other));
 struct efs_expiry_record g={.deadline_ns=2*NS,.root=1,.root_generation=9,.policy_revision=1,.max_ns=20*NS,.parent=1,.name="g"};
 apply(kv,other,0,&g);
 struct efs_expiry_due gdue={.deadline_ns=2*NS,.ino=other,.generation=1,.revision=1,.parent=1,.name="g"};
 policy.enabled=1;assert(!efs_meta_apply_expiry_policy(kv,1,1,0,2,&policy));
 assert(efs_meta_apply_expire(kv,1,"g",3*NS,&gdue)==EFS_ERR_STALE);
 /* Enabled again: the same deadline acts. */
 assert(!efs_meta_apply_expire(kv,1,"c",3*NS,&due));
 assert(efs_meta_apply_lookup(kv,1,"c",&dent)==EFS_ERR_NOT_FOUND);
 assert(!efs_meta_apply_check(kv));efs_kv_mem_free(kv);
}
int main(void)
{
 inheritance_gates();
 namespace_gates();
 policy_gates();
 char path[EFS_TEST_PATH];efs_test_path(path,sizeof(path),"efs-expiry-XXXXXX");assert(efs_test_mkdtemp(path));
 struct efs_kv *kv=efs_kv_lsm_open(path,NULL);assert(kv);
 struct efs_expiry_policy p;assert(!efs_expiry_policy_get(kv,1,1,&p));assert(!p.enabled && !p.revision);
 struct efs_expiry_record r;assert(!efs_expiry_materialize(&p,1,1,NS,&r));assert(!r.deadline_ns);
 p=(struct efs_expiry_policy){.ttl_ns=10*NS,.max_ns=20*NS,.enabled=1,.owner_uid=123};
 assert(efs_expiry_policy_set(kv,1,1,123,0,&p)==EFS_ERR_ACCES);
 assert(!efs_expiry_policy_set(kv,1,1,0,0,&p));
 assert(efs_expiry_policy_set(kv,1,1,0,0,&p)==EFS_ERR_STALE);
 assert(!efs_expiry_policy_get(kv,1,1,&p));assert(p.revision==1 && p.owner_uid==123 && p.max_ns==20*NS);
 assert(!efs_expiry_materialize(&p,1,1,NS,&r));assert(r.deadline_ns==11*NS);
 apply(kv,4097,0,&r);
 struct efs_expiry_record old;assert(!efs_expiry_get(kv,4097,1,&old));assert(old.revision==1);
 struct efs_expiry_record absent;assert(!efs_expiry_get(kv,4097,2,&absent));assert(!absent.revision);
 struct efs_expiry_page page;assert(!efs_expiry_due_page(kv,1,10*NS,NULL,&page));assert(!page.count);
 assert(!efs_expiry_due_page(kv,1,11*NS,NULL,&page));assert(page.count==1 && page.rows[0].revision==1);
 assert(efs_expiry_renew(&old,&p,456,123,1,S_IFREG|0600,2*NS,12*NS,&r)==EFS_ERR_ACCES);
 assert(efs_expiry_renew(&old,&p,123,123,2,S_IFREG|0600,2*NS,12*NS,&r)==EFS_ERR_INVAL);
 assert(efs_expiry_renew(&old,&p,123,123,1,S_IFLNK|0600,2*NS,12*NS,&r)==EFS_ERR_INVAL);
 assert(efs_expiry_renew(&old,&p,123,123,1,S_IFREG|0600,2*NS,23*NS,&r)==EFS_ERR_INVAL);
 assert(efs_expiry_renew(&old,&p,123,123,1,S_IFREG|0600,2*NS,0,&r)==EFS_ERR_ACCES);
 assert(!efs_expiry_renew(&old,&p,123,123,1,S_IFREG|0600,2*NS,12*NS,&r));apply(kv,4097,1,&r);
 struct efs_expiry_change c;assert(efs_expiry_plan(kv,4097,1,1,&old,&c)==EFS_ERR_STALE);
 assert(!efs_expiry_due_page(kv,1,11*NS,NULL,&page));assert(!page.count);
 assert(!efs_expiry_due_page(kv,1,12*NS,NULL,&page));assert(page.count==1 && page.rows[0].revision==2);
 p.ttl_ns=5*NS;assert(!efs_expiry_policy_set(kv,1,1,0,1,&p));
 assert(!efs_expiry_get(kv,4097,1,&old));assert(old.deadline_ns==12*NS && old.policy_revision==1);
 assert(!efs_expiry_policy_get(kv,1,1,&p));assert(p.revision==2);
 assert(!efs_expiry_materialize(&p,1,1,NS,&r));assert(r.deadline_ns==6*NS);
 for(unsigned i=0;i<140;i++)apply(kv,8193+4096ull*i,0,&r);
 uint8_t after[27];unsigned total=0;int more;
 do {assert(!efs_expiry_due_page(kv,1,6*NS,total?after:NULL,&page));assert(page.count<=64);total+=page.count;more=page.more;memcpy(after,page.resume,27);}while(more);
 assert(total==140);
 efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(path,NULL);assert(kv);
 assert(!efs_expiry_get(kv,4097,1,&old));assert(old.revision==2 && old.deadline_ns==12*NS);
 assert(!efs_expiry_due_page(kv,1,12*NS,NULL,&page));assert(page.count==64 && page.more);
 assert(!efs_expiry_renew(&old,&p,0,123,1,S_IFREG|0600,2*NS,0,&r));apply(kv,4097,2,&r);
 assert(!efs_expiry_get(kv,4097,1,&old));assert(old.revision==3 && !old.deadline_ns);
 struct efs_expiry_clock clock={0};assert(!efs_expiry_clock_sample(&clock,10*NS,NS));
 assert(efs_expiry_clock_sample(&clock,11*NS,2*NS));
 assert(!efs_expiry_clock_sample(&clock,50*NS,3*NS));
 assert(!efs_expiry_clock_sample(&clock,49*NS,4*NS));
 assert(!efs_expiry_clock_sample(&clock,50*NS,5*NS));
 efs_expiry_clock_reset(&clock);assert(!efs_expiry_clock_sample(&clock,50*NS,5*NS));
 assert(efs_expiry_clock_sample(&clock,51*NS,6*NS));
 assert(!efs_expiry_materialize(&p,1,1,NS,&r));assert(efs_expiry_materialize(&p,1,1,UINT64_MAX,&r)==EFS_ERR_INVAL);
 efs_kv_lsm_close(kv);DIR *d=opendir(path);assert(d);struct dirent *e;
 while((e=readdir(d)))if(strcmp(e->d_name,".") && strcmp(e->d_name,"..")){char f[512];snprintf(f,sizeof(f),"%s/%s",path,e->d_name);assert(!unlink(f));}
 closedir(d);assert(!rmdir(path));puts("expiry: cluster policy CAS (off by default), source-policy coverage, durable policy/CAS, FileID isolation, fixed materialization, authorization/retention/link refusal, renewal index replacement, bounded paging, cold recovery and clock refusal PASS");return 0;
}
