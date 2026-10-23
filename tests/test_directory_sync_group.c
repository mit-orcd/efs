/* Actual queue, deterministic blocked barriers: never acknowledge arrivals
 * during an earlier barrier, never reuse success, propagate batch failures. */
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../src/server/directory_sync_group.h"
#include "test_tmpdir.h"
static struct directory_sync_group group=DIRECTORY_SYNC_GROUP_INIT;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int calls, released, failing;
static int sync_barrier(void *ctx, int fd)
{
    (void)ctx;
    pthread_mutex_lock(&gate);
    int call=++calls;
    pthread_cond_broadcast(&changed);
    while (released<call) pthread_cond_wait(&changed,&gate);
    int fail=call==failing;
    pthread_mutex_unlock(&gate);
    return fail ? EFS_ERR_IO : fsync(fd) ? EFS_ERR_IO : EFS_OK;
}
struct job {int fd,rc;};
static void *run(void *p)
{
    struct job *j=p;
    j->rc=directory_sync_group_run(&group,j->fd,sync_barrier,NULL);
    return NULL;
}
static void wait_call(int n)
{
    pthread_mutex_lock(&gate);
    while(calls<n)pthread_cond_wait(&changed,&gate);
    pthread_mutex_unlock(&gate);
}
static void release(int n)
{
    pthread_mutex_lock(&gate);released=n;
    pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);
}
static void wait_queued(int n)
{
    for(int tries=0;tries<10000;tries++) {
        int count=0;pthread_mutex_lock(&group.mu);
        for(unsigned b=0;b<DIRECTORY_SYNC_BUCKETS;b++)
            for(struct directory_sync_waiter *w=group.head[b];w;w=w->next) count++;
        pthread_mutex_unlock(&group.mu);
        if(count==n)return;
        usleep(1000);
    }
    assert(!"waiters failed to enqueue");
}
int main(void)
{
    char path[EFS_TEST_PATH];efs_test_path(path,sizeof(path),"efs-dir-sync-group-XXXXXX");assert(efs_test_mkdtemp(path));
    int fd=open(path,O_RDONLY|O_DIRECTORY);assert(fd>=0);
    struct job first={fd,999},next[8];pthread_t leader,followers[8];
    assert(!pthread_create(&leader,NULL,run,&first));wait_call(1);
    for(int i=0;i<8;i++) {
        next[i]=(struct job){fd,999};
        assert(!pthread_create(&followers[i],NULL,run,&next[i]));
    }
    wait_queued(8);release(1);wait_call(2);
    assert(!pthread_join(leader,NULL));assert(first.rc==EFS_OK);
    /* Arrival after the batch was sealed needs a THIRD barrier. */
    struct job late={fd,999};pthread_t last;
    assert(!pthread_create(&last,NULL,run,&late));wait_queued(1);
    pthread_mutex_lock(&gate);failing=2;pthread_mutex_unlock(&gate);
    release(2);wait_call(3);
    for(int i=0;i<8;i++) {assert(!pthread_join(followers[i],NULL));assert(next[i].rc==EFS_ERR_IO);}
    release(3);assert(!pthread_join(last,NULL));assert(late.rc==EFS_OK);
    assert(calls==3); /* 10 calls -> 3 real barriers, failure cannot poison retry */
    release(100);assert(directory_sync_group_run(&group,fd,sync_barrier,NULL)==EFS_OK);
    assert(calls==4); /* no cached success */
    assert(directory_sync_group_run(&group,-1,sync_barrier,NULL)==EFS_ERR_IO);
    /* Force a hash collision using real pinned directory descriptors. Different
     * inode identities must never share a barrier even in the same bucket. */
    struct stat original;assert(!fstat(fd,&original));
    char children[512][256];int collision=-1,created=0;
    for(int i=0;i<512;i++) {
        snprintf(children[i],sizeof(children[i]),"%s/c%d",path,i);
        assert(!mkdir(children[i],0700));created++;
        int other=open(children[i],O_RDONLY|O_DIRECTORY);assert(other>=0);
        struct stat st;assert(!fstat(other,&st));
        if((((uint64_t)st.st_ino^(uint64_t)st.st_dev)%DIRECTORY_SYNC_BUCKETS)==
           (((uint64_t)original.st_ino^(uint64_t)original.st_dev)%DIRECTORY_SYNC_BUCKETS)) {
            collision=other;break;
        }
        close(other);
    }
    assert(collision>=0);
    pthread_mutex_lock(&gate);released=4;pthread_mutex_unlock(&gate);
    first=(struct job){fd,999};assert(!pthread_create(&leader,NULL,run,&first));wait_call(5);
    struct job distinct={collision,999};pthread_t separate;
    assert(!pthread_create(&separate,NULL,run,&distinct));wait_queued(1);
    late=(struct job){fd,999};assert(!pthread_create(&last,NULL,run,&late));wait_queued(2);
    release(5);wait_call(6);release(6);wait_call(7);release(7);
    assert(!pthread_join(leader,NULL));assert(!pthread_join(separate,NULL));assert(!pthread_join(last,NULL));
    assert(first.rc==EFS_OK && distinct.rc==EFS_OK && late.rc==EFS_OK && calls==7);
    close(collision);for(int i=0;i<created;i++)assert(!rmdir(children[i]));
    close(fd);assert(!rmdir(path));
    puts("directory sync group: batching, late arrivals, failure fanout, fresh retry, inode collision PASS");
}
