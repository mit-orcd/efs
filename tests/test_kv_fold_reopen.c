/* Deterministic real async fold/newer-flush overlap, then SIGKILL/reopen.
 * Link-wrap iterator scheduling only: no production fault hook or added sync.
 * The child verifies the warm view before dying; the parent verifies that
 * exactly that view survives cold open, including tombstones.
 * Ported from W123 9921b990b; the counter key stands in for its TIP row. */
#include "efs/kv_lsm.h"
#include "../src/kv/kv_lsm_internal.h"
#include "test_tmpdir.h"
#include <assert.h>
#include <dirent.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

static const uint8_t tipkey[]={0,2,EFS_KV_KIND_INODE};
static const uint8_t deadkey[]={0,2,EFS_KV_KIND_INODE,1};
static const uint8_t livekey[]={0,2,EFS_KV_KIND_INODE,2};
static const uint8_t fillerkey[]={0,2,EFS_KV_KIND_INODE,255};
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static int entered,released;
static __thread int fold_owner;
int __real_kv_seg_iter_next(struct kv_seg_iter *,const uint8_t **,uint32_t *,const uint8_t **,uint32_t *,uint8_t *);
static void await(int *flag)
{
    struct timespec end;assert(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=15;
    while(!*flag)assert(!pthread_cond_timedwait(&cv,&gate,&end));
}
int __wrap_kv_seg_iter_next(struct kv_seg_iter *it,const uint8_t **k,uint32_t *kn,const uint8_t **v,uint32_t *vn,uint8_t *op)
{
    if(fold_owner) {
        assert(!pthread_mutex_lock(&gate));
        if(!entered){entered=1;assert(!pthread_cond_broadcast(&cv));await(&released);}
        assert(!pthread_mutex_unlock(&gate));
    }
    return __real_kv_seg_iter_next(it,k,kn,v,vn,op);
}
static void put_tip(struct efs_kv *kv,uint64_t n)
{
    uint8_t value[8];for(unsigned i=0;i<8;i++)value[7-i]=n>>(8*i);
    assert(!efs_kv_put(kv,tipkey,sizeof(tipkey),value,8));
}
static void expect(struct efs_kv *kv)
{
    uint8_t tipv[8];uint32_t n=8;uint64_t tip=0;
    assert(!efs_kv_get(kv,tipkey,sizeof(tipkey),tipv,&n)&&n==8);
    for(unsigned i=0;i<8;i++)tip=tip<<8|tipv[i];
    assert(tip==111);
    uint8_t value=0;n=1;
    assert(efs_kv_get(kv,deadkey,sizeof(deadkey),&value,&n)==EFS_ERR_NOT_FOUND);
    n=1;assert(!efs_kv_get(kv,livekey,sizeof(livekey),&value,&n)&&n==1&&value==9);
}
static void *fold(void *arg)
{
    struct kv_lsm *l=arg;fold_owner=1;
    lsm_lock(l);l->compact_busy=1;
    assert(!kv_compact_locked(l,1));
    l->compact_busy=0;pthread_cond_broadcast(&l->cv);lsm_unlock(l);
    return NULL;
}
static struct efs_kv_lsm_cfg cfg={.memtable_max=16<<20,.l0_max=64};
static void child(const char *dir,int ready)
{
    #ifdef __linux__
    assert(!prctl(PR_SET_PDEATHSIG,SIGKILL));
    #endif
    struct efs_kv *kv=efs_kv_lsm_open(dir,&cfg);assert(kv);
    struct kv_lsm *l=kv->ctx;
    /* Keep only our explicitly scheduled real compactor. */
    kv_compactor_stop(l);
    uint8_t *filler=calloc(1,1<<20),value=1;assert(filler);
    put_tip(kv,106);
    assert(!efs_kv_put(kv,fillerkey,sizeof(fillerkey),filler,1<<20));free(filler);
    assert(!efs_kv_put(kv,deadkey,sizeof(deadkey),&value,1));
    assert(!efs_kv_put(kv,livekey,sizeof(livekey),&value,1));
    assert(!efs_kv_lsm_flush(kv));assert(!efs_kv_lsm_compact(kv));
    put_tip(kv,107);assert(!efs_kv_del(kv,livekey,sizeof(livekey)));
    assert(!efs_kv_lsm_flush(kv));put_tip(kv,108);assert(!efs_kv_lsm_flush(kv));
    lsm_lock(l);assert(l->n_l0==2&&l->n_l1>0);l->cfg.l0_max=2;lsm_unlock(l);
    pthread_t thread;assert(!pthread_create(&thread,NULL,fold,l));
    assert(!pthread_mutex_lock(&gate));await(&entered);assert(!pthread_mutex_unlock(&gate));
    /* Fold inputs are fixed, but its output id is not allocated yet. */
    put_tip(kv,111);assert(!efs_kv_del(kv,deadkey,sizeof(deadkey)));value=9;
    assert(!efs_kv_put(kv,livekey,sizeof(livekey),&value,1));
    assert(!efs_kv_lsm_flush(kv));
    lsm_lock(l);uint64_t flush_id=l->l0[0].seq;lsm_unlock(l);
    assert(!pthread_mutex_lock(&gate));released=1;assert(!pthread_cond_broadcast(&cv));assert(!pthread_mutex_unlock(&gate));
    assert(!pthread_join(thread,NULL));
    lsm_lock(l);
    assert(l->n_l0==2&&l->l0[0].seq==flush_id&&l->l0[1].seq>flush_id);
    lsm_unlock(l);
    expect(kv);fprintf(stderr,"Warm TIP111 and tombstones verified before SIGKILL\n");
    assert(write(ready,"R",1)==1);close(ready);
    for(;;)pause(); /* Parent kills; no engine close, flush or destructor. */
}
static int scan(void *arg,const uint8_t *k,uint32_t kn,const uint8_t *v,uint32_t vn)
{
    unsigned *count=arg;
    if(kn==sizeof(tipkey)&&!memcmp(k,tipkey,kn))assert(vn==8&&v[7]==111);
    else if(kn==sizeof(livekey)&&!memcmp(k,livekey,kn))assert(vn==1&&v[0]==9);
    else {assert(kn==sizeof(fillerkey)&&!memcmp(k,fillerkey,kn)&&vn==(1<<20));}
    (*count)++;return 0;
}
int main(void)
{
    alarm(60);
    char dir[EFS_TEST_PATH],path[EFS_TEST_PATH+256];
    efs_test_path(dir,sizeof(dir),"efs-fold-reopen-XXXXXX");assert(mkdtemp(dir));
    int p[2];assert(!pipe(p));pid_t pid=fork();assert(pid>=0);
    if(!pid){close(p[0]);child(dir,p[1]);_exit(1);}
    close(p[1]);char ready;assert(read(p[0],&ready,1)==1&&ready=='R');close(p[0]);
    assert(!kill(pid,SIGKILL));int status;assert(waitpid(pid,&status,0)==pid&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
    for(unsigned i=0;i<3;i++) {
        struct efs_kv *kv=efs_kv_lsm_open(dir,&cfg);assert(kv);expect(kv);
        unsigned count=0;assert(!efs_kv_scan(kv,scan,&count)&&count==3);
        int rc=efs_kv_lsm_compact(kv);assert(rc==EFS_OK||rc==EFS_ERR_BUSY);
        efs_kv_lsm_close(kv);
    }
    DIR *d=opendir(dir);assert(d);struct dirent *e;
    while((e=readdir(d)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,"..")){
        snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);assert(!unlink(path));
    }
    closedir(d);assert(!rmdir(dir));
    puts("KV async fold SIGKILL/reopen PASS: real manifest, warm/cold TIP and tombstones");
}
