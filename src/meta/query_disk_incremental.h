/* Included only by query_disk.c: candidate-only bounded scratch. A positive
 * FULL result is a resource/coverage fallback, never an IO-error fallback. */
#define INCREMENTAL_FULL 1
#define DIRTY_MAX 1024
#define CHANGE_BATCH 256
struct dirty_dir { struct disk_node node;uint64_t at;unsigned pending,done; };
struct incremental {
    uint64_t changed_partitions,changes,change_bytes;
    int changes_fd;
    unsigned buffered,count;
    efs_ino_t changed[CHANGE_BATCH];
    struct dirty_dir dirs[DIRTY_MAX];
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
    if(s->count==DIRTY_MAX)return INCREMENTAL_FULL;
    struct dirty_dir dir={0};int rc=locate(d,ino,&dir.node,&dir.at);
    if(rc==EFS_ERR_NOT_FOUND)return INCREMENTAL_FULL;
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
    if(same && !(old->part[shard].dependencies&s->changed_partitions))return rc;
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
                rc=node_at(old,shard,old_at,&before,0);if(rc)break;
                if(before.row.ino>=encoded){found=before.row.ino==encoded;break;}
                old_at++;
            }
            if(rc)break;
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
        if(!rc && !same)d->part[shard].dependencies|=dependency_bits(&n.row);
        if(!rc && fold){rc=efs_meta_apply_getattr(&d->kv,n.row.ino,refuse_coord,NULL,&n.stat);if(!rc && d->refolded<UINT32_MAX)d->refolded++;}
        int changed=!found || memcmp(&n.row,&before.row,sizeof(n.row)) || memcmp(&n.stat,&before.stat,sizeof(n.stat));
        if(!rc && changed)rc=changed_add(d,s,n.row.ino);
        if(!rc && same && changed)rc=node_at(d,shard,i,&n,1);
        if(!rc && !same){batch[used++]=n;
            if(used==NODE_BATCH || i+1==d->part[shard].inodes){rc=io_at(fd,batch,used*sizeof(*batch),(i+1-used)*sizeof(*batch),1);used=0;}
        }
    }
    if(fd>=0)close(fd);
    return rc;
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
        if(different && cmp<=0 && havea){rc=entry_from(ak,x.key_len,av,x.value_len,&e);if(!rc)rc=dirty_add(d,s,e.parent);}
        if(different && cmp>=0 && haveb && !rc){rc=entry_from(bk,y.key_len,bv,y.value_len,&e);if(!rc)rc=dirty_add(d,s,e.parent);}
        if(cmp<=0)a++;
        if(cmp>=0)b++;
    }
    return rc;
}
/* Each parent-shard index is sorted by child inode. At most 4096 bounded
 * seeks discover aliases, with no scan of unrelated namespace names. */
static int aliases_dirty(struct efs_query_disk *d,struct incremental *s,efs_ino_t ino)
{
    for(uint32_t shard=0;shard<SHARDS;shard++){
        if(cancelled(d))return EFS_ERR_BUSY;
        uint64_t lo=0,hi=d->part[shard].names;struct disk_alias alias;int rc=EFS_OK;
        while(lo<hi){uint64_t mid=lo+(hi-lo)/2;
            rc=disk_io(d,shard,'a',&alias,sizeof(alias),mid*sizeof(alias),0);if(rc)return rc;
            if(alias.child<ino)lo=mid+1;else hi=mid;}
        for(;lo<d->part[shard].names;lo++){
            rc=disk_io(d,shard,'a',&alias,sizeof(alias),lo*sizeof(alias),0);if(rc)return rc;
            if(alias.child!=ino)break;
            if(alias.ordinal>=d->part[shard].names)return EFS_ERR_PROTO;
            rc=dirty_add(d,s,alias.parent);if(rc)return rc;
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
        child_dir->node.link=e;
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
    struct incremental s={.changes_fd=-1};int rc=EFS_OK;
    for(uint32_t shard=0;shard<SHARDS;shard++)if(d->part[shard].revision!=old->part[shard].revision)s.changed_partitions|=UINT64_C(1)<<(shard%64);
    for(uint32_t shard=0;shard<SHARDS && !rc;shard++)rc=incremental_nodes(d,old,&s,shard);
    for(uint32_t shard=0;shard<SHARDS && !rc;shard++){
        if(d->part[shard].revision==old->part[shard].revision)continue;
        /* Removed inode IDs still invalidate hard-link parents in old edges. */
        uint64_t candidate_at=0;
        for(uint64_t i=0;i<old->part[shard].inodes && !rc;i++){
            if(cancelled(d)){rc=EFS_ERR_BUSY;break;}
            struct disk_node n,now;rc=node_at(old,shard,i,&n,0);if(rc)break;
            int found=0;
            while(candidate_at<d->part[shard].inodes){
                rc=node_at(d,shard,candidate_at,&now,0);if(rc)break;
                if(now.row.ino>=n.row.ino){found=now.row.ino==n.row.ino;break;}
                candidate_at++;
            }
            if(!rc && !found)rc=changed_add(d,&s,n.row.ino);
        }
        if(!rc)rc=changed_dentries(d,old,&s,shard);
    }
    if(!rc)rc=changed_flush(&s);
    /* Candidate derived files must all exist before aliases/parents are
     * resolved. Stream the journal with fixed scratch instead of falling
     * back to a full namespace fold at the 1,025th changed inode. */
    for(uint64_t at=0;at<s.changes && !rc;){
        unsigned n=s.changes-at>CHANGE_BATCH?CHANGE_BATCH:(unsigned)(s.changes-at);
        rc=io_at(s.changes_fd,s.changed,n*sizeof(efs_ino_t),at*sizeof(efs_ino_t),0);
        for(unsigned i=0;i<n && !rc;i++){
            if(cancelled(d)){rc=EFS_ERR_BUSY;break;}
            struct disk_node node;int lookup=locate(d,s.changed[i],&node,NULL);
            if(lookup && lookup!=EFS_ERR_NOT_FOUND){rc=lookup;break;}
            if(!lookup && S_ISDIR(node.row.mode))rc=dirty_add(d,&s,node.row.ino);
            /* Old edges for removed/renamed IDs already dirtied their parents. */
            if(!rc)rc=aliases_dirty(d,&s,s.changed[i]);
        }
        at+=n;
    }
    /* Sorted insertion can move the current entry: process an explicit done
     * bit rather than an index walk that could skip newly inserted ancestors. */
    while(!rc){
        struct dirty_dir *dir=NULL;
        for(unsigned i=0;i<s.count;i++)if(!s.dirs[i].done){dir=&s.dirs[i];break;}
        if(!dir)break;
        dir->done=1;
        efs_ino_t ino=dir->node.row.ino,parent=dir->node.row.parent;
        if(ino!=EFS_ROOT_INO && dir->node.row.nlink){
            if(!parent || parent==ino){rc=EFS_ERR_PROTO;break;}
            rc=dirty_add(d,&s,parent);
        }
    }
    for(unsigned i=0;i<s.count;i++){
        memset(&s.dirs[i].node.rollup,0,sizeof(s.dirs[i].node.rollup));
        memset(&s.dirs[i].node.link,0,sizeof(s.dirs[i].node.link));
        s.dirs[i].node.children=0;s.dirs[i].done=0;
    }
    /* Refold only affected parent prefixes. Inspect all possible layout
     * lanes and use the same placement rule as a fresh fold; this also
     * preserves validation when a source bitmap is incomplete. */
    for(unsigned pi=0;pi<s.count && !rc;pi++){
        struct efs_meta_row *row=&s.dirs[pi].node.row;
        unsigned lanes=row->layout==EFS_META_LAYOUT_LOCAL?1:EFS_META_LANES;
        for(unsigned lane=0;lane<lanes && !rc;lane++){
            struct parent_fold ctx={d,&s,efs_kv_lane_shard(row->ino,(uint8_t)lane)};
            uint8_t prefix[EFS_KV_KEY_MAX];uint32_t length;
            rc=efs_kv_key_dentry_prefix(ctx.shard,row->ino,prefix,&length);
            if(!rc)rc=raw_prefix(d,prefix,length,parent_fold_entry,&ctx);
        }
    }
    for(unsigned i=0;i<s.count && !rc;i++){
        struct dirty_dir *dir=&s.dirs[i];struct efs_query_rollup *r=&dir->node.rollup;
        if(dir->node.row.nlink && dir->node.row.ino!=EFS_ROOT_INO && !dir->node.link.parent){rc=EFS_ERR_IO;break;}
        int has=r->tree_files || r->tree_dirs;
        rc=rollup_tree_add(r,r->imm_files,r->imm_dirs,r->imm_bytes);if(rc)break;
        if(r->imm_files+r->imm_dirs)expand(&r->tree_tmin,&r->tree_tmax,r->imm_tmin,r->imm_tmax,has);
    }
    unsigned done=0;
    while(!rc && done<s.count){
        int progressed=0;
        for(unsigned i=0;i<s.count && !rc;i++){
            struct dirty_dir *dir=&s.dirs[i];if(dir->done || dir->pending)continue;
            dir->done=1;done++;progressed=1;
            struct dirty_dir *parent=dirty_find(&s,dir->node.link.parent);
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
    if(s.changes_fd>=0){
        close(s.changes_fd);
        char path[EFS_MAX_PATH];int cleanup=filename(d,0,'c',path,sizeof(path));
        if(!cleanup && unlink(path))cleanup=EFS_ERR_IO;
        /* A leftover temporary file must not be published or uncharged. */
        if(cleanup)rc=cleanup;
        else d->disk_bytes-=s.change_bytes;
    }
    return rc;
}
