#ifndef EFS_READ_INDEX_PAIR_REMOTE_H
#define EFS_READ_INDEX_PAIR_REMOTE_H
/* Fixed, reusable helpers overlap two independently fresh hosted-group
 * proofs when their leaders differ. No queue or per-request thread creation:
 * saturation and failed initialization keep the ordinary scalar path. */
#define HOST_READ_PAIR_WORKERS 8
#define HOST_READ_PAIR_STACK (128u << 10)
struct host_read_pair_task {
    struct efs_raft_host *host;
    uint8_t group;
    uint64_t deadline;
    int rc,hint;
};
struct host_read_pair_worker {
    pthread_cond_t cv;
    struct host_read_pair_task *pending;
    int busy,done;
};
static pthread_mutex_t host_read_pair_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t host_read_pair_once=PTHREAD_ONCE_INIT;
static struct host_read_pair_worker host_read_pair_workers[HOST_READ_PAIR_WORKERS];
static unsigned host_read_pair_count;
static void *host_read_pair_run(void *arg)
{
    struct host_read_pair_worker *worker=arg;
    pthread_mutex_lock(&host_read_pair_mu);
    for(;;) {
        while(!worker->pending)pthread_cond_wait(&worker->cv,&host_read_pair_mu);
        struct host_read_pair_task *task=worker->pending;
        worker->pending=NULL;
        pthread_mutex_unlock(&host_read_pair_mu);
        uint64_t previous=efs_net_deadline_ms();
        efs_net_set_deadline_ms(task->deadline);
        task->rc=host_read_index(task->host,task->group,&task->hint);
        efs_net_set_deadline_ms(previous);
        pthread_mutex_lock(&host_read_pair_mu);
        worker->done=1;
        pthread_cond_broadcast(&worker->cv);
    }
    return NULL;
}
static void host_read_pair_init(void)
{
    pthread_attr_t attr;
    if(pthread_attr_init(&attr))return;
    if(pthread_attr_setstacksize(&attr,HOST_READ_PAIR_STACK)) {
        pthread_attr_destroy(&attr);return;
    }
    /* Joinable resources must not accumulate; workers live until process
     * exit and hold no host reference while idle. Every submitted task is
     * completed before its handler returns or its host can be destroyed. */
    if(pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED)) {
        pthread_attr_destroy(&attr);return;
    }
    for(unsigned i=0;i<HOST_READ_PAIR_WORKERS;i++) {
        struct host_read_pair_worker *worker=&host_read_pair_workers[i];
        if(pthread_cond_init(&worker->cv,NULL))break;
        pthread_t tid;
        if(pthread_create(&tid,&attr,host_read_pair_run,worker)) {
            pthread_cond_destroy(&worker->cv);break;
        }
        host_read_pair_count++;
    }
    pthread_attr_destroy(&attr);
}
static int host_read_index_scalar_pair(struct efs_raft_host *host,uint8_t first,
                                       uint8_t second,int *hint)
{
    int rc=host_read_index(host,first,hint);
    return rc==EFS_OK?host_read_index(host,second,hint):rc;
}
static int host_read_index_remote_pair(struct efs_raft_host *host,uint8_t first,
                                       uint8_t second,int *hint)
{
    pthread_once(&host_read_pair_once,host_read_pair_init);
    struct host_read_pair_task task={host,second,efs_net_deadline_ms(),EFS_ERR_BUSY,-1};
    struct host_read_pair_worker *worker=NULL;
    pthread_mutex_lock(&host_read_pair_mu);
    for(unsigned i=0;i<host_read_pair_count;i++) {
        if(host_read_pair_workers[i].busy)continue;
        worker=&host_read_pair_workers[i];
        worker->busy=1;worker->done=0;worker->pending=&task;
        pthread_cond_signal(&worker->cv);break;
    }
    pthread_mutex_unlock(&host_read_pair_mu);
    if(!worker)return host_read_index_scalar_pair(host,first,second,hint);
    int first_hint=-1,rc=host_read_index(host,first,&first_hint);
    /* Even a failed first proof waits for the second task's bounded existing
     * read deadline. No callback or stack/host reference escapes this RPC. */
    pthread_mutex_lock(&host_read_pair_mu);
    while(!worker->done)pthread_cond_wait(&worker->cv,&host_read_pair_mu);
    worker->busy=0;worker->done=0;
    pthread_mutex_unlock(&host_read_pair_mu);
    if(hint)*hint=rc?first_hint:task.hint;
    return rc?rc:task.rc;
}
#endif
