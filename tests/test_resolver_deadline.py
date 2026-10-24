#!/usr/bin/env python3
"""Actual resolver ownership under hung NSS, bounded admission and late completion."""
from pathlib import Path
import os,subprocess,tempfile,sys
if sys.platform != "linux":
    print("resolver ownership: SKIP (Linux network implementation)")
    raise SystemExit(0)
root=Path(__file__).resolve().parents[1]
s=(root/'src/common/network.c').read_text();prefix=s[:s.index('int efs_listen_tcp(')]
code=r'''
#include <assert.h>
#include <pthread.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <dirent.h>
static pthread_mutex_t fake_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_cv=PTHREAD_COND_INITIALIZER;
static int frozen=1,entered,create_failure;
static int fake_getaddrinfo(const char *host,const char *service,const struct addrinfo *hints,struct addrinfo **out)
{
 (void)host;(void)hints;
 pthread_mutex_lock(&fake_mu);entered++;
 while(frozen)pthread_cond_wait(&fake_cv,&fake_mu);
 pthread_mutex_unlock(&fake_mu);
 struct addrinfo *r=calloc(1,sizeof(*r));assert(r);
 struct sockaddr_in *a=calloc(1,sizeof(*a));assert(a);
 a->sin_family=AF_INET;a->sin_port=htons(atoi(service));a->sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 r->ai_addr=(struct sockaddr *)a;r->ai_addrlen=sizeof(*a);*out=r;return 0;
}
static void fake_freeaddrinfo(struct addrinfo *r) {free(r->ai_addr);free(r);}
static int fake_create(pthread_t *t,const pthread_attr_t *a,void *(*worker)(void *),void *arg)
{if(create_failure) {create_failure=0;return EAGAIN;} return pthread_create(t,a,worker,arg);}
#define getaddrinfo fake_getaddrinfo
#define freeaddrinfo fake_freeaddrinfo
#define pthread_create fake_create
'''+prefix+r'''
#undef getaddrinfo
#undef freeaddrinfo
#undef pthread_create
int efs_tcp_keepalive(int fd) {(void)fd;return 0;}
static unsigned descriptors(void)
{DIR *d=opendir("/proc/self/fd");assert(d);unsigned n=0;while(readdir(d))n++;closedir(d);return n;}
int main(void)
{
 unsigned baseline=descriptors();struct sockaddr_in address;
 for(unsigned i=0;i<EFS_RESOLVER_JOBS;i++) {
  uint64_t start=resolver_now_ms();efs_net_set_deadline_ms(start+20);
  assert(resolve_ipv4_deadline("hung.test",1234,&address)==-1 && errno==ETIMEDOUT);
  assert(resolver_now_ms()-start<500);
 }
 assert(__atomic_load_n(&resolver_jobs,__ATOMIC_ACQUIRE)==EFS_RESOLVER_JOBS);
 efs_net_set_deadline_ms(resolver_now_ms()+1000);
 assert(resolve_ipv4_deadline("full.test",1234,&address)==-1 && errno==EAGAIN);
 pthread_mutex_lock(&fake_mu);assert(entered==1);frozen=0;pthread_cond_broadcast(&fake_cv);pthread_mutex_unlock(&fake_mu);
 uint64_t end=resolver_now_ms()+2000;
 while(__atomic_load_n(&resolver_jobs,__ATOMIC_ACQUIRE) && resolver_now_ms()<end)usleep(1000);
 assert(!__atomic_load_n(&resolver_jobs,__ATOMIC_ACQUIRE) && descriptors()==baseline);
 efs_net_set_deadline_ms(resolver_now_ms());
 assert(resolve_ipv4_deadline("expired.test",1234,&address)==-1 && errno==ETIMEDOUT);
 assert(!resolver_jobs);
 efs_net_set_deadline_ms(resolver_now_ms()+1000);create_failure=1;
 assert(resolve_ipv4_deadline("create-fail.test",1234,&address)==-1 && errno==EAGAIN);
 assert(!resolver_jobs && descriptors()==baseline);
 int listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
 struct sockaddr_in local={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
 assert(!bind(listener,(struct sockaddr *)&local,sizeof(local)) && !listen(listener,1));
 socklen_t len=sizeof(local);assert(!getsockname(listener,(struct sockaddr *)&local,&len));
 int client=efs_connect_tcp("ready.test",ntohs(local.sin_port));assert(client>=0);close(client);close(listener);
 end=resolver_now_ms()+2000;
 while(__atomic_load_n(&resolver_jobs,__ATOMIC_ACQUIRE) && resolver_now_ms()<end)usleep(1000);
 assert(!resolver_jobs && descriptors()==baseline);
 puts("resolver: hung NSS deadlines, eight-job cap, late completion ownership, thread failure and hostname connect and checkout-local DNS budget PASS");
 return 0;
}
'''

# Include the actual pool checkout against the actual resolver/connection code:
# a hung NSS call must inherit checkout's local limit even without a parent RPC.
node=(root/'src/client/node_cache.c').read_text();a=node.index('struct efs_conn *efs_client_conn_get(')
checkout=node[a:node.index('\n}',a)+2].replace('+ 5000','+ 50')
stubs=r'''
struct efs_client g_client;
static int64_t monotonic_ms(void) {return (int64_t)resolver_now_ms();}
static int node_index_for_id(efs_node_id_t id) {assert(id==1);return 0;}
/* K33 live view: an unknown id waits for the view; id 1 is always known here. */
static int node_index_or_await(efs_node_id_t id) {return node_index_for_id(id);}
static int pool_size(void) {return 1;}
static int conn_fd_is_dead(struct efs_conn *c) {(void)c;assert(0);return 0;}
/* Background reprobe (tests/test_node_reprobe.py) is not under test here. */
__attribute__((unused)) static void node_probe_maybe_start(int idx,int64_t now) {(void)idx;(void)now;}
uint64_t efs_client_rpc_deadline_ms(void) {return efs_net_deadline_ms();}
void efs_client_rpc_set_deadline_ms(uint64_t d) {efs_net_set_deadline_ms(d);}
int efs_set_recv_timeout(int fd,int ms) {(void)fd;(void)ms;assert(0);return -1;}
int efs_set_send_timeout(int fd,int ms) {(void)fd;(void)ms;assert(0);return -1;}
struct efs_conn *efs_conn_wrap_tcp(int fd,int server) {(void)fd;(void)server;assert(0);return NULL;}
int efs_rdma_available(void) {assert(0);return 0;}
int efs_rdma_client_upgrade(struct efs_conn *c) {(void)c;assert(0);return -1;}
int efs_conn_declare(struct efs_conn *c){(void)c;return 0;}
int efs_rdma_transport(void) {assert(0);return EFS_TRANSPORT_TCP;}
void efs_conn_destroy(struct efs_conn *c) {(void)c;assert(0);}
void efs_conn_bind_gen(struct efs_conn *c,uint64_t *g) {(void)c;(void)g;assert(0);}
void efs_conn_note_ok(struct efs_conn *c) {(void)c;assert(0);}
#define EFS_NODE_DOWN_FAILS 4
#define EFS_NODE_DOWN_MS 30000
'''
code=code.replace('#include <assert.h>','#include "client_internal.h"\n#include "efs/rdma.h"\n#include <assert.h>',1)
code=code.replace('int main(void)',stubs+checkout+'\nint main(void)',1)
needle_test=''' assert(!__atomic_load_n(&resolver_jobs,__ATOMIC_ACQUIRE) && descriptors()==baseline);
 efs_net_set_deadline_ms(resolver_now_ms());'''
replacement=''' assert(!__atomic_load_n(&resolver_jobs,__ATOMIC_ACQUIRE) && descriptors()==baseline);
 pthread_mutex_init(&g_client.conn_lock[0],NULL);pthread_cond_init(&g_client.conn_cv[0],NULL);
 strcpy(g_client.nodes[0].addr,"hung.test");g_client.nodes[0].port=1234;
 pthread_mutex_lock(&fake_mu);frozen=1;pthread_mutex_unlock(&fake_mu);
 efs_net_set_deadline_ms(0);uint64_t checkout_start=resolver_now_ms();
 assert(!efs_client_conn_get(1));uint64_t checkout_elapsed=resolver_now_ms()-checkout_start;
 assert(checkout_elapsed>=40 && checkout_elapsed<500);
 printf("checkout-local hung DNS elapsed_ms=%llu restored_deadline=0 busy=0 peer_failures=0\\n",(unsigned long long)checkout_elapsed);
 assert(!efs_net_deadline_ms() && !g_client.conn_busy[0][0] && !g_client.node_fail_streak[0] && !g_client.node_down_until_ms[0]);
 pthread_mutex_lock(&fake_mu);frozen=0;pthread_cond_broadcast(&fake_cv);pthread_mutex_unlock(&fake_mu);
 end=resolver_now_ms()+2000;
 while(__atomic_load_n(&resolver_jobs,__ATOMIC_ACQUIRE) && resolver_now_ms()<end)usleep(1000);
 assert(!__atomic_load_n(&resolver_jobs,__ATOMIC_ACQUIRE) && descriptors()==baseline);
 pthread_cond_destroy(&g_client.conn_cv[0]);pthread_mutex_destroy(&g_client.conn_lock[0]);
 efs_net_set_deadline_ms(resolver_now_ms());'''
assert needle_test in code
code=code.replace(needle_test,replacement,1)

with tempfile.TemporaryDirectory(prefix='efs-resolver-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.environ.get('EFS_TEST_SANITIZE') else []
 subprocess.run(['cc','-D_GNU_SOURCE','-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),*flags,str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
