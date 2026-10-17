/* Included only by query_disk.c: candidate-only bounded scratch. A positive
 * FULL result is a resource/coverage fallback, never an IO-error fallback. */
#define INCREMENTAL_FULL 1
#define DIRTY_MAX 1024
#define DIRTY_HEAP_MAX (16u << 20)
#define CHANGE_BATCH 256
struct dirty_dir { struct disk_node node;uint64_t at;unsigned pending,done; };
struct stat_change_range { efs_ino_t first,last; };
struct incremental {
    uint64_t changed_partitions,changes,change_bytes;
    uint64_t changed_shards[SHARDS/64]; /* exact set behind changed_partitions */
    int changes_fd;
    unsigned buffered,count,capacity;
    struct dirty_dir *dirs;
    size_t heap_bytes;
    efs_ino_t changed[CHANGE_BATCH];
    struct dirty_dir initial[DIRTY_MAX];
    struct stat_change_range stat_changes[SHARDS][3];
    struct stat_change_range alias_bounds[SHARDS];
    uint8_t alias_known[SHARDS];
    efs_ino_t sort_ids[SORT_ROWS];
    struct disk_alias alias_rows[CHANGE_BATCH];
};
/* Each inode is visited once in the candidate stream; removed IDs occur only
 * in the old-only pass. No namespace-sized deduplication set is needed.
 * Keep the journal private, disk-budgeted and transient. IO failures must
 * abort publication, never silently become a full-rebuild fallback. */
static int changed_flush(struct incremental *s)
{
    if(!s->buffered)return EFS_OK;
    uint64_t offset=(s->changes-s->buffered)*sizeof(efs_ino_t);
    int rc=io_at(s->changes_fd,s->changed,s->buffered*sizeof(efs_ino_t),offset,1);
    if(!rc)s->buffered=0;
    return rc;
}
static int changed_add(struct efs_query_disk *d,struct incremental *s,efs_ino_t ino)
{
    if(s->changes>=INT64_MAX/sizeof(efs_ino_t))return EFS_ERR_OVERFLOW;
    int rc=charge_disk(d,sizeof(efs_ino_t));if(rc)return rc;
    s->change_bytes+=sizeof(efs_ino_t);
    if(s->changes_fd<0){s->changes_fd=make_file(d,0,'c');if(s->changes_fd<0)return EFS_ERR_IO;}
    s->changed[s->buffered++]=ino;s->changes++;
    return s->buffered==CHANGE_BATCH?changed_flush(s):EFS_OK;
}
static unsigned dir_lower(const struct incremental *s,efs_ino_t ino)
{
    unsigned lo=0,hi=s->count;
    while(lo<hi){unsigned mid=lo+(hi-lo)/2;if(s->dirs[mid].node.row.ino<ino)lo=mid+1;else hi=mid;}
    return lo;
}
static struct dirty_dir *dirty_find(struct incremental *s,efs_ino_t ino)
{ unsigned at=dir_lower(s,ino);return at<s->count && s->dirs[at].node.row.ino==ino?&s->dirs[at]:NULL; }
static int dirty_add(struct efs_query_disk *d,struct incremental *s,efs_ino_t ino)
{
    unsigned at=dir_lower(s,ino);
    if(at<s->count && s->dirs[at].node.row.ino==ino)return EFS_OK;
    if(s->count==s->capacity){
        size_t bytes=(size_t)s->capacity*2*sizeof(*s->dirs);
        /* Charge simultaneous old/new scratch before allocation. The inline
         * initial buffer stays in working admission; heap growth is bounded
         * by both the caller's unchanged memory cap and a 16MiB scratch cap. */
        if(bytes>DIRTY_HEAP_MAX || bytes>d->limit-d->bytes){
            fprintf(stderr,"query-disk: incremental fallback=dirty-directory-budget count=%u limit=%u ino=%llu\n",
                    s->count,s->capacity,(unsigned long long)ino);
            return INCREMENTAL_FULL;
        }
        struct dirty_dir *next=malloc(bytes);if(!next)return EFS_ERR_NOMEM;
        memcpy(next,s->dirs,s->count*sizeof(*next));
        if(s->heap_bytes)free(s->dirs);
        d->bytes+=bytes-s->heap_bytes;s->heap_bytes=bytes;
        s->dirs=next;s->capacity*=2;
    }
    struct dirty_dir dir={0};int rc=locate(d,ino,&dir.node,&dir.at);
    if(rc==EFS_ERR_NOT_FOUND){
        fprintf(stderr,"query-disk: incremental fallback=missing-directory ino=%llu count=%u\n",
                (unsigned long long)ino,s->count);
        return INCREMENTAL_FULL;
    }
    if(rc)return rc;
    if(!S_ISDIR(dir.node.row.mode))return EFS_ERR_PROTO;
    memmove(s->dirs+at+1,s->dirs+at,(s->count-at)*sizeof(*s->dirs));
    s->dirs[at]=dir;s->count++;return EFS_OK;
}
static int inputs_changed(const struct efs_query_disk *d,const struct efs_query_disk *old,const struct efs_meta_row *r)
{
    uint64_t lanes=S_ISDIR(r->mode)?(r->layout==EFS_META_LAYOUT_LOCAL?0:r->used_shards):r->active_lanes;
    while(lanes){unsigned lane=(unsigned)__builtin_ctzll(lanes);lanes&=lanes-1;
        uint32_t dep=efs_kv_lane_shard(r->ino,(uint8_t)lane);
        if(d->part[dep].revision!=old->part[dep].revision)return 1;
    }
    return 0;
}
/* Revisions include non-dentry edits. Reuse immutable name/alias files only
 * after proving the entire sorted dentry family byte-identical. Counts alone
 * never prove equality; errors/cancellation abort the candidate. */
static int dentry_family_same(struct efs_query_disk *d,struct efs_query_disk *old,
                              uint32_t shard,int *same)
{
    *same=0;if(d->part[shard].names!=old->part[shard].names)return EFS_OK;
    uint8_t ak[EFS_KV_KEY_MAX],bk[EFS_KV_KEY_MAX],av[VALUE_MAX],bv[VALUE_MAX];
    for(uint64_t i=0;i<d->part[shard].names;i++){
        if(cancelled(d))return EFS_ERR_BUSY;
        struct slot a,b;int rc=read_record(d,shard,d->part[shard].begin[1]+i,&a,ak,av);
        if(!rc)rc=read_record(old,shard,old->part[shard].begin[1]+i,&b,bk,bv);
        if(rc)return rc;
        if(a.key_len!=b.key_len || a.value_len!=b.value_len ||
           memcmp(ak,bk,a.key_len) || memcmp(av,bv,a.value_len))return EFS_OK;
    }
    *same=1;return EFS_OK;
}
/* Compare complete immutable stat-input families once. These inclusive inode
 * bounds are conservative: a hole within a changed range still uses the exact
 * lookup below. Outside the range, all generations/lanes of that inode,
 * including absence, are proven unchanged by a complete sorted stream merge.
 * Fixed scratch is included in sizeof(incremental) working admission. */
static int stat_family_changes(struct efs_query_disk *d,struct efs_query_disk *old,
                               struct incremental *state,uint32_t shard,unsigned family)
{
    const unsigned indexes[]={2,4,5};unsigned kind=indexes[family];
    struct stat_change_range *range=&state->stat_changes[shard][family];
    uint64_t a=d->part[shard].begin[kind],b=old->part[shard].begin[kind];
    uint64_t ae=d->part[shard].end[kind],be=old->part[shard].end[kind];
    uint8_t ak[EFS_KV_KEY_MAX],bk[EFS_KV_KEY_MAX],av[VALUE_MAX],bv[VALUE_MAX];
    struct slot as={0},bs={0};int have_a=0,have_b=0;
    while(a<ae || b<be){
        if(cancelled(d))return EFS_ERR_BUSY;
        int rc=EFS_OK;
        if(a<ae && !have_a){rc=read_record(d,shard,a,&as,ak,av);have_a=1;}
        if(!rc && b<be && !have_b){rc=read_record(old,shard,b,&bs,bk,bv);have_b=1;}
        if(rc)return rc;
        if((a<ae && as.key_len<11) || (b<be && bs.key_len<11))return EFS_ERR_PROTO;
        int order=a==ae?1:b==be?-1:cmpkey(ak,as.key_len,bk,bs.key_len);
        int changed=order || as.value_len!=bs.value_len || memcmp(av,bv,as.value_len);
        if(changed){
            const uint8_t *key=order>0?bk:ak;efs_ino_t ino=0;
            for(unsigned i=3;i<11;i++)ino=(ino<<8)|key[i];
            if(!ino)return EFS_ERR_PROTO;
            if(!range->first || ino<range->first)range->first=ino;
            if(ino>range->last)range->last=ino;
        }
        if(order<=0){a++;have_a=0;}
        if(order>=0){b++;have_b=0;}
    }
    return EFS_OK;
}
/* Whole-partition revisions are conservative invalidation hints. For regular
 * files, getattr reads the canonical inode, active lane records and append
 * cursor. Pending transaction/guard/reduction rows were excluded when the raw
 * snapshot was built. Prove those exact inputs unchanged before reusing stat;
 * compare PVER conservatively as well for publication-view evolution. */
static int stat_value_same(struct efs_query_disk *d,struct efs_query_disk *old,
                          const struct incremental *state,
                          const uint8_t *key,uint32_t length,int *same)
{
    uint32_t shard=((uint32_t)key[0]<<8)|key[1];
    if(d->part[shard].revision==old->part[shard].revision){*same=1;return EFS_OK;}
    unsigned family=key[2]==EFS_KV_KIND_LANE?0:key[2]==EFS_KV_KIND_APPEND_CUR?1:2;
    const struct stat_change_range *range=&state->stat_changes[shard][family];
    efs_ino_t ino=0;for(unsigned i=3;i<11;i++)ino=(ino<<8)|key[i];
    if(!range->first || ino<range->first || ino>range->last){*same=1;return EFS_OK;}
    int kind=kind_at(key[2]);
    if(kind>=0 && d->part[shard].begin[kind]==d->part[shard].end[kind] &&
       old->part[shard].begin[kind]==old->part[shard].end[kind]){
        *same=key[2]!=EFS_KV_KIND_LANE;return EFS_OK;
    }
    uint8_t a[VALUE_MAX],b[VALUE_MAX];uint32_t an=sizeof(a),bn=sizeof(b);
    int ar=raw_get(d,key,length,a,&an),br=raw_get(old,key,length,b,&bn);
    if(ar && ar!=EFS_ERR_NOT_FOUND)return ar;
    if(br && br!=EFS_ERR_NOT_FOUND)return br;
    /* Within a changed range, absent active lanes retain the conservative
     * exact-lookup behavior. The complete stream proof above is stronger. */
    *same=ar==br && ((ar==EFS_ERR_NOT_FOUND && key[2]!=EFS_KV_KIND_LANE) ||
                    (!ar && an==bn && !memcmp(a,b,an)));
    return EFS_OK;
}
static int file_stat_same(struct efs_query_disk *d,struct efs_query_disk *old,
                          const struct incremental *state,
                          const struct efs_meta_row *row,int *same)
{
    uint8_t key[EFS_KV_KEY_MAX];uint32_t length;int rc=EFS_OK;
    *same=1;
    uint64_t lanes=row->active_lanes;
    while(lanes && !rc && *same){
        if(cancelled(d))return EFS_ERR_BUSY;
        unsigned lane=(unsigned)__builtin_ctzll(lanes);lanes&=lanes-1;
        rc=efs_kv_key_lane(efs_kv_lane_shard(row->ino,(uint8_t)lane),row->ino,row->generation,(uint8_t)lane,key,&length);
        if(!rc)rc=stat_value_same(d,old,state,key,length,same);
    }
    if(!rc && *same){
        rc=efs_kv_key_append_cur(efs_kv_inode_shard(row->ino),row->ino,row->generation,key,&length);
        if(!rc)rc=stat_value_same(d,old,state,key,length,same);
    }
    if(!rc && *same){
        rc=efs_kv_key_pver(efs_kv_inode_shard(row->ino),row->ino,key,&length);
        if(!rc)rc=stat_value_same(d,old,state,key,length,same);
    }
    return rc;
}
/* Whether an unchanged shard may hold a node whose getattr inputs changed.
 * Exact when the old generation knows the shard's dependency row; the
 * 64-bit filter otherwise. The candidate inherits a known row. */
static int dependencies_changed(struct efs_query_disk *d,struct efs_query_disk *old,
                                const struct incremental *s,uint32_t shard)
{
    if(d->deps && old->deps && old->deps_known[shard]){
        memcpy(d->deps[shard],old->deps[shard],sizeof(d->deps[shard]));
        d->deps_known[shard]=1;
        for(unsigned w=0;w<SHARDS/64;w++)if(d->deps[shard][w]&s->changed_shards[w])return 1;
        return 0;
    }
    /* An empty filter is exact: the zeroed row becomes known. */
    if(!old->part[shard].dependencies){d->deps_known[shard]=d->deps!=NULL;return 0;}
    return (old->part[shard].dependencies&s->changed_partitions)!=0;
}
static int incremental_nodes(struct efs_query_disk *d,struct efs_query_disk *old,struct incremental *s,uint32_t shard)
{
    int same=d->part[shard].revision==old->part[shard].revision;
    int rc=EFS_OK,fd=-1;struct disk_node batch[NODE_BATCH];unsigned used=0;
    if(same && d->part[shard].inodes){
        char from[EFS_MAX_PATH],to[EFS_MAX_PATH];
        rc=filename(old,shard,'d',from,sizeof(from));if(!rc)rc=filename(d,shard,'d',to,sizeof(to));
        if(!rc && link(from,to))rc=EFS_ERR_IO;
        if(!rc)d->shared_d[shard]=1;
    } else if(d->part[shard].inodes){fd=make_file(d,shard,'d');if(fd<0)rc=EFS_ERR_IO;}
    if(rc)return rc;
    if(same && !dependencies_changed(d,old,s,shard))return rc;
    if(same && d->walked<UINT32_MAX)d->walked++;
    /* Walked in full below: recompute the exact row from every node. */
    dependency_reset(d,shard);
    if(!same)d->part[shard].dependencies=0;
    efs_ino_t previous=0;uint64_t old_at=0;
    for(uint64_t i=0;i<d->part[shard].inodes && !rc;i++){
        if(cancelled(d)){rc=EFS_ERR_BUSY;break;}
        struct disk_node n={0},before={0};int found=0,fold=0;
        if(same){rc=node_at(old,shard,i,&n,0);before=n;found=1;fold=inputs_changed(d,old,&n.row);}
        else {
            struct slot slot;uint8_t key[EFS_KV_KEY_MAX],val[VALUE_MAX];
            rc=read_record(d,shard,d->part[shard].begin[0]+i,&slot,key,val);
            if(!rc)rc=efs_meta_unpack_inode(val,slot.value_len,&n.row);
            efs_ino_t encoded=0;
            if(!rc && slot.key_len!=11)rc=EFS_ERR_PROTO;
            if(!rc)for(unsigned k=3;k<11;k++)encoded=(encoded<<8)|key[k];
            if(!rc && (encoded!=n.row.ino || efs_kv_inode_shard(encoded)!=shard || (i && encoded<=previous)))rc=EFS_ERR_PROTO;
            previous=encoded;if(rc)break;
            /* Both streams are inode-sorted. A forward merge preserves the
             * old stat/link without a binary lookup for every unchanged row. */
            while(old_at<old->part[shard].inodes){
                if(cancelled(d)){rc=EFS_ERR_BUSY;break;}
                rc=node_at(old,shard,old_at,&before,0);if(rc)break;
                if(before.row.ino>=encoded){found=before.row.ino==encoded;break;}
                /* This old-only ID precedes the next candidate ID. Record
                 * removal now, avoiding a second scan of both streams. */
                rc=changed_add(d,s,before.row.ino);if(rc)break;
                old_at++;
            }
            if(rc)break;
            if(found)old_at++;
            /* Own-shard lane/PVER/DSEQ changes can affect getattr even if the
             * inode row is identical. Re-fold unless all stat inputs are proved row-only. */
            fold=1;
            if(found){
                n.stat=before.stat;n.rollup=before.rollup;n.link=before.link;n.children=before.children;
                /* With no stat inputs outside an identical canonical row,
                 * another inode's own-shard edit cannot change this stat.
                 * Regular files also require an empty APPEND_CUR family in
                 * both views; otherwise even inactive lanes may have an open
                 * append frontier. Keep all lane/auxiliary cases conservative. */
                int row_only=S_ISDIR(n.row.mode)?n.row.layout==EFS_META_LAYOUT_LOCAL:
                    (S_ISREG(n.row.mode) && !n.row.active_lanes &&
                     d->part[shard].begin[4]==d->part[shard].end[4] &&
                     old->part[shard].begin[4]==old->part[shard].end[4]);
                if(row_only && !memcmp(&n.row,&before.row,sizeof(n.row)))fold=0;
            }
        }
        if(!rc)dependency_note(d,shard,&n.row);
        if(!rc && fold && found && S_ISREG(n.row.mode) && !memcmp(&n.row,&before.row,sizeof(n.row))){
            int unchanged=0;rc=file_stat_same(d,old,s,&n.row,&unchanged);
            if(!rc && unchanged)fold=0;
        }
        if(!rc && fold){rc=efs_meta_apply_getattr(&d->kv,n.row.ino,refuse_coord,NULL,&n.stat);if(!rc && d->refolded<UINT32_MAX)d->refolded++;}
        int changed=!found || memcmp(&n.row,&before.row,sizeof(n.row)) || memcmp(&n.stat,&before.stat,sizeof(n.stat));
        if(!rc && changed)rc=changed_add(d,s,n.row.ino);
        if(!rc && same && changed)rc=node_at(d,shard,i,&n,1);
        if(!rc && !same){batch[used++]=n;
            if(used==NODE_BATCH || i+1==d->part[shard].inodes){rc=io_at(fd,batch,used*sizeof(*batch),(i+1-used)*sizeof(*batch),1);used=0;}
        }
    }
    while(!rc && !same && old_at<old->part[shard].inodes){
        if(cancelled(d)){rc=EFS_ERR_BUSY;break;}
        struct disk_node removed;rc=node_at(old,shard,old_at++,&removed,0);
        if(!rc)rc=changed_add(d,s,removed.row.ino);
    }
    if(!rc)d->deps_known[shard]=d->deps!=NULL;
    if(fd>=0)close(fd);
    return rc;
}
/* An old-only edge may belong to a removed subtree. Its surviving ancestor
 * is independently invalidated by that directory's old parent edge. A missing
 * old parent therefore needs no candidate rollup; new edges still require a
 * real parent and keep the strict validation/fallback below. */
static int dirty_old_parent(struct efs_query_disk *d,struct incremental *s,efs_ino_t ino)
{
    if(dirty_find(s,ino))return EFS_OK;
    struct disk_node node;int rc=locate(d,ino,&node,NULL);
    if(rc==EFS_ERR_NOT_FOUND)return EFS_OK;
    return rc ? rc : dirty_add(d,s,ino);
}
/* Compare only revised dentry streams. Mark both sides of deletes/renames,
 * not just parents visible in the new generation. */
static int changed_dentries(struct efs_query_disk *d,struct efs_query_disk *old,struct incremental *s,uint32_t shard)
{
    uint64_t a=0,b=0;int rc=EFS_OK;
    while((a<old->part[shard].names || b<d->part[shard].names) && !rc){
        if(cancelled(d))return EFS_ERR_BUSY;
        struct slot x={0},y={0};uint8_t ak[EFS_KV_KEY_MAX],bk[EFS_KV_KEY_MAX],av[VALUE_MAX],bv[VALUE_MAX];
        int havea=a<old->part[shard].names,haveb=b<d->part[shard].names;
        if(havea)rc=read_record(old,shard,old->part[shard].begin[1]+a,&x,ak,av);
        if(!rc && haveb)rc=read_record(d,shard,d->part[shard].begin[1]+b,&y,bk,bv);
        if(rc)break;
        int cmp=!havea?1:!haveb?-1:cmpkey(ak,x.key_len,bk,y.key_len);
        int different=cmp || x.value_len!=y.value_len || memcmp(av,bv,x.value_len);
        struct efs_name_entry e;
        if(different && cmp<=0 && havea){rc=entry_from(ak,x.key_len,av,x.value_len,&e);if(!rc)rc=dirty_old_parent(d,s,e.parent);}
        if(different && cmp>=0 && haveb && !rc){rc=entry_from(bk,y.key_len,bv,y.value_len,&e);if(!rc)rc=dirty_add(d,s,e.parent);}
        if(cmp<=0)a++;
        if(cmp>=0)b++;
    }
    return rc;
}
/* Visit each shard with ascending changed IDs. Once its alias index ends,
 * the remaining larger IDs cannot have links there. Keep the lower bound
 * between searches instead of restarting every ID from zero. The fixed
 * journal batch is private scratch and needs no additional allocation. */
static int changed_ino_compare(const void *ap,const void *bp)
{
    efs_ino_t a=*(const efs_ino_t *)ap,b=*(const efs_ino_t *)bp;
    return a<b ? -1 : a>b ? 1 : 0;
}
static int aliases_dirty_batch(struct efs_query_disk *d,struct incremental *s,
                               efs_ino_t *changed,unsigned count)
{
    if(!count)return cancelled(d)?EFS_ERR_BUSY:EFS_OK;
    qsort(changed,count,sizeof(*changed),changed_ino_compare);
    for(uint32_t shard=0;shard<SHARDS;shard++){
        if(cancelled(d))return EFS_ERR_BUSY;
        if(!d->part[shard].names)continue;
        if(!s->alias_known[shard]){
            struct disk_alias first,last;int rc=disk_io(d,shard,'a',&first,sizeof(first),0,0);
            if(rc)return rc;
            if(d->part[shard].names>1)rc=disk_io(d,shard,'a',&last,sizeof(last),(d->part[shard].names-1)*sizeof(last),0);
            else last=first;
            if(rc)return rc;
            if(first.child>last.child || first.ordinal>=d->part[shard].names || last.ordinal>=d->part[shard].names)return EFS_ERR_PROTO;
            s->alias_bounds[shard]=(struct stat_change_range){first.child,last.child};
            s->alias_known[shard]=1;
        }
        const struct stat_change_range *bounds=&s->alias_bounds[shard];
        if(changed[0]>bounds->last || changed[count-1]<bounds->first)continue;
        uint64_t at=0;
        for(unsigned i=0;i<count && at<d->part[shard].names;i++){
            if(cancelled(d))return EFS_ERR_BUSY;
            if(i && changed[i]==changed[i-1])continue;
            efs_ino_t ino=changed[i];
            if(ino<bounds->first)continue;
            if(ino>bounds->last)break;
            uint64_t lo=at,hi=d->part[shard].names;struct disk_alias alias;int rc=EFS_OK;
            while(lo<hi){uint64_t mid=lo+(hi-lo)/2;
                rc=disk_io(d,shard,'a',&alias,sizeof(alias),mid*sizeof(alias),0);if(rc)return rc;
                if(alias.child<ino)lo=mid+1;else hi=mid;}
            at=lo;
            for(;at<d->part[shard].names;at++){
                rc=disk_io(d,shard,'a',&alias,sizeof(alias),at*sizeof(alias),0);if(rc)return rc;
                if(alias.child!=ino)break;
                if(alias.ordinal>=d->part[shard].names)return EFS_ERR_PROTO;
                rc=dirty_add(d,s,alias.parent);if(rc)return rc;
            }
        }
    }
    return EFS_OK;
}

/* Large change sets use one globally sorted private journal, not repeated
 * searches of every alias partition for each 256-ID batch. Fixed merge buffers
 * live in admitted heap scratch; transient run files are charged at their
 * simultaneous peak, and every error aborts publication. */
static int changes_sort(struct efs_query_disk *d,struct incremental *s)
{
    if(s->changes<=SORT_ROWS){
        int rc=io_at(s->changes_fd,s->sort_ids,s->changes*sizeof(efs_ino_t),0,0);
        if(!rc)qsort(s->sort_ids,(size_t)s->changes,sizeof(efs_ino_t),changed_ino_compare);
        if(!rc)rc=io_at(s->changes_fd,s->sort_ids,s->changes*sizeof(efs_ino_t),0,1);
        return rc;
    }
    if(s->change_bytes>UINT64_MAX/2)return EFS_ERR_OVERFLOW;
    uint64_t extra=s->change_bytes*2;int rc=charge_disk(d,extra);if(rc)return rc;
    uint64_t runs=0;unsigned pass=0;char path[EFS_MAX_PATH];
    for(uint64_t at=0;at<s->changes && !rc;at+=SORT_ROWS){
        if(cancelled(d)){rc=EFS_ERR_BUSY;break;}
        unsigned n=(unsigned)(s->changes-at>SORT_ROWS?SORT_ROWS:s->changes-at);
        rc=io_at(s->changes_fd,s->sort_ids,n*sizeof(efs_ino_t),at*sizeof(efs_ino_t),0);
        if(!rc)qsort(s->sort_ids,n,sizeof(efs_ino_t),changed_ino_compare);
        if(!rc)rc=run_path(d,SHARDS,0,runs++,path);
        int fd=rc?-1:open(path,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);
        if(!rc && fd<0)rc=EFS_ERR_IO;
        if(!rc)rc=io_at(fd,s->sort_ids,n*sizeof(efs_ino_t),0,1);
        if(fd>=0 && close(fd) && !rc)rc=EFS_ERR_IO;
    }
    while(runs>1 && !rc){
        for(uint64_t i=0;i<runs && !rc;i+=2){
            char a[EFS_MAX_PATH],b[EFS_MAX_PATH],out[EFS_MAX_PATH];
            rc=run_path(d,SHARDS,pass,i,a);
            if(!rc)rc=run_path(d,SHARDS,pass+1,i/2,out);
            if(rc)break;
            if(i+1==runs){if(rename(a,out))rc=EFS_ERR_IO;continue;}
            rc=run_path(d,SHARDS,pass,i+1,b);if(rc)break;
            int fa=open(a,O_RDONLY|O_CLOEXEC),fb=open(b,O_RDONLY|O_CLOEXEC),fo=open(out,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);
            struct stat sa,sb;
            if(fa<0 || fb<0 || fo<0 || fstat(fa,&sa) || fstat(fb,&sb))rc=EFS_ERR_IO;
            if(!rc && (sa.st_size<0 || sb.st_size<0 || sa.st_size%sizeof(efs_ino_t) || sb.st_size%sizeof(efs_ino_t)))rc=EFS_ERR_IO;
            if(!rc)rc=merge_names(d,fa,fb,fo,(uint64_t)sa.st_size/sizeof(efs_ino_t),(uint64_t)sb.st_size/sizeof(efs_ino_t),sizeof(efs_ino_t),changed_ino_compare,(uint8_t *)s->sort_ids);
            if(fa>=0 && close(fa) && !rc)rc=EFS_ERR_IO;
            if(fb>=0 && close(fb) && !rc)rc=EFS_ERR_IO;
            if(fo>=0 && close(fo) && !rc)rc=EFS_ERR_IO;
            if(!rc){int ua=unlink(a),ub=unlink(b);if(ua || ub)rc=EFS_ERR_IO;}
        }
        runs=(runs+1)/2;pass++;
    }
    if(!rc){
        char final[EFS_MAX_PATH];rc=run_path(d,SHARDS,pass,0,path);
        if(!rc)rc=filename(d,0,'c',final,sizeof(final));
        if(!rc && rename(path,final))rc=EFS_ERR_IO;
        if(!rc){
            if(close(s->changes_fd))rc=EFS_ERR_IO;
            s->changes_fd=-1;
            if(!rc){s->changes_fd=open(final,O_RDWR|O_CLOEXEC);if(s->changes_fd<0)rc=EFS_ERR_IO;}
        }
    }
    if(!rc)d->disk_bytes-=extra;
    return rc;
}
struct change_cursor { uint64_t at;unsigned used,count;efs_ino_t ino;int valid; };
static int change_next(struct efs_query_disk *d,struct incremental *s,struct change_cursor *c)
{
    if(c->at==s->changes){c->valid=0;return EFS_OK;}
    if(c->used==c->count){
        if(cancelled(d))return EFS_ERR_BUSY;
        c->count=(unsigned)(s->changes-c->at>CHANGE_BATCH?CHANGE_BATCH:s->changes-c->at);c->used=0;
        int rc=io_at(s->changes_fd,s->changed,c->count*sizeof(efs_ino_t),c->at*sizeof(efs_ino_t),0);if(rc)return rc;
    }
    c->ino=s->changed[c->used++];c->at++;c->valid=1;return EFS_OK;
}
static int aliases_dirty_sorted(struct efs_query_disk *d,struct incremental *s)
{
    for(uint32_t shard=0;shard<SHARDS;shard++){
        if(cancelled(d))return EFS_ERR_BUSY;
        if(!d->part[shard].names)continue;
        struct change_cursor ids={0};int rc=change_next(d,s,&ids);if(rc)return rc;
        for(uint64_t at=0;at<d->part[shard].names && ids.valid;at+=CHANGE_BATCH){
            if(cancelled(d))return EFS_ERR_BUSY;
            unsigned n=(unsigned)(d->part[shard].names-at>CHANGE_BATCH?CHANGE_BATCH:d->part[shard].names-at);
            rc=disk_io(d,shard,'a',s->alias_rows,n*sizeof(struct disk_alias),at*sizeof(struct disk_alias),0);if(rc)return rc;
            for(unsigned i=0;i<n && ids.valid;i++){
                struct disk_alias *alias=&s->alias_rows[i];
                while(ids.valid && ids.ino<alias->child){rc=change_next(d,s,&ids);if(rc)return rc;}
                if(!ids.valid || alias->child!=ids.ino)continue;
                if(alias->ordinal>=d->part[shard].names)return EFS_ERR_PROTO;
                rc=dirty_add(d,s,alias->parent);if(rc)return rc;
            }
        }
    }
    return EFS_OK;
}

struct parent_fold { struct efs_query_disk *disk;struct incremental *state;uint32_t shard; };
static int parent_fold_entry(void *user,const uint8_t *key,uint32_t kn,const uint8_t *value,uint32_t vn)
{
    struct parent_fold *ctx=user;struct efs_query_disk *d=ctx->disk;
    if(cancelled(d))return EFS_ERR_BUSY;
    struct efs_name_entry e;int rc=entry_from(key,kn,value,vn,&e);if(rc)return rc;
    if(!e.dentry.ino || e.dentry.type==EFS_META_DENT_TOMBSTONE)return EFS_OK;
    struct dirty_dir *parent=dirty_find(ctx->state,e.parent),*child_dir=dirty_find(ctx->state,e.dentry.ino);
    if(!parent && !child_dir)return EFS_OK;
    struct disk_node p,c;uint32_t chosen;
    if(parent)p=parent->node;else rc=locate(d,e.parent,&p,NULL);
    if(!rc)rc=chosen_shard(d,&p.row,&e,&chosen);
    if(rc)return rc;
    if(chosen!=ctx->shard)return EFS_OK;
    if(child_dir)c=child_dir->node;else rc=locate(d,e.dentry.ino,&c,NULL);
    if(rc)return rc;
    if(!S_ISDIR(p.row.mode) || c.row.generation!=e.dentry.generation || c.row.ino==e.parent){rc=EFS_ERR_PROTO;return rc;}
    if(S_ISDIR(c.row.mode) && child_dir){
        if(child_dir->node.link.parent || c.row.parent!=e.parent){rc=EFS_ERR_PROTO;return rc;}
        child_dir->node.link=(struct disk_link){e.parent,ctx->shard};
    }else if(S_ISDIR(c.row.mode)){
        /* Parent layout changes can select a different immutable alias
         * partition even without changing the child's own inode row. */
        if(c.row.parent!=e.parent)return EFS_ERR_PROTO;
        if(c.link.parent!=e.parent || c.link.shard!=ctx->shard){
            uint64_t at;rc=locate(d,c.row.ino,&c,&at);if(rc)return rc;
            c.link=(struct disk_link){e.parent,ctx->shard};
            rc=node_at(d,efs_kv_inode_shard(c.row.ino),at,&c,1);if(rc)return rc;
        }
    }
    if(!parent)return EFS_OK;
    struct efs_query_rollup *r=&parent->node.rollup;int has=r->imm_files+r->imm_dirs!=0;
    if(S_ISDIR(c.row.mode))r->imm_dirs++;
    else{if(UINT64_MAX-r->imm_bytes<c.stat.size){rc=EFS_ERR_QUOTA;return rc;}r->imm_files++;r->imm_bytes+=c.stat.size;}
    expand(&r->imm_tmin,&r->imm_tmax,low_time(&c.stat),high_time(&c.stat),has);
    if(S_ISDIR(c.row.mode)){
        if(child_dir)parent->pending++;
        else{
            int treehas=r->tree_files || r->tree_dirs;
            rc=rollup_tree_add(r,c.rollup.tree_files,c.rollup.tree_dirs,c.rollup.tree_bytes);if(rc)return rc;
            if(c.rollup.tree_files || c.rollup.tree_dirs)expand(&r->tree_tmin,&r->tree_tmax,c.rollup.tree_tmin,c.rollup.tree_tmax,treehas);
        }
    }
    return EFS_OK;
}
static int incremental_build(struct efs_query_disk *d,struct efs_query_disk *old)
{
    uint64_t started=query_build_clock(),ranges_done=0,nodes_done=0,edges_done=0,aliases_done=0;
    /* Accounted fixed scratch belongs on the heap: server workers have 1MiB
     * stacks, including the callers and KV/read buffers. */
    struct incremental *s=calloc(1,sizeof(*s));if(!s)return EFS_ERR_NOMEM;
    s->changes_fd=-1;s->capacity=DIRTY_MAX;s->dirs=s->initial;int rc=EFS_OK;
    for(uint32_t shard=0;shard<SHARDS;shard++)if(d->part[shard].revision!=old->part[shard].revision){
        s->changed_partitions|=UINT64_C(1)<<(shard%64);
        s->changed_shards[shard/64]|=UINT64_C(1)<<(shard%64);
    }
    for(uint32_t shard=0;shard<SHARDS && !rc;shard++){
        if(d->part[shard].revision==old->part[shard].revision)continue;
        for(unsigned family=0;family<3 && !rc;family++)rc=stat_family_changes(d,old,s,shard,family);
    }
    ranges_done=query_build_clock();
    for(uint32_t shard=0;shard<SHARDS && !rc;shard++)rc=incremental_nodes(d,old,s,shard);
    nodes_done=query_build_clock();
    for(uint32_t shard=0;shard<SHARDS && !rc;shard++){
        if(d->part[shard].revision==old->part[shard].revision || d->shared_names[shard])continue;
        /* Old-only IDs were journaled during the inode-stream merge. */
        if(!rc)rc=changed_dentries(d,old,s,shard);
    }
    edges_done=query_build_clock();
    if(!rc)rc=changed_flush(s);
    /* Candidate derived files must all exist before aliases/parents are
     * resolved. Stream the journal with fixed scratch instead of falling
     * back to a full namespace fold at the 1,025th changed inode. */
    for(uint64_t at=0;at<s->changes && !rc;){
        unsigned n=s->changes-at>CHANGE_BATCH?CHANGE_BATCH:(unsigned)(s->changes-at);
        rc=io_at(s->changes_fd,s->changed,n*sizeof(efs_ino_t),at*sizeof(efs_ino_t),0);
        for(unsigned i=0;i<n && !rc;i++){
            if(cancelled(d)){rc=EFS_ERR_BUSY;break;}
            struct disk_node node;int lookup=locate(d,s->changed[i],&node,NULL);
            if(lookup && lookup!=EFS_ERR_NOT_FOUND){rc=lookup;break;}
            if(!lookup && S_ISDIR(node.row.mode))rc=dirty_add(d,s,node.row.ino);
            /* Old edges for removed/renamed IDs already dirtied their parents. */
        }
        if(!rc && s->changes<=CHANGE_BATCH)rc=aliases_dirty_batch(d,s,s->changed,n);
        at+=n;
    }
    if(!rc && s->changes>CHANGE_BATCH){
        rc=changes_sort(d,s);
        if(!rc)rc=aliases_dirty_sorted(d,s);
    }
    aliases_done=query_build_clock();
    /* Sorted insertion can move the current entry: process an explicit done
     * bit rather than an index walk that could skip newly inserted ancestors. */
    while(!rc){
        struct dirty_dir *dir=NULL;
        for(unsigned i=0;i<s->count;i++)if(!s->dirs[i].done){dir=&s->dirs[i];break;}
        if(!dir)break;
        dir->done=1;
        efs_ino_t ino=dir->node.row.ino,parent=dir->node.row.parent;
        if(ino!=EFS_ROOT_INO && dir->node.row.nlink){
            if(!parent || parent==ino){rc=EFS_ERR_PROTO;break;}
            rc=dirty_add(d,s,parent);
        }
    }
    for(unsigned i=0;i<s->count;i++){
        memset(&s->dirs[i].node.rollup,0,sizeof(s->dirs[i].node.rollup));
        memset(&s->dirs[i].node.link,0,sizeof(s->dirs[i].node.link));
        s->dirs[i].node.children=0;s->dirs[i].done=0;
    }
    /* Refold only affected parent prefixes. Inspect all possible layout
     * lanes and use the same placement rule as a fresh fold; this also
     * preserves validation when a source bitmap is incomplete. */
    for(unsigned pi=0;pi<s->count && !rc;pi++){
        struct efs_meta_row *row=&s->dirs[pi].node.row;
        unsigned lanes=row->layout==EFS_META_LAYOUT_LOCAL?1:EFS_META_LANES;
        for(unsigned lane=0;lane<lanes && !rc;lane++){
            struct parent_fold ctx={d,s,efs_kv_lane_shard(row->ino,(uint8_t)lane)};
            uint8_t prefix[EFS_KV_KEY_MAX];uint32_t length;
            rc=efs_kv_key_dentry_prefix(ctx.shard,row->ino,prefix,&length);
            if(!rc)rc=raw_prefix(d,prefix,length,parent_fold_entry,&ctx);
        }
    }
    for(unsigned i=0;i<s->count && !rc;i++){
        struct dirty_dir *dir=&s->dirs[i];struct efs_query_rollup *r=&dir->node.rollup;
        if(dir->node.row.nlink && dir->node.row.ino!=EFS_ROOT_INO && !dir->node.link.parent){rc=EFS_ERR_IO;break;}
        int has=r->tree_files || r->tree_dirs;
        rc=rollup_tree_add(r,r->imm_files,r->imm_dirs,r->imm_bytes);if(rc)break;
        if(r->imm_files+r->imm_dirs)expand(&r->tree_tmin,&r->tree_tmax,r->imm_tmin,r->imm_tmax,has);
    }
    unsigned done=0;
    while(!rc && done<s->count){
        int progressed=0;
        for(unsigned i=0;i<s->count && !rc;i++){
            struct dirty_dir *dir=&s->dirs[i];if(dir->done || dir->pending)continue;
            dir->done=1;done++;progressed=1;
            struct dirty_dir *parent=dirty_find(s,dir->node.link.parent);
            if(parent){
                struct efs_query_rollup *a=&parent->node.rollup,*b=&dir->node.rollup;
                if(!parent->pending || UINT64_MAX-a->tree_bytes<b->tree_bytes){rc=EFS_ERR_PROTO;break;}
                int has=a->tree_files || a->tree_dirs;
                rc=rollup_tree_add(a,b->tree_files,b->tree_dirs,b->tree_bytes);if(rc)break;
                if(b->tree_files || b->tree_dirs)expand(&a->tree_tmin,&a->tree_tmax,b->tree_tmin,b->tree_tmax,has);
                parent->pending--;
            }
            rc=node_at(d,efs_kv_inode_shard(dir->node.row.ino),dir->at,&dir->node,1);
        }
        if(!progressed && !rc)rc=EFS_ERR_PROTO;
    }
    if(s->changes_fd>=0){
        close(s->changes_fd);
        char path[EFS_MAX_PATH];int cleanup=filename(d,0,'c',path,sizeof(path));
        if(!cleanup && unlink(path))cleanup=EFS_ERR_IO;
        /* A leftover temporary file must not be published or uncharged. */
        if(cleanup)rc=cleanup;
        else d->disk_bytes-=s->change_bytes;
    }
    if(s->heap_bytes)free(s->dirs);
    uint64_t finished=query_build_clock();
    if(finished-started>=UINT64_C(1000000000))
        fprintf(stderr,"query-disk: incremental rc=%d ranges_ms=%llu nodes_ms=%llu edges_ms=%llu aliases_ms=%llu folds_ms=%llu changes=%llu dirs=%u walked=%u\n",
            rc,(unsigned long long)((ranges_done-started)/1000000),
            (unsigned long long)((nodes_done-ranges_done)/1000000),
            (unsigned long long)((edges_done-nodes_done)/1000000),
            (unsigned long long)((aliases_done-edges_done)/1000000),
            (unsigned long long)((finished-aliases_done)/1000000),
            (unsigned long long)s->changes,s->count,d->walked);
    free(s);
    return rc;
}
