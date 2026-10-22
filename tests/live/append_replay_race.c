/* Simultaneous identical production APPEND RPCs must reserve once. */
#include "efs/protocol.h"
#include "efs/kv_key.h"
#include "live_session.h"
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#define WORKERS 32
static pthread_barrier_t gate;
static int port;
static efs_ino_t ino;
static uint64_t sequence=1,acknowledged=0,length=8,expected_offset=0;
static unsigned expected_status=EFS_INODE_RPC_OK;
static struct efs_msg_inode_reply replies[WORKERS];
static void transfer(int fd, void *buf, size_t size, int writing)
{
    char *p=buf;
    while(size) {
        ssize_t n=writing?send(fd,p,size,MSG_NOSIGNAL):recv(fd,p,size,0);
        assert(n>0);p+=n;size-=(size_t)n;
    }
}
static void *worker(void *arg)
{
    unsigned index=(unsigned)(uintptr_t)arg;
    uint8_t frame[1+sizeof(struct efs_msg_inode_append)+EFS_APPEND_ACK_LEN]={EFS_MSG_INODE_APPEND};
    struct efs_msg_inode_append request={.export_id=1,.ino=ino,.len=length};
    uint32_t epoch=1;uint64_t seq=sequence;
    memcpy(frame+1,&request,sizeof(request));
    memset(frame+1+sizeof(request),0xa5,EFS_OPID_UUID_LEN);
    memcpy(frame+1+sizeof(request)+16,&epoch,4);
    memcpy(frame+1+sizeof(request)+EFS_SESS_WIRE_LEN,&seq,8);
    memcpy(frame+1+sizeof(request)+EFS_APPEND_OPID_LEN,&acknowledged,8);
    pthread_barrier_wait(&gate);
    for(unsigned attempt=0;attempt<400;attempt++) {
        int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
        struct timeval timeout={.tv_sec=15};
        assert(!setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
        struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(port+(attempt%4)),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
        if(connect(fd,(struct sockaddr *)&addr,sizeof(addr))) {close(fd);usleep(50000);continue;}
        uint32_t size=htonl(sizeof(frame));transfer(fd,&size,4,1);transfer(fd,frame,sizeof(frame),1);
        transfer(fd,&size,4,0);assert(ntohl(size)==1+sizeof(replies[index]));
        if(index==0 && attempt==0) {close(fd);continue;} /* lose committed reply */
        uint8_t type;transfer(fd,&type,1,0);assert(type==EFS_MSG_INODE_APPEND_REPLY);
        transfer(fd,&replies[index],sizeof(replies[index]),0);close(fd);
        if(replies[index].status==expected_status)return NULL;
        assert(replies[index].status==EFS_INODE_RPC_BUSY || replies[index].status==EFS_INODE_RPC_NOT_PRIMARY || replies[index].status==EFS_INODE_RPC_ERROR);
        usleep(10000);
    }
    assert(!"append retry exhausted");return NULL;
}
int main(int argc,char **argv)
{
    assert(argc==3 || argc==8);
    if(argc==8) {sequence=strtoull(argv[3],NULL,10);acknowledged=strtoull(argv[4],NULL,10);length=strtoull(argv[5],NULL,10);expected_status=atoi(argv[6]);expected_offset=strtoull(argv[7],NULL,10);}
    port=atoi(argv[1]);ino=strtoull(argv[2],NULL,10);
    if(expected_status==EFS_INODE_RPC_OK) { /* K5: create and admit the appender's session */
        uint8_t uuid[16];memset(uuid,0xa5,sizeof(uuid));
        live_session_create((unsigned)port,uuid,1);live_session_admit((unsigned)port,uuid,1,efs_kv_inode_shard(ino));
    }
    assert(!pthread_barrier_init(&gate,NULL,WORKERS));pthread_t threads[WORKERS];
    for(uintptr_t i=0;i<WORKERS;i++)assert(!pthread_create(&threads[i],NULL,worker,(void *)i));
    for(unsigned i=0;i<WORKERS;i++)assert(!pthread_join(threads[i],NULL));
    unsigned bad=0;
    for(unsigned i=0;i<WORKERS;i++) {
        printf("request=%u offset=%llu end=%llu\n",i,(unsigned long long)replies[i].inode.atime,(unsigned long long)replies[i].inode.size);
        bad+=replies[i].status!=expected_status;
        if(expected_status==EFS_INODE_RPC_OK) bad+=replies[i].inode.atime!=expected_offset || replies[i].inode.size!=expected_offset+length;
    }
    pthread_barrier_destroy(&gate);
    printf("identical_requests=%u lost_committed_reply=1 wrong_results=%u\n",WORKERS,bad);
    return bad?1:0;
}
