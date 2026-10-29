/* Real Linux sockets, actual production accounting helper. */
#include "../src/common/tcp_counts.h"
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
static void pair(int *a, int *b)
{
    int l=socket(AF_INET,SOCK_STREAM,0);*a=socket(AF_INET,SOCK_STREAM,0);assert(l>=0&&*a>=0);
    struct sockaddr_in s={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(l,(void*)&s,sizeof(s))&&!listen(l,1));socklen_t n=sizeof(s);
    assert(!getsockname(l,(void*)&s,&n)&&!connect(*a,(void*)&s,n));*b=accept(l,0,0);assert(*b>=0);close(l);
}
static void check(int fd,uint64_t want,unsigned want_fin)
{
    for(unsigned i=0;i<2000;i++) {
        uint64_t tx,rx;unsigned state,fin;
        if (!efs_tcp_payload_counts(fd,&tx,&rx,&state,&fin) && rx==want && fin==want_fin) {
            printf("rx=%llu fin=%u state=%u PASS\n",(unsigned long long)rx,fin,state);return;
        }
        usleep(1000);
    }
    assert(!"TCP accounting did not reach expected payload/FIN counts");
}
int main(void)
{
    int a,b;setbuf(stdout,NULL);pair(&a,&b);check(b,0,0);
    assert(write(a,"x",1)==1);check(b,1,0);
    uint64_t tx,rx;unsigned state,fin;assert(!efs_tcp_payload_counts(a,&tx,&rx,&state,&fin)&&tx==1&&rx==0);char c;assert(read(b,&c,1)==1);check(b,1,0);
    assert(!shutdown(a,SHUT_WR));check(b,1,1);assert(!shutdown(b,SHUT_WR));check(a,0,1);check(b,1,1);close(a);close(b);
    pair(&a,&b);assert(!shutdown(b,SHUT_RD));check(b,0,0);
    assert(write(a,"y",1)==1);check(b,1,0);assert(!shutdown(a,SHUT_WR));check(b,1,1);close(a);close(b);
    pair(&a,&b);assert(write(a,"z",1)==1);check(b,1,0);
    struct linger abortive={1,0};assert(!setsockopt(a,SOL_SOCKET,SO_LINGER,&abortive,sizeof(abortive)));close(a);
    usleep(10000);check(b,1,0);assert(read(b,&c,1)==1);check(b,1,0);close(b);
    pair(&a,&b);check(b,0,0);assert(send(a,"u",1,MSG_OOB)==1);check(b,1,0);assert(!shutdown(a,SHUT_WR));check(b,1,1);close(a);close(b);
    assert(efs_tcp_payload_counts(-1,&tx,&rx,&state,&fin)==-1);
    puts("TCP payload accounting: FIN, both-close, SHUT_RD, consumed bytes, RST and urgent one-byte negatives PASS");
}
