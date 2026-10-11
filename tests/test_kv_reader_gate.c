#include <assert.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned fail_file,fail_directory,file_syncs,directory_syncs;
static int fault_sync(int fd)
{
    struct stat st;assert(!fstat(fd,&st));
    if(S_ISREG(st.st_mode)) {
        file_syncs++;if(fail_file){fail_file--;errno=EIO;return -1;}
    } else if(S_ISDIR(st.st_mode)) {
        directory_syncs++;if(fail_directory){fail_directory--;errno=EIO;return -1;}
    }
    return fsync(fd);
}
#define fsync fault_sync
#include "../src/kv/kv_lsm.c"
#undef fsync
static void header(const char *dir,char out[128])
{
    char path[8192];snprintf(path,sizeof(path),"%s/MANIFEST",dir);
    FILE *f=fopen(path,"r");assert(f && fgets(out,128,f));assert(!fclose(f));
}
static void replace(const char *dir,const char *text)
{
    char path[8192];snprintf(path,sizeof(path),"%s/MANIFEST",dir);
    FILE *f=fopen(path,"w");assert(f && fputs(text,f)>=0);assert(!fclose(f));
}
static int cleanup(const char *path,const struct stat *st,int kind,struct FTW *f)
{(void)st;(void)f;return kind==FTW_DP?rmdir(path):unlink(path);}
int main(void)
{
    char dir[]="/tmp/efs-kv-reader-gate-XXXXXX",line[128];assert(mkdtemp(dir));
    struct efs_kv *kv=efs_kv_lsm_open(dir,NULL);assert(kv);
    assert(efs_kv_lsm_require_reader(kv,0)==EFS_ERR_INVAL);
    assert(efs_kv_lsm_require_reader(kv,2)==EFS_ERR_INVAL);
    struct efs_kv *mem=efs_kv_mem_create();assert(mem);
    assert(efs_kv_lsm_require_reader(mem,1)==EFS_ERR_INVAL);efs_kv_mem_free(mem);
    const uint8_t key[]={0,1,250},value[]={41};
    assert(!efs_kv_put(kv,key,sizeof(key),value,sizeof(value)));assert(!efs_kv_lsm_flush(kv));
    header(dir,line);unsigned long long seq;
    assert(sscanf(line,"KVLSM1 %llu",&seq)==1);
    fail_file=1;assert(efs_kv_lsm_require_reader(kv,1)==EFS_ERR_IO);
    header(dir,line);assert(sscanf(line,"KVLSM1 %llu",&seq)==1);
    /* First directory sync is the historical generic best-effort barrier;
     * the minimum-reader API checks and repeats it explicitly. */
    fail_directory=2;assert(efs_kv_lsm_require_reader(kv,1)==EFS_ERR_IO);
    header(dir,line);assert(sscanf(line,"KVLSM1 %llu",&seq)==0);
    file_syncs=directory_syncs=0;assert(!efs_kv_lsm_require_reader(kv,1));
    assert(file_syncs && directory_syncs>=2);
    unsigned proven_files=file_syncs,proven_dirs=directory_syncs;
    assert(!efs_kv_lsm_require_reader(kv,1));
    assert(file_syncs==proven_files && directory_syncs==proven_dirs);
    /* A serialized apply owner can persist the capability barrier even if
     * its later metadata batch aborts. No deadlock on the writer gate. */
    assert(!efs_kv_lsm_apply_begin(kv));assert(!efs_kv_lsm_require_reader(kv,1));
    assert(!efs_kv_lsm_apply_end(kv,NULL));
    assert(!efs_kv_lsm_compact(kv));efs_kv_lsm_close(kv);
    kv=efs_kv_lsm_open(dir,NULL);assert(kv);uint8_t got[1];uint32_t bytes=sizeof(got);
    assert(!efs_kv_get(kv,key,sizeof(key),got,&bytes) && got[0]==41);
    assert(!efs_kv_lsm_require_reader(kv,1));header(dir,line);
    assert(sscanf(line,"KVLSM1 %llu",&seq)==0);efs_kv_lsm_close(kv);
    pid_t pid=fork();assert(pid>=0);
    if(!pid){kv=efs_kv_lsm_open(dir,NULL);assert(kv);assert(!efs_kv_lsm_require_reader(kv,1));_exit(0);}
    int status;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    kv=efs_kv_lsm_open(dir,NULL);assert(kv);efs_kv_lsm_close(kv);
    /* Private fixture rewrites only. Unknown versions/capabilities and a
     * wrapped unsigned parse must refuse open, never degrade to empty state. */
    const char *bad[]={"KVLSM3 1 1\n","KVLSM2 1 2\n","KVLSM2 1 0\n",
        "KVLSM2 1 4294967297\n","KVLSM2 1 1 extra\n","KVLSM2 1 -1\n"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        replace(dir,bad[i]);assert(!efs_kv_lsm_open(dir,NULL));
    }
    assert(!nftw(dir,cleanup,32,FTW_DEPTH|FTW_PHYS));
    puts("minimum reader: legacy preservation, durable retries, apply abort, cold recovery and unknown-format refusal PASS");
}
