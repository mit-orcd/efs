#include "efs/name_index.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c,m) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,m);failures++; } } while (0)
static char path[256];
static void cleanup(void)
{
    DIR *d=opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e=readdir(d))) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char p[512];snprintf(p,sizeof(p),"%s/%s",path,e->d_name);unlink(p);
    }
    closedir(d);rmdir(path);
}
static int put_dentry(struct efs_kv *kv, uint32_t shard, efs_ino_t parent,
                       const char *name, efs_ino_t ino)
{
    uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_DENT_BYTES];uint32_t len;
    struct efs_meta_dentry d={ino,1,S_IFREG};
    int rc=efs_kv_key_dentry(shard,parent,name,key,&len);
    if (!rc) rc=efs_meta_pack_dentry(&d,value,sizeof(value));
    return rc ? rc : efs_kv_put(kv,key,len,value,sizeof(value));
}
struct found { unsigned n;efs_ino_t parents[32];char names[32][EFS_MAX_NAME];int fail; };
static int collect(void *user, const struct efs_name_entry *entry)
{
    struct found *f=user;
    if (f->fail) { f->fail=0;return EFS_ERR_ACCES; }
    if (f->n>=32) return EFS_ERR_NOMEM;
    f->parents[f->n]=entry->parent;
    strcpy(f->names[f->n++],entry->name);
    return EFS_OK;
}
int main(void)
{
    snprintf(path,sizeof(path),"/tmp/efs-name-index-%d",getpid());cleanup();
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4};
    struct efs_kv *kv=efs_kv_lsm_open(path,&cfg);
    CHECK(kv!=NULL,"open store");if (!kv) return 1;
    CHECK(!put_dentry(kv,3,100,"report-aaa.dat",201),"first report");
    CHECK(!put_dentry(kv,3,101,"report-aaa.dat",201),"hardlink other parent");
    CHECK(!put_dentry(kv,3,100,"report-bbb.txt",202),"second report");
    CHECK(!put_dentry(kv,3,100,"old-tomb",0),"layout tombstone");
    CHECK(!put_dentry(kv,3,100,"a\nb",203),"newline name");
    CHECK(!put_dentry(kv,3,100,"caf\xc3\xa9.dat",204),"UTF8 name");
    CHECK(!put_dentry(kv,4,100,"other-shard",205),"partition isolation");
    CHECK(!efs_kv_lsm_flush(kv),"flush base");
    CHECK(!put_dentry(kv,3,102,"unflushed.dat",206),"unflushed name");
    struct efs_kv_lsm_snapshot *source=NULL;
    CHECK(!efs_kv_lsm_snapshot_pin(kv,1<<20,&source),"capture source");
    struct efs_name_index *index=NULL,*newer=NULL;
    CHECK(!efs_name_index_build(source,3,1<<20,&index),"build partition");
    if (!index) { efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(kv);cleanup();return 1; }
    CHECK(efs_name_index_count(index)==7,"names include unflushed and tombstone, exclude other shard");
    CHECK(efs_name_index_revision(index)==7,"dirty revision belongs to source");
    CHECK(efs_name_index_build(source,3,32,&newer)==EFS_ERR_NOMEM && !newer,
          "index memory bound is explicit");
    CHECK(efs_name_index_build(source,3,sizeof(struct efs_name_entry)+64,&newer)==EFS_ERR_NOMEM && !newer,
          "partial index allocation is discarded when budget is exhausted");
    struct found f={0};uint32_t cursor=0;
    CHECK(!efs_name_index_query(index,EFS_NAME_EXACT,"report-aaa.dat",&cursor,10,collect,&f) && f.n==2,
          "exact seeks duplicate names");
    CHECK(f.parents[0]==100 && f.parents[1]==101,"stable parent ordering");
    f=(struct found){0};cursor=0;uint32_t visits=99;
    CHECK(!efs_name_index_query_visits(index,EFS_NAME_EXACT,"absent",&cursor,0,collect,&f,&visits) && !visits,
          "empty exact range needs no visit budget");
    cursor=0;
    CHECK(efs_name_index_query_visits(index,EFS_NAME_EXACT,"report-aaa.dat",&cursor,1,collect,&f,&visits)==EFS_ERR_BUSY && visits==1,
          "actual visited count excludes sorted seek distance");

    f=(struct found){0};cursor=0;
    CHECK(efs_name_index_query(index,EFS_NAME_PREFIX,"report-",&cursor,1,collect,&f)==EFS_ERR_BUSY && f.n==1,
          "prefix bounded page is explicit");
    int rc;
    do { rc=efs_name_index_query(index,EFS_NAME_PREFIX,"report-",&cursor,1,collect,&f); } while (rc==EFS_ERR_BUSY);
    CHECK(!rc && f.n==3,"continued pages neither duplicate nor lose names");
    f=(struct found){0};cursor=0;
    do { rc=efs_name_index_query(index,EFS_NAME_SUFFIX,".dat",&cursor,2,collect,&f); } while (rc==EFS_ERR_BUSY);
    CHECK(!rc && f.n==4,"suffix bounded full candidate scan");
    f=(struct found){0};cursor=0;
    CHECK(!efs_name_index_query(index,EFS_NAME_CONTAINS,"bbb",&cursor,10,collect,&f) && f.n==1,
          "substring matching");
    f=(struct found){0};cursor=0;
    CHECK(!efs_name_index_query(index,EFS_NAME_EXACT,"old-tomb",&cursor,10,collect,&f) && f.n==1,
          "layout tombstones are retained for authoritative precedence");
    f=(struct found){.fail=1};cursor=0;
    CHECK(efs_name_index_query(index,EFS_NAME_PREFIX,"report-",&cursor,10,collect,&f)==EFS_ERR_ACCES,
          "candidate validation failure is preserved");
    CHECK(!efs_name_index_query(index,EFS_NAME_PREFIX,"report-",&cursor,10,collect,&f) && f.n==3,
          "failed callback can retry the same candidate");
    CHECK(!put_dentry(kv,3,100,"later.dat",207),"mutate live after capture");
    CHECK(!efs_name_index_build(source,3,1<<20,&newer) && efs_name_index_count(newer)==7,
          "old captured index excludes new names");
    efs_name_index_free(newer);newer=NULL;
    efs_kv_lsm_snapshot_release(source);source=NULL;
    uint8_t pending[]={0,3,EFS_KV_KIND_INTENT,1};
    CHECK(!efs_kv_put(kv,pending,sizeof(pending),(const uint8_t *)"pending",7),"unresolved transaction");
    CHECK(!efs_kv_lsm_snapshot_pin(kv,1<<20,&source),"capture pending source");
    CHECK(efs_name_index_build(source,3,1<<20,&newer)==EFS_ERR_BUSY && !newer,
          "do not build from unresolved participant images");
    CHECK(!efs_name_index_build(source,4,1<<20,&newer) && efs_name_index_count(newer)==1,
          "pending work does not block unrelated partition build");
    efs_name_index_free(newer);newer=NULL;
    efs_kv_lsm_snapshot_release(source);source=NULL;
    CHECK(!efs_kv_del(kv,pending,sizeof(pending)),"resolve pending marker");
    CHECK(!efs_kv_lsm_snapshot_pin(kv,1<<20,&source),"capture resolved source");
    CHECK(!efs_name_index_build(source,3,1<<20,&newer) && efs_name_index_count(newer)==8 &&
          efs_name_index_revision(newer)>efs_name_index_revision(index),"new partition version advances");
    CHECK(efs_name_index_count(index)==7,"published old index remains immutable");
    efs_name_index_free(newer);newer=NULL;
    efs_kv_lsm_snapshot_release(source);source=NULL;
    for (unsigned kind=EFS_KV_KIND_GUARD;kind<=EFS_KV_KIND_REDUCE;kind++) {
        pending[2]=(uint8_t)kind;
        CHECK(!efs_kv_put(kv,pending,sizeof(pending),(const uint8_t *)"pending",7),"pending guard/reduction");
        CHECK(!efs_kv_lsm_snapshot_pin(kv,1<<20,&source),"capture pending guard/reduction");
        CHECK(efs_name_index_build(source,3,1<<20,&newer)==EFS_ERR_BUSY && !newer,
              "guard/reduction blocks new index publication");
        efs_kv_lsm_snapshot_release(source);source=NULL;
        CHECK(!efs_kv_del(kv,pending,sizeof(pending)),"clear pending guard/reduction");
    }
    uint8_t key[EFS_KV_KEY_MAX];uint32_t kl;
    CHECK(!efs_kv_key_dentry(3,100,"report-aaa.dat",key,&kl) && !efs_kv_del(kv,key,kl),"remove one hardlink name");
    CHECK(!efs_kv_lsm_snapshot_pin(kv,1<<20,&source),"capture unlink");
    CHECK(!efs_name_index_build(source,3,1<<20,&newer),"build after unlink");
    f=(struct found){0};cursor=0;
    CHECK(!efs_name_index_query(newer,EFS_NAME_EXACT,"report-aaa.dat",&cursor,10,collect,&f) && f.n==1 && f.parents[0]==101,
          "unlink removes only the selected name in a fresh index");

    efs_name_index_free(newer);efs_name_index_free(index);
    efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(kv);cleanup();
    if (failures) return 1;
    puts("test_name_index: OK");return 0;
}
