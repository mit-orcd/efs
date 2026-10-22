/* Existing private scale fixture only. Stream the immutable old generation
 * while its source changes and a new generation is built. Restore the fixture's
 * original +1 mutation so the independent recovery oracle remains applicable.
 * Run (Linux): make tests/measure/bench_query_disk_concurrent, then
 *   tests/measure/bench_query_disk_concurrent ROOT FILES
 * on a ROOT built by bench_query_disk_scale with the same FILES; exit 2 = usage. */
#define main scale_constructor_main
#include "bench_query_disk_scale.c"
#undef main
#include <pthread.h>

struct parallel_export {
    struct oracle oracle;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int first, release, done, rc;
    unsigned building;
    uint64_t overlap_rows;
};
static int parallel_emit(void *ctx,const struct efs_name_entry *entry,const char *path)
{
    struct parallel_export *p=ctx;
    int rc=emit(&p->oracle,entry,path);if(rc)return rc;
    if(p->oracle.count==1){
        pthread_mutex_lock(&p->mutex);p->first=1;
        pthread_cond_broadcast(&p->condition);
        while(!p->release)pthread_cond_wait(&p->condition,&p->mutex);
        pthread_mutex_unlock(&p->mutex);
    }
    if(__atomic_load_n(&p->building,__ATOMIC_ACQUIRE))p->overlap_rows++;
    return EFS_OK;
}
static void *parallel_reader(void *ctx)
{
    struct parallel_export *p=ctx;uint64_t cursor=0;
    do{
        uint64_t before=cursor;
        p->rc=efs_query_catalog_page(p->oracle.catalog,EFS_ROOT_INO,
            EFS_NAME_PREFIX,"file-",0,&cursor,4096,NULL,NULL,parallel_emit,p);
        if(p->rc)break;
        if(cursor==before && cursor!=(UINT64_C(4096)<<32)){p->rc=EFS_ERR_PROTO;break;}
    }while(cursor!=(UINT64_C(4096)<<32));
    if(!p->rc && p->oracle.count!=p->oracle.n)p->rc=EFS_ERR_PROTO;
    for(unsigned s=0;s<SHARDS && !p->rc;s++)
        if(p->oracle.seen[s]!=population(p->oracle.n,s))p->rc=EFS_ERR_PROTO;
    pthread_mutex_lock(&p->mutex);p->done=1;
    pthread_cond_broadcast(&p->condition);pthread_mutex_unlock(&p->mutex);
    return NULL;
}
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    char *end;errno=0;uint64_t n=strtoull(argv[2],&end,10);
    if(errno || *end || !n || n>(UINT64_C(1)<<32))return 2;
    char kvpath[EFS_MAX_PATH],cache[EFS_MAX_PATH];
    if(snprintf(kvpath,sizeof(kvpath),"%s/kv",argv[1])>=(int)sizeof(kvpath) ||
       snprintf(cache,sizeof(cache),"%s/cache",argv[1])>=(int)sizeof(cache))return 2;
    struct stat st;
    if(lstat(argv[1],&st) || !S_ISDIR(st.st_mode) ||
       lstat(kvpath,&st) || !S_ISDIR(st.st_mode) ||
       lstat(cache,&st) || !S_ISDIR(st.st_mode))return 2;
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,4<<20,4,0};
    struct batch b={.kv=efs_kv_lsm_open(kvpath,&cfg)};if(!b.kv)return 1;
    struct efs_kv_lsm_snapshot *old_source=NULL,*source=NULL;
    struct efs_query_catalog *old=NULL,*next=NULL,*restored=NULL;
    uint64_t samples=0,watermark[4]={1,2,3,4};double start=clock_s();
    int rc=efs_kv_lsm_snapshot_pin(b.kv,64<<20,&old_source),started=0,touched=0;
    if(!rc)rc=efs_query_catalog_catchup_disk(cache,old_source,4<<20,NULL,NULL,&old);
    if(!rc)rc=check(old,n,0,&samples,1);
    struct parallel_export p={.oracle={.catalog=old,.n=n,.changed=1},
        .mutex=PTHREAD_MUTEX_INITIALIZER,.condition=PTHREAD_COND_INITIALIZER};
    pthread_t reader;
    if(!rc){if(pthread_create(&reader,NULL,parallel_reader,&p))rc=EFS_ERR_IO;else started=1;}
    if(started){
        pthread_mutex_lock(&p.mutex);
        while(!p.first && !p.done)pthread_cond_wait(&p.condition,&p.mutex);
        if(!p.first)rc=p.rc?p.rc:EFS_ERR_PROTO;
        pthread_mutex_unlock(&p.mutex);
    }
    if(!rc){touched=1;rc=inode(&b,file_ino(0),SHARDS,0,0);}
    if(!rc)rc=flush(&b);
    if(!rc)rc=efs_kv_lsm_snapshot_pin(b.kv,64<<20,&source);
    __atomic_store_n(&p.building,1,__ATOMIC_RELEASE);
    pthread_mutex_lock(&p.mutex);p.release=1;
    pthread_cond_broadcast(&p.condition);pthread_mutex_unlock(&p.mutex);
    if(!rc)rc=efs_query_catalog_build_disk(cache,source,old,4<<20,NULL,NULL,&next);
    __atomic_store_n(&p.building,0,__ATOMIC_RELEASE);
    if(started){pthread_join(reader,NULL);if(!rc)rc=p.rc;}
    if(!rc && !p.overlap_rows)rc=EFS_ERR_PROTO;
    if(!rc)rc=check(old,n,0,&samples,1);
    if(!rc)rc=check(next,n,1,&samples,0);
    uint64_t builder_bytes=efs_query_catalog_bytes(next);
    uint64_t source_bytes=efs_kv_lsm_snapshot_bytes(old_source)+efs_kv_lsm_snapshot_bytes(source);
    efs_kv_lsm_snapshot_release(source);source=NULL;
    efs_kv_lsm_snapshot_release(old_source);old_source=NULL;
    /* A failed test also attempts to restore its private source; the original
     * failure remains the verdict. It never mutates an EFS service namespace. */
    if(touched){
        b.count=0;int reset=inode(&b,file_ino(0),SHARDS,0,1);
        if(!reset)reset=flush(&b);
        if(!rc)rc=reset;
    }
    if(!rc)rc=efs_kv_lsm_snapshot_pin(b.kv,64<<20,&source);
    if(!rc)rc=efs_query_catalog_build_disk(cache,source,next,4<<20,NULL,NULL,&restored);
    if(!rc)rc=check(restored,n,0,&samples,1);
    if(!rc)rc=efs_query_catalog_checkpoint(restored,watermark);
    printf("{\"phase\":\"concurrent-export-mutation\",\"files\":%llu,\"old_exported\":%llu,\"rows_during_build\":%llu,\"builder_bytes\":%llu,\"retained_source_bytes\":%llu,\"seconds\":%.6f,\"rc\":%d}\n",
        (unsigned long long)n,(unsigned long long)p.oracle.count,
        (unsigned long long)p.overlap_rows,(unsigned long long)builder_bytes,
        (unsigned long long)source_bytes,clock_s()-start,rc);
    pthread_cond_destroy(&p.condition);pthread_mutex_destroy(&p.mutex);
    efs_query_catalog_free(restored);efs_query_catalog_free(next);efs_query_catalog_free(old);
    efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(b.kv);
    return rc?1:0;
}
