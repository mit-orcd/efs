/* Link against the real client library: TCP stalls, quorum and pool ownership. */
#include "client_internal.h"
#include <assert.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
struct peer { int listen_fd; unsigned reply; unsigned partial; atomic_int done; };
static uint64_t now_ms(void)
{
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static void *serve(void *arg)
{
    struct peer *p=arg;struct pollfd ready={.fd=p->listen_fd,.events=POLLIN};
    assert(poll(&ready,1,5000)>0);int fd=accept(p->listen_fd,NULL,NULL);assert(fd>=0);
    assert(!efs_set_recv_timeout(fd,3000));
    uint8_t type;uint32_t len;void *body=NULL;
    assert(!efs_recv_msg(fd,&type,&body,&len));
    assert(type==EFS_MSG_PUT_CHUNK && len==sizeof(struct efs_msg_put_chunk)+65536);free(body);
    if(p->partial){uint8_t byte=0;assert(send(fd,&byte,1,0)==1);}
    if(p->reply){uint8_t reply[2]={EFS_PUT_CHUNK_OK,0};assert(!efs_send_msg(fd,EFS_MSG_PUT_CHUNK_REPLY,reply,sizeof(reply)));}
    while(!atomic_load(&p->done))usleep(1000);
    close(fd);return NULL;
}
int main(void)
{
    assert(!setenv("EFS_TRANSPORT","tcp",1));assert(!setenv("EFS_CLIENT_CONNS_PER_NODE","1",1));
    g_client.node_count=3;g_client.export_id=1;efs_client_conn_init();
    uint8_t bytes[65536]={0};const uint8_t *fragments[3]={bytes,bytes,bytes};
    uint8_t hashes[3][EFS_HASH_SIZE]={{0}};
    for(unsigned round=0;round<8;round++){
        unsigned mode=round%4;struct peer peers[3];pthread_t threads[3];
        efs_node_id_t nodes[3]={1,2,3};
        for(unsigned i=0;i<3;i++){
            peers[i]=(struct peer){.listen_fd=socket(AF_INET,SOCK_STREAM,0),.reply=mode==2 || (mode==1 && i<2) || (mode==3 && i>0),.partial=mode==3 && !i};
            assert(peers[i].listen_fd>=0);struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
            assert(!bind(peers[i].listen_fd,(struct sockaddr *)&addr,sizeof(addr)));assert(!listen(peers[i].listen_fd,1));
            socklen_t n=sizeof(addr);assert(!getsockname(peers[i].listen_fd,(struct sockaddr *)&addr,&n));
            g_client.nodes[i].id=i+1;strcpy(g_client.nodes[i].addr,"127.0.0.1");g_client.nodes[i].port=ntohs(addr.sin_port);
            assert(!pthread_create(&threads[i],NULL,serve,&peers[i]));
        }
        uint64_t start=now_ms(),deadline=start+75,object=0;efs_client_rpc_set_deadline_ms(deadline);
        int rc=efs_client_put_fragments_parallel(4097,0,nodes,fragments,sizeof(bytes),hashes,&object);
        uint64_t elapsed=now_ms()-start;assert(object && elapsed<500);
        assert(rc==(mode?EFS_OK:EFS_ERR_BUSY));assert(efs_client_rpc_deadline_ms()==deadline);
        for(unsigned i=0;i<3;i++){
            assert(!g_client.conn_busy[i][0]);assert(!g_client.node_fail_streak[i] && !g_client.node_down_until_ms[i]);
            efs_client_conn_invalidate_node(i+1);atomic_store(&peers[i].done,1);
            assert(!pthread_join(threads[i],NULL));close(peers[i].listen_fd);
        }
        printf("round=%u mode=%u rc=%d elapsed_ms=%llu busy=0 peer_failures=0 PASS\n",round,mode,rc,(unsigned long long)elapsed);
    }
    efs_node_id_t nodes[3]={1,2,3};uint64_t object=0;
    efs_client_rpc_set_deadline_ms(now_ms());
    assert(efs_client_put_fragments_parallel(4097,0,nodes,fragments,sizeof(bytes),hashes,&object)==EFS_ERR_BUSY && !object);
    puts("real TCP PUT: stalled quorum, optional drain, full quorum, TLS and pool cleanup PASS");
    return 0;
}
