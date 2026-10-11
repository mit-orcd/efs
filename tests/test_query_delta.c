/* Production disk builder: changes beyond the former in-memory ID limit must
 * retain incremental work and agree with an independent RAM catalog oracle. */
#include "efs/query_catalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ftw.h>
static int failures;
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);failures++;}}while(0)
static int clean_fixture(const char *path,const struct stat *st,int kind,struct FTW *walk)
{(void)st;(void)kind;(void)walk;return remove(path);}
int main(void)
{
    char store[]="/tmp/efs-query-delta-kv-XXXXXX",cache[]="/tmp/efs-query-delta-cache-XXXXXX";
    CHECK(mkdtemp(store));CHECK(mkdtemp(cache));
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4};
    struct efs_kv *kv=efs_kv_lsm_open(store,&cfg);CHECK(kv);if(!kv)return 1;
    struct efs_meta_attrs attrs={1000,1000,100};efs_ino_t dir;
    unsigned files=6000;const char *n=getenv("EFS_QUERY_DELTA_FILES");
    if(n){unsigned long value=strtoul(n,NULL,10);if(value<6000 || value>100000)return 2;files=(unsigned)value;}
    efs_ino_t *ids=calloc(files,sizeof(*ids));CHECK(ids);if(!ids){efs_kv_lsm_close(kv);return 1;}
    CHECK(!efs_meta_apply_init(kv,100));
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,EFS_ROOT_INO,S_IFDIR|0755,"delta",&dir));
    for(unsigned i=0;i<files;i++){
        char name[32];snprintf(name,sizeof(name),"file-%04u",i);
        CHECK(!efs_meta_apply_create_file(kv,&attrs,dir,S_IFREG|0644,name,&ids[i]));
        if(getenv("EFS_QUERY_DELTA_LANES")){
            struct efs_meta_row row;uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_INO_BYTES],lane[EFS_META_LANE_BYTES]={0};uint32_t kn;
            CHECK(!efs_meta_apply_get_inode(kv,ids[i],&row));row.active_lanes=1;
            CHECK(!efs_kv_key_inode(efs_kv_inode_shard(ids[i]),ids[i],key,&kn));
            CHECK(!efs_meta_pack_inode(&row,value,sizeof(value)));CHECK(!efs_kv_put(kv,key,kn,value,sizeof(value)));
            /* Canonical committed lane: EOF 8192, seq 1, one allocated chunk. */
            lane[6]=32;lane[31]=1;lane[63]=1;
            CHECK(!efs_kv_key_lane(efs_kv_lane_shard(ids[i],0),ids[i],row.generation,0,key,&kn));
            CHECK(!efs_kv_put(kv,key,kn,lane,sizeof(lane)));
        }
    }
    struct efs_kv_lsm_snapshot *source=NULL;
    struct efs_query_catalog *old=NULL,*next=NULL,*oracle=NULL;
    CHECK(!efs_kv_lsm_snapshot_pin(kv,16<<20,&source));
    CHECK(!efs_query_catalog_build_disk(cache,source,NULL,4<<20,NULL,NULL,&old));
    efs_kv_lsm_snapshot_release(source);source=NULL;
    const unsigned counts[]={1023,1024,1025,4096};
    for(unsigned round=0;old && round<4;round++){
        for(unsigned i=0;i<counts[round];i++){
            struct efs_meta_row row;uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_INO_BYTES];uint32_t kn;
            CHECK(!efs_meta_apply_get_inode(kv,ids[i],&row));row.base_size=round+1;
            CHECK(!efs_kv_key_inode(efs_kv_inode_shard(ids[i]),ids[i],key,&kn));
            CHECK(!efs_meta_pack_inode(&row,value,sizeof(value)));
            CHECK(!efs_kv_put(kv,key,kn,value,sizeof(value)));
        }
        CHECK(!efs_kv_lsm_snapshot_pin(kv,16<<20,&source));
        CHECK(!efs_query_catalog_build_disk(cache,source,old,4<<20,NULL,NULL,&next));
        CHECK(!efs_query_catalog_build(source,NULL,256<<20,NULL,NULL,&oracle));
        if(next && oracle){
            struct efs_query_build_times t=efs_query_catalog_build_times(next);
            printf("files=%u delta=%u refolded=%u rebuilt=%u stats_ms=%.3f\n",files,counts[round],efs_query_catalog_refolded(next),efs_query_catalog_rebuilt(next),t.stats_ns/1e6);
            if(!getenv("EFS_QUERY_DELTA_BASELINE"))CHECK(efs_query_catalog_refolded(next)==counts[round]);
            for(unsigned i=0;i<files;i++){
                struct efs_meta_row a,b;struct efs_meta_stat x,y;
                CHECK(!efs_query_catalog_inode(next,ids[i],&a,&x));
                CHECK(!efs_query_catalog_inode(oracle,ids[i],&b,&y));
                CHECK(!memcmp(&a,&b,sizeof(a)) && !memcmp(&x,&y,sizeof(x)));
            }
            struct efs_query_rollup a,b;
            CHECK(!efs_query_catalog_stats(next,dir,&a));CHECK(!efs_query_catalog_stats(oracle,dir,&b));CHECK(!memcmp(&a,&b,sizeof(a)));
            CHECK(!efs_query_catalog_stats(next,EFS_ROOT_INO,&a));CHECK(!efs_query_catalog_stats(oracle,EFS_ROOT_INO,&b));CHECK(!memcmp(&a,&b,sizeof(a)));
        }
        efs_query_catalog_free(oracle);oracle=NULL;efs_query_catalog_free(old);old=next;next=NULL;
        efs_kv_lsm_snapshot_release(source);source=NULL;
    }
    if(old && getenv("EFS_QUERY_DELTA_LANES")){
        for(unsigned phase=0;phase<2;phase++){
            efs_ino_t target=ids[files-1-phase];struct efs_meta_row row;
            uint8_t key[EFS_KV_KEY_MAX],lane[EFS_META_LANE_BYTES]={0};uint32_t kn;
            CHECK(!efs_meta_apply_get_inode(kv,target,&row));
            CHECK(!efs_kv_key_lane(efs_kv_lane_shard(target,0),target,row.generation,0,key,&kn));
            if(!phase){lane[6]=64;lane[14]=1;lane[15]=44;lane[31]=2;lane[63]=2;CHECK(!efs_kv_put(kv,key,kn,lane,sizeof(lane)));}
            else CHECK(!efs_kv_del(kv,key,kn));
            CHECK(!efs_kv_lsm_snapshot_pin(kv,16<<20,&source));
            CHECK(!efs_query_catalog_build_disk(cache,source,old,4<<20,NULL,NULL,&next));
            CHECK(!efs_query_catalog_build(source,NULL,256<<20,NULL,NULL,&oracle));
            if(next && oracle){
                struct efs_meta_stat a,b;struct efs_query_rollup x,y;
                CHECK(!efs_query_catalog_inode(next,target,NULL,&a));CHECK(!efs_query_catalog_inode(oracle,target,NULL,&b));CHECK(!memcmp(&a,&b,sizeof(a)));
                CHECK(a.size==(phase?0:16384) && a.alloc==(phase?0:2));
                if(!getenv("EFS_QUERY_DELTA_BASELINE"))CHECK(efs_query_catalog_refolded(next)==1);
                CHECK(!efs_query_catalog_stats(next,dir,&x));CHECK(!efs_query_catalog_stats(oracle,dir,&y));CHECK(!memcmp(&x,&y,sizeof(x)));
                printf("lane-only=%s refolded=%u size=%llu alloc=%llu\n",phase?"absent":"changed",efs_query_catalog_refolded(next),(unsigned long long)a.size,(unsigned long long)a.alloc);
            }
            efs_query_catalog_free(oracle);oracle=NULL;efs_query_catalog_free(old);old=next;next=NULL;
            efs_kv_lsm_snapshot_release(source);source=NULL;
        }
    }
    /* Deleted old IDs and inserted candidate IDs must invalidate parents
     * without assuming that the two sorted inode streams align by ordinal. */
    for(unsigned phase=0;old && phase<2;phase++){
        for(unsigned i=0;i<2048;i++){
            char name[32];
            if(!phase){efs_ino_t added;snprintf(name,sizeof(name),"added-%04u",i);
                CHECK(!efs_meta_apply_create_file(kv,&attrs,dir,S_IFREG|0644,name,&added));}
            else {snprintf(name,sizeof(name),"file-%04u",i);CHECK(!efs_meta_apply_unlink(kv,dir,name,200));}
        }
        CHECK(!efs_kv_lsm_snapshot_pin(kv,16<<20,&source));
        CHECK(!efs_query_catalog_build_disk(cache,source,old,4<<20,NULL,NULL,&next));
        CHECK(!efs_query_catalog_build(source,NULL,256<<20,NULL,NULL,&oracle));
        if(next && oracle){
            struct efs_query_rollup a,b;
            CHECK(!efs_query_catalog_stats(next,dir,&a));CHECK(!efs_query_catalog_stats(oracle,dir,&b));CHECK(!memcmp(&a,&b,sizeof(a)));
            CHECK(a.tree_files==files+(phase?0:2048));
            if(!getenv("EFS_QUERY_DELTA_BASELINE"))CHECK(efs_query_catalog_refolded(next)<files);
            printf("phase=%s refolded=%u tree_files=%llu\n",phase?"delete":"create",efs_query_catalog_refolded(next),(unsigned long long)a.tree_files);
        }
        efs_query_catalog_free(oracle);oracle=NULL;efs_query_catalog_free(old);old=next;next=NULL;
        efs_kv_lsm_snapshot_release(source);source=NULL;
    }
    if(old && getenv("EFS_QUERY_DELTA_DIRS")){
        /* Distinct changed directories need their common root as well.
         * Preserve the documented resource fallback and prove its result
         * still agrees with the full RAM oracle on both sides of the bound. */
        efs_ino_t dirs[1025];
        for(unsigned i=0;i<1025;i++){
            char name[32];snprintf(name,sizeof(name),"dir-%04u",i);
            CHECK(!efs_meta_apply_mkdir(kv,&attrs,EFS_ROOT_INO,S_IFDIR|0755,name,&dirs[i]));
        }
        CHECK(!efs_kv_lsm_snapshot_pin(kv,16<<20,&source));
        CHECK(!efs_query_catalog_build_disk(cache,source,old,4<<20,NULL,NULL,&next));
        efs_kv_lsm_snapshot_release(source);source=NULL;
        efs_query_catalog_free(old);old=next;next=NULL;
        for(unsigned round=0;old && round<3;round++){
            unsigned count=1023+round;
            for(unsigned i=0;i<count;i++){
                struct efs_meta_row row;uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_INO_BYTES];uint32_t kn;
                CHECK(!efs_meta_apply_get_inode(kv,dirs[i],&row));row.base_ctime=300+round;
                CHECK(!efs_kv_key_inode(efs_kv_inode_shard(dirs[i]),dirs[i],key,&kn));
                CHECK(!efs_meta_pack_inode(&row,value,sizeof(value)));
                CHECK(!efs_kv_put(kv,key,kn,value,sizeof(value)));
            }
            CHECK(!efs_kv_lsm_snapshot_pin(kv,16<<20,&source));
            CHECK(!efs_query_catalog_build_disk(cache,source,old,4<<20,NULL,NULL,&next));
            CHECK(!efs_query_catalog_build(source,NULL,256<<20,NULL,NULL,&oracle));
            if(next && oracle){
                unsigned refolded=efs_query_catalog_refolded(next);
                CHECK(refolded==(round?files+1027:count));
                for(unsigned i=0;i<1025;i++){
                    struct efs_meta_stat a,b;struct efs_query_rollup x,y;
                    CHECK(!efs_query_catalog_inode(next,dirs[i],NULL,&a));CHECK(!efs_query_catalog_inode(oracle,dirs[i],NULL,&b));CHECK(!memcmp(&a,&b,sizeof(a)));
                    CHECK(!efs_query_catalog_stats(next,dirs[i],&x));CHECK(!efs_query_catalog_stats(oracle,dirs[i],&y));CHECK(!memcmp(&x,&y,sizeof(x)));
                }
                struct efs_query_rollup a,b;
                CHECK(!efs_query_catalog_stats(next,EFS_ROOT_INO,&a));CHECK(!efs_query_catalog_stats(oracle,EFS_ROOT_INO,&b));CHECK(!memcmp(&a,&b,sizeof(a)));
                printf("dirty-directory delta=%u closure=%u refolded=%u fallback=%s\n",count,count+1,refolded,round?"budget":"none");
            }
            efs_query_catalog_free(oracle);oracle=NULL;efs_query_catalog_free(old);old=next;next=NULL;
            efs_kv_lsm_snapshot_release(source);source=NULL;
        }
    }
    efs_query_catalog_free(old);efs_kv_lsm_close(kv);free(ids);
    /* Keep failing fixtures for diagnosis, but successful repeated oracles
     * must not consume the seed's tmpfs indefinitely. These are our mkdtemp
     * roots; physical traversal never follows a symlink outside them. */
    if(!failures){CHECK(!nftw(cache,clean_fixture,16,FTW_DEPTH|FTW_PHYS));CHECK(!nftw(store,clean_fixture,16,FTW_DEPTH|FTW_PHYS));}
    printf("query large delta: %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
