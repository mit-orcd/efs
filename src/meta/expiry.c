#include "efs/expiry.h"
#include "efs/txn.h"
#include <string.h>
static void put64(uint8_t *p,uint64_t n) {for(unsigned i=0;i<8;i++)p[7-i]=(uint8_t)(n>>(8*i));}
static uint64_t get64(const uint8_t *p) {uint64_t n=0;for(unsigned i=0;i<8;i++)n=(n<<8)|p[i];return n;}
static void key(uint8_t *p,uint32_t shard,uint8_t kind,uint64_t ino,uint64_t gen)
{ p[0]=(uint8_t)(shard>>8);p[1]=(uint8_t)shard;p[2]=kind;put64(p+3,ino);put64(p+11,gen); }
void efs_expiry_policy_key(uint64_t root,uint64_t gen,uint8_t out[EFS_EXPIRY_KEY_BYTES])
{key(out,efs_kv_inode_shard(root),EFS_KV_KIND_EXPIRY_POLICY,root,gen);}
void efs_expiry_record_key(uint64_t ino,uint64_t gen,uint8_t out[EFS_EXPIRY_KEY_BYTES])
{key(out,efs_kv_inode_shard(ino),EFS_KV_KIND_EXPIRY,ino,gen);}
void efs_expiry_index_key(uint64_t ino,uint64_t gen,uint64_t deadline,uint8_t out[EFS_EXPIRY_INDEX_BYTES])
{uint32_t sh=efs_kv_anchor_shard(efs_kv_inode_shard(ino));out[0]=(uint8_t)(sh>>8);out[1]=(uint8_t)sh;out[2]=EFS_KV_KIND_EXPIRY_INDEX;put64(out+3,deadline);put64(out+11,ino);put64(out+19,gen);}
int efs_expiry_policy_decode(const uint8_t *v,uint32_t n,struct efs_expiry_policy *p)
{
    if(!v || !p)return EFS_ERR_INVAL;
    memset(p,0,sizeof(*p));
    if(n!=EFS_EXPIRY_POLICY_BYTES || v[28]>1 || v[29]>1)return EFS_ERR_PROTO;
    for(unsigned i=30;i<40;i++)if(v[i])return EFS_ERR_PROTO;
    p->revision=get64(v);p->ttl_ns=get64(v+8);p->max_ns=get64(v+16);
    p->owner_uid=((uint32_t)v[24]<<24)|((uint32_t)v[25]<<16)|((uint32_t)v[26]<<8)|v[27];
    p->enabled=v[28];p->allow_clear=v[29];
    if(!p->revision || p->ttl_ns>p->max_ns || (p->enabled && !p->ttl_ns))return EFS_ERR_PROTO;
    return EFS_OK;
}
int efs_expiry_policy_get(struct efs_kv *kv,uint64_t root,uint64_t gen,struct efs_expiry_policy *p)
{
    if(!p || !root || !gen)return EFS_ERR_INVAL;
    memset(p,0,sizeof(*p));uint8_t k[19],v[40];uint32_t n=sizeof(v);
    efs_expiry_policy_key(root,gen,k);
    int rc=efs_kv_get(kv,k,sizeof(k),v,&n);
    if(rc==EFS_ERR_NOT_FOUND)return EFS_OK;
    if(rc)return rc;
    return efs_expiry_policy_decode(v,n,p);
}
int efs_expiry_policy_set(struct efs_kv *kv,uint64_t root,uint64_t gen,uint32_t uid,uint64_t expected,const struct efs_expiry_policy *p)
{
    if(uid)return EFS_ERR_ACCES;
    if(!p || p->enabled>1 || p->allow_clear>1 || p->ttl_ns>p->max_ns || (p->enabled && !p->ttl_ns) || expected==UINT64_MAX)return EFS_ERR_INVAL;
    struct efs_expiry_policy old;int rc=efs_expiry_policy_get(kv,root,gen,&old);if(rc)return rc;
    if(old.revision!=expected)return EFS_ERR_STALE;
    uint8_t k[19],v[40]={0};efs_expiry_policy_key(root,gen,k);
    rc=efs_txn_key_busy(kv,k,sizeof(k));if(rc)return rc;
    rc=efs_txn_key_guarded(kv,k,sizeof(k));if(rc)return rc;
    put64(v,expected+1);put64(v+8,p->ttl_ns);put64(v+16,p->max_ns);
    v[24]=(uint8_t)(p->owner_uid>>24);v[25]=(uint8_t)(p->owner_uid>>16);v[26]=(uint8_t)(p->owner_uid>>8);v[27]=(uint8_t)p->owner_uid;
    v[28]=p->enabled;v[29]=p->allow_clear;
    return efs_kv_put(kv,k,sizeof(k),v,sizeof(v));
}
int efs_expiry_get(struct efs_kv *kv,uint64_t ino,uint64_t gen,struct efs_expiry_record *r)
{
    if(!r || !ino || !gen)return EFS_ERR_INVAL;
    memset(r,0,sizeof(*r));uint8_t k[19],v[EFS_EXPIRY_RECORD_BYTES];uint32_t n=sizeof(v);
    efs_expiry_record_key(ino,gen,k);
    int rc=efs_kv_get(kv,k,sizeof(k),v,&n);if(rc==EFS_ERR_NOT_FOUND)return EFS_OK;if(rc)return rc;
    if(n!=sizeof(v) || v[48]>1)return EFS_ERR_PROTO;
    for(unsigned i=49;i<56;i++)if(v[i])return EFS_ERR_PROTO;
    r->deadline_ns=get64(v);r->revision=get64(v+8);r->root=get64(v+16);r->root_generation=get64(v+24);r->policy_revision=get64(v+32);r->max_ns=get64(v+40);r->allow_clear=v[48];
    r->parent=get64(v+56);memcpy(r->name,v+64,EFS_MAX_NAME);
    if(!memchr(r->name,0,sizeof(r->name)) || strchr(r->name,'/') ||
       (!!r->parent != !!r->name[0]))return EFS_ERR_PROTO;
    return r->revision && r->root && r->root_generation && r->policy_revision && r->max_ns ? EFS_OK:EFS_ERR_PROTO;
}
int efs_expiry_plan(struct efs_kv *kv,uint64_t ino,uint64_t gen,uint64_t expected,const struct efs_expiry_record *r,struct efs_expiry_change *c)
{
    if(!r || !c || expected==UINT64_MAX || !r->root || !r->root_generation || !r->policy_revision || !r->max_ns || r->allow_clear>1)return EFS_ERR_INVAL;
    if(strnlen(r->name,sizeof(r->name))==sizeof(r->name) || strchr(r->name,'/') ||
       (!!r->parent != !!r->name[0]))return EFS_ERR_INVAL;
    memset(c,0,sizeof(*c));struct efs_expiry_record old;int rc=efs_expiry_get(kv,ino,gen,&old);if(rc)return rc;
    if(old.revision!=expected)return EFS_ERR_STALE;
    efs_expiry_record_key(ino,gen,c->record_key);
    rc=efs_txn_key_busy(kv,c->record_key,19);if(rc)return rc;
    rc=efs_txn_key_guarded(kv,c->record_key,19);if(rc)return rc;
    put64(c->value,r->deadline_ns);put64(c->value+8,expected+1);put64(c->value+16,r->root);put64(c->value+24,r->root_generation);put64(c->value+32,r->policy_revision);put64(c->value+40,r->max_ns);c->value[48]=r->allow_clear;put64(c->value+56,r->parent);memcpy(c->value+64,r->name,strlen(r->name));
    c->items[c->count++]=(struct efs_kv_item){EFS_KV_PUT,c->record_key,19,c->value,EFS_EXPIRY_RECORD_BYTES};
    if(old.deadline_ns && old.deadline_ns!=r->deadline_ns){efs_expiry_index_key(ino,gen,old.deadline_ns,c->old_index);c->items[c->count++]=(struct efs_kv_item){EFS_KV_DEL,c->old_index,27,NULL,0};}
    if(r->deadline_ns){efs_expiry_index_key(ino,gen,r->deadline_ns,c->new_index);c->items[c->count++]=(struct efs_kv_item){EFS_KV_PUT,c->new_index,27,c->value,EFS_EXPIRY_RECORD_BYTES};}
    return EFS_OK;
}
int efs_expiry_materialize(const struct efs_expiry_policy *p,uint64_t root,uint64_t gen,uint64_t now,struct efs_expiry_record *r)
{
    if(!p || !r || !root || !gen)return EFS_ERR_INVAL;
    memset(r,0,sizeof(*r));if(!p->enabled)return EFS_OK;
    if(!p->revision || !p->ttl_ns || p->ttl_ns>p->max_ns || now>UINT64_MAX-p->ttl_ns)return EFS_ERR_INVAL;
    *r=(struct efs_expiry_record){.deadline_ns=now+p->ttl_ns,.root=root,.root_generation=gen,.policy_revision=p->revision,.max_ns=p->max_ns,.allow_clear=p->allow_clear};return EFS_OK;
}
int efs_expiry_renew(const struct efs_expiry_record *old,const struct efs_expiry_policy *p,uint32_t uid,uint32_t owner,uint32_t nlink,uint32_t mode,uint64_t now,uint64_t deadline,struct efs_expiry_record *r)
{
    if(!old || !p || !r)return EFS_ERR_INVAL;
    if(!S_ISREG(mode) || nlink!=1)return EFS_ERR_INVAL;
    if(uid && uid!=owner)return EFS_ERR_ACCES;
    if(!p->revision || !p->enabled)return EFS_ERR_ACCES;
    if(!deadline && uid && !p->allow_clear)return EFS_ERR_ACCES;
    if(deadline && (deadline<=now || deadline-now>p->max_ns))return EFS_ERR_INVAL;
    *r=*old;r->deadline_ns=deadline;r->policy_revision=p->revision;r->max_ns=p->max_ns;r->allow_clear=p->allow_clear;return EFS_OK;
}
struct page_ctx {struct efs_expiry_page *page;uint64_t now;};
static int due(void *user,const uint8_t *k,uint32_t kn,const uint8_t *v,uint32_t vn)
{
    struct page_ctx *c=user;if(kn!=27 || vn!=EFS_EXPIRY_RECORD_BYTES || !get64(v+8))return EFS_ERR_PROTO;
    uint64_t deadline=get64(k+3);if(deadline!=get64(v))return EFS_ERR_PROTO;
    if(deadline>c->now)return 1;
    if(!deadline || !get64(k+11) || !get64(k+19) ||
       efs_kv_anchor_shard(efs_kv_inode_shard(get64(k+11)))!=k[1])return EFS_ERR_PROTO;
    if(c->page->count==EFS_EXPIRY_PAGE_MAX){c->page->more=1;return 1;}
    struct efs_expiry_due *r=&c->page->rows[c->page->count++];
    *r=(struct efs_expiry_due){.deadline_ns=deadline,.ino=get64(k+11),
        .generation=get64(k+19),.revision=get64(v+8),.parent=get64(v+56),
        .root=get64(v+16),.root_generation=get64(v+24)};
    memcpy(r->name,v+64,EFS_MAX_NAME);
    if(!memchr(r->name,0,sizeof(r->name)) || strchr(r->name,'/') ||
       (!!r->parent != !!r->name[0]))return EFS_ERR_PROTO;
    memcpy(c->page->resume,k,27);return EFS_OK;
}
int efs_expiry_due_page(struct efs_kv *kv,uint32_t anchor,uint64_t now,const uint8_t *after,struct efs_expiry_page *page)
{
    if(!page || (anchor!=1 && anchor!=2))return EFS_ERR_INVAL;
    memset(page,0,sizeof(*page));uint8_t prefix[3]={0,(uint8_t)anchor,EFS_KV_KIND_EXPIRY_INDEX},start[27];uint32_t sn=3;
    memcpy(start,prefix,3);
    if(after){if(memcmp(after,prefix,3))return EFS_ERR_INVAL;memcpy(start,after,27);sn=27;int i=26;while(i>=0 && ++start[i]==0)i--;if(i<0)return EFS_OK;}
    struct page_ctx c={page,now};int rc=efs_kv_scan_from(kv,prefix,3,start,sn,due,&c);return rc==1?EFS_OK:rc;
}
void efs_expiry_clock_reset(struct efs_expiry_clock *c)
{ if(c)memset(c,0,sizeof(*c)); }
int efs_expiry_clock_sample(struct efs_expiry_clock *c,uint64_t wall,uint64_t mono)
{
    if(!c)return 0;
    int safe=0;
    if(c->initialized && wall>=c->wall && mono>c->mono){uint64_t w=wall-c->wall,m=mono-c->mono;safe=(w>m?w-m:m-w)<=1000000000ull;}
    if(c->initialized && !safe)c->uncertain=1;
    c->wall=wall;c->mono=mono;c->initialized=1;c->healthy=(uint8_t)(safe && !c->uncertain);return c->healthy;
}
static const uint8_t switch_key[3]={0,1,EFS_KV_KIND_EXPIRY_SWITCH};
int efs_expiry_switch_get(struct efs_kv *kv,uint64_t *version,uint8_t *enabled)
{
    if(!kv || !version || !enabled)return EFS_ERR_INVAL;
    *version=0;*enabled=0;
    uint8_t v[9];uint32_t n=sizeof(v);
    int rc=efs_kv_get(kv,switch_key,sizeof(switch_key),v,&n);
    if(rc==EFS_ERR_NOT_FOUND)return EFS_OK;
    if(rc)return rc;
    if(n!=sizeof(v) || v[8]>1 || !get64(v))return EFS_ERR_PROTO;
    *version=get64(v);*enabled=v[8];return EFS_OK;
}
int efs_expiry_switch_apply(struct efs_kv *kv,uint64_t expected,uint8_t enabled)
{
    if(enabled>1)return EFS_ERR_INVAL;
    uint64_t version;uint8_t old;
    int rc=efs_expiry_switch_get(kv,&version,&old);
    if(rc)return rc;
    if(version!=expected)return EFS_ERR_STALE;
    if(version==UINT64_MAX)return EFS_ERR_INVAL;
    uint8_t v[9];put64(v,version+1);v[8]=enabled;
    return efs_kv_put(kv,switch_key,sizeof(switch_key),v,sizeof(v));
}
int efs_expiry_covered(const uint8_t *policy,uint32_t policy_len)
{
    struct efs_expiry_policy p;
    if(!policy || policy_len==EFS_TXN_ABSENT)return 0;
    if(efs_expiry_policy_decode(policy,policy_len,&p)!=EFS_OK)return 0;
    return p.revision && p.enabled;
}
