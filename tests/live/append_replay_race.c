/* Simultaneous identical production APPEND RPCs must reserve once. */
#include "efs/protocol.h"
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
    uint8_t frame[1+sizeof(struct efs_msg_inode_append)+EFS_APPEND_OPID_LEN]={EFS_MSG_INODE_APPEND};
    struct efs_msg_inode_append request={.export_id=1,.ino=ino,.len=8};
    uint32_t epoch=1;uint64_t seq=1;
    memcpy(frame+1,&request,sizeof(request));
    memset(frame+1+sizeof(request),0xa5,EFS_OPID_UUID_LEN);
    memcpy(frame+1+sizeof(request)+16,&epoch,4);
    memcpy(frame+1+sizeof(request)+EFS_SESS_WIRE_LEN,&seq,8);
    pthread_barrier_wait(&gate);
    for(unsigned attempt=0;attempt<100;attempt++) {
        int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
        struct timeval timeout={.tv_sec=15};
        assert(!setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
        struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(port+(attempt%4)),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
        assert(!connect(fd,(struct sockaddr *)&addr,sizeof(addr)));
        uint32_t size=htonl(sizeof(frame));transfer(fd,&size,4,1);transfer(fd,frame,sizeof(frame),1);
        transfer(fd,&size,4,0);assert(ntohl(size)==1+sizeof(replies[index]));
        uint8_t type;transfer(fd,&type,1,0);assert(type==EFS_MSG_INODE_APPEND_REPLY);
        transfer(fd,&replies[index],sizeof(replies[index]),0);close(fd);
        if(replies[index].status==EFS_INODE_RPC_OK)return NULL;
        assert(replies[index].status==EFS_INODE_RPC_BUSY || replies[index].status==EFS_INODE_RPC_NOT_PRIMARY);
        usleep(10000);
    }
    assert(!"append retry exhausted");return NULL;
}
int main(int argc,char **argv)
{
    assert(argc==3);port=atoi(argv[1]);ino=strtoull(argv[2],NULL,10);
    assert(!pthread_barrier_init(&gate,NULL,WORKERS));pthread_t threads[WORKERS];
    for(uintptr_t i=0;i<WORKERS;i++)assert(!pthread_create(&threads[i],NULL,worker,(void *)i));
    for(unsigned i=0;i<WORKERS;i++)assert(!pthread_join(threads[i],NULL));
    unsigned bad=0;
    for(unsigned i=0;i<WORKERS;i++) {
        printf("request=%u offset=%llu end=%llu\n",i,(unsigned long long)replies[i].inode.atime,(unsigned long long)replies[i].inode.size);
        bad+=replies[i].inode.atime!=0 || replies[i].inode.size!=8;
    }
    pthread_barrier_destroy(&gate);
    printf("identical_requests=%u wrong_offsets=%u\n",WORKERS,bad);
    return bad?1:0;
}
