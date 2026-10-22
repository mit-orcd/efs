#include "efs/query_catalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
static int failures;
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);failures++;} }while(0)
struct result { uint64_t count,hash; };
static int collect(void *ctx,const struct efs_name_entry *e,const char *path)
{
    struct result *r=ctx;r->count++;
    for(const char *p=path;*p;p++)r->hash=r->hash*33+(unsigned char)*p;
    r->hash=r->hash*33+e->dentry.ino;return 0;
}
static int compare(struct efs_kv_lsm_snapshot *s,struct efs_query_catalog *memory,struct efs_query_catalog *disk,efs_ino_t ino)
{
    (void)s;struct efs_query_rollup a,b;struct efs_meta_row ra,rb;struct efs_meta_stat sa,sb;
    CHECK(!efs_query_catalog_inode(memory,ino,&ra,&sa));CHECK(!efs_query_catalog_inode(disk,ino,&rb,&sb));
    CHECK(!memcmp(&ra,&rb,sizeof(ra)));CHECK(!memcmp(&sa,&sb,sizeof(sa)));
    CHECK(!efs_query_catalog_stats(memory,ino,&a));CHECK(!efs_query_catalog_stats(disk,ino,&b));CHECK(!memcmp(&a,&b,sizeof(a)));
    struct result ma={0},da={0};uint64_t mc=0,dc=0;
    do { int rc=efs_query_catalog_page(memory,ino,EFS_NAME_PREFIX,"",0,&mc,7,NULL,NULL,collect,&ma);CHECK(!rc);if(rc)return rc; }while(mc!=(UINT64_C(4096)<<32));
    do { int rc=efs_query_catalog_page(disk,ino,EFS_NAME_PREFIX,"",0,&dc,7,NULL,NULL,collect,&da);CHECK(!rc);if(rc)return rc; }while(dc!=(UINT64_C(4096)<<32));
    CHECK(ma.count==da.count && ma.hash==da.hash);
    ma=(struct result){0};da=(struct result){0};
    CHECK(!efs_query_catalog_find(memory,ino,EFS_NAME_CONTAINS,"",100000,collect,&ma));
    CHECK(!efs_query_catalog_find(disk,ino,EFS_NAME_CONTAINS,"",100000,collect,&da));
    CHECK(ma.count==da.count && ma.hash==da.hash);
    return 0;
}
static int cancel(void *p){(void)p;return 1;}
struct expected_path { const char *path;efs_ino_t ino;unsigned count; };
static int check_path(void *ctx,const struct efs_name_entry *e,const char *path)
{
    struct expected_path *p=ctx;CHECK(!strcmp(p->path,path) && p->ino==e->dentry.ino);p->count++;return 0;
}
static unsigned view_count(const char *root)
{
    unsigned count=0;DIR *dir=opendir(root);CHECK(dir);if(!dir)return 0;
    struct dirent *entry;
    while((entry=readdir(dir)))if(!strncmp(entry->d_name,"view-",5))count++;
    closedir(dir);return count;
}
static void test_catchup_retirement(void)
{
    char store[]="/tmp/efs-query-retire-kv-XXXXXX",root[]="/tmp/efs-query-retire-cache-XXXXXX";
    CHECK(mkdtemp(store));CHECK(mkdtemp(root));
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_SYNC,0,4,0};struct efs_kv *kv=efs_kv_lsm_open(store,&cfg);
    CHECK(kv);if(!kv)return;
    struct efs_meta_attrs attrs={1000,1000,100};efs_ino_t file;
    CHECK(!efs_meta_apply_init(kv,100));
    CHECK(!efs_meta_apply_create_file(kv,&attrs,EFS_ROOT_INO,S_IFREG|0644,"before",&file));
    CHECK(!efs_kv_lsm_flush(kv));
    struct efs_kv_lsm_snapshot *source=NULL;struct efs_query_catalog *catalog=NULL,*next=NULL;
    uint64_t watermark[4]={1,2,3,4};
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    CHECK(!efs_query_catalog_build_disk(root,source,NULL,4<<20,NULL,NULL,&catalog));
    CHECK(catalog && !efs_query_catalog_checkpoint(catalog,watermark));
    efs_query_catalog_free(catalog);catalog=NULL;efs_kv_lsm_snapshot_release(source);source=NULL;
    CHECK(view_count(root)==1);
    CHECK(!efs_meta_apply_rename(kv,EFS_ROOT_INO,"before",EFS_ROOT_INO,"after",200));
    CHECK(!efs_kv_lsm_flush(kv) && !efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    CHECK(!efs_query_catalog_catchup_disk(root,source,4<<20,NULL,NULL,&next));
    CHECK(view_count(root)==2);
    /* An abandoned catch-up must preserve the existing checkpoint. */
    efs_query_catalog_free(next);next=NULL;CHECK(view_count(root)==1);
    CHECK(!efs_query_catalog_catchup_disk(root,source,4<<20,NULL,NULL,&next));
    watermark[0]++;CHECK(next && !efs_query_catalog_checkpoint(next,watermark));
    CHECK(view_count(root)==1); /* The unreferenced startup input is retired. */
    struct expected_path expected={"after",file,0};uint64_t cursor=0;
    CHECK(next && !efs_query_catalog_page(next,EFS_ROOT_INO,EFS_NAME_EXACT,"after",0,&cursor,4096,NULL,NULL,check_path,&expected) && expected.count==1);
    CHECK(!efs_query_catalog_build_disk(root,source,next,4<<20,NULL,NULL,&catalog));
    CHECK(catalog && !efs_query_catalog_checkpoint(catalog,watermark));
    efs_query_catalog_free(next);next=NULL;CHECK(view_count(root)==1);
    efs_query_catalog_free(catalog);efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(kv);
}
static void test_restart_catchup(void)
{
    char store[]="/tmp/efs-query-catchup-kv-XXXXXX",root[]="/tmp/efs-query-catchup-cache-XXXXXX";
    CHECK(mkdtemp(store));CHECK(mkdtemp(root));
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_SYNC,0,4,0};struct efs_kv *kv=efs_kv_lsm_open(store,&cfg);
    CHECK(kv);if(!kv)return;
    struct efs_meta_attrs attrs={1000,1000,100};efs_ino_t dir,file;
    CHECK(!efs_meta_apply_init(kv,100));
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,EFS_ROOT_INO,S_IFDIR|0755,"dir",&dir));
    CHECK(!efs_meta_apply_create_file(kv,&attrs,dir,S_IFREG|0644,"old",&file));
    CHECK(!efs_kv_lsm_flush(kv) && !efs_kv_lsm_quiesce(kv));
    struct efs_kv_lsm_snapshot *source=NULL;struct efs_query_catalog *old=NULL,*next=NULL,*memory=NULL;
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    CHECK(!efs_query_catalog_build_disk(root,source,NULL,4<<20,NULL,NULL,&old));
    uint64_t wm[4]={1,2,3,4};CHECK(!efs_query_catalog_checkpoint(old,wm));
    efs_kv_lsm_snapshot_release(source);source=NULL;
    efs_kv_lsm_query_tracking(kv,0);
    CHECK(!efs_meta_apply_rename(kv,dir,"old",dir,"new",200));
    efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(store,&cfg);CHECK(kv);if(!kv)return;
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    CHECK(efs_query_catalog_restore_disk(root,source,wm,4<<20,&next)==EFS_ERR_STALE && !next);
    CHECK(efs_query_catalog_catchup_disk(root,source,4<<20,cancel,NULL,&next)==EFS_ERR_BUSY && !next);
    CHECK(!efs_query_catalog_catchup_disk(root,source,4<<20,NULL,NULL,&next));
    CHECK(next && efs_query_catalog_rebuilt(next)>0 && efs_query_catalog_rebuilt(next)<4096);
    CHECK(!efs_query_catalog_build(source,NULL,32<<20,NULL,NULL,&memory));
    if(next && memory){compare(source,memory,next,EFS_ROOT_INO);compare(source,memory,next,dir);}
    struct expected_path expected={"dir/old",file,0};uint64_t cursor=0;
    CHECK(!efs_query_catalog_page(old,EFS_ROOT_INO,EFS_NAME_EXACT,"old",0,&cursor,4096,NULL,NULL,check_path,&expected) && expected.count==1);
    expected=(struct expected_path){"dir/new",file,0};cursor=0;
    CHECK(next && !efs_query_catalog_page(next,EFS_ROOT_INO,EFS_NAME_EXACT,"new",0,&cursor,4096,NULL,NULL,check_path,&expected) && expected.count==1);
    wm[1]++;CHECK(next && !efs_query_catalog_checkpoint(next,wm));
    /* The independently retained old handle still serves its original cut. */
    expected=(struct expected_path){"dir/old",file,0};cursor=0;
    CHECK(!efs_query_catalog_page(old,EFS_ROOT_INO,EFS_NAME_EXACT,"old",0,&cursor,4096,NULL,NULL,check_path,&expected) && expected.count==1);
    efs_query_catalog_free(next);next=NULL;efs_query_catalog_free(old);old=NULL;
    efs_kv_lsm_snapshot_release(source);source=NULL;efs_kv_lsm_close(kv);
    /* Corrupt derived revisions: authority still opens, and equal numeric
     * counters in a new lineage cannot reuse any previous raw partition. */
    char sidecar[512];snprintf(sidecar,sizeof(sidecar),"%s/QUERY-REVISIONS",store);CHECK(!unlink(sidecar));
    kv=efs_kv_lsm_open(store,&cfg);CHECK(kv);if(!kv)return;
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    CHECK(!efs_query_catalog_catchup_disk(root,source,4<<20,NULL,NULL,&next));
    CHECK(next && efs_query_catalog_rebuilt(next)==4096);
    if(next && memory)compare(source,memory,next,EFS_ROOT_INO);
    efs_query_catalog_free(next);efs_query_catalog_free(memory);efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(kv);
}
/* Lane row with big-endian EOF, one commit and one allocated chunk. */
static void lane_put(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,unsigned lane,uint64_t eof)
{
    uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_LANE_BYTES]={0};uint32_t kn;
    for(unsigned i=0;i<8;i++)value[i]=(uint8_t)(eof>>(56-8*i));
    value[31]=1;value[63]=1;
    CHECK(!efs_kv_key_lane(efs_kv_lane_shard(ino,(uint8_t)lane),ino,gen,(uint8_t)lane,key,&kn));
    CHECK(!efs_kv_put(kv,key,kn,value,sizeof(value)));
}
static void same_stat(struct efs_kv_lsm_snapshot *source,struct efs_query_catalog *disk,efs_ino_t ino)
{
    struct efs_query_catalog *memory=NULL;struct efs_meta_row a,b;struct efs_meta_stat sa,sb;
    CHECK(!efs_query_catalog_build(source,NULL,32<<20,NULL,NULL,&memory));if(!memory || !disk)return;
    CHECK(!efs_query_catalog_inode(memory,ino,&a,&sa) && !efs_query_catalog_inode(disk,ino,&b,&sb));
    CHECK(!memcmp(&a,&b,sizeof(a)) && !memcmp(&sa,&sb,sizeof(sa)));
    efs_query_catalog_free(memory);
}
/* Exact cross-shard dependencies: only partitions whose inodes read a changed
 * partition are rescanned. One-lane files depend on their own shard only, so
 * an unrelated edit, even one colliding modulo 64 with a real dependency,
 * walks nothing; the real dependency is still refreshed. A restored
 * generation has no exact rows and keeps the conservative filter. */
static void test_dependency_rows(void)
{
    char store[]="/tmp/efs-query-deps-kv-XXXXXX",root[]="/tmp/efs-query-deps-cache-XXXXXX";
    CHECK(mkdtemp(store));CHECK(mkdtemp(root));
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4,0};struct efs_kv *kv=efs_kv_lsm_open(store,&cfg);
    CHECK(kv);if(!kv)return;
    struct efs_meta_attrs attrs={1000,1000,100};efs_ino_t dir,ids[600];
    CHECK(!efs_meta_apply_init(kv,100));
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,EFS_ROOT_INO,S_IFDIR|0755,"deps",&dir));
    for(unsigned i=0;i<600;i++){
        char name[32];snprintf(name,sizeof(name),"f%04u",i);
        CHECK(!efs_meta_apply_create_file(kv,&attrs,dir,S_IFREG|0644,name,&ids[i]));
    }
    /* X: two lanes, lane 1 in another shard L. */
    efs_ino_t x=ids[0];struct efs_meta_row row;CHECK(!efs_meta_apply_get_inode(kv,x,&row));
    uint32_t xs=efs_kv_inode_shard(x),lshard=efs_kv_lane_shard(x,1);CHECK(lshard!=xs);
    row.active_lanes=3;
    {uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_INO_BYTES];uint32_t kn;
     CHECK(!efs_kv_key_inode(xs,x,key,&kn));CHECK(!efs_meta_pack_inode(&row,value,sizeof(value)));
     CHECK(!efs_kv_put(kv,key,kn,value,sizeof(value)));}
    lane_put(kv,x,row.generation,0,8192);lane_put(kv,x,row.generation,1,8192);
    /* Y: an unrelated (unreferenced) inode whose shard collides with L
     * modulo 64. Allocation clusters by directory, so place it directly. */
    efs_ino_t y=0;
    for(uint32_t ys=lshard%64;ys<4096 && !y;ys+=64)
        if(ys!=lshard && ys!=xs)y=(UINT64_C(7000001)<<12)|ys;
    CHECK(y && efs_kv_inode_shard(y)%64==lshard%64 && efs_kv_inode_shard(y)!=lshard);
    if(!y){efs_kv_lsm_close(kv);return;}
    struct efs_kv_lsm_snapshot *source=NULL;struct efs_query_catalog *d0=NULL,*d1=NULL,*d2=NULL,*r=NULL,*d3=NULL;
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    CHECK(!efs_query_catalog_build_disk(root,source,NULL,16<<20,NULL,NULL,&d0));
    same_stat(source,d0,x);efs_kv_lsm_snapshot_release(source);source=NULL;
    /* Lane 1 grows: only L changes; X's shard must be rescanned and refolded. */
    lane_put(kv,x,row.generation,1,1<<20);
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    CHECK(!efs_query_catalog_build_disk(root,source,d0,16<<20,NULL,NULL,&d1));
    CHECK(efs_query_catalog_rebuilt(d1)==1 && efs_query_catalog_walked(d1)==1 && efs_query_catalog_refolded(d1)==1);
    same_stat(source,d1,x);
    {struct efs_meta_row a;struct efs_meta_stat st;CHECK(d1 && !efs_query_catalog_inode(d1,x,&a,&st) && st.size>8192);}
    efs_kv_lsm_snapshot_release(source);source=NULL;
    /* Y's shard collides with L in the 64-bit filter but is not an input. */
    {struct efs_meta_row yr=row;uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_INO_BYTES];uint32_t kn;
     yr.ino=y;yr.nlink=0;yr.active_lanes=0;yr.base_size=77;
     CHECK(!efs_kv_key_inode(efs_kv_inode_shard(y),y,key,&kn));CHECK(!efs_meta_pack_inode(&yr,value,sizeof(value)));
     CHECK(!efs_kv_put(kv,key,kn,value,sizeof(value)));}
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    CHECK(!efs_query_catalog_build_disk(root,source,d1,16<<20,NULL,NULL,&d2));
    CHECK(efs_query_catalog_rebuilt(d2)==1 && efs_query_catalog_walked(d2)==0 && efs_query_catalog_refolded(d2)==1);
    same_stat(source,d2,x);
    /* Restored generations fall back to the persisted filter, still exact. */
    uint64_t wm[4]={5,6,7,8};CHECK(d2 && !efs_query_catalog_checkpoint(d2,wm));
    CHECK(!efs_query_catalog_restore_disk(root,source,wm,16<<20,&r));
    efs_kv_lsm_snapshot_release(source);source=NULL;
    lane_put(kv,x,row.generation,1,2<<20);
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    CHECK(r && !efs_query_catalog_build_disk(root,source,r,16<<20,NULL,NULL,&d3));
    CHECK(efs_query_catalog_walked(d3)>=1);
    same_stat(source,d3,x);
    efs_query_catalog_free(d3);efs_query_catalog_free(r);efs_query_catalog_free(d2);
    efs_query_catalog_free(d1);efs_query_catalog_free(d0);
    efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(kv);
}
int main(void)
{
    if(!getenv("EFS_QUERY_TEST_QUOTA") && !getenv("EFS_QUERY_TEST_NOSPACE")){test_restart_catchup();test_catchup_retirement();test_dependency_rows();}
    char store[256],root[256];snprintf(store,sizeof(store),"/tmp/efs-query-disk-kv-%d",getpid());snprintf(root,sizeof(root),"/tmp/efs-query-disk-cache-%d",getpid());
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4,0};struct efs_kv *kv=efs_kv_lsm_open(store,&cfg);CHECK(kv);if(!kv)return 1;
    struct efs_meta_attrs attrs={1000,1000,100};efs_ino_t dir,sub,file;
    CHECK(!efs_meta_apply_init(kv,100));CHECK(!efs_meta_apply_mkdir(kv,&attrs,EFS_ROOT_INO,S_IFDIR|0755,"dir",&dir));
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,dir,S_IFDIR|0755,"sub",&sub));
    efs_ino_t empty;
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,EFS_ROOT_INO,S_IFDIR|0755,"empty",&empty));
    unsigned files=3000;
    const char *size=getenv("EFS_QUERY_TEST_FILES");
    if(size){unsigned long count=strtoul(size,NULL,10);if(count<1 || count>200000)return 2;files=(unsigned)count;}
    unsigned branches=0;efs_ino_t branch[64];
    const char *fanout=getenv("EFS_QUERY_TEST_BRANCHES");
    if(fanout){unsigned long n=strtoul(fanout,NULL,10);if(n<1 || n>64)return 2;branches=(unsigned)n;}
    for(unsigned i=0;i<branches;i++){
        char name[32];snprintf(name,sizeof(name),"branch-%02u",i);
        CHECK(!efs_meta_apply_mkdir(kv,&attrs,sub,S_IFDIR|0755,name,&branch[i]));
    }
    for(unsigned i=0;i<files;i++) {
        char name[32];snprintf(name,sizeof(name),"file-%08u",files-1-i);
        CHECK(!efs_meta_apply_create_file(kv,&attrs,branches?branch[i%branches]:sub,S_IFREG|0644,name,&file));
    }
    efs_ino_t removed,removed_file;
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,EFS_ROOT_INO,S_IFDIR|0755,"removed",&removed));
    CHECK(!efs_meta_apply_create_file(kv,&attrs,removed,S_IFREG|0644,"gone",&removed_file));
    CHECK(!efs_meta_apply_unlink(kv,removed,"gone",101));
    CHECK(!efs_meta_apply_rmdir(kv,EFS_ROOT_INO,"removed",102));
    struct efs_kv_lsm_snapshot *source=NULL;CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    struct efs_query_catalog *memory=NULL,*disk=NULL,*next=NULL;
    CHECK(!efs_query_catalog_build(source,NULL,32<<20,NULL,NULL,&memory));
    if(!memory)return 1;
    int rc=efs_query_catalog_build_disk(root,source,NULL,4<<20,NULL,NULL,&disk);
    if(getenv("EFS_QUERY_TEST_QUOTA")) {
        CHECK(rc==EFS_ERR_QUOTA && !disk);
        efs_query_catalog_free(memory);efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(kv);
        printf("query disk: generation quota %s\n",failures?"FAIL":"PASS");return failures?1:0;
    }
    if(getenv("EFS_QUERY_TEST_NOSPACE")) {
        CHECK(rc==EFS_ERR_IO && !disk);
        efs_query_catalog_free(memory);efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(kv);
        printf("query disk: free-space guard %s\n",failures?"FAIL":"PASS");return failures?1:0;
    }
    CHECK(!rc && disk);if(!disk){fprintf(stderr,"disk build rc=%d\n",rc);return 1;}
    compare(source,memory,disk,EFS_ROOT_INO);compare(source,memory,disk,dir);compare(source,memory,disk,sub);
    struct result scoped={0};
    CHECK(!efs_query_catalog_find(memory,empty,EFS_NAME_CONTAINS,"",4,collect,&scoped) && !scoped.count);
    CHECK(!efs_query_catalog_find(disk,empty,EFS_NAME_CONTAINS,"",4,collect,&scoped) && !scoped.count);
    CHECK(efs_query_catalog_find(disk,sub,EFS_NAME_CONTAINS,"",1,collect,&scoped)==EFS_ERR_BUSY);
    uint64_t bytes=efs_query_catalog_bytes(disk);CHECK(bytes<(4<<20));
    struct efs_query_build_times times=efs_query_catalog_build_times(disk);
    CHECK(!times.restored && times.index_ns && times.stats_ns && times.persist_ns && !times.restore_ns);
    struct efs_query_build_times memory_times=efs_query_catalog_build_times(memory);
    CHECK(!memory_times.restored && memory_times.index_ns && memory_times.stats_ns && !memory_times.persist_ns);
    CHECK(efs_query_catalog_build_disk(root,source,disk,1,NULL,NULL,&next)==EFS_ERR_NOMEM && !next);
    CHECK(efs_query_catalog_build_disk(root,source,disk,4<<20,cancel,NULL,&next)==EFS_ERR_BUSY && !next);
    CHECK(!efs_query_catalog_build_disk(root,source,disk,4<<20,NULL,NULL,&next));CHECK(!efs_query_catalog_rebuilt(next));CHECK(!efs_query_catalog_refolded(next));
    compare(source,memory,next,EFS_ROOT_INO);CHECK(efs_query_catalog_bytes(next)==bytes);efs_query_catalog_free(next);next=NULL;
    /* One row edit must not re-fold every unrelated lane-free inode in its
     * partition. An APPEND_CUR change, however, must invalidate a stable row. */
    efs_kv_lsm_snapshot_release(source);source=NULL;efs_query_catalog_free(memory);memory=NULL;
    struct efs_meta_row edited;CHECK(!efs_meta_apply_get_inode(kv,file,&edited));
    edited.base_size=123;
    uint8_t ik[EFS_KV_KEY_MAX],iv[EFS_META_INO_BYTES];uint32_t ikn=0;
    CHECK(!efs_kv_key_inode(efs_kv_inode_shard(file),file,ik,&ikn));
    CHECK(!efs_meta_pack_inode(&edited,iv,sizeof(iv)));CHECK(!efs_kv_put(kv,ik,ikn,iv,sizeof(iv)));
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    CHECK(!efs_query_catalog_build_disk(root,source,disk,4<<20,NULL,NULL,&next));
    CHECK(efs_query_catalog_refolded(next)==1);
    CHECK(!efs_query_catalog_build(source,NULL,32<<20,NULL,NULL,&memory));
    compare(source,memory,next,sub);
    struct efs_meta_row fr1,fr2;struct efs_meta_stat fs1,fs2;
    CHECK(!efs_query_catalog_inode(memory,file,&fr1,&fs1));
    CHECK(!efs_query_catalog_inode(next,file,&fr2,&fs2));
    CHECK(!memcmp(&fr1,&fr2,sizeof(fr1)) && !memcmp(&fs1,&fs2,sizeof(fs1)));
    efs_query_catalog_free(disk);disk=next;next=NULL;
    efs_kv_lsm_snapshot_release(source);source=NULL;efs_query_catalog_free(memory);memory=NULL;
    uint8_t ak[EFS_KV_KEY_MAX],av[24]={0};uint32_t akn=0;
    /* Big-endian watermark/frontier 512/256 with one open reservation. */
    av[6]=2;av[14]=1;av[19]=1;
    CHECK(!efs_kv_key_append_cur(efs_kv_inode_shard(file),file,edited.generation,ak,&akn));
    CHECK(!efs_kv_put(kv,ak,akn,av,sizeof(av)));
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    CHECK(!efs_query_catalog_build_disk(root,source,disk,4<<20,NULL,NULL,&next));
    CHECK(efs_query_catalog_refolded(next)>0);
    CHECK(!efs_query_catalog_build(source,NULL,32<<20,NULL,NULL,&memory));
    compare(source,memory,next,sub);
    CHECK(!efs_query_catalog_inode(memory,file,&fr1,&fs1));
    CHECK(!efs_query_catalog_inode(next,file,&fr2,&fs2));
    CHECK(!memcmp(&fr1,&fr2,sizeof(fr1)) && !memcmp(&fs1,&fs2,sizeof(fs1)));
    struct efs_meta_row ar;struct efs_meta_stat ast;
    CHECK(!efs_query_catalog_inode(next,file,&ar,&ast) && ast.size==256);
    efs_query_catalog_free(disk);disk=next;next=NULL;
    efs_kv_lsm_snapshot_release(source);efs_query_catalog_free(memory);memory=NULL;
    CHECK(!efs_meta_apply_rename(kv,dir,"sub",EFS_ROOT_INO,"moved",200));
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));CHECK(!efs_query_catalog_build(source,NULL,32<<20,NULL,NULL,&memory));
    CHECK(!efs_query_catalog_build_disk(root,source,disk,4<<20,NULL,NULL,&next));
    compare(source,memory,next,EFS_ROOT_INO);CHECK(efs_query_catalog_bytes(next)==bytes);
    char path[256];CHECK(!efs_query_catalog_path(disk,sub,EFS_ROOT_INO,path,sizeof(path)) && !strcmp(path,"dir/sub"));
    CHECK(!efs_query_catalog_path(next,sub,EFS_ROOT_INO,path,sizeof(path)) && !strcmp(path,"moved"));
    uint64_t wm[4]={11,12,21,22};struct efs_query_catalog *restored=NULL;
    CHECK(!efs_query_catalog_checkpoint(next,wm));
    efs_query_catalog_free(next);next=NULL;
    CHECK(!efs_query_catalog_restore_disk(root,source,wm,4<<20,&restored));
    times=efs_query_catalog_build_times(restored);
    CHECK(times.restored && times.restore_ns && !times.index_ns && !times.stats_ns && !times.persist_ns);
    times=efs_query_catalog_build_times(disk);
    CHECK(!times.restored && times.index_ns && times.stats_ns && times.persist_ns);

    if(restored)compare(source,memory,restored,EFS_ROOT_INO);
    uint64_t stale[4]={11,13,21,22};
    CHECK(efs_query_catalog_restore_disk(root,source,stale,4<<20,&next)==EFS_ERR_STALE && !next);
    /* A live directory entry stored outside its authoritative placement must
     * not be silently counted or produce an apparently complete catalog. */
    struct efs_meta_row malformed;CHECK(!efs_meta_apply_get_inode(kv,dir,&malformed));
    malformed.ino=UINT64_C(9000001)<<12;malformed.parent=EFS_ROOT_INO;malformed.nlink=2;
    uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_INO_BYTES];uint32_t kn;
    CHECK(!efs_kv_key_inode(efs_kv_inode_shard(malformed.ino),malformed.ino,key,&kn));
    CHECK(!efs_meta_pack_inode(&malformed,value,sizeof(value)));CHECK(!efs_kv_put(kv,key,kn,value,sizeof(value)));
    struct efs_meta_dentry misplaced={0};misplaced.ino=malformed.ino;misplaced.generation=malformed.generation;misplaced.type=S_IFDIR;
    uint8_t dent[EFS_META_DENT_BYTES];CHECK(!efs_meta_pack_dentry(&misplaced,dent,sizeof(dent)));
    CHECK(!efs_kv_key_dentry(0,EFS_ROOT_INO,"misplaced",key,&kn));CHECK(!efs_kv_put(kv,key,kn,dent,sizeof(dent)));
    struct efs_kv_lsm_snapshot *bad_source=NULL;CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&bad_source));
    CHECK(efs_query_catalog_build(bad_source,NULL,32<<20,NULL,NULL,&next)==EFS_ERR_IO && !next);
    CHECK(efs_query_catalog_build_disk(root,bad_source,NULL,4<<20,NULL,NULL,&next)==EFS_ERR_IO && !next);
    efs_kv_lsm_snapshot_release(bad_source);
    efs_query_catalog_free(restored);
    efs_query_catalog_free(disk);disk=NULL;
    /* Startup recovery keeps current, removes abandoned candidates and locks
     * out another publisher. Corrupt bytes must never become a restored view. */
    char orphan[512];snprintf(orphan,sizeof(orphan),"%s/view-abandoned",root);
    CHECK(!mkdir(orphan,0700));
    int lease=-1,second=-1;CHECK(!efs_query_catalog_disk_recover(root,&lease));
    CHECK(access(orphan,F_OK)!=0);
    CHECK(efs_query_catalog_disk_recover(root,&second)==EFS_ERR_BUSY && second<0);
    char current[512],base[256],generation[768];snprintf(current,sizeof(current),"%s/current",root);
    ssize_t len=readlink(current,base,sizeof(base)-1);CHECK(len>0);
    if(len>0) {
        base[len]=0;snprintf(generation,sizeof(generation),"%s/%s",root,base);
        /* Cache ABI changes must reject old checkpoints, never reinterpret
         * their larger inode rows. This cache is rebuilt from authoritative KV. */
        char manifest[1024];snprintf(manifest,sizeof(manifest),"%s/manifest",generation);
        int manifest_fd=open(manifest,O_RDWR);uint64_t version=0,legacy=5;
        CHECK(manifest_fd>=0);
        if(manifest_fd>=0){
            CHECK(pread(manifest_fd,&version,sizeof(version),8)==sizeof(version));
            CHECK(version==7);
            uint64_t row_bytes=0;
            CHECK(pread(manifest_fd,&row_bytes,sizeof(row_bytes),24)==sizeof(row_bytes));
            CHECK(row_bytes==312); /* Previously 584: no max-name per regular file. */
            CHECK(pwrite(manifest_fd,&legacy,sizeof(legacy),8)==sizeof(legacy));
            CHECK(efs_query_catalog_restore_disk(root,source,wm,4<<20,&next)==EFS_ERR_PROTO && !next);
            CHECK(pwrite(manifest_fd,&version,sizeof(version),8)==sizeof(version));
            close(manifest_fd);
            CHECK(!efs_query_catalog_restore_disk(root,source,wm,4<<20,&next));
            if(next)compare(source,memory,next,EFS_ROOT_INO);
            efs_query_catalog_free(next);next=NULL;
        }
        DIR *directory=opendir(generation);CHECK(directory);struct dirent *entry;
        while(directory && (entry=readdir(directory))) {
            size_t size=strlen(entry->d_name);
            if(size<2 || strcmp(entry->d_name+size-2,".r"))continue;
            char raw[1024];snprintf(raw,sizeof(raw),"%s/%s",generation,entry->d_name);
            int fd=open(raw,O_RDWR);unsigned char byte=0;CHECK(fd>=0);
            if(fd>=0){CHECK(pread(fd,&byte,1,0)==1);byte^=1;CHECK(pwrite(fd,&byte,1,0)==1);close(fd);}break;
        }
        if(directory)closedir(directory);
        CHECK(efs_query_catalog_restore_disk(root,source,wm,4<<20,&next)==EFS_ERR_IO && !next);
    }
    if(lease>=0)close(lease);
    efs_query_catalog_free(disk);efs_query_catalog_free(next);efs_query_catalog_free(memory);efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(kv);
    printf("query disk: RAM oracle, external sort, bounded metadata, retained snapshot, rename and cancellation %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
