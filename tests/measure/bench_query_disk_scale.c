/* Offline synthetic metadata, not real filesystem/data capacity acceptance.
 * Real production KV/catalog formats; bounded generator and streaming oracle.
 * Never deletes a fixture. Generation needs a fresh private root; explicit
 * --populate-only stages it, --resume-build reuses a known complete unmutated
 * source, and --recover verifies the postmutation source in a fresh process.
 * The caller owns the source exclusively and retains its population verdict.
 * Run (Linux): make tests/measure/bench_query_disk_scale, then
 *   tests/measure/bench_query_disk_scale ROOT FILES [--populate-only|--resume-build|--recover]
 * ROOT must not exist unless --resume-build or --recover; exit 2 = usage. */
#include "efs/query_catalog.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define BATCH 256
#define SHARDS 4096
struct batch {
    struct efs_kv *kv;unsigned count;
    struct efs_kv_item items[BATCH];
    uint8_t key[BATCH][EFS_KV_KEY_MAX],val[BATCH][EFS_META_INO_BYTES];
};
static double clock_s(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static int flush(struct batch *b)
{ int rc=b->count?efs_kv_batch(b->kv,b->items,b->count):0;if(!rc)b->count=0;return rc; }
static int inode(struct batch *b,efs_ino_t ino,efs_ino_t parent,int dir,uint64_t n)
{
    unsigned i=b->count;uint32_t kl;
    struct efs_meta_row r={.ino=ino,.parent=parent,.generation=1,.content_epoch=1,
        .mode=(dir?S_IFDIR|0755:S_IFREG|0644),.nlink=dir?(ino==EFS_ROOT_INO?(uint32_t)(n+2):2):1,.uid=1000,.gid=1000,
        .nents=dir?(uint32_t)n:0,.base_size=dir?0:n,.base_mtime=100,.base_ctime=100,.base_atime=100};
    int rc=efs_kv_key_inode(efs_kv_inode_shard(ino),ino,b->key[i],&kl);
    if(!rc)rc=efs_meta_pack_inode(&r,b->val[i],sizeof(b->val[i]));
    if(rc)return rc;
    b->items[i]=(struct efs_kv_item){EFS_KV_PUT,b->key[i],kl,b->val[i],EFS_META_INO_BYTES};
    return ++b->count==BATCH?flush(b):0;
}
static int name(struct batch *b,efs_ino_t parent,const char *name,efs_ino_t ino,int dir)
{
    unsigned i=b->count;uint32_t kl;
    struct efs_meta_dentry d={ino,1,dir?S_IFDIR:S_IFREG};
    int rc=efs_kv_key_dentry(efs_kv_inode_shard(parent),parent,name,b->key[i],&kl);
    if(!rc)rc=efs_meta_pack_dentry(&d,b->val[i],sizeof(b->val[i]));
    if(rc)return rc;
    b->items[i]=(struct efs_kv_item){EFS_KV_PUT,b->key[i],kl,b->val[i],EFS_META_DENT_BYTES};
    return ++b->count==BATCH?flush(b):0;
}
static efs_ino_t file_ino(uint64_t g){return ((g/SHARDS+2)<<12)|(g%SHARDS);}
static uint64_t population(uint64_t n,unsigned s){return n<=s?0:(n-1-s)/SHARDS+1;}
struct oracle { const struct efs_query_catalog *catalog;uint64_t n,count,seen[SHARDS],samples;unsigned changed; };
static int emit(void *ctx,const struct efs_name_entry *e,const char *path)
{
    struct oracle *o=ctx;unsigned shard;unsigned long long index;int length=0;
    if(sscanf(path,"dir-%4x/file-%16llx%n",&shard,&index,&length)!=2 || path[length] || shard>=SHARDS)return EFS_ERR_PROTO;
    uint64_t g=index;
    if(g>=o->n || g%SHARDS!=shard || g!=o->seen[shard]*SHARDS+shard ||
       e->parent!=SHARDS+shard || e->dentry.ino!=file_ino(g) || e->dentry.generation!=1)return EFS_ERR_PROTO;
    o->seen[shard]++;o->count++;
    if(o->count%1000000==0){printf("{\"phase\":\"export-progress\",\"count\":%llu}\n",(unsigned long long)o->count);fflush(stdout);}
    if(g%8191==0 || g+1==o->n){
        struct efs_meta_row row;struct efs_meta_stat st;
        int rc=efs_query_catalog_inode(o->catalog,file_ino(g),&row,&st);
        if(rc)return rc;
        if(row.parent!=SHARDS+shard || row.generation!=1 ||
           row.mode!=(S_IFREG|0644) || st.ino!=file_ino(g) || st.generation!=1 ||
           st.mode!=(S_IFREG|0644) || st.nlink!=1 || st.uid!=1000 || st.gid!=1000 ||
           st.mtime!=100 || st.ctime!=100 || st.atime!=100 || st.alloc || st.lanes ||
           st.size!=((g&511)+(g==0?o->changed:0)))return EFS_ERR_PROTO;
        o->samples++;
    }
    return 0;
}
static int check(const struct efs_query_catalog *c,uint64_t n,int stream,uint64_t *samples,unsigned changed)
{
    uint64_t dirs=n<SHARDS?n:SHARDS,tail=n%512;
    struct efs_query_rollup r;int rc=efs_query_catalog_stats(c,EFS_ROOT_INO,&r);
    uint64_t bytes=(n/512)*UINT64_C(130816)+tail*(tail?tail-1:0)/2+changed;
    if(rc)return rc;
    if(r.tree_files!=n || r.tree_dirs!=dirs || r.tree_bytes!=bytes ||
       r.imm_dirs!=dirs || r.imm_files || r.imm_bytes ||
       r.imm_tmin!=100 || r.imm_tmax!=100 || r.tree_tmin!=100 || r.tree_tmax!=100)return EFS_ERR_PROTO;
    for(unsigned s=0;s<dirs;s++){
        rc=efs_query_catalog_stats(c,SHARDS+s,&r);if(rc)return rc;
        uint64_t count=population(n,s);
        uint64_t expected_bytes=count*(s&511)+(s==0?changed:0);
        if(r.imm_files!=count || r.tree_files!=count || r.imm_dirs || r.tree_dirs ||
           r.imm_bytes!=expected_bytes || r.tree_bytes!=expected_bytes ||
           r.imm_tmin!=100 || r.imm_tmax!=100 || r.tree_tmin!=100 || r.tree_tmax!=100)return EFS_ERR_PROTO;
    }
    if(stream){
        struct oracle o={.catalog=c,.n=n,.changed=changed};uint64_t cursor=0;
        do{
            uint64_t before=cursor;
            rc=efs_query_catalog_page(c,EFS_ROOT_INO,EFS_NAME_PREFIX,"file-",0,&cursor,4096,NULL,NULL,emit,&o);
            if(rc)return rc;
            if(cursor==before && cursor!=(UINT64_C(4096)<<32))return EFS_ERR_PROTO;
        }while(cursor!=(UINT64_C(4096)<<32));
        if(o.count!=n)return EFS_ERR_PROTO;
        for(unsigned s=0;s<SHARDS;s++)if(o.seen[s]!=population(n,s))return EFS_ERR_PROTO;
        *samples=o.samples;
    }
    return 0;
}
/* Advisory eviction is restricted to this private fixture. It never drops
 * host/global caches. Filesystems may retain pages: report it as a cold hint,
 * separately from the mandatory close/reopen correctness oracle. */
static int cold_hint(const char *path,unsigned depth,uint64_t *files)
{
    if(depth>4)return EFS_ERR_INVAL;
    DIR *dir=opendir(path);if(!dir)return EFS_ERR_IO;
    struct dirent *entry;int rc=0;
    while(!rc && (entry=readdir(dir))){
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        char name[EFS_MAX_PATH];struct stat st;
        if(snprintf(name,sizeof(name),"%s/%s",path,entry->d_name)>=(int)sizeof(name)){rc=EFS_ERR_INVAL;break;}
        if(lstat(name,&st)){rc=EFS_ERR_IO;break;}
        if(S_ISDIR(st.st_mode))rc=cold_hint(name,depth+1,files);
        else if(S_ISREG(st.st_mode)){
            int fd=open(name,O_RDONLY|O_NOFOLLOW);if(fd<0){rc=EFS_ERR_IO;break;}
            if(posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED))rc=EFS_ERR_IO;
            close(fd);(*files)++;
        }else if(!S_ISLNK(st.st_mode))rc=EFS_ERR_INVAL;
    }
    closedir(dir);return rc;
}
static int recover(const char *root,uint64_t n)
{
    char kvpath[EFS_MAX_PATH],cache[EFS_MAX_PATH];uint64_t hinted=0,samples=0;
    if(snprintf(kvpath,sizeof(kvpath),"%s/kv",root)>=(int)sizeof(kvpath) ||
       snprintf(cache,sizeof(cache),"%s/cache",root)>=(int)sizeof(cache))return 2;
    int rc=cold_hint(root,0,&hinted);double start=clock_s();
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,4<<20,4,0};
    struct efs_kv *kv=rc?NULL:efs_kv_lsm_open(kvpath,&cfg);
    struct efs_kv_lsm_snapshot *source=NULL;struct efs_query_catalog *catalog=NULL;
    if(!rc && !kv)rc=EFS_ERR_IO;
    if(!rc)rc=efs_kv_lsm_snapshot_pin(kv,64<<20,&source);
    if(!rc)rc=efs_query_catalog_catchup_disk(cache,source,4<<20,NULL,NULL,&catalog);
    double restored=clock_s()-start;start=clock_s();
    if(!rc)rc=check(catalog,n,1,&samples,1);
    printf("{\"phase\":\"recovery\",\"files\":%llu,\"hinted_files\":%llu,\"seconds\":%.6f,\"verify_seconds\":%.6f,\"rebuilt\":%u,\"builder_bytes\":%llu,\"rc\":%d}\n",
        (unsigned long long)n,(unsigned long long)hinted,restored,clock_s()-start,
        efs_query_catalog_rebuilt(catalog),(unsigned long long)efs_query_catalog_bytes(catalog),rc);
    efs_query_catalog_free(catalog);efs_kv_lsm_snapshot_release(source);if(kv)efs_kv_lsm_close(kv);
    return rc?1:0;
}
int main(int argc,char **argv)
{
    if(argc!=3 && argc!=4)return 2;
    int reuse=argc==4 && !strcmp(argv[3],"--resume-build");
    int populate_only=argc==4 && !strcmp(argv[3],"--populate-only");
    int recovery=argc==4 && !strcmp(argv[3],"--recover");
    if(argc==4 && !reuse && !populate_only && !recovery)return 2;
    char *end;errno=0;uint64_t n=strtoull(argv[2],&end,10);
    if(errno || *end || !n || n>(UINT64_C(1)<<32))return 2;
    if(recovery)return recover(argv[1],n);
    if(!reuse && mkdir(argv[1],0700)){perror("fresh fixture root");return 2;}
    char kvpath[EFS_MAX_PATH],cache[EFS_MAX_PATH];
    if(snprintf(kvpath,sizeof(kvpath),"%s/kv",argv[1])>=(int)sizeof(kvpath) ||
       snprintf(cache,sizeof(cache),"%s/cache",argv[1])>=(int)sizeof(cache))return 2;
    if(reuse) {
        struct stat st;
        if(lstat(argv[1],&st) || !S_ISDIR(st.st_mode) ||
           lstat(kvpath,&st) || !S_ISDIR(st.st_mode))return 2;
    }
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,4<<20,4,0};
    struct batch b={0};b.kv=efs_kv_lsm_open(kvpath,&cfg);if(!b.kv)return 1;
    double start=clock_s();uint64_t dirs=n<SHARDS?n:SHARDS;
    int rc=EFS_OK;
    if(!reuse) {
        rc=inode(&b,EFS_ROOT_INO,0,1,dirs);
        uint64_t completed_files=0;
        for(unsigned s=0;s<dirs && !rc;s++){
            char label[32];snprintf(label,sizeof(label),"dir-%04x",s);
            rc=inode(&b,SHARDS+s,EFS_ROOT_INO,1,population(n,s));
            if(!rc)rc=name(&b,EFS_ROOT_INO,label,SHARDS+s,1);
        }
        if(!rc)rc=flush(&b);
        for(unsigned s=0;s<dirs && !rc;s++){
            uint64_t count=population(n,s);
            for(uint64_t r=0;r<count && !rc;r++){
                uint64_t g=r*SHARDS+s;rc=inode(&b,file_ino(g),SHARDS+s,0,g&511);
            }
            for(uint64_t r=0;r<count && !rc;r++){
                uint64_t g=r*SHARDS+s;char label[32];snprintf(label,sizeof(label),"file-%016llx",(unsigned long long)g);
                rc=name(&b,SHARDS+s,label,file_ino(g),0);
            }
            if(!rc)rc=flush(&b);
            if(!rc)completed_files+=count;
            printf("{\"phase\":\"populate\",\"shard\":%u,\"completed_files\":%llu,\"target_files\":%llu,\"elapsed_s\":%.3f,\"rc\":%d}\n",s,(unsigned long long)completed_files,(unsigned long long)n,clock_s()-start,rc);fflush(stdout);
        }
        if(!rc)rc=efs_kv_lsm_flush(b.kv);
        printf("{\"phase\":\"populated\",\"files\":%llu,\"target_files\":%llu,\"seconds\":%.6f,\"rc\":%d}\n",(unsigned long long)completed_files,(unsigned long long)n,clock_s()-start,rc);fflush(stdout);
    } else {
        /* Explicit reuse of a known complete, unmutated private source. No
         * population success is inferred here: the full export/rollup oracle
         * below still has to prove every entry before any mutation occurs. */
        printf("{\"phase\":\"source_reused\",\"files\":%llu}\n",(unsigned long long)n);fflush(stdout);
    }
    if(populate_only){efs_kv_lsm_close(b.kv);return rc?1:0;}
    struct efs_kv_lsm_snapshot *source=NULL;struct efs_query_catalog *c=NULL,*next=NULL;
    if(!rc)rc=efs_kv_lsm_snapshot_pin(b.kv,64<<20,&source);
    uint64_t samples=0,watermark[4]={1,2,3,4};
    for(unsigned round=0;round<2 && !rc;round++){
        start=clock_s();rc=efs_query_catalog_build_disk(cache,source,c,4<<20,NULL,NULL,&next);
        double elapsed=clock_s()-start;
        double verify_start=clock_s();
        if(!rc)rc=check(next,n,round==0,&samples,0);
        double verify_elapsed=clock_s()-verify_start;
        printf("{\"phase\":\"build\",\"round\":%u,\"files\":%llu,\"seconds\":%.6f,\"builder_bytes\":%llu,\"source_bytes\":%llu,\"verify_seconds\":%.6f,\"samples\":%llu,\"rc\":%d}\n",round,(unsigned long long)n,elapsed,(unsigned long long)efs_query_catalog_bytes(next),(unsigned long long)efs_kv_lsm_snapshot_bytes(source),verify_elapsed,(unsigned long long)samples,rc);fflush(stdout);
        efs_query_catalog_free(c);c=next;next=NULL;
    }
    if(!rc)rc=efs_query_catalog_checkpoint(c,watermark);
    efs_query_catalog_free(c);c=NULL;efs_kv_lsm_snapshot_release(source);source=NULL;efs_kv_lsm_close(b.kv);
    b.kv=efs_kv_lsm_open(kvpath,&cfg);if(!b.kv)rc=EFS_ERR_IO;
    if(!rc)rc=efs_kv_lsm_snapshot_pin(b.kv,64<<20,&source);
    if(!rc)rc=efs_query_catalog_restore_disk(cache,source,watermark,4<<20,&c);
    if(!rc)rc=check(c,n,0,&samples,0);
    printf("{\"phase\":\"cold\",\"files\":%llu,\"rc\":%d}\n",(unsigned long long)n,rc);fflush(stdout);
    /* One inode-only change must update all aliases/ancestors while retaining
     * the old snapshot. Verify every exported entry again after the refresh. */
    if(!rc)rc=inode(&b,file_ino(0),SHARDS,0,1);
    if(!rc)rc=flush(&b);
    efs_kv_lsm_snapshot_release(source);source=NULL;
    if(!rc)rc=efs_kv_lsm_snapshot_pin(b.kv,64<<20,&source);
    start=clock_s();
    if(!rc)rc=efs_query_catalog_build_disk(cache,source,c,4<<20,NULL,NULL,&next);
    double mutation_elapsed=clock_s()-start,verify_start=clock_s();
    if(!rc)rc=check(c,n,0,&samples,0);
    if(!rc)rc=check(next,n,1,&samples,1);
    printf("{\"phase\":\"mutation\",\"files\":%llu,\"seconds\":%.6f,\"verify_seconds\":%.6f,\"builder_bytes\":%llu,\"source_bytes\":%llu,\"rebuilt\":%u,\"refolded\":%u,\"rc\":%d}\n",
        (unsigned long long)n,mutation_elapsed,clock_s()-verify_start,
        (unsigned long long)efs_query_catalog_bytes(next),(unsigned long long)efs_kv_lsm_snapshot_bytes(source),
        efs_query_catalog_rebuilt(next),efs_query_catalog_refolded(next),rc);fflush(stdout);
    efs_query_catalog_free(next);next=NULL;
    efs_query_catalog_free(c);efs_kv_lsm_snapshot_release(source);if(b.kv)efs_kv_lsm_close(b.kv);
    return rc?1:0;
}
