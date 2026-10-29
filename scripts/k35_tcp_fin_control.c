#define _GNU_SOURCE
#include <linux/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
static void sample(int fd,const char *tag){struct tcp_info t; socklen_t n=sizeof(t);assert(!getsockopt(fd,IPPROTO_TCP,TCP_INFO,&t,&n));printf("%s state=%u sent=%llu received=%llu\n",tag,t.tcpi_state,(unsigned long long)t.tcpi_bytes_sent,(unsigned long long)t.tcpi_bytes_received);}
int main(void){int l=socket(AF_INET,SOCK_STREAM,0),a=socket(AF_INET,SOCK_STREAM,0);assert(l>=0&&a>=0);struct sockaddr_in s={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};assert(!bind(l,(void*)&s,sizeof(s))&&!listen(l,1));socklen_t n=sizeof(s);assert(!getsockname(l,(void*)&s,&n)&&!connect(a,(void*)&s,n));int b=accept(l,0,0);assert(b>=0);sample(a,"before A");sample(b,"before B");assert(!shutdown(a,SHUT_WR));usleep(50000);sample(b,"peer FIN B");assert(!shutdown(b,SHUT_WR));usleep(50000);sample(a,"both FIN A");sample(b,"both FIN B");close(a);close(b);close(l);}
