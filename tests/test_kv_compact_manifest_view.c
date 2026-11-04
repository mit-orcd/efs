#define _GNU_SOURCE
/* W123/W126: compaction keeps writer ownership and every durability barrier,
 * but readers/capacity peeks progress while MANIFEST publication is in I/O.
 * Real SST/WAL/manifest files; only the selected fsync is held or fails. */
#include "efs/kv.h"
#include "efs/kv_lsm.h"
#include "../src/kv/kv_lsm_internal.h"
#include "test_tmpdir.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
static int failures;
#define CHECK(x,m) do {if(!(x)){fprintf(stderr,"FAIL: %s\n",m);failures++;}} while(0)
static struct {
 pthread_mutex_t mu;pthread_cond_t cv;
 struct efs_kv *kv;char dir[EFS_TEST_PATH];
 int mode,entered,release,done[2],rc[2],compact_rc,sst_synced,manifest_synced;
} g={.mu=PTHREAD_MUTEX_INITIALIZER,.cv=PTHREAD_COND_INITIALIZER};
static __thread int compacting;
int __real_fsync(int);
int __wrap_fsync(int fd)
{
 if(!compacting)return __real_fsync(fd);
 char link[64],path[EFS_TEST_PATH+128];snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
 ssize_t n=readlink(link,path,sizeof(path)-1);if(n<0)return __real_fsync(fd);path[n]=0;
 char manifest[EFS_TEST_PATH+32];snprintf(manifest,sizeof(manifest),"%s/MANIFEST.tmp",g.dir);
 int isdir=!strcmp(path,g.dir),ism=!strcmp(path,manifest);
 if((g.mode==2&&ism)||(g.mode!=2&&isdir)) {
  pthread_mutex_lock(&g.mu);g.entered=1;pthread_cond_broadcast(&g.cv);
  while(!g.release)pthread_cond_wait(&g.cv,&g.mu);
  pthread_mutex_unlock(&g.mu);
  if(g.mode){errno=EIO;return -1;}
 }
 int rc=__real_fsync(fd);
 if(!rc){if(ism)g.manifest_synced++;else if(strstr(path,".sst.tmp"))g.sst_synced++;}
 return rc;
}
static const uint8_t keys[5][4]={{0,1,7,'a'},{0,1,7,'b'},{0,1,7,'c'},{0,1,7,'d'},{0,1,7,'e'}};
static int mapping(struct efs_kv *kv,int extra)
{
 static const uint8_t values[5]={2,0,3,4,5};
 for(unsigned i=0;i<5;i++) {
  uint8_t v=0;uint32_t n=1;int rc=efs_kv_get(kv,keys[i],4,&v,&n);
  if(i==1||(i==4&&!extra)){if(rc!=EFS_ERR_NOT_FOUND)return 1;}
  else if(rc||n!=1||v!=values[i])return 1;
 }
 return 0;
}
static int scan_count(void *arg,const uint8_t *k,uint32_t kn,const uint8_t *v,uint32_t vn)
{
 unsigned *n=arg;if(kn!=4||vn!=1||k[3]=='b'||k[3]=='e'||!*v)return EFS_ERR_PROTO;(*n)++;return 0;
}
static void *probe(void *arg)
{
 unsigned i=*(unsigned *)arg;int rc;
 if(i)rc=efs_kv_lsm_apply_room(g.kv,1);
 else {unsigned n=0;rc=mapping(g.kv,0);if(!rc)rc=efs_kv_scan(g.kv,scan_count,&n);if(!rc&&n!=3)rc=EFS_ERR_PROTO;}
 pthread_mutex_lock(&g.mu);g.rc[i]=rc;g.done[i]=1;pthread_cond_broadcast(&g.cv);pthread_mutex_unlock(&g.mu);return NULL;
}
static void *compact(void *unused)
{(void)unused;compacting=1;g.compact_rc=efs_kv_lsm_compact(g.kv);compacting=0;return NULL;}
static void run(int mode)
{
 g.mode=mode;g.entered=g.release=g.done[0]=g.done[1]=0;g.sst_synced=g.manifest_synced=0;
 efs_test_path(g.dir,sizeof(g.dir),"efs-compact-view-XXXXXX");assert(efs_test_mkdtemp(g.dir));
 struct efs_kv_lsm_cfg cfg={.sync_mode=EFS_KV_LSM_SYNC,.memtable_max=1u<<20,.l0_max=64};
 g.kv=efs_kv_lsm_open(g.dir,&cfg);assert(g.kv);
 uint8_t v=1;assert(!efs_kv_put(g.kv,keys[0],4,&v,1));assert(!efs_kv_put(g.kv,keys[1],4,&v,1));
 v=3;assert(!efs_kv_put(g.kv,keys[2],4,&v,1));assert(!efs_kv_lsm_flush(g.kv));
 v=2;assert(!efs_kv_put(g.kv,keys[0],4,&v,1));assert(!efs_kv_del(g.kv,keys[1],4));
 v=4;assert(!efs_kv_put(g.kv,keys[3],4,&v,1));assert(!efs_kv_lsm_flush(g.kv));
 assert(!mapping(g.kv,0));
 char manifest[EFS_TEST_PATH+32],saved[4096];snprintf(manifest,sizeof(manifest),"%s/MANIFEST",g.dir);
 FILE *f=fopen(manifest,"rb");assert(f);size_t saved_n=fread(saved,1,sizeof(saved),f);assert(saved_n&&saved_n<sizeof(saved)&&!ferror(f));fclose(f);
 pthread_t worker,readers[2];unsigned ids[2]={0,1};assert(!pthread_create(&worker,NULL,compact,NULL));
 pthread_mutex_lock(&g.mu);while(!g.entered)pthread_cond_wait(&g.cv,&g.mu);pthread_mutex_unlock(&g.mu);
 CHECK(g.sst_synced>0,"compaction published before SST file fsync");
 CHECK(mode==2||g.manifest_synced>0,"directory barrier preceded MANIFEST file fsync");
 int held=pthread_mutex_trylock(&((struct kv_lsm *)g.kv->ctx)->mu);
 if(!held)pthread_mutex_unlock(&((struct kv_lsm *)g.kv->ctx)->mu);
 CHECK(held==EBUSY,"compaction released writer ownership before publication finished");
 for(unsigned i=0;i<2;i++)assert(!pthread_create(readers+i,NULL,probe,ids+i));
 struct timespec end;clock_gettime(CLOCK_REALTIME,&end);end.tv_nsec+=500000000;
 if(end.tv_nsec>=1000000000){end.tv_nsec-=1000000000;end.tv_sec++;}
 pthread_mutex_lock(&g.mu);
 while(!g.done[0]||!g.done[1]){int rc=pthread_cond_timedwait(&g.cv,&g.mu,&end);if(rc==ETIMEDOUT)break;assert(!rc);}
 int early[2]={g.done[0],g.done[1]};g.release=1;pthread_cond_broadcast(&g.cv);pthread_mutex_unlock(&g.mu);
 for(unsigned i=0;i<2;i++){assert(!pthread_join(readers[i],NULL));}
 assert(!pthread_join(worker,NULL));
 CHECK(early[0],"GET/scan waited for MANIFEST durability I/O");
 CHECK(early[1],"capacity waited for MANIFEST durability I/O");
 CHECK(!g.rc[0]&&!g.rc[1],"equivalent published mapping or pressure changed");
 CHECK(g.compact_rc==(mode?EFS_ERR_IO:EFS_OK),"barrier error was ignored");
 v=5;int wr=efs_kv_put(g.kv,keys[4],4,&v,1);
 CHECK(wr==(mode?EFS_ERR_IO:EFS_OK),"failed engine allowed mutation, or successful engine refused");
 efs_kv_lsm_close(g.kv);
 if(mode) {
  /* Deterministic lost-rename model: old MANIFEST still needs every old SST.
   * The engine is stopped, and these are this test's own current-format files. */
  f=fopen(manifest,"wb");assert(f);assert(fwrite(saved,1,saved_n,f)==saved_n);assert(!fflush(f)&&!fsync(fileno(f)));assert(!fclose(f));
 }
 g.kv=efs_kv_lsm_open(g.dir,&cfg);assert(g.kv);CHECK(!mapping(g.kv,!mode),"cold mapping/tombstone or retained old inputs lost");efs_kv_lsm_close(g.kv);
}
int main(void)
{
 alarm(30);for(int mode=0;mode<3;mode++)run(mode);
 if(failures)return 1;
 puts("KV compaction publication PASS: readers/peeks progress, writer retained, both barrier failures and cold mappings");return 0;
}
