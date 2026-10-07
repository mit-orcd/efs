#!/usr/bin/env python3
"""Production pool checkout includes spurious wakeups and late connects in one budget."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/node_cache.c').read_text();a=s.index('struct efs_conn *efs_client_conn_get(');f=s[a:s.index('\n}',a)+2]
code=r'''
#include "client_internal.h"
#include "efs/network.h"
#include "efs/rdma.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
struct efs_client g_client;
static uint64_t deadline;static int64_t advance;static int late,destroyed;
static struct efs_conn connection;
static int64_t real_ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int64_t monotonic_ms(void) {return real_ms()+__atomic_load_n(&advance,__ATOMIC_RELAXED);}
uint64_t efs_client_rpc_deadline_ms(void) {return deadline;}
static int node_index_for_id(efs_node_id_t id) {assert(id==1);return 0;}
static int pool_size(void) {return 1;}
static int conn_fd_is_dead(struct efs_conn *c) {(void)c;return 0;}
int efs_connect_tcp(const char *h,uint16_t p) {(void)h;(void)p;assert(late);__atomic_fetch_add(&advance,2000,__ATOMIC_RELAXED);return 42;}
int efs_set_recv_timeout(int fd,int ms) {(void)fd;(void)ms;return 0;}
int efs_set_send_timeout(int fd,int ms) {(void)fd;(void)ms;return 0;}
struct efs_conn *efs_conn_wrap_tcp(int fd,int server) {(void)fd;(void)server;return &connection;}
int efs_rdma_available(void) {return 0;}
int efs_rdma_client_upgrade(struct efs_conn *c) {(void)c;return 0;}
int efs_rdma_transport(void) {return EFS_TRANSPORT_TCP;}
void efs_conn_destroy(struct efs_conn *c) {assert(c==&connection);destroyed++;}
void efs_conn_bind_gen(struct efs_conn *c,uint64_t *p) {(void)c;(void)p;}
void efs_conn_note_ok(struct efs_conn *c) {(void)c;}
#define EFS_NODE_DOWN_FAILS 4
#define EFS_NODE_DOWN_MS 30000
'''+f+r'''
static void *wake(void *p) {(void)p;for(int i=0;i<3;i++){usleep(1000);pthread_mutex_lock(&g_client.conn_lock[0]);__atomic_fetch_add(&advance,2000,__ATOMIC_RELAXED);pthread_cond_broadcast(&g_client.conn_cv[0]);pthread_mutex_unlock(&g_client.conn_lock[0]);}return NULL;}
int main(void) {
 pthread_mutex_init(&g_client.conn_lock[0],NULL);pthread_cond_init(&g_client.conn_cv[0],NULL);
 g_client.conn_busy[0][0]=1;deadline=monotonic_ms()+10;int64_t start=real_ms();
 assert(!efs_client_conn_get(1)&&real_ms()-start<150);
 deadline=0;advance=0;pthread_t t;assert(!pthread_create(&t,NULL,wake,NULL));start=real_ms();
 assert(!efs_client_conn_get(1)&&real_ms()-start<150);assert(!pthread_join(t,NULL));
 advance=0;g_client.conn_busy[0][0]=0;deadline=monotonic_ms();assert(!efs_client_conn_get(1)&&!g_client.conn_busy[0][0]);
 deadline=0;g_client.conn[0][0]=&connection;connection.last_ok_ms=monotonic_ms();
 assert(efs_client_conn_get(1)==&connection&&g_client.conn_busy[0][0]);
 g_client.conn_busy[0][0]=0;g_client.conn[0][0]=NULL;late=1;deadline=monotonic_ms()+10;
 assert(!efs_client_conn_get(1)&&destroyed==1&&!g_client.conn_busy[0][0]&&!g_client.conn[0][0]);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-conn-budget-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('connection checkout: inherited budget, spurious wakeups, reuse and late-connect cleanup PASS')
