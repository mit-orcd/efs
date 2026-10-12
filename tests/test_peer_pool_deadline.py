#!/usr/bin/env python3
"""Production pool checkout honors the caller budget without borrowing slots."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include "efs/network.h"
#include "efs/rdma.h"
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
static int remaining=-1,waits,connects;
int efs_net_remaining_ms(int cap){return remaining<0?cap:remaining<cap?remaining:cap;}
static int controlled_wait(pthread_cond_t *cv,pthread_mutex_t *mu,const struct timespec *end){
 (void)cv;(void)mu;struct timespec now;clock_gettime(CLOCK_REALTIME,&now);
 long long delta=(end->tv_sec-now.tv_sec)*1000000000LL+end->tv_nsec-now.tv_nsec;
 assert(delta>0 && delta<=(remaining<0?5000:remaining)*1000000LL);
 assert(delta>=(remaining<0?4990:2)*1000000LL);waits++;return ETIMEDOUT;}
#define pthread_cond_timedwait controlled_wait
#include "peer_pool.c"
#undef pthread_cond_timedwait
int efs_connect_tcp(const char *host,uint16_t port){(void)host;(void)port;connects++;return -1;}
int efs_set_recv_timeout(int fd,int ms){(void)fd;(void)ms;return 0;}
int efs_set_send_timeout(int fd,int ms){(void)fd;(void)ms;return 0;}
struct efs_conn *efs_conn_wrap_tcp(int fd,int server){(void)fd;(void)server;return NULL;}
void efs_conn_destroy(struct efs_conn *conn){(void)conn;assert(0);}
int efs_rdma_available(void){return 0;}
int efs_rdma_client_upgrade(struct efs_conn *conn){(void)conn;return -1;}
int efs_rdma_transport(void){return EFS_TRANSPORT_TCP;}
int main(void){struct efs_conn owners[EFS_PEER_CONNS_PER_NODE];
 struct peer_slot *p=peer_slot_for("127.0.0.1",17432);assert(p);
 for(int i=0;i<EFS_PEER_CONNS_PER_NODE;i++){p->conn[i]=&owners[i];p->busy[i]=1;}
 remaining=12;assert(!server_peer_conn_get("127.0.0.1",17432) && waits==1 && !connects);
 remaining=-1;assert(!server_peer_conn_get("127.0.0.1",17432) && waits==2 && !connects);
 remaining=0;assert(!server_peer_conn_get("127.0.0.1",17432) && waits==2);
 p->busy[0]=0;assert(!server_peer_conn_get("127.0.0.1",17432) && !p->busy[0]);
 remaining=12;assert(server_peer_conn_get("127.0.0.1",17432)==&owners[0] && p->busy[0]);
 server_peer_conn_release("127.0.0.1",17432,&owners[0]);assert(!p->busy[0]);
 for(int i=1;i<EFS_PEER_CONNS_PER_NODE;i++)assert(p->busy[i] && p->conn[i]==&owners[i]);
 puts("peer pool: scoped wait12ms, unchanged unscoped5s, expired refusal, retained slot ownership and live checkout/release PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-peer-deadline-') as t:
    p=Path(t);(p/'test.c').write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/server'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=10)
