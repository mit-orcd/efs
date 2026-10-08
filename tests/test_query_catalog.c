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
static int capture(struct efs_kv *kv,const struct efs_query_catalog *old,struct efs_query_catalog **out)
{
    struct efs_kv_lsm_snapshot *s=NULL;
    int rc=efs_kv_lsm_snapshot_pin(kv,8<<20,&s);
    if (!rc) rc=efs_query_catalog_build(s,old,32<<20,NULL,NULL,out);
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
static int cancel(void *ctx) { (void)ctx;return 1; }
int main(void)
{
    snprintf(store,sizeof(store),"/tmp/efs-query-catalog-%d",getpid());cleanup();
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4};
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
    char path[128];
    CHECK(!efs_query_catalog_path(c,b,EFS_ROOT_INO,path,sizeof(path)) && !strcmp(path,"a/b"),"relative ancestry");
    CHECK(efs_query_catalog_path(c,a,b,path,sizeof(path))==EFS_ERR_NOT_FOUND,"outside scope");
    CHECK(efs_query_catalog_path(c,b,EFS_ROOT_INO,path,3)==EFS_ERR_NAMETOOLONG,"path limit");
    struct found f={0};
    CHECK(!efs_query_catalog_find(c,a,EFS_NAME_EXACT,"report.dat",1,collect,&f) && f.n==1 &&
          !strcmp(f.path[0],"b/report.dat"),"exact query spends only matching candidate budget");
    f=(struct found){0};
    CHECK(efs_query_catalog_find(c,a,EFS_NAME_SUFFIX,".dat",1,collect,&f)==EFS_ERR_BUSY,"global scan budget explicit");
    f=(struct found){.fail=1};
    CHECK(efs_query_catalog_find(c,a,EFS_NAME_EXACT,"report.dat",10,collect,&f)==EFS_ERR_ACCES,"emitter denial preserved");
    CHECK(!capture(kv,c,&newer) && efs_query_catalog_rebuilt(newer)==0,"unchanged partitions reused");
    efs_query_catalog_free(newer);newer=NULL;
    CHECK(!efs_meta_apply_rename(kv,a,"b",EFS_ROOT_INO,"moved",230),"move subtree");
    CHECK(!efs_meta_apply_unlink(kv,a,"alias.dat",240),"unlink alias");
    CHECK(!capture(kv,c,&newer) && efs_query_catalog_rebuilt(newer)>0 &&
          efs_query_catalog_rebuilt(newer)<4096,"refresh changed partitions only");
    if (!newer) goto done;
    CHECK(!efs_query_catalog_stats(newer,a,&stat) && !stat.tree_files && !stat.tree_dirs && !stat.tree_bytes,"moved source empty");
    CHECK(!efs_query_catalog_stats(c,a,&stat) && stat.tree_files==2,"old catalog immutable");
    CHECK(!efs_query_catalog_path(newer,b,EFS_ROOT_INO,path,sizeof(path)) && !strcmp(path,"moved"),"move changes path");
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
    uint8_t pending[]={0,0,EFS_KV_KIND_INTENT,1};
    CHECK(!efs_kv_put(kv,pending,sizeof(pending),(const uint8_t *)"x",1),"pending marker");
    CHECK(capture(kv,c,&newer)==EFS_ERR_BUSY && !newer,"unresolved transaction refuses publication");
    CHECK(!efs_query_catalog_stats(c,b,&stat) && stat.tree_bytes==6000,"failed refresh retains published image");
    CHECK(!efs_kv_del(kv,pending,sizeof(pending)),"clear pending");
    CHECK(!efs_meta_apply_get_inode(kv,b,&r),"dir for corruption");
    r.parent=a;row_put(kv,&r);
    CHECK(capture(kv,c,&newer)==EFS_ERR_PROTO && !newer,"mismatched parent fails closed");
done:
    efs_query_catalog_free(newer);efs_query_catalog_free(c);efs_kv_lsm_close(kv);cleanup();
    if (failures) return 1;
    puts("test_query_catalog: OK");return 0;
}
