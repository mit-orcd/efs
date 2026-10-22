/* Offline applied-image tests: no network or live cluster is modified. */
#include "efs/query_catalog.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int failures;
#define CHECK(c,m) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,m); failures++; } } while (0)
static char store[256];
static void cleanup(void)
{
    DIR *d=opendir(store); if (!d) return;
    struct dirent *e;
    while ((e=readdir(d))) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char p[512];snprintf(p,sizeof(p),"%s/%s",store,e->d_name);unlink(p);
    }
    closedir(d);rmdir(store);
}
static void be64(uint8_t *p,uint64_t v) { for (int i=7;i>=0;i--) { p[i]=(uint8_t)v;v>>=8; } }
static void row_put(struct efs_kv *kv,struct efs_meta_row *r)
{
    uint8_t k[EFS_KV_KEY_MAX],v[EFS_META_INO_BYTES];uint32_t kl;
    CHECK(!efs_kv_key_inode(efs_kv_inode_shard(r->ino),r->ino,k,&kl),"inode key");
    CHECK(!efs_meta_pack_inode(r,v,sizeof(v)),"pack inode");
    CHECK(!efs_kv_put(kv,k,kl,v,sizeof(v)),"write inode");
}
static void dent_put(struct efs_kv *kv,uint32_t sh,efs_ino_t parent,const char *name,efs_ino_t ino)
{
    uint8_t k[EFS_KV_KEY_MAX],v[EFS_META_DENT_BYTES];uint32_t kl;
    struct efs_meta_dentry d={ino,1,ino ? S_IFREG : EFS_META_DENT_TOMBSTONE};
    CHECK(!efs_kv_key_dentry(sh,parent,name,k,&kl),"dentry key");
    CHECK(!efs_meta_pack_dentry(&d,v,sizeof(v)),"pack dentry");
    CHECK(!efs_kv_put(kv,k,kl,v,sizeof(v)),"write dentry");
}
struct compare_catalogs { const struct efs_query_catalog *incremental,*full; };
static int compare_inode(void *ctx,const uint8_t *key,uint32_t kl,const uint8_t *v,uint32_t vl)
{
    (void)key;(void)kl;struct compare_catalogs *pair=ctx;struct efs_meta_row row,a,b;
    struct efs_meta_stat sa,sb;struct efs_query_rollup ra,rb;
    CHECK(!efs_meta_unpack_inode(v,vl,&row),"oracle unpack inode");
    CHECK(!efs_query_catalog_inode(pair->incremental,row.ino,&a,&sa),"incremental inode");
    CHECK(!efs_query_catalog_inode(pair->full,row.ino,&b,&sb),"full inode");
    CHECK(!memcmp(&a,&b,sizeof(a)) && !memcmp(&sa,&sb,sizeof(sa)),"incremental folded metadata equals full rebuild");
    if (S_ISDIR(row.mode)) {
        CHECK(!efs_query_catalog_stats(pair->incremental,row.ino,&ra) &&
              !efs_query_catalog_stats(pair->full,row.ino,&rb) && !memcmp(&ra,&rb,sizeof(ra)),
              "incremental directory rollups equal independent full rebuild");
        struct efs_name_entry ea,eb;
        int ar=efs_query_catalog_directory_link(pair->incremental,row.ino,&ea);
        int br=efs_query_catalog_directory_link(pair->full,row.ino,&eb);
        CHECK(ar==br && (ar || !memcmp(&ea,&eb,sizeof(ea))),"incremental directory binding equals full rebuild");
    }
    return 0;
}
static struct efs_query_catalog *disk_previous;
static char disk_root[256];
static int capture(struct efs_kv *kv,const struct efs_query_catalog *old,struct efs_query_catalog **out)
{
    struct efs_kv_lsm_snapshot *s=NULL;
    int rc=efs_kv_lsm_snapshot_pin(kv,8<<20,&s);
    if (!rc) rc=efs_query_catalog_build(s,old,32<<20,NULL,NULL,out);
    if (!rc && old) {
        struct efs_query_catalog *full=NULL;
        CHECK(!efs_query_catalog_build(s,NULL,32<<20,NULL,NULL,&full),"full rebuild oracle");
        if (full) {
            struct compare_catalogs pair={*out,full};
            for (uint32_t shard=0;shard<4096;shard++) {
                uint8_t prefix[3]={(uint8_t)(shard>>8),(uint8_t)shard,EFS_KV_KIND_INODE};
                CHECK(!efs_kv_lsm_snapshot_scan(s,prefix,3,NULL,0,compare_inode,&pair),"oracle inode scan");
            }
            efs_query_catalog_free(full);
        }
    }
    if(!rc){
        struct efs_query_catalog *full=NULL,*disk=NULL;
        if(!disk_root[0])snprintf(disk_root,sizeof(disk_root),"/tmp/efs-query-incremental-%d",getpid());
        CHECK(!efs_query_catalog_build(s,NULL,32<<20,NULL,NULL,&full),"fresh RAM oracle for disk incremental");
        struct efs_query_rollup before,after;int retained=disk_previous && !efs_query_catalog_stats(disk_previous,EFS_ROOT_INO,&before);
        int drc=efs_query_catalog_build_disk(disk_root,s,disk_previous,4<<20,NULL,NULL,&disk);
        CHECK(!drc && disk,"bounded disk incremental build");
        if(full && disk){
            struct compare_catalogs pair={disk,full};
            for(uint32_t shard=0;shard<4096;shard++){
                uint8_t prefix[3]={(uint8_t)(shard>>8),(uint8_t)shard,EFS_KV_KIND_INODE};
                CHECK(!efs_kv_lsm_snapshot_scan(s,prefix,3,NULL,0,compare_inode,&pair),"all disk rows/links/rollups equal full oracle");
            }
            if(!efs_query_catalog_rebuilt(disk))CHECK(!efs_query_catalog_refolded(disk),"unchanged disk inputs fold no attributes");
            if(retained)CHECK(!efs_query_catalog_stats(disk_previous,EFS_ROOT_INO,&after) && !memcmp(&before,&after,sizeof(before)),"incremental writes preserve retained disk aggregates");
        }
        efs_query_catalog_free(full);
        if(disk){efs_query_catalog_free(disk_previous);disk_previous=disk;}
    }
    efs_kv_lsm_snapshot_release(s);return rc;
}
struct found { int n; char path[32][1024]; int fail; };
static int collect(void *ctx,const struct efs_name_entry *e,const char *path)
{
    (void)e;struct found *f=ctx;
    if (f->fail) return EFS_ERR_ACCES;
    if (f->n>=32 || strlen(path)>=sizeof(f->path[0])) return EFS_ERR_NOMEM;
    strcpy(f->path[f->n++],path);return EFS_OK;
}
static int page_collect(void *ctx,const struct efs_name_entry *e,const char *path)
{
    struct found *f=ctx;
    if (f->n && f->n%2==0) return EFS_ERR_AGAIN;
    return collect(ctx,e,path);
}
static int cancel(void *ctx) { (void)ctx;return 1; }
int main(void)
{
    snprintf(store,sizeof(store),"/tmp/efs-query-catalog-%d",getpid());cleanup();
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4,0};
    struct efs_kv *kv=efs_kv_lsm_open(store,&cfg);
    CHECK(kv!=NULL,"open");if (!kv) return 1;
    struct efs_meta_attrs attrs={1000,1000,100};
    efs_ino_t a=0,b=0,file=0,other=0;
    CHECK(!efs_meta_apply_init(kv,100),"root");
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,EFS_ROOT_INO,S_IFDIR|0755,"a",&a),"mkdir a");
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,a,S_IFDIR|0755,"b",&b),"mkdir b");
    CHECK(!efs_meta_apply_create_file(kv,&attrs,b,S_IFREG|0644,"report.dat",&file),"create file");
    CHECK(!efs_meta_apply_create_file(kv,&attrs,EFS_ROOT_INO,S_IFREG|0644,"outside.dat",&other),"create outside");
    struct efs_meta_row r;
    CHECK(!efs_meta_apply_get_inode(kv,file,&r),"file row");
    r.base_size=4096;r.active_lanes=1ULL<<1;row_put(kv,&r);
    /* Encoded committed lane: logical sparse EOF, not allocated payload. */
    uint8_t key[EFS_KV_KEY_MAX],lane[64]={0};uint32_t kl;
    be64(lane,8192);be64(lane+8,200);be64(lane+16,210);be64(lane+24,1);be64(lane+56,1);
    CHECK(!efs_kv_key_lane(efs_kv_lane_shard(file,1),file,1,1,key,&kl),"lane key");
    CHECK(!efs_kv_put(kv,key,kl,lane,sizeof(lane)),"committed sparse EOF");
    CHECK(!efs_meta_apply_link(kv,b,"report.dat",a,"alias.dat",220),"hardlink");
    struct efs_query_catalog *c=NULL,*newer=NULL;
    int rc=capture(kv,NULL,&c);
    CHECK(!rc && c,"initial catalog");
    if (!c) goto done;
    CHECK(efs_query_catalog_rebuilt(c)==4096,"initial full partition build");
    struct efs_query_rollup stat;
    CHECK(!efs_query_catalog_stats(c,EFS_ROOT_INO,&stat) && stat.imm_files==1 && stat.imm_dirs==1 &&
          stat.tree_files==3 && stat.tree_dirs==2 && stat.tree_bytes==16384,"fold lanes and count hardlink names");
    CHECK(!efs_query_catalog_stats(c,a,&stat) && stat.imm_files==1 && stat.imm_bytes==8192 &&
          stat.tree_files==2 && stat.tree_bytes==16384,"subtree rollup");
    CHECK(!efs_query_catalog_stats(c,b,&stat) && stat.imm_tmax==220,"folded file timestamps");
    struct found listed={0};
    CHECK(!efs_query_catalog_ls(c,a,100,collect,&listed) && listed.n==2,
          "immediate listing excludes grandchildren and outside names");
    int saw_b=0,saw_alias=0;
    for (int i=0;i<listed.n;i++) { saw_b|=!strcmp(listed.path[i],"b");saw_alias|=!strcmp(listed.path[i],"alias.dat"); }
    CHECK(saw_b && saw_alias,"listing reads catalog directory entries");
    listed=(struct found){0};
    CHECK(efs_query_catalog_ls(c,a,1,collect,&listed)==EFS_ERR_OVERFLOW,"listing budget refuses partial success");
    listed=(struct found){.fail=1};
    CHECK(efs_query_catalog_ls(c,a,100,collect,&listed)==EFS_ERR_ACCES,"listing preserves authorization callback errors");
    char path[128];
    CHECK(!efs_query_catalog_path(c,b,EFS_ROOT_INO,path,sizeof(path)) && !strcmp(path,"a/b"),"relative ancestry");
    CHECK(efs_query_catalog_path(c,a,b,path,sizeof(path))==EFS_ERR_NOT_FOUND,"outside scope");
    struct efs_name_entry binding;
    CHECK(!efs_query_catalog_directory_link(c,b,&binding) && binding.parent==a && !strcmp(binding.name,"b"),
          "directory binding retained for fresh same-parent rename validation");
    CHECK(efs_query_catalog_directory_link(c,EFS_ROOT_INO,&binding)==EFS_ERR_NOT_FOUND,"root has no directory binding");
    CHECK(efs_query_catalog_path(c,b,EFS_ROOT_INO,path,3)==EFS_ERR_NAMETOOLONG,"path limit");
    struct found f={0};
    CHECK(!efs_query_catalog_find(c,a,EFS_NAME_EXACT,"report.dat",1,collect,&f) && f.n==1 &&
          !strcmp(f.path[0],"b/report.dat"),"exact query spends only matching candidate budget");
    f=(struct found){0};
    CHECK(efs_query_catalog_find(c,a,EFS_NAME_SUFFIX,".dat",1,collect,&f)==EFS_ERR_BUSY,"subtree scan budget explicit");
    f=(struct found){.fail=1};
    CHECK(efs_query_catalog_find(c,a,EFS_NAME_EXACT,"report.dat",10,collect,&f)==EFS_ERR_ACCES,"emitter denial preserved");
    CHECK(!capture(kv,c,&newer) && efs_query_catalog_rebuilt(newer)==0,"unchanged partitions reused");
    CHECK(newer && !efs_query_catalog_refolded(newer) && efs_query_catalog_reused_edges(newer)==4096,
          "unchanged folded stats and every edge contribution reused");
    efs_query_catalog_free(newer);newer=NULL;
    /* Inserting/removing an earlier shard shifts every later global ordinal.
     * Unchanged inode partitions must map by their own local ordinal. */
    CHECK(!efs_meta_apply_get_inode(kv,file,&r),"ordinal-shift source inode");
    efs_ino_t detached=UINT64_C(999999)<<12;
    CHECK(efs_kv_inode_shard(detached)<efs_kv_inode_shard(file),"earlier shard fixture");
    r.ino=detached;r.nlink=0;r.active_lanes=0;r.base_size=777;row_put(kv,&r);
    CHECK(!capture(kv,c,&newer),"earlier inode insertion shifts unchanged partitions");
    struct efs_meta_stat shifted_stat;
    CHECK(newer && !efs_query_catalog_inode(newer,file,NULL,&shifted_stat) && shifted_stat.size==8192,
          "shifted unchanged inode retains its own folded size");
    CHECK(!efs_kv_key_inode(efs_kv_inode_shard(detached),detached,key,&kl) && !efs_kv_del(kv,key,kl),
          "remove earlier-shard inode");
    struct efs_query_catalog *shrunk=NULL;
    CHECK(!capture(kv,newer,&shrunk),"earlier inode removal shifts unchanged partitions back");
    CHECK(shrunk && !efs_query_catalog_inode(shrunk,file,NULL,&shifted_stat) && shifted_stat.size==8192,
          "shrunk ordinal maps to correct old inode");
    CHECK(efs_query_catalog_inode(c,detached,NULL,NULL)==EFS_ERR_NOT_FOUND,"old generation remains immutable");
    efs_query_catalog_free(shrunk);efs_query_catalog_free(newer);newer=NULL;
    /* A lane-only change must invalidate a stat cached on another inode shard. */
    CHECK(efs_kv_lane_shard(file,1)!=efs_kv_inode_shard(file),"cross-shard dependency fixture");
    be64(lane,12288);be64(lane+8,225);be64(lane+24,2);
    CHECK(!efs_kv_key_lane(efs_kv_lane_shard(file,1),file,1,1,key,&kl),"changed lane key");
    CHECK(!efs_kv_put(kv,key,kl,lane,sizeof(lane)),"lane-only update");
    CHECK(!capture(kv,c,&newer),"incremental lane-only refresh");
    struct efs_meta_stat changed;
    CHECK(newer && !efs_query_catalog_inode(newer,file,NULL,&changed) && changed.size==12288,
          "cross-shard lane invalidates cached folded EOF");
    CHECK(newer && efs_query_catalog_refolded(newer)>0 && efs_query_catalog_reused_edges(newer)<4096,
          "lane invalidates dependent edge aggregates");
    efs_query_catalog_free(newer);newer=NULL;
    /* Restore original EOF so subsequent legacy oracle assertions stay useful. */
    be64(lane,8192);be64(lane+8,200);be64(lane+24,3);
    CHECK(!efs_kv_put(kv,key,kl,lane,sizeof(lane)),"restore lane EOF");
    CHECK(!efs_meta_apply_rename(kv,a,"b",EFS_ROOT_INO,"moved",230),"move subtree");
    CHECK(!efs_meta_apply_unlink(kv,a,"alias.dat",240),"unlink alias");
    dent_put(kv,efs_kv_inode_shard(a),a,"deleted-one",0);
    dent_put(kv,efs_kv_inode_shard(a),a,"deleted-two",0);
    CHECK(!capture(kv,c,&newer) && efs_query_catalog_rebuilt(newer)>0 &&
          efs_query_catalog_rebuilt(newer)<4096,"refresh changed partitions only");
    if (!newer) goto done;
    listed=(struct found){0};
    CHECK(efs_query_catalog_ls(newer,a,1,collect,&listed)==EFS_ERR_OVERFLOW && !listed.n,
          "empty directory tombstones consume scan budget");
    CHECK(efs_query_catalog_ls_bounded(newer,a,100,cancel,NULL,collect,&listed)==EFS_ERR_BUSY && !listed.n,
          "tombstone-only listing observes cancellation before emission");

    CHECK(!efs_query_catalog_stats(newer,a,&stat) && !stat.tree_files && !stat.tree_dirs && !stat.tree_bytes,"moved source empty");
    CHECK(!efs_query_catalog_stats(c,a,&stat) && stat.tree_files==2,"old catalog immutable");
    CHECK(!efs_query_catalog_path(newer,b,EFS_ROOT_INO,path,sizeof(path)) && !strcmp(path,"moved"),"move changes path");
    CHECK(!efs_query_catalog_directory_link(newer,b,&binding) && binding.parent==EFS_ROOT_INO && !strcmp(binding.name,"moved"),
          "new directory binding names moved subtree");
    efs_query_catalog_free(c);c=newer;newer=NULL;
    /* APPEND_CUR alone changes visible frontier without changing inode/lane. */
    uint8_t cur[24]={0};be64(cur,10000);be64(cur+8,6000);cur[19]=1;
    CHECK(!efs_kv_key_append_cur(efs_kv_inode_shard(file),file,1,key,&kl),"append cursor key");
    CHECK(!efs_kv_put(kv,key,kl,cur,sizeof(cur)),"open append frontier");
    CHECK(!capture(kv,c,&newer) && efs_query_catalog_rebuilt(newer)==1,"append cursor dirties partition");
    CHECK(newer && !efs_query_catalog_stats(newer,b,&stat) && stat.tree_bytes==6000,"open frontier overrides physical EOF");
    efs_query_catalog_free(c);c=newer;newer=NULL;
    /* SPLITTING hashed tombstone wins over retained local entry. */
    CHECK(!efs_meta_apply_get_inode(kv,b,&r),"dir row");
    r.layout=EFS_META_LAYOUT_SPLITTING;row_put(kv,&r);
    uint32_t hashed=efs_kv_dentry_shard(b,"report.dat",EFS_META_LAYOUT_HASHED);
    dent_put(kv,hashed,b,"report.dat",0);
    CHECK(!capture(kv,c,&newer),"split tombstone build");
    CHECK(newer && !efs_query_catalog_stats(newer,b,&stat) && !stat.tree_files,"tombstone masks local name");
    f=(struct found){0};
    CHECK(newer && !efs_query_catalog_find(newer,EFS_ROOT_INO,EFS_NAME_EXACT,"report.dat",10,collect,&f) && !f.n,
          "find agrees with split tombstone");
    efs_query_catalog_free(newer);newer=NULL;
    char duplicate[32];
    for (unsigned i=0;;i++) {
        snprintf(duplicate,sizeof(duplicate),"duplicate-%u",i);
        if (efs_kv_dentry_shard(b,duplicate,EFS_META_LAYOUT_HASHED)!=efs_kv_inode_shard(b)) break;
    }
    dent_put(kv,efs_kv_inode_shard(b),b,duplicate,file);
    dent_put(kv,efs_kv_dentry_shard(b,duplicate,EFS_META_LAYOUT_HASHED),b,duplicate,file);
    CHECK(!capture(kv,c,&newer),"split mirrored candidate build");
    f=(struct found){0};
    CHECK(newer && !efs_query_catalog_find(newer,EFS_ROOT_INO,EFS_NAME_EXACT,duplicate,10,collect,&f) && f.n==1,
          "split duplicate emits once across physical partitions");
    CHECK(newer && !efs_query_catalog_stats(newer,b,&stat) && stat.tree_files==1 && stat.tree_bytes==6000,
          "split duplicate counted once");
    efs_query_catalog_free(newer);newer=NULL;
    r.layout=EFS_META_LAYOUT_HASHED;row_put(kv,&r);
    CHECK(!capture(kv,c,&newer),"fully hashed catalog");
    CHECK(newer && !efs_query_catalog_stats(newer,b,&stat) && stat.tree_files==1,"hashed ignores old local names");
    efs_query_catalog_free(newer);newer=NULL;
    struct efs_kv_lsm_snapshot *source=NULL;
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source),"capture limits source");
    CHECK(efs_query_catalog_build(source,c,1,NULL,NULL,&newer)==EFS_ERR_NOMEM && !newer,"budget failure explicit");
    CHECK(efs_query_catalog_build(source,c,efs_query_catalog_bytes(c)/2,NULL,NULL,&newer)==EFS_ERR_NOMEM && !newer,
          "partial candidate allocations released on exhaustion");
    CHECK(efs_query_catalog_build(source,c,32<<20,cancel,NULL,&newer)==EFS_ERR_BUSY && !newer,"cancel candidate");
    efs_kv_lsm_snapshot_release(source);
    uint64_t fence=0,live=0;
    source=NULL;
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source),"transaction fence pin");
    CHECK(!efs_kv_lsm_snapshot_transaction_revision(source,&fence),"captured transaction fence");
    CHECK(!efs_meta_apply_get_inode(kv,b,&r),"ordinary mutation row");
    row_put(kv,&r);
    CHECK(!efs_kv_lsm_transaction_revision(kv,&live) && live==fence,
          "ordinary writes do not invalidate transaction fence");
    uint8_t pending[]={0,0,EFS_KV_KIND_INTENT,1};
    CHECK(!efs_kv_put(kv,pending,sizeof(pending),(const uint8_t *)"x",1),"pending marker");
    CHECK(!efs_kv_lsm_transaction_revision(kv,&live) && live>fence,
          "prepare changes fence even when ordinary metadata is unchanged");
    uint64_t pinned=0;
    CHECK(!efs_kv_lsm_snapshot_transaction_revision(source,&pinned) && pinned==fence,
          "captured fence remains immutable");
    efs_kv_lsm_snapshot_release(source);
    fence=live;
    CHECK(capture(kv,c,&newer)==EFS_ERR_BUSY && !newer,"unresolved transaction refuses publication");
    CHECK(!efs_query_catalog_stats(c,b,&stat) && stat.tree_bytes==6000,"failed refresh retains published image");
    CHECK(!efs_kv_del(kv,pending,sizeof(pending)),"clear pending");
    CHECK(!efs_kv_lsm_transaction_revision(kv,&live) && live>fence,
          "resolve deletion changes fence after pending records disappear");
    uint64_t revisions_before[4096],revisions_after[4096];
    CHECK(!efs_kv_lsm_metadata_revisions(kv,revisions_before,4096),"tracking baseline");
    uint64_t activity_disabled[8],activity_after[8];
    CHECK(!efs_kv_lsm_transaction_activity(kv,activity_disabled),"transaction activity before disabled tracking");
    fence=live;
    efs_kv_lsm_query_tracking(kv,0);
    CHECK(!efs_kv_put(kv,pending,sizeof(pending),(const uint8_t *)"x",1),"disabled transaction marker");
    CHECK(!efs_kv_del(kv,pending,sizeof(pending)),"disabled transaction resolve");
    CHECK(!efs_meta_apply_get_inode(kv,b,&r),"disabled ordinary row");row_put(kv,&r);
    CHECK(!efs_kv_lsm_metadata_revisions(kv,revisions_after,4096) &&
          !memcmp(revisions_before,revisions_after,sizeof(revisions_before)),"disabled tracking leaves all hints unchanged");
    CHECK(!efs_kv_lsm_transaction_revision(kv,&live) && live==fence,"disabled tracking leaves transaction fence unchanged");
    CHECK(!efs_kv_lsm_transaction_activity(kv,activity_after) &&
          !memcmp(activity_disabled,activity_after,sizeof(activity_after)),"disabled tracking leaves diagnostic counters unchanged");
    efs_kv_lsm_query_tracking(kv,1);
    CHECK(!capture(kv,NULL,&newer),"reenable builds authoritative image without dirty history");
    CHECK(newer && efs_query_catalog_rebuilt(newer)==4096,"reenable requires full initial build");
    efs_query_catalog_free(newer);newer=NULL;
    source=NULL;
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source),"diagnostic activity pin");
    uint64_t activity_pin[8],activity_live[8],activity_expect[8];
    CHECK(!efs_kv_lsm_snapshot_transaction_activity(source,activity_pin),"pinned activity counts");
    CHECK(!efs_kv_lsm_transaction_activity(kv,activity_live) &&
          !memcmp(activity_pin,activity_live,sizeof(activity_pin)),"same cut activity counters");
    memcpy(activity_expect,activity_pin,sizeof(activity_expect));
    const uint8_t txn_kinds[4]={EFS_KV_KIND_INTENT,EFS_KV_KIND_GUARD,EFS_KV_KIND_REDUCE,EFS_KV_KIND_DECISION};
    for(unsigned k=0;k<4;k++) {
        uint8_t marker[4]={0,0,txn_kinds[k],255};
        CHECK(!efs_kv_put(kv,marker,sizeof(marker),(const uint8_t *)"x",1),"typed diagnostic PUT");
        activity_expect[2*k]++;
        CHECK(!efs_kv_lsm_transaction_activity(kv,activity_live) &&
              !memcmp(activity_expect,activity_live,sizeof(activity_expect)),"exact kind PUT only");
        CHECK(!efs_kv_del(kv,marker,sizeof(marker)),"typed diagnostic DEL");
        activity_expect[2*k+1]++;
        CHECK(!efs_kv_lsm_transaction_activity(kv,activity_live) &&
              !memcmp(activity_expect,activity_live,sizeof(activity_expect)),"exact kind DEL only");
    }
    CHECK(!efs_meta_apply_get_inode(kv,b,&r),"ordinary diagnostic row");row_put(kv,&r);
    CHECK(!efs_kv_lsm_transaction_activity(kv,activity_live) &&
          !memcmp(activity_expect,activity_live,sizeof(activity_expect)),"ordinary row changes no transaction activity");
    CHECK(!efs_kv_lsm_snapshot_transaction_activity(source,activity_after) &&
          !memcmp(activity_pin,activity_after,sizeof(activity_pin)),"pinned diagnostic activity remains immutable");
    efs_kv_lsm_snapshot_release(source);source=NULL;
    /* Many matching names in one outside directory must not leak into a
     * scoped search; scope decisions must not survive into another query. */
    for (unsigned i=0;i<6;i++) {
        char name[32];efs_ino_t created;
        snprintf(name,sizeof(name),"scope-cache-%u",i);
        CHECK(!efs_meta_apply_create_file(kv,&attrs,EFS_ROOT_INO,S_IFREG|0644,name,&created),"outside scope file");
    }
    CHECK(!capture(kv,NULL,&newer),"scope cache image");
    f=(struct found){0};
    CHECK(newer && !efs_query_catalog_find(newer,a,EFS_NAME_PREFIX,"scope-cache-",100,collect,&f) && !f.n,
          "repeated outside parent excluded");
    f=(struct found){0};
    CHECK(newer && !efs_query_catalog_find(newer,EFS_ROOT_INO,EFS_NAME_PREFIX,"scope-cache-",100,collect,&f) && f.n==6,
          "scope cache belongs to one query root");
    uint64_t cursor=0;int pages=0,total=0;
    do {
        f=(struct found){0};uint64_t before=cursor;
        CHECK(!efs_query_catalog_page(newer,EFS_ROOT_INO,EFS_NAME_PREFIX,"scope-cache-",0,
              &cursor,3,NULL,NULL,page_collect,&f),"bounded page");
        CHECK(cursor>before || f.n,"page progress");
        total+=f.n;pages++;
        CHECK(pages<5000,"bounded page completion");
    } while (cursor!=(UINT64_C(4096)<<32) && pages<5000);
    CHECK(total==6 && pages>1,"paging neither loses nor duplicates entries");
    efs_query_catalog_retain(newer);
    efs_query_catalog_free(newer);
    CHECK(!efs_query_catalog_stats(newer,EFS_ROOT_INO,&stat),"retained catalog survives publisher retirement");
    efs_query_catalog_free(newer);newer=NULL;
    /* Exercise catalog lookup through a larger, same-shard inode population,
     * including misses and an older immutable generation. */
    efs_ino_t indexed[512];
    for (unsigned i=0;i<512;i++) {
        char name[32];snprintf(name,sizeof(name),"index-%u",i);
        CHECK(!efs_meta_apply_create_file(kv,&attrs,EFS_ROOT_INO,S_IFREG|0644,name,&indexed[i]),"indexed file");
    }
    CHECK(!capture(kv,NULL,&newer),"larger inode catalog");
    for (unsigned i=0;i<512 && newer;i++) {
        struct efs_meta_row indexed_row;struct efs_meta_stat indexed_stat;
        CHECK(!efs_query_catalog_inode(newer,indexed[i],&indexed_row,&indexed_stat) &&
              indexed_row.ino==indexed[i] && indexed_stat.ino==indexed[i] && !indexed_stat.size,
              "every indexed inode resolves to its own metadata");
        CHECK(efs_query_catalog_inode(c,indexed[i],NULL,NULL)==EFS_ERR_NOT_FOUND,"old image excludes new inode");
    }
    CHECK(newer && efs_query_catalog_inode(newer,UINT64_MAX,NULL,NULL)==EFS_ERR_NOT_FOUND,"absent inode lookup");
    CHECK(newer && efs_query_catalog_inode(newer,0,NULL,NULL)==EFS_ERR_NOT_FOUND,
          "lookup below first retained key");
    CHECK(newer && efs_query_catalog_inode(newer,indexed[0]+(UINT64_C(1)<<40),NULL,NULL)==EFS_ERR_NOT_FOUND,
          "lookup above last key in a populated partition");
    struct efs_query_catalog *shared=NULL;
    CHECK(!capture(kv,newer,&shared) && !efs_query_catalog_refolded(shared),
          "unchanged decoded inode blocks reused without refolding");
    efs_query_catalog_free(newer);newer=NULL;
    for (unsigned i=0;i<512 && shared;i++) {
        struct efs_meta_row row;struct efs_meta_stat value;
        CHECK(!efs_query_catalog_inode(shared,indexed[i],&row,&value) &&
              row.ino==indexed[i] && value.ino==indexed[i],
              "shared inode blocks survive original catalog retirement");
    }
    CHECK(shared && !efs_query_catalog_stats(shared,EFS_ROOT_INO,&stat),
          "directory rollup survives source catalog retirement");
    efs_query_catalog_free(shared);
    efs_ino_t extdir,extfile,maxfile;
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,EFS_ROOT_INO,S_IFDIR|0755,"extrema",&extdir),"extrema directory");
    CHECK(!efs_meta_apply_create_file(kv,&attrs,extdir,S_IFREG|0644,"low",&extfile),"extrema low file");
    CHECK(!efs_meta_apply_create_file(kv,&attrs,extdir,S_IFREG|0644,"high",&maxfile),"extrema high file");
    CHECK(!efs_meta_apply_get_inode(kv,maxfile,&r),"extrema row");r.base_mtime=900000;r.base_size=9999;row_put(kv,&r);
    CHECK(!capture(kv,NULL,&newer),"incremental extrema additions");efs_query_catalog_free(newer);newer=NULL;
    CHECK(!efs_meta_apply_unlink(kv,extdir,"high",400),"delete maximum time/size child");
    CHECK(!capture(kv,NULL,&newer),"incremental deletion recomputes extrema");efs_query_catalog_free(newer);newer=NULL;
    CHECK(!efs_meta_apply_link(kv,extdir,"low",EFS_ROOT_INO,"extrema-alias",401),"cross-parent alias");
    CHECK(!capture(kv,NULL,&newer),"incremental hardlink invalidation");efs_query_catalog_free(newer);newer=NULL;
    CHECK(!efs_meta_apply_get_inode(kv,extfile,&r),"aliased inode row");r.base_size=777;r.base_mtime=800000;row_put(kv,&r);
    CHECK(!capture(kv,NULL,&newer),"incremental updates every hardlink parent");efs_query_catalog_free(newer);newer=NULL;
    CHECK(!efs_meta_apply_rename(kv,extdir,"low",EFS_ROOT_INO,"extrema-moved",402),"move file across parents");
    CHECK(!capture(kv,NULL,&newer),"incremental both rename parents");efs_query_catalog_free(newer);newer=NULL;
    CHECK(!efs_meta_apply_rmdir(kv,EFS_ROOT_INO,"extrema",403),"remove empty directory");
    CHECK(!capture(kv,NULL,&newer),"incremental removed directory");efs_query_catalog_free(newer);newer=NULL;
    CHECK(!efs_meta_apply_unlink(kv,EFS_ROOT_INO,"extrema-moved",404),"remove one alias");
    CHECK(!efs_meta_apply_get_inode(kv,extfile,&r),"remaining aliased inode");r.base_size=888;row_put(kv,&r);
    CHECK(!capture(kv,NULL,&newer),"removed old alias and changed surviving alias");efs_query_catalog_free(newer);newer=NULL;
    CHECK(!efs_meta_apply_unlink(kv,EFS_ROOT_INO,"extrema-alias",405),"remove final alias");
    CHECK(!capture(kv,NULL,&newer),"removed inode dependencies come from changed old edges");efs_query_catalog_free(newer);newer=NULL;
    for(unsigned i=0;i<1100;i++){
        char name[32];efs_ino_t added;snprintf(name,sizeof(name),"cap-fallback-%u",i);
        CHECK(!efs_meta_apply_create_file(kv,&attrs,EFS_ROOT_INO,S_IFREG|0644,name,&added),"bounded-set overflow fixture");
    }
    CHECK(!capture(kv,NULL,&newer),"oversized incremental change set falls back exactly");
    CHECK(disk_previous && efs_query_catalog_refolded(disk_previous)>=1100,"fallback really performs a full attribute build");
    efs_query_catalog_free(newer);newer=NULL;
    CHECK(!efs_meta_apply_get_inode(kv,b,&r),"dir for corruption");
    r.parent=a;row_put(kv,&r);
    CHECK(capture(kv,c,&newer)==EFS_ERR_PROTO && !newer,"mismatched parent fails closed");
done:
    efs_query_catalog_free(disk_previous);if(disk_root[0])rmdir(disk_root);
    efs_query_catalog_free(newer);efs_query_catalog_free(c);efs_kv_lsm_close(kv);cleanup();
    if (failures) return 1;
    puts("test_query_catalog: OK");return 0;
}
