#!/usr/bin/env python3
"""A peer in client cooldown is reprobed in the background, never by the caller."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/node_cache.c').read_text()
a=s.index('/* One real exchange');b=s.index('struct efs_conn *efs_client_conn_get(')
probe=s[a:b]
assert 'node_probe_maybe_start(idx, now);' in s[b:s.index('\n}',b)], 'cooldown checkout must request a reprobe'
code=r'''
#include "client_internal.h"
#include "efs/network.h"
#include <assert.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
struct efs_client g_client;
static int connect_ok,connects,closed;
int efs_connect_tcp(const char *h,uint16_t p){assert(!strcmp(h,"10.0.0.2")&&p==1234);__atomic_fetch_add(&connects,1,__ATOMIC_SEQ_CST);return connect_ok?77:-1;}
static int fake_close(int fd){assert(fd==77);__atomic_fetch_add(&closed,1,__ATOMIC_SEQ_CST);return 0;}
#define close fake_close
#define EFS_NODE_PROBE_MS 1000
static void node_probe_maybe_start(int idx, int64_t now);
#define EFS_NODE_DOWN_MS 30000
#define EFS_NODE_ADMISSION_MS 5000
static int64_t monotonic_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
int node_index_for_id(efs_node_id_t id){(void)id;return 0;}
uint64_t efs_client_rpc_deadline_ms(void){return 0;}
void efs_client_rpc_set_deadline_ms(uint64_t d){(void)d;}
int efs_set_recv_timeout(int fd,int ms){(void)fd;(void)ms;return 0;}
int efs_net_remaining_ms(int cap){return cap;}
int efs_set_send_timeout(int fd,int ms){(void)fd;(void)ms;return 0;}
struct efs_conn *efs_conn_wrap_tcp(int fd,int server){(void)server;return (struct efs_conn *)(intptr_t)fd;}
void efs_conn_destroy(struct efs_conn *c){fake_close((int)(intptr_t)c);}
int efs_conn_declare(struct efs_conn *c){(void)c;return connect_ok?EFS_OK:EFS_ERR_NET;}
'''+probe+r'''
static void wait_idle(void){for(int i=0;i<2000;i++){pthread_mutex_lock(&g_client.conn_lock[0]);int busy=g_client.node_probe_inflight[0];pthread_mutex_unlock(&g_client.conn_lock[0]);if(!busy)return;usleep(1000);}assert(!"probe did not finish");}
int main(void){
 (void)node_probes_stop; /* exercised by test_node_probe_exchange.py */
 pthread_mutex_init(&g_client.conn_lock[0],NULL);pthread_cond_init(&g_client.conn_cv[0],NULL);
 strcpy(g_client.nodes[0].addr,"10.0.0.2");g_client.nodes[0].port=1234;
 g_client.node_down_until_ms[0]=50000;g_client.node_fail_streak[0]=4;
 /* Peer still down: one probe, cooldown kept, next probe only after 1 s. */
 pthread_mutex_lock(&g_client.conn_lock[0]);node_probe_maybe_start(0,10000);
 assert(g_client.node_probe_after_ms[0]==11000);
 node_probe_maybe_start(0,10001);pthread_mutex_unlock(&g_client.conn_lock[0]);
 wait_idle();assert(connects==1&&!closed&&g_client.node_down_until_ms[0]==50000&&g_client.node_fail_streak[0]==4); /* refused: no evidence of silence, the mark is left alone */
 pthread_mutex_lock(&g_client.conn_lock[0]);node_probe_maybe_start(0,10500);pthread_mutex_unlock(&g_client.conn_lock[0]);
 wait_idle();assert(connects==1);
 /* Listening again: the probe ends the cooldown and the failure streak. */
 connect_ok=1;pthread_mutex_lock(&g_client.conn_lock[0]);node_probe_maybe_start(0,11000);pthread_mutex_unlock(&g_client.conn_lock[0]);
 wait_idle();assert(connects==2&&closed==1&&!g_client.node_down_until_ms[0]&&!g_client.node_fail_streak[0]);
 puts("client node reprobe: background only, rate-limited, keeps cooldown on failure, ends it on an answered declaration PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-node-reprobe-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
