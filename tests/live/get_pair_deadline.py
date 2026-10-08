#!/usr/bin/env python3
"""Actual merge-base GET pair, real TCP partial replies and inherited/default budgets.
Run from a built Linux source tree; no cluster or mounts are touched.
"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'src/client/read.c').read_text()
def fn(signature):
    a=s.index(signature+'\n{');return s[a:s.index('\n}',a)+2]
a=s.index('struct frag_get_job {');job=s[a:s.index('\n};',a)+3]
functions=[fn(x) for x in ['static void read_verify_init(void)',
 'static int fragment_reply_verified(const uint8_t *data, uint32_t len,\n                                   const uint8_t sum[EFS_HASH_SIZE])',
 'static int get_one_reply(struct efs_conn *conn, struct frag_get_job *j)']]
if 'static void get_two_parallel_run(' in s:
    functions.append(fn('static void get_two_parallel_run(struct frag_get_job *jobs)'))
functions.append(fn('static void get_two_parallel(struct frag_get_job *jobs)'))
code=r'''
#include "client_internal.h"
#include "efs/checksum.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static pthread_once_t read_verify_once=PTHREAD_ONCE_INIT;
static int read_verify;
'''+job+'\n'+'\n'.join(functions)+r'''
struct peer {int listener;atomic_int done;};
static uint64_t now_ms(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static void *serve(void *arg){
 struct peer *p=arg;struct pollfd f={.fd=p->listener,.events=POLLIN};assert(poll(&f,1,5000)>0);
 int fd=accept(p->listener,NULL,NULL);assert(fd>=0);assert(!efs_set_recv_timeout(fd,3000));
 uint8_t type;uint32_t len;void *body=NULL;assert(!efs_recv_msg(fd,&type,&body,&len));
 assert(type==EFS_MSG_GET_CHUNK && len==sizeof(struct efs_msg_get_chunk));free(body);
 uint8_t byte=0;assert(send(fd,&byte,1,0)==1);
 while(!atomic_load(&p->done)){usleep(1000);}
 close(fd);return NULL;
}
int main(void){
 assert(!setenv("EFS_TRANSPORT","tcp",1));assert(!setenv("EFS_CLIENT_CONNS_PER_NODE","1",1));
 g_client.node_count=2;g_client.export_id=1;efs_client_conn_init();
 uint8_t bytes[2][65536];struct frag_get_job jobs[2];
 for(unsigned mode=0;mode<2;mode++){
  struct peer peers[2];pthread_t threads[2];
  for(unsigned i=0;i<2;i++){
   peers[i]=(struct peer){.listener=socket(AF_INET,SOCK_STREAM,0)};assert(peers[i].listener>=0);
   struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
   assert(!bind(peers[i].listener,(struct sockaddr *)&addr,sizeof(addr)));assert(!listen(peers[i].listener,1));
   socklen_t n=sizeof(addr);assert(!getsockname(peers[i].listener,(struct sockaddr *)&addr,&n));
   g_client.nodes[i].id=i+1;strcpy(g_client.nodes[i].addr,"127.0.0.1");g_client.nodes[i].port=ntohs(addr.sin_port);
   jobs[i]=(struct frag_get_job){.node=i+1,.ino=4097,.fi=(int)i,.frag_len=65536,.out=bytes[i]};
   assert(!pthread_create(&threads[i],NULL,serve,&peers[i]));
  }
  uint64_t start=now_ms(),deadline=mode?start+75:0;efs_client_rpc_set_deadline_ms(deadline);
  get_two_parallel(jobs);uint64_t elapsed=now_ms()-start;
  printf("GET pair mode=%u elapsed_ms=%llu restored_deadline=%llu\n",mode,(unsigned long long)elapsed,(unsigned long long)efs_client_rpc_deadline_ms());fflush(stdout);
  assert(elapsed<(mode?500u:6000u) && efs_client_rpc_deadline_ms()==deadline);
  for(unsigned i=0;i<2;i++){
   assert(jobs[i].rc!=EFS_OK&&!jobs[i].len&&!g_client.conn_busy[i][0]);
   assert(!g_client.node_fail_streak[i]&&!g_client.node_down_until_ms[i]);
   atomic_store(&peers[i].done,1);assert(!pthread_join(threads[i],NULL));close(peers[i].listener);
  }
 }
 puts("merge-base GET pair: partial reply cannot exceed default/inherited budget; pool and TLS retained PASS");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-get-pair-') as d:
    p=Path(d)/'t.c';p.write_text(code)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),str(root/'libefs.a'),'-lpthread','-lm','-ldl','-libverbs','-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=70)
