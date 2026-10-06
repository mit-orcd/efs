#define EFS_STOP_MS 100
#include "stop_control.h"
#include <assert.h>
#include <signal.h>
static int mode, released, calls, forced;
static int drain(uint64_t deadline,int force)
{
    assert(deadline);
    __atomic_add_fetch(&calls,1,__ATOMIC_RELAXED);
    assert(!efs_stop_mutation_enter());
    if(force) {++forced;return 0;}
    int m=__atomic_load_n(&mode,__ATOMIC_ACQUIRE);
    if(m==2)
        while(!__atomic_load_n(&released,__ATOMIC_ACQUIRE))usleep(1000);
    return m ? -1 : 0;
}
int main(void)
{
    signal(SIGPIPE,SIG_IGN);
    char mount[]="/tmp/efs-control-test-XXXXXX";
    assert(mkdtemp(mount));
    char first[108],second[108];
    assert(!efs_stop_path("/tmp/nonexistent/../efs-target/",first,sizeof(first)));
    assert(!efs_stop_path("/tmp/./efs-target",second,sizeof(second)));
    assert(!strcmp(first,second));
    assert(!efs_stop_start(mount,drain));
    struct stat st;assert(!stat(g_stop.path,&st));
    assert((st.st_mode&0777)==0600);
    assert(efs_stop_mutation_enter());efs_stop_mutation_leave();
    assert(!efs_stop_client(mount,"DRAIN\n"));
    assert(!efs_stop_mutation_enter());
    assert(!efs_stop_client(mount,"RESUME\n"));
    assert(efs_stop_mutation_enter());efs_stop_mutation_leave();
    __atomic_store_n(&mode,1,__ATOMIC_RELEASE);
    assert(efs_stop_client(mount,"DRAIN\n")==1);
    assert(efs_stop_mutation_enter());efs_stop_mutation_leave();
    __atomic_store_n(&mode,2,__ATOMIC_RELEASE);
    uint64_t before=efs_stop_now();
    assert(efs_stop_client(mount,"DRAIN\n")==1);
    assert(efs_stop_now()-before<500); /* caller is not tied to accepted job */
    int saved=__atomic_load_n(&calls,__ATOMIC_RELAXED);
    assert(efs_stop_client(mount,"DRAIN\n")==1);
    assert(efs_stop_client(mount,"FORCE\n")==1); /* live callback still owns state */
    assert(saved==__atomic_load_n(&calls,__ATOMIC_RELAXED));
    assert(efs_stop_mutation_enter());efs_stop_mutation_leave();
    __atomic_store_n(&released,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&g_stop.done,__ATOMIC_ACQUIRE))usleep(1000);
    __atomic_store_n(&mode,0,__ATOMIC_RELEASE);
    assert(!efs_stop_client(mount,"DRAIN\n"));
    assert(!efs_stop_client(mount,"RESUME\n"));
    assert(!efs_stop_client(mount,"FORCE\n"));assert(forced==1);
    assert(!efs_stop_mutation_enter());
    assert(!efs_stop_client(mount,"RESUME\n"));
    /* Existing mutation cannot be detached underneath its owned work. */
    assert(efs_stop_mutation_enter());
    assert(efs_stop_client(mount,"DRAIN\n")==1);
    efs_stop_mutation_leave();
    assert(!efs_stop_client(mount,"DRAIN\n"));
    efs_stop_finish();
    assert(access(g_stop.path,F_OK)!=0);
    assert(efs_stop_client(mount,"DRAIN\n")==3);
    rmdir(mount);
    puts("stop control: clean drain, refusal, live-job timeout, mutation gate, force and cleanup PASS");
}
