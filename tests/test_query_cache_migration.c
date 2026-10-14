/* Cross-build oracle: compile against each tree's libefs.a, then run
 * OLD_EXE create ROOT and NEW_EXE migrate ROOT. ROOT must be a new directory.
 * This uses actual v6 files, not a header retagged over newer records. */
#include "../src/meta/query_disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);} }while(0)
static unsigned views(const char *root)
{
    DIR *d=opendir(root);REQUIRE(d);unsigned n=0;struct dirent *e;
    while((e=readdir(d)))n+=!strncmp(e->d_name,"view-",5);
    closedir(d);return n;
}
struct names { unsigned count;uint64_t hash; };
static int collect(void *ctx,const struct efs_name_entry *entry,const char *path)
{
    struct names *n=ctx;n->count++;
    for(const char *p=path;*p;p++)n->hash=n->hash*33+(unsigned char)*p;
    n->hash=n->hash*33+entry->dentry.ino;return 0;
}
static struct names verify(struct efs_query_disk *d)
{
    struct efs_query_rollup total;REQUIRE(!efs_query_disk_stats(d,EFS_ROOT_INO,&total));
    REQUIRE(total.tree_files==100 && total.tree_dirs==1);
    struct names n={0};uint64_t cursor=0;
    do {REQUIRE(!efs_query_disk_page(d,EFS_ROOT_INO,EFS_NAME_PREFIX,"",0,&cursor,7,NULL,NULL,collect,&n));}
    while(cursor!=(UINT64_C(4096)<<32));
    REQUIRE(n.count==101);return n;
}
int main(int argc,char **argv)
{
    REQUIRE(argc==3);int create=!strcmp(argv[1],"create");REQUIRE(create || !strcmp(argv[1],"migrate"));
    char store[4096],cache[4096],oracle[4096],current[4096];
    REQUIRE(snprintf(store,sizeof(store),"%s/kv",argv[2])<(int)sizeof(store));
    REQUIRE(snprintf(cache,sizeof(cache),"%s/cache",argv[2])<(int)sizeof(cache));
    REQUIRE(snprintf(oracle,sizeof(oracle),"%s/oracle",argv[2])<(int)sizeof(oracle));
    REQUIRE(snprintf(current,sizeof(current),"%s/current",cache)<(int)sizeof(current));
    if(create){REQUIRE(!mkdir(store,0700));REQUIRE(!mkdir(cache,0700));}
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_SYNC,0,4};struct efs_kv *kv=efs_kv_lsm_open(store,&cfg);REQUIRE(kv);
    uint64_t wm[4]={1,2,3,4};struct efs_query_disk *d=NULL;struct efs_kv_lsm_snapshot *source=NULL;
    if(create){
        struct efs_meta_attrs attrs={1000,1000,100};efs_ino_t dir,file;
        REQUIRE(!efs_meta_apply_init(kv,100));REQUIRE(!efs_meta_apply_mkdir(kv,&attrs,EFS_ROOT_INO,S_IFDIR|0755,"nested",&dir));
        for(unsigned i=0;i<100;i++){
            char name[EFS_MAX_NAME];memset(name,'x',sizeof(name));
            snprintf(name,16,"name-%08u-",i);name[strlen(name)]='x';name[EFS_MAX_NAME-1]=0;
            REQUIRE(!efs_meta_apply_create_file(kv,&attrs,i&1?dir:EFS_ROOT_INO,S_IFREG|0644,name,&file));
        }
    }else{
        char target[256];ssize_t len=readlink(current,target,sizeof(target)-1);REQUIRE(len>0);target[len]=0;
        char manifest[4096];REQUIRE(snprintf(manifest,sizeof(manifest),"%s/%s/manifest",cache,target)<(int)sizeof(manifest));
        int fd=open(manifest,O_RDONLY);uint64_t version=0;REQUIRE(fd>=0 && pread(fd,&version,8,8)==8);close(fd);REQUIRE(version==6);
        REQUIRE(efs_query_disk_restore(cache,wm,8<<20,&d)==EFS_ERR_PROTO && !d);
        REQUIRE(views(cache)==1);char after[256];ssize_t n=readlink(current,after,sizeof(after)-1);REQUIRE(n==len && !memcmp(after,target,(size_t)len));
    }
    REQUIRE(!efs_kv_lsm_snapshot_pin(kv,16<<20,&source));
    REQUIRE(!efs_query_disk_build(cache,source,NULL,8<<20,NULL,NULL,NULL,&d));
    struct names actual=verify(d);
    if(create){FILE *f=fopen(oracle,"w");REQUIRE(f);REQUIRE(fprintf(f,"%u %llu\n",actual.count,(unsigned long long)actual.hash)>0);REQUIRE(!fclose(f));}
    else {unsigned count;unsigned long long hash;FILE *f=fopen(oracle,"r");REQUIRE(f);REQUIRE(fscanf(f,"%u %llu",&count,&hash)==2);fclose(f);REQUIRE(count==actual.count && hash==actual.hash);REQUIRE(views(cache)==2);}
    REQUIRE(!efs_query_disk_checkpoint(d,wm));REQUIRE(views(cache)==1);
    efs_query_disk_free(d);d=NULL;REQUIRE(!efs_query_disk_restore(cache,wm,8<<20,&d));
    struct names restored=verify(d);REQUIRE(restored.count==actual.count && restored.hash==actual.hash);
    efs_query_disk_free(d);efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(kv);
    puts(create?"actual old-build checkpoint: PASS":"actual v6 to v7 cold migration, long names, cursor oracle and retirement: PASS");return 0;
}
