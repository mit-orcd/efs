/* Included only by query_disk.c: candidate-only bounded scratch. A positive
 * FULL result is a resource/coverage fallback, never an IO-error fallback. */
#define INCREMENTAL_FULL 1
#define CHANGE_MAX 1024
struct dirty_dir { struct disk_node node;uint64_t at;unsigned pending,done; };
struct incremental {
    efs_ino_t changed[CHANGE_MAX];unsigned changes,count;
    struct dirty_dir dirs[CHANGE_MAX];
};
static unsigned ino_lower(const efs_ino_t *ids,unsigned n,efs_ino_t ino)
{
    unsigned lo=0,hi=n;
    while(lo<hi){unsigned mid=lo+(hi-lo)/2;if(ids[mid]<ino)lo=mid+1;else hi=mid;}
    return lo;
}
static int changed_add(struct incremental *s,efs_ino_t ino)
{
    unsigned at=ino_lower(s->changed,s->changes,ino);
    if(at<s->changes && s->changed[at]==ino)return EFS_OK;
    if(s->changes==CHANGE_MAX)return INCREMENTAL_FULL;
    memmove(s->changed+at+1,s->changed+at,(s->changes-at)*sizeof(*s->changed));
    s->changed[at]=ino;s->changes++;return EFS_OK;
}
static int changed_has(const struct incremental *s,efs_ino_t ino)
{ unsigned at=ino_lower(s->changed,s->changes,ino);return at<s->changes && s->changed[at]==ino; }
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
    if(s->count==CHANGE_MAX)return INCREMENTAL_FULL;
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
    efs_ino_t previous=0;
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
            int lookup=locate(old,encoded,&before,NULL);
            if(lookup && lookup!=EFS_ERR_NOT_FOUND){rc=lookup;break;}
            found=!lookup;
            /* Own-shard lane/PVER/DSEQ changes can affect getattr even if the
             * inode row is identical. Re-fold every inode in a revised part. */
            fold=1;
            if(found){n.stat=before.stat;n.rollup=before.rollup;n.link=before.link;n.children=before.children;}
        }
        if(!rc && fold){rc=efs_meta_apply_getattr(&d->kv,n.row.ino,refuse_coord,NULL,&n.stat);if(!rc && d->refolded<UINT32_MAX)d->refolded++;}
        int changed=!found || memcmp(&n.row,&before.row,sizeof(n.row)) || memcmp(&n.stat,&before.stat,sizeof(n.stat));
        if(!rc && changed)rc=changed_add(s,n.row.ino);
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
static int incremental_build(struct efs_query_disk *d,struct efs_query_disk *old)
{
    struct incremental s={0};int rc=EFS_OK;
    for(uint32_t shard=0;shard<SHARDS && !rc;shard++)rc=incremental_nodes(d,old,&s,shard);
    for(uint32_t shard=0;shard<SHARDS && !rc;shard++){
        if(d->part[shard].revision==old->part[shard].revision)continue;
        /* Removed inode IDs still invalidate hard-link parents in old edges. */
        for(uint64_t i=0;i<old->part[shard].inodes && !rc;i++){
            struct disk_node n,now;rc=node_at(old,shard,i,&n,0);if(rc)break;
            int lookup=locate(d,n.row.ino,&now,NULL);
            if(lookup==EFS_ERR_NOT_FOUND)rc=changed_add(&s,n.row.ino);else if(lookup)rc=lookup;
        }
        if(!rc)rc=changed_dentries(d,old,&s,shard);
    }
    for(unsigned i=0;i<s.changes && !rc;i++){
        struct disk_node n;int lookup=locate(d,s.changed[i],&n,NULL);
        if(lookup==EFS_ERR_NOT_FOUND)continue;
        if(lookup){rc=lookup;break;}
        if(S_ISDIR(n.row.mode))rc=dirty_add(d,&s,n.row.ino);
    }
    /* Every live alias of a changed inode matters. Removed/renamed old edges
     * already invalidated their parents in changed_dentries; scanning old
     * aliases again adds no dependency. Keep discovery bounded in RAM. */
    if(!rc && s.changes){
        struct efs_query_disk *view=d;
        for(uint32_t shard=0;shard<SHARDS && !rc;shard++)for(uint64_t i=0;i<view->part[shard].names && !rc;i++){
            if(cancelled(d)){rc=EFS_ERR_BUSY;break;}
            struct slot slot;uint8_t key[EFS_KV_KEY_MAX],val[VALUE_MAX];struct efs_name_entry e;
            rc=read_record(view,shard,view->part[shard].begin[1]+i,&slot,key,val);
            if(!rc)rc=entry_from(key,slot.key_len,val,slot.value_len,&e);
            if(!rc && e.dentry.type!=EFS_META_DENT_TOMBSTONE && changed_has(&s,e.dentry.ino))rc=dirty_add(d,&s,e.parent);
        }
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
    for(uint32_t shard=0;shard<SHARDS && !rc && s.count;shard++)for(uint64_t i=0;i<d->part[shard].names && !rc;i++){
        if(cancelled(d)){rc=EFS_ERR_BUSY;break;}
        struct slot slot;uint8_t key[EFS_KV_KEY_MAX],val[VALUE_MAX];struct efs_name_entry e;
        rc=read_record(d,shard,d->part[shard].begin[1]+i,&slot,key,val);
        if(!rc)rc=entry_from(key,slot.key_len,val,slot.value_len,&e);
        if(rc)break;
        if(!e.dentry.ino || e.dentry.type==EFS_META_DENT_TOMBSTONE)continue;
        struct dirty_dir *parent=dirty_find(&s,e.parent),*child_dir=dirty_find(&s,e.dentry.ino);
        if(!parent && !child_dir)continue;
        struct disk_node p,c;uint32_t chosen;
        if(parent)p=parent->node;else rc=locate(d,e.parent,&p,NULL);
        if(!rc)rc=chosen_shard(d,&p.row,&e,&chosen);
        if(rc)break;
        if(chosen!=shard)continue;
        if(child_dir)c=child_dir->node;else rc=locate(d,e.dentry.ino,&c,NULL);
        if(rc)break;
        if(!S_ISDIR(p.row.mode) || c.row.generation!=e.dentry.generation || c.row.ino==e.parent){rc=EFS_ERR_PROTO;break;}
        if(S_ISDIR(c.row.mode) && child_dir){
            if(child_dir->node.link.parent || c.row.parent!=e.parent){rc=EFS_ERR_PROTO;break;}
            child_dir->node.link=e;
        }
        if(!parent)continue;
        struct efs_query_rollup *r=&parent->node.rollup;int has=r->imm_files+r->imm_dirs!=0;
        if(S_ISDIR(c.row.mode))r->imm_dirs++;
        else{if(UINT64_MAX-r->imm_bytes<c.stat.size){rc=EFS_ERR_QUOTA;break;}r->imm_files++;r->imm_bytes+=c.stat.size;}
        expand(&r->imm_tmin,&r->imm_tmax,low_time(&c.stat),high_time(&c.stat),has);
        if(S_ISDIR(c.row.mode)){
            if(child_dir)parent->pending++;
            else{
                int treehas=r->tree_files || r->tree_dirs;
                rc=rollup_tree_add(r,c.rollup.tree_files,c.rollup.tree_dirs,c.rollup.tree_bytes);if(rc)break;
                if(c.rollup.tree_files || c.rollup.tree_dirs)expand(&r->tree_tmin,&r->tree_tmax,c.rollup.tree_tmin,c.rollup.tree_tmax,treehas);
            }
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
    return rc;
}
