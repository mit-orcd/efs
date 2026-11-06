#define EFS_STOP_MS 100
#include "stop_control.h"
#include <assert.h>
#include <signal.h>
#include "test_tmpdir.h"
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
    char mount[EFS_TEST_PATH];efs_test_path(mount,sizeof(mount),"efs-control-test-XXXXXX");
    assert(efs_test_mkdtemp(mount));
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
    /* DI: a stop tool asked (DRAIN above), so the verdict stays for it. */
    char record[sizeof(g_stop.status)];strcpy(record,g_stop.status);
    efs_stop_exit(0,NULL);
    FILE *f=fopen(record,"r");assert(f);char line[256];assert(fgets(line,sizeof(line),f));fclose(f);
    assert(strstr(line,"phase=done") && strstr(line,"rc=0"));
    assert(!unlink(record));
    /* Nobody asked (unmount or signal): .sock and .stop both go at exit. */
    for(int rc=0;rc<2;rc++) {
        assert(!efs_stop_start(mount,drain));
        assert(!access(record,F_OK) && !access(g_stop.path,F_OK));
        efs_stop_finish();
        efs_stop_exit(rc,rc ? "unclean" : NULL);
        assert(access(record,F_OK)!=0 && access(g_stop.path,F_OK)!=0);
    }
    rmdir(mount);
    /* Path derivation: $TMPDIR (trailing '/' dropped), else /tmp; an
     * endpoint that does not fit sun_path is refused, never truncated. */
    char path[108],want[160];
    setenv("TMPDIR","/var/efs-di-tmp//",1);
    assert(efs_tmp_path(path,sizeof(path),"x-%d",7)==(int)strlen("/var/efs-di-tmp/x-7"));
    assert(!strcmp(path,"/var/efs-di-tmp/x-7"));
    assert(!efs_stop_path("/mnt/efs",path,sizeof(path)));
    assert(!strncmp(path,"/var/efs-di-tmp/efs-control-",28) && strstr(path,".sock"));
    snprintf(want,sizeof(want),"%s",path);
    for(int unset=0;unset<2;unset++) { /* empty, then unset: /tmp */
        if(unset)unsetenv("TMPDIR"); else setenv("TMPDIR","",1);
        assert(!strcmp(efs_tmpdir(),"/tmp"));
        assert(!efs_stop_path("/mnt/efs",path,sizeof(path)));
        assert(!strncmp(path,"/tmp/",5) && !strcmp(path+5,want+16));
    }
    char longdir[120];memset(longdir,'d',sizeof(longdir)-1);longdir[0]='/';longdir[sizeof(longdir)-1]=0;
    setenv("TMPDIR",longdir,1);
    assert(efs_stop_path("/mnt/efs",path,sizeof(path))==-1);
    puts("stop control: clean drain, refusal, live-job timeout, mutation gate, force, cleanup, exit record and TMPDIR path PASS");
}
