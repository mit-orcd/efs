#include "efs/query_catalog.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct record { uint32_t key, klen, val, vlen; };
struct partition {
    uint32_t refs, count, capacity, used, arena_cap;
    uint64_t revision, bytes, limit;
    /* Exact bounds for retained key kinds; absent transaction kinds are O(1). */
    uint32_t kind_begin[256], kind_end[256];
    struct record *records;
    uint8_t *arena;
    struct efs_name_index *names;
};
struct node {
    struct efs_meta_row row;
    struct efs_meta_stat stat;
    struct efs_query_rollup rollup;
    const struct efs_name_entry *link;
    uint32_t children;
};
struct efs_query_catalog {
    struct partition *part[4096];
    struct node *nodes;
    uint32_t count, capacity, rebuilt;
    uint32_t *node_index, node_slots;
    uint64_t bytes, limit;
    struct efs_kv kv;
    int (*stop)(void *);
    void *stop_ctx;
};
static int compare_key(const uint8_t *a, uint32_t an, const uint8_t *b, uint32_t bn)
{
    uint32_t n=an<bn ? an : bn;
    int rc=n ? memcmp(a,b,n) : 0;
    return rc ? rc : an<bn ? -1 : an>bn ? 1 : 0;
}
static uint32_t seek_record(const struct partition *p, const uint8_t *key, uint32_t len)
{
    uint32_t lo=p->kind_begin[key[2]],hi=p->kind_end[key[2]];
    while (lo<hi) {
        uint32_t mid=lo+(hi-lo)/2;
        const struct record *r=&p->records[mid];
        if (compare_key(p->arena+r->key,r->klen,key,len)<0) lo=mid+1;
        else hi=mid;
    }
    return lo;
}
static int cache_get(void *ctx, const uint8_t *key, uint32_t len, uint8_t *value, uint32_t *vn)
{
    struct efs_query_catalog *c=ctx;
    if (!key || len<3 || !vn) return EFS_ERR_INVAL;
    uint32_t shard=((uint32_t)key[0]<<8)|key[1];
    if (shard>=4096 || !c->part[shard]) return EFS_ERR_NOT_FOUND;
    struct partition *p=c->part[shard];
    uint32_t at=seek_record(p,key,len);
    if (at==p->kind_end[key[2]]) return EFS_ERR_NOT_FOUND;
    const struct record *r=&p->records[at];
    if (compare_key(p->arena+r->key,r->klen,key,len)) return EFS_ERR_NOT_FOUND;
    if (*vn<r->vlen || (r->vlen && !value)) { *vn=r->vlen;return EFS_ERR_INVAL; }
    if (r->vlen) memcpy(value,p->arena+r->val,r->vlen);
    *vn=r->vlen;
    return EFS_OK;
}
static int cache_scan(void *ctx, const uint8_t *prefix, uint32_t plen,
                       const uint8_t *start, uint32_t slen,
                       int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t), void *user)
{
    struct efs_query_catalog *c=ctx;
    if (!prefix || plen<3 || !cb) return EFS_ERR_INVAL;
    uint32_t shard=((uint32_t)prefix[0]<<8)|prefix[1];
    if (shard>=4096 || !c->part[shard]) return EFS_OK;
    struct partition *p=c->part[shard];
    uint32_t at=seek_record(p,slen ? start : prefix,slen ? slen : plen);
    for (;at<p->kind_end[prefix[2]];at++) {
        const struct record *r=&p->records[at];
        const uint8_t *key=p->arena+r->key;
        if (r->klen<plen || memcmp(key,prefix,plen)) break;
        int rc=cb(user,key,r->klen,p->arena+r->val,r->vlen);
        if (rc) return rc;
    }
    return EFS_OK;
}
static int cache_prefix(void *ctx, const uint8_t *prefix, uint32_t plen,
                         int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t), void *user)
{
    return cache_scan(ctx,prefix,plen,NULL,0,cb,user);
}
static const struct efs_kv_ops cache_ops={.get=cache_get,.scan_prefix=cache_prefix,.scan_from=cache_scan};
static void partition_release(struct partition *p)
{
    if (!p || --p->refs) return;
    efs_name_index_free(p->names);free(p->records);free(p->arena);free(p);
}
void efs_query_catalog_free(struct efs_query_catalog *c)
{
    if (!c) return;
    for (uint32_t i=0;i<4096;i++) partition_release(c->part[i]);
    free(c->node_index);free(c->nodes);free(c);
}
static int save_record(void *ctx, const uint8_t *key, uint32_t kl, const uint8_t *value, uint32_t vl)
{
    struct partition *p=ctx;
    uint64_t used=(uint64_t)p->used+kl+vl;
    if (used>UINT32_MAX) return EFS_ERR_NOMEM;
    if (p->count==p->capacity) {
        uint64_t capacity=p->capacity ? (uint64_t)p->capacity*2 : 64;
        uint64_t add=(capacity-p->capacity)*sizeof(*p->records);
        if (capacity>UINT32_MAX || add>p->limit-p->bytes) return EFS_ERR_NOMEM;
        struct record *r=realloc(p->records,capacity*sizeof(*r));
        if (!r) return EFS_ERR_NOMEM;
        p->records=r;p->capacity=(uint32_t)capacity;p->bytes+=add;
    }
    if (used>p->arena_cap) {
        uint64_t capacity=p->arena_cap ? (uint64_t)p->arena_cap*2 : 4096;
        if (capacity<used) capacity=used;
        if (capacity>UINT32_MAX || capacity-p->arena_cap>p->limit-p->bytes)
            return EFS_ERR_NOMEM;
        uint8_t *a=realloc(p->arena,capacity);
        if (!a) return EFS_ERR_NOMEM;
        p->arena=a;p->bytes+=capacity-p->arena_cap;p->arena_cap=(uint32_t)capacity;
    }
    struct record r={p->used,kl,p->used+kl,vl};
    memcpy(p->arena+r.key,key,kl);
    if (vl) memcpy(p->arena+r.val,value,vl);
    p->records[p->count++]=r;p->used=(uint32_t)used;
    return EFS_OK;
}
static int partition_build(struct efs_kv_lsm_snapshot *source, uint32_t shard,
                            uint64_t revision, uint64_t limit, struct partition **out)
{
    *out=NULL;
    if (limit<sizeof(struct partition)) return EFS_ERR_NOMEM;
    struct partition *p=calloc(1,sizeof(*p));
    if (!p) return EFS_ERR_NOMEM;
    p->refs=1;p->revision=revision;p->bytes=sizeof(*p);p->limit=limit;
    /* Ascending kinds preserve total key order, without sorting/copying. */
    const uint8_t kinds[]={EFS_KV_KIND_INODE,EFS_KV_KIND_DENTRY,EFS_KV_KIND_LANE,
        EFS_KV_KIND_DSEQ,EFS_KV_KIND_APPEND_CUR,EFS_KV_KIND_PVER};
    uint8_t prefix[3]={(uint8_t)(shard>>8),(uint8_t)shard,0};
    int rc=EFS_OK;
    for (uint32_t i=0;i<sizeof(kinds) && !rc;i++) {
        prefix[2]=kinds[i];
        p->kind_begin[kinds[i]]=p->count;
        rc=efs_kv_lsm_snapshot_scan(source,prefix,3,NULL,0,save_record,p);
        p->kind_end[kinds[i]]=p->count;
    }
    if (!rc && p->count && p->count<p->capacity) {
        struct record *tight=realloc(p->records,(size_t)p->count*sizeof(*tight));
        if (tight) {
            p->bytes-=(uint64_t)(p->capacity-p->count)*sizeof(*tight);
            p->records=tight;p->capacity=p->count;
        }
    }
    if (!rc && p->used && p->used<p->arena_cap) {
        uint8_t *tight=realloc(p->arena,p->used);
        if (tight) { p->bytes-=p->arena_cap-p->used;p->arena=tight;p->arena_cap=p->used; }
    }
    if (!rc) rc=efs_name_index_build(source,shard,limit-p->bytes,&p->names);
    if (rc) { partition_release(p);return rc; }
    p->bytes+=efs_name_index_bytes(p->names);
    *out=p;return EFS_OK;
}
/* Compact immutable inode -> node table. Keeping the probe array separate
 * avoids binary-search cache misses across the much larger metadata rows. */
static uint32_t node_hash(efs_ino_t ino)
{
    uint64_t x=ino;
    x^=x>>30;x*=UINT64_C(0xbf58476d1ce4e5b9);
    x^=x>>27;x*=UINT64_C(0x94d049bb133111eb);
    x^=x>>31;return (uint32_t)x;
}
static struct node *find_node(const struct efs_query_catalog *c, efs_ino_t ino)
{
    if (!c->node_slots) return NULL;
    uint32_t at=node_hash(ino)&(c->node_slots-1);
    for (uint32_t probes=0;probes<c->node_slots;probes++) {
        uint32_t index=c->node_index[at];
        if (!index) return NULL;
        if (c->nodes[index-1].row.ino==ino) return &c->nodes[index-1];
        at=(at+1)&(c->node_slots-1);
    }
    return NULL;
}
static int build_node_index(struct efs_query_catalog *c)
{
    if (!c->count) return EFS_OK;
    uint64_t slots=1;
    while (slots<(uint64_t)c->count*2) slots<<=1;
    uint64_t bytes=slots*sizeof(*c->node_index);
    if (slots>UINT32_MAX || bytes>c->limit-c->bytes) return EFS_ERR_NOMEM;
    c->node_index=calloc((size_t)slots,sizeof(*c->node_index));
    if (!c->node_index) return EFS_ERR_NOMEM;
    c->node_slots=(uint32_t)slots;c->bytes+=bytes;
    for (uint32_t i=0;i<c->count;i++) {
        if (!(i&255) && c->stop && c->stop(c->stop_ctx)) return EFS_ERR_BUSY;
        uint32_t at=node_hash(c->nodes[i].row.ino)&(c->node_slots-1);
        while (c->node_index[at]) {
            if (c->nodes[c->node_index[at]-1].row.ino==c->nodes[i].row.ino) return EFS_ERR_PROTO;
            at=(at+1)&(c->node_slots-1);
        }
        c->node_index[at]=i+1;
    }
    return EFS_OK;
}
static int add_node(void *ctx, const uint8_t *key, uint32_t kl, const uint8_t *value, uint32_t vl)
{
    struct efs_query_catalog *c=ctx;
    struct efs_meta_row row;
    if (kl!=11) return EFS_ERR_PROTO;
    int rc=efs_meta_unpack_inode(value,vl,&row);
    if (rc) return rc;
    efs_ino_t encoded=0;
    for (uint32_t i=3;i<11;i++) encoded=(encoded<<8)|key[i];
    if (encoded!=row.ino || (((uint32_t)key[0]<<8)|key[1])!=efs_kv_inode_shard(row.ino))
        return EFS_ERR_PROTO;
    if (c->count==c->capacity) {
        uint64_t capacity=c->capacity ? (uint64_t)c->capacity*2 : 128;
        uint64_t add=(capacity-c->capacity)*sizeof(*c->nodes);
        if (capacity>UINT32_MAX || add>c->limit-c->bytes) return EFS_ERR_NOMEM;
        struct node *p=realloc(c->nodes,capacity*sizeof(*p));
        if (!p) return EFS_ERR_NOMEM;
        c->nodes=p;c->capacity=(uint32_t)capacity;c->bytes+=add;
    }
    memset(&c->nodes[c->count],0,sizeof(*c->nodes));c->nodes[c->count++].row=row;
    return EFS_OK;
}
static int refuse_coord(void *ctx, const struct efs_txid *t, uint32_t shard, int *decision)
{
    (void)ctx;(void)t;(void)shard;(void)decision;
    return EFS_ERR_BUSY;
}
static void expand(uint64_t *lo, uint64_t *hi, uint64_t low, uint64_t high, int has)
{
    if (!has || low<*lo) *lo=low;
    if (!has || high>*hi) *hi=high;
}
static uint64_t low_time(const struct efs_meta_stat *s)
{
    uint64_t t=s->mtime;
    if (s->ctime<t) t=s->ctime;
    if (s->atime<t) t=s->atime;
    return t;
}
static uint64_t high_time(const struct efs_meta_stat *s)
{
    uint64_t t=s->mtime;
    if (s->ctime>t) t=s->ctime;
    if (s->atime>t) t=s->atime;
    return t;
}
struct edge_context { struct efs_query_catalog *c; uint32_t shard; };
static int stopped(struct efs_query_catalog *c);
static int add_edge(void *ctx, const struct efs_name_entry *entry)
{
    struct edge_context *e=ctx;
    struct efs_query_catalog *c=e->c;
    if (stopped(c)) return EFS_ERR_BUSY;
    struct node *parent=find_node(c,entry->parent);
    if (!parent || !S_ISDIR(parent->row.mode)) return EFS_ERR_IO;
    uint32_t chosen=efs_kv_inode_shard(parent->row.ino);
    if (parent->row.layout!=EFS_META_LAYOUT_LOCAL) {
        uint32_t hashed=efs_kv_dentry_shard(entry->parent,entry->name,EFS_META_LAYOUT_HASHED);
        uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_DENT_BYTES];uint32_t kl,vl=sizeof(value);
        int rc=efs_kv_key_dentry(hashed,entry->parent,entry->name,key,&kl);
        if (!rc) rc=efs_kv_get(&c->kv,key,kl,value,&vl);
        if (rc==EFS_OK || parent->row.layout==EFS_META_LAYOUT_HASHED) chosen=hashed;
        else if (rc!=EFS_ERR_NOT_FOUND) return rc;
    }
    if (chosen!=e->shard || !entry->dentry.ino || entry->dentry.type==EFS_META_DENT_TOMBSTONE)
        return EFS_OK;
    struct node *child=find_node(c,entry->dentry.ino);
    if (!child || child->row.generation!=entry->dentry.generation) return EFS_ERR_IO;
    struct efs_query_rollup *r=&parent->rollup;
    int has=r->imm_files+r->imm_dirs!=0;
    if (S_ISDIR(child->row.mode)) {
        if (child->link || child->row.parent!=entry->parent) return EFS_ERR_PROTO;
        child->link=entry;r->imm_dirs++;parent->children++;
    } else {
        r->imm_files++;
        if (UINT64_MAX-r->imm_bytes<child->stat.size) return EFS_ERR_QUOTA;
        r->imm_bytes+=child->stat.size;
    }
    expand(&r->imm_tmin,&r->imm_tmax,low_time(&child->stat),high_time(&child->stat),has);
    return EFS_OK;
}
static int stopped(struct efs_query_catalog *c)
{
    return c->stop && c->stop(c->stop_ctx);
}
int efs_query_catalog_build(struct efs_kv_lsm_snapshot *source, const struct efs_query_catalog *old,
                             uint64_t max_bytes, int (*stop)(void *), void *ctx,
                             struct efs_query_catalog **out)
{
    uint64_t revisions[4096];
    if (out) *out=NULL;
    if (!source || !out) return EFS_ERR_INVAL;
    if (max_bytes<sizeof(struct efs_query_catalog)) return EFS_ERR_NOMEM;
    struct efs_query_catalog *c=calloc(1,sizeof(*c));
    if (!c) return EFS_ERR_NOMEM;
    c->bytes=sizeof(*c);c->limit=max_bytes;c->kv=(struct efs_kv){&cache_ops,c};
    c->stop=stop;c->stop_ctx=ctx;
    int rc=efs_kv_lsm_snapshot_revisions(source,revisions,4096);
    for (uint32_t shard=0;shard<4096 && !rc;shard++) {
        if (stopped(c)) { rc=EFS_ERR_BUSY;break; }
        if (old && old->part[shard]->revision==revisions[shard]) {
            c->part[shard]=old->part[shard];c->part[shard]->refs++;
        } else {
            rc=partition_build(source,shard,revisions[shard],max_bytes-c->bytes,&c->part[shard]);
            if (rc) break;
            c->rebuilt++;
        }
        if (c->part[shard]->bytes>max_bytes-c->bytes) { rc=EFS_ERR_NOMEM;break; }
        c->bytes+=c->part[shard]->bytes;
        uint8_t prefix[3]={(uint8_t)(shard>>8),(uint8_t)shard,EFS_KV_KIND_INODE};
        rc=cache_prefix(c,prefix,3,add_node,c);
    }
    if (!rc && c->count && c->count<c->capacity) {
        struct node *tight=realloc(c->nodes,(size_t)c->count*sizeof(*tight));
        if (tight) {
            c->bytes-=(uint64_t)(c->capacity-c->count)*sizeof(*tight);
            c->nodes=tight;c->capacity=c->count;
        }
    }
    if (!rc) rc=build_node_index(c);
    for (uint32_t i=0;i<c->count && !rc;i++) {
        if (stopped(c)) { rc=EFS_ERR_BUSY;break; }
        rc=efs_meta_apply_getattr(&c->kv,c->nodes[i].row.ino,refuse_coord,NULL,&c->nodes[i].stat);
    }
    for (uint32_t shard=0;shard<4096 && !rc;shard++) {
        if (stopped(c)) { rc=EFS_ERR_BUSY;break; }
        struct edge_context edge={c,shard};uint32_t cursor=0;
        rc=efs_name_index_query(c->part[shard]->names,EFS_NAME_PREFIX,"",&cursor,UINT32_MAX,add_edge,&edge);
    }
    uint32_t *queue=NULL,head=0,tail=0,dirs=0;
    if (!rc) {
        uint64_t bytes=(uint64_t)c->count*sizeof(*queue);
        if (bytes>max_bytes-c->bytes) rc=EFS_ERR_NOMEM;
        else { queue=malloc(bytes ? bytes : 1);if (!queue) rc=EFS_ERR_NOMEM; }
    }
    for (uint32_t i=0;i<c->count && !rc;i++) {
        struct node *n=&c->nodes[i];
        if (!S_ISDIR(n->row.mode)) continue;
        if (n->row.nlink && n->row.ino!=EFS_ROOT_INO && !n->link) { rc=EFS_ERR_IO;break; }
        dirs++;
        n->rollup.tree_files=n->rollup.imm_files;n->rollup.tree_dirs=n->rollup.imm_dirs;
        n->rollup.tree_bytes=n->rollup.imm_bytes;n->rollup.tree_tmin=n->rollup.imm_tmin;
        n->rollup.tree_tmax=n->rollup.imm_tmax;
        if (!n->children) queue[tail++]=i;
    }
    while (head<tail && !rc) {
        if (stopped(c)) { rc=EFS_ERR_BUSY;break; }
        struct node *child=&c->nodes[queue[head++]];
        if (!child->link) continue;
        struct node *parent=find_node(c,child->link->parent);
        struct efs_query_rollup *a=&parent->rollup,*b=&child->rollup;
        int has=a->tree_files+a->tree_dirs!=0;
        if (UINT64_MAX-a->tree_bytes<b->tree_bytes) { rc=EFS_ERR_QUOTA;break; }
        a->tree_files+=b->tree_files;a->tree_dirs+=b->tree_dirs;a->tree_bytes+=b->tree_bytes;
        if (b->tree_files+b->tree_dirs) expand(&a->tree_tmin,&a->tree_tmax,b->tree_tmin,b->tree_tmax,has);
        if (!--parent->children) queue[tail++]=(uint32_t)(parent-c->nodes);
    }
    if (!rc && head!=dirs) rc=EFS_ERR_PROTO; /* cyclic directory graph */
    free(queue);
    if (rc) { efs_query_catalog_free(c);return rc; }
    c->stop=NULL;c->stop_ctx=NULL;
    *out=c;return EFS_OK;
}
uint64_t efs_query_catalog_bytes(const struct efs_query_catalog *c) { return c ? c->bytes : 0; }
uint32_t efs_query_catalog_rebuilt(const struct efs_query_catalog *c) { return c ? c->rebuilt : 0; }
int efs_query_catalog_inode(const struct efs_query_catalog *c, efs_ino_t ino,
                             struct efs_meta_row *row, struct efs_meta_stat *stat)
{
    if (!c) return EFS_ERR_INVAL;
    struct node *n=find_node(c,ino);
    if (!n) return EFS_ERR_NOT_FOUND;
    if (row) *row=n->row;
    if (stat) *stat=n->stat;
    return EFS_OK;
}
int efs_query_catalog_stats(const struct efs_query_catalog *c, efs_ino_t ino, struct efs_query_rollup *out)
{
    if (!c || !out) return EFS_ERR_INVAL;
    struct node *n=find_node(c,ino);
    if (!n) return EFS_ERR_NOT_FOUND;
    if (!S_ISDIR(n->row.mode)) return EFS_ERR_INVAL;
    *out=n->rollup;return EFS_OK;
}
int efs_query_catalog_directory_link(const struct efs_query_catalog *c,efs_ino_t ino,struct efs_name_entry *out)
{
    if (!c || !out) return EFS_ERR_INVAL;
    struct node *n=find_node(c,ino);
    if (!n || !n->link) return EFS_ERR_NOT_FOUND;
    *out=*n->link;return EFS_OK;
}
int efs_query_catalog_path(const struct efs_query_catalog *c, efs_ino_t dir, efs_ino_t root, char *out, size_t cap)
{
    if (!c || !out || !cap) return EFS_ERR_INVAL;
    size_t len=0;out[0]=0;
    for (uint32_t steps=0;dir!=root && steps<=c->count;steps++) {
        struct node *n=find_node(c,dir);
        if (!n || !S_ISDIR(n->row.mode) || !n->link) return EFS_ERR_NOT_FOUND;
        size_t nl=strlen(n->link->name),add=nl+(len ? 1 : 0);
        if (add>=cap-len) return EFS_ERR_NAMETOOLONG;
        memmove(out+add,out,len+1);memcpy(out,n->link->name,nl);
        if (len) out[nl]='/';
        len+=add;dir=n->link->parent;
    }
    return dir==root ? EFS_OK : EFS_ERR_PROTO;
}
struct find_context {
    const struct efs_query_catalog *c;
    efs_ino_t root;
    uint32_t shard;
    int (*emit)(void *,const struct efs_name_entry *,const char *);
    void *user;
    /* Direct-mapped, query-local negative scope memo (16 KiB). Collisions
     * only cause recomputation; no permissions or successful paths are cached. */
    efs_ino_t outside[2048];
};
static int find_candidate(void *ctx, const struct efs_name_entry *entry)
{
    struct find_context *f=ctx;
    uint32_t scope_slot=(uint32_t)((entry->parent^(entry->parent>>32))*2654435761U)&2047U;
    if (entry->parent && f->outside[scope_slot]==entry->parent) return EFS_OK;
    struct node *parent=find_node(f->c,entry->parent);
    if (!parent) return EFS_ERR_IO;
    /* Membership is immutable and independent of a candidate binding. Avoid
     * KV/directory-layout lookups for names outside the requested subtree. */
    char path[EFS_MAX_PATH];
    int rc=efs_query_catalog_path(f->c,entry->parent,f->root,path,sizeof(path));
    if (rc==EFS_ERR_NOT_FOUND) {
        f->outside[scope_slot]=entry->parent;return EFS_OK;
    }
    if (rc) return rc;
    struct efs_meta_dentry authoritative;
    rc=efs_meta_apply_lookup((struct efs_kv *)&f->c->kv,entry->parent,entry->name,&authoritative);
    if (rc==EFS_ERR_NOT_FOUND) return EFS_OK;
    if (rc) return rc;
    if (!entry->dentry.ino || authoritative.ino!=entry->dentry.ino ||
        authoritative.generation!=entry->dentry.generation) return EFS_OK;
    /* Select exactly one physical source during directory spreading. */
    uint32_t chosen=efs_kv_inode_shard(entry->parent);
    if (parent->row.layout!=EFS_META_LAYOUT_LOCAL) {
        uint32_t hashed=efs_kv_dentry_shard(entry->parent,entry->name,EFS_META_LAYOUT_HASHED);
        uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_DENT_BYTES];uint32_t kl,vl=sizeof(value);
        rc=efs_kv_key_dentry(hashed,entry->parent,entry->name,key,&kl);
        if (!rc) rc=efs_kv_get((struct efs_kv *)&f->c->kv,key,kl,value,&vl);
        if (rc==EFS_OK || parent->row.layout==EFS_META_LAYOUT_HASHED) chosen=hashed;
        else if (rc!=EFS_ERR_NOT_FOUND) return rc;
    }
    if (chosen!=f->shard) return EFS_OK;
    size_t len=strlen(path),nl=strlen(entry->name);
    if (len+nl+(len ? 1 : 0)>=sizeof(path)) return EFS_ERR_NAMETOOLONG;
    if (len) path[len++]='/';
    memcpy(path+len,entry->name,nl+1);
    return f->emit(f->user,entry,path);
}
int efs_query_catalog_find(const struct efs_query_catalog *c, efs_ino_t root, enum efs_name_match match,
                            const char *term, uint64_t visit_budget,
                            int (*emit)(void *,const struct efs_name_entry *,const char *), void *user)
{
    if (!c || !term || !emit || !visit_budget) return EFS_ERR_INVAL;
    struct node *n=find_node(c,root);
    if (!n) return EFS_ERR_NOT_FOUND;
    if (!S_ISDIR(n->row.mode)) return EFS_ERR_INVAL;
    struct find_context f={.c=c,.root=root,.emit=emit,.user=user};
    for (uint32_t shard=0;shard<4096;shard++) {
        f.shard=shard;uint32_t cursor=0;
        uint32_t visits=0;
        uint32_t budget=visit_budget>UINT32_MAX ? UINT32_MAX : (uint32_t)visit_budget;
        int rc=efs_name_index_query_visits(c->part[shard]->names,match,term,&cursor,budget,find_candidate,&f,&visits);
        if (rc) return rc;
        visit_budget-=visits;
    }
    return EFS_OK;
}

/* Per-query scan accounting must never mutate the shared immutable catalog.
 * Charge every visited lane record, including tombstones skipped by readdir. */
struct listing_scan {
    const struct efs_query_catalog *catalog;
    uint64_t remaining;
    int (*stop)(void *);
    void *stop_ctx;
    int (*callback)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t);
    void *user;
};
static int listing_record(void *ctx,const uint8_t *key,uint32_t kn,
                          const uint8_t *value,uint32_t vn)
{
    struct listing_scan *s=ctx;
    if (s->stop && s->stop(s->stop_ctx)) return EFS_ERR_BUSY;
    if (!s->remaining) return EFS_ERR_OVERFLOW;
    s->remaining--;
    return s->callback(s->user,key,kn,value,vn);
}
static int listing_get(void *ctx,const uint8_t *key,uint32_t kn,uint8_t *value,uint32_t *vn)
{
    struct listing_scan *s=ctx;
    return cache_get((void *)s->catalog,key,kn,value,vn);
}
static int listing_from(void *ctx,const uint8_t *prefix,uint32_t pn,
                        const uint8_t *start,uint32_t sn,
                        int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *user)
{
    struct listing_scan *s=ctx;
    s->callback=cb;s->user=user;
    return cache_scan((void *)s->catalog,prefix,pn,start,sn,listing_record,s);
}
static int listing_prefix(void *ctx,const uint8_t *prefix,uint32_t pn,
                          int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *user)
{
    return listing_from(ctx,prefix,pn,NULL,0,cb,user);
}
static const struct efs_kv_ops listing_ops={
    .get=listing_get,.scan_prefix=listing_prefix,.scan_from=listing_from
};

int efs_query_catalog_ls_bounded(const struct efs_query_catalog *c, efs_ino_t root,
                          uint64_t budget,int (*stop)(void *),void *stop_ctx,
                          int (*emit)(void *,const struct efs_name_entry *,const char *),void *user)
{
    if (!c || !emit || !budget) return EFS_ERR_INVAL;
    struct node *node=find_node(c,root);
    if (!node) return EFS_ERR_NOT_FOUND;
    if (!S_ISDIR(node->row.mode)) return EFS_ERR_INVAL;
    struct listing_scan scan={.catalog=c,.remaining=budget,.stop=stop,.stop_ctx=stop_ctx};
    struct efs_kv bounded={.ops=&listing_ops,.ctx=&scan};
    struct efs_meta_dir_cursor cursor={0};
    while (!cursor.done) {
        struct efs_meta_dir_ent entries[64];uint32_t n=0;
        int rc=efs_meta_apply_readdir(&bounded,root,&cursor,entries,64,&n);
        if (rc) return rc;
        for (uint32_t i=0;i<n;i++) {
            struct efs_name_entry e={0};e.parent=root;e.dentry=entries[i].d;
            memcpy(e.name,entries[i].name,sizeof(e.name));
            rc=emit(user,&e,e.name);
            if (rc) return rc;
        }
    }
    return EFS_OK;
}

int efs_query_catalog_ls(const struct efs_query_catalog *c,efs_ino_t root,uint64_t budget,
                          int (*emit)(void *,const struct efs_name_entry *,const char *),void *user)
{
    return efs_query_catalog_ls_bounded(c,root,budget,NULL,NULL,emit,user);
}
