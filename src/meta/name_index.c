#include "efs/name_index.h"
#include <stdlib.h>
#include <string.h>

struct efs_name_index {
    uint64_t revision;
    uint64_t max_entries;
    uint32_t count, capacity;
    struct efs_name_entry *entries;
};

void efs_name_index_free(struct efs_name_index *index)
{
    if (!index) return;
    free(index->entries);
    free(index);
}
uint64_t efs_name_index_revision(const struct efs_name_index *index)
{
    return index ? index->revision : 0;
}
uint32_t efs_name_index_count(const struct efs_name_index *index)
{
    return index ? index->count : 0;
}
static int has_pending(void *user, const uint8_t *key, uint32_t klen,
                        const uint8_t *val, uint32_t vlen)
{
    (void)user; (void)key; (void)klen; (void)val; (void)vlen;
    return EFS_ERR_BUSY;
}
static int append_entry(void *user, const uint8_t *key, uint32_t klen,
                         const uint8_t *val, uint32_t vlen)
{
    struct efs_name_index *index=user;
    struct efs_name_entry entry;
    if (klen<=11 || klen-11>=EFS_MAX_NAME || memchr(key+11,0,klen-11) ||
        memchr(key+11,'/',klen-11)) return EFS_ERR_PROTO;
    memset(&entry,0,sizeof(entry));
    int rc=efs_meta_unpack_dentry(val,vlen,&entry.dentry);
    if (rc!=EFS_OK) return rc;
    for (uint32_t i=3;i<11;i++) entry.parent=(entry.parent<<8)|key[i];
    memcpy(entry.name,key+11,klen-11);
    if (index->count==index->capacity) {
        uint64_t next=index->capacity ? (uint64_t)index->capacity*2 : 128;
        if (next>index->max_entries) next=index->max_entries;
        if (next<=index->capacity) return EFS_ERR_NOMEM;
        struct efs_name_entry *p=realloc(index->entries,next*sizeof(*p));
        if (!p) return EFS_ERR_NOMEM;
        index->entries=p;
        index->capacity=(uint32_t)next;
    }
    index->entries[index->count++]=entry;
    return EFS_OK;
}
static int entry_compare(const void *ap, const void *bp)
{
    const struct efs_name_entry *a=ap,*b=bp;
    int rc=strcmp(a->name,b->name);
    if (rc) return rc;
    return a->parent<b->parent ? -1 : a->parent>b->parent ? 1 : 0;
}
int efs_name_index_build(struct efs_kv_lsm_snapshot *source, uint32_t shard,
                          uint64_t max_bytes, struct efs_name_index **out)
{
    uint8_t prefix[3]={(uint8_t)(shard>>8),(uint8_t)shard,0};
    uint64_t revisions[4096];
    if (out) *out=NULL;
    if (!source || !out || shard>EFS_KV_SHARD_MASK) return EFS_ERR_INVAL;
    if (max_bytes<sizeof(struct efs_name_index)) return EFS_ERR_NOMEM;
    const uint8_t pending[]={EFS_KV_KIND_INTENT,EFS_KV_KIND_GUARD,EFS_KV_KIND_REDUCE};
    for (uint32_t i=0;i<sizeof(pending);i++) {
        prefix[2]=pending[i];
        int rc=efs_kv_lsm_snapshot_scan(source,prefix,3,NULL,0,has_pending,NULL);
        if (rc!=EFS_OK) return rc;
    }
    struct efs_name_index *index=calloc(1,sizeof(*index));
    if (!index) return EFS_ERR_NOMEM;
    index->max_entries=(max_bytes-sizeof(*index))/sizeof(*index->entries);
    if (index->max_entries>UINT32_MAX) index->max_entries=UINT32_MAX;
    if (index->max_entries>SIZE_MAX/sizeof(*index->entries))
        index->max_entries=SIZE_MAX/sizeof(*index->entries);
    int rc=efs_kv_lsm_snapshot_revisions(source,revisions,4096);
    if (rc==EFS_OK) {
        index->revision=revisions[shard];
        prefix[2]=EFS_KV_KIND_DENTRY;
        rc=efs_kv_lsm_snapshot_scan(source,prefix,3,NULL,0,append_entry,index);
    }
    if (rc!=EFS_OK) { efs_name_index_free(index);return rc; }
    if (index->count>1)
        qsort(index->entries,index->count,sizeof(*index->entries),entry_compare);
    *out=index;
    return EFS_OK;
}
static uint32_t lower_bound(const struct efs_name_index *index, const char *term)
{
    uint32_t lo=0,hi=index->count;
    while (lo<hi) {
        uint32_t mid=lo+(hi-lo)/2;
        if (strcmp(index->entries[mid].name,term)<0) lo=mid+1;
        else hi=mid;
    }
    return lo;
}
int efs_name_index_query(const struct efs_name_index *index,
                          enum efs_name_match match, const char *term,
                          uint32_t *cursor, uint32_t visit_budget,
                          int (*emit)(void *, const struct efs_name_entry *),
                          void *user)
{
    if (!index || !term || !cursor || !emit || match<EFS_NAME_EXACT ||
        match>EFS_NAME_CONTAINS || *cursor>index->count || !visit_budget ||
        strnlen(term,EFS_MAX_NAME)>=EFS_MAX_NAME) return EFS_ERR_INVAL;
    size_t len=strlen(term);
    uint32_t at=*cursor,visited=0;
    if (match==EFS_NAME_EXACT || match==EFS_NAME_PREFIX) {
        uint32_t first=lower_bound(index,term);
        if (at<first) at=first;
    }
    for (;at<index->count;at++) {
        const struct efs_name_entry *entry=&index->entries[at];
        size_t nl=strlen(entry->name);
        int matches=0;
        switch (match) {
        case EFS_NAME_EXACT: matches=!strcmp(entry->name,term);break;
        case EFS_NAME_PREFIX: matches=!strncmp(entry->name,term,len);break;
        case EFS_NAME_SUFFIX: matches=nl>=len && !memcmp(entry->name+nl-len,term,len);break;
        case EFS_NAME_CONTAINS: matches=strstr(entry->name,term)!=NULL;break;
        }
        if (!matches && (match==EFS_NAME_EXACT || match==EFS_NAME_PREFIX)) break;
        if (visited==visit_budget) { *cursor=at;return EFS_ERR_BUSY; }
        visited++;
        if (matches) {
            int rc=emit(user,entry);
            if (rc!=EFS_OK) { *cursor=at;return rc; }
        }
        *cursor=at+1;
    }
    *cursor=at;
    return EFS_OK;
}
