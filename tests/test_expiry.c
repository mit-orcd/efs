#include "efs/expiry.h"
#include "efs/meta_apply.h"
#include "efs/session.h"
#include "efs/kv_lsm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
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
static void namespace_gates(void)
{
 struct efs_kv *kv=efs_kv_mem_create();assert(kv);assert(!efs_meta_apply_init(kv,NS));
 struct efs_meta_attrs at={123,123,NS};efs_ino_t ino;
 assert(!efs_meta_apply_create_file(kv,&at,1,S_IFREG|0600,"a",&ino));
 struct efs_expiry_record r={.deadline_ns=2*NS,.root=1,.root_generation=1,.policy_revision=1,.max_ns=20*NS};
 apply(kv,ino,0,&r);
 struct efs_expiry_due due={2*NS,ino,1,1};
 assert(efs_meta_apply_expire(kv,1,"a",NS,&due)==EFS_ERR_STALE);
 r.deadline_ns=3*NS;apply(kv,ino,1,&r);
 assert(efs_meta_apply_expire(kv,1,"a",4*NS,&due)==EFS_ERR_STALE);
 due.deadline_ns=3*NS;due.revision=2;
 uint8_t uuid[16]={1};assert(!efs_lease_open(kv,ino,1,uuid,1));
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
 struct efs_expiry_page page;assert(!efs_expiry_due_page(kv,1,4*NS,NULL,&page));assert(!page.count);
 efs_ino_t replacement;assert(!efs_meta_apply_create_file(kv,&at,1,S_IFREG|0600,"a",&replacement));assert(replacement!=ino);
 assert(efs_meta_apply_expire(kv,1,"a",4*NS,&due)==EFS_ERR_STALE);
 assert(!efs_meta_apply_lookup(kv,1,"a",&dent));assert(dent.ino==replacement);
 assert(!efs_meta_apply_check(kv));efs_kv_mem_free(kv);
}
int main(void)
{
 namespace_gates();
 char path[]="/tmp/efs-expiry-XXXXXX";assert(mkdtemp(path));
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
 closedir(d);assert(!rmdir(path));puts("expiry: durable policy/CAS, FileID isolation, fixed materialization, authorization/retention/link refusal, renewal index replacement, bounded paging, cold recovery and clock refusal PASS");return 0;
}
