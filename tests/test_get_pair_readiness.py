#!/usr/bin/env python3
"""A control-only wake must not block a healthy parallel fragment reply."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = (root/'src/client/read.c').read_text()
a = s.index('static int get_pair_start(') if 'static int get_pair_start(' in s else s.index('static void get_two_parallel_run(')
b = s.index('\n/* A partial TCP frame', a)
code = r'''
#include "client_internal.h"
#include "efs/network.h"
#include <assert.h>
#include <poll.h>
#include <string.h>
#include <time.h>
struct frag_get_job {efs_node_id_t node;efs_ino_t ino;uint32_t chunk_index;int fi;uint32_t frag_len;uint8_t *out;uint32_t len;int rc;uint64_t chunk_generation;const uint8_t *expected_sum;};
struct efs_client g_client;
static struct efs_conn connections[2];
static uint64_t now=10000;
static int wake,healthy_read,blocked_read,drops,releases,cold;
static uint64_t deadline=15000;
int efs_client_rpc_past_deadline(void){return now>=deadline;}
uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
void efs_client_rpc_set_deadline_ms(uint64_t d){deadline=d;}
int efs_client_node_is_down(efs_node_id_t n){(void)n;return 0;}
struct efs_conn *efs_client_conn_get(efs_node_id_t n){if(cold&&n==(efs_node_id_t)cold){now=deadline;return NULL;}return &connections[n-1];}
void efs_client_conn_drop(efs_node_id_t n,struct efs_conn*c){assert(c==&connections[n-1]);drops++;}
void efs_client_conn_release(efs_node_id_t n,struct efs_conn*c){assert(n==(efs_node_id_t)(cold?3-cold:2)&&c==&connections[n-1]);releases++;}
void efs_client_node_note_fail(efs_node_id_t n){(void)n;}
void efs_client_node_note_ok(efs_node_id_t n){assert(n==(efs_node_id_t)(cold?3-cold:2));}
int efs_conn_send_msg(struct efs_conn*c,uint8_t t,const void*p,uint32_t n){(void)c;(void)t;(void)p;(void)n;return 0;}
int efs_conn_reply_watch_quick(struct efs_conn*c){return c==&connections[(cold?3-cold:2)-1]&&(wake||cold)?EFS_CONN_REPLY_READY:c==&connections[0]?10:11;}
static int get_one_reply(struct efs_conn*c,struct frag_get_job*j){
 if(c==&connections[(cold?cold:1)-1]){blocked_read++;now=15000;return -1;}
 assert(wake||cold);healthy_read++;j->rc=EFS_OK;j->len=j->frag_len;return 0;
}
static int fake_clock(clockid_t id,struct timespec*t){assert(id==CLOCK_MONOTONIC);t->tv_sec=now/1000;t->tv_nsec=now%1000*1000000;return 0;}
static int fake_poll(struct pollfd*p,nfds_t n,int ms){
 assert(ms>0);if(!wake){assert(n==(efs_node_id_t)(cold?3-cold:2));wake=1;now+=1;p[0].revents=POLLIN;return 1;}
 assert(n==1);now+=ms;return 0;
}
#define clock_gettime fake_clock
#define poll fake_poll
'''+s[a:b]+r'''
int main(void){struct frag_get_job j[2]={{.node=1,.frag_len=65536},{.node=2,.frag_len=65536}};
 get_two_parallel_run(j);
 assert(!blocked_read&&healthy_read==1&&releases==1&&drops==1);
 assert(j[0].rc==EFS_ERR_NET&&j[1].rc==EFS_OK&&j[1].len==65536&&now==15000);
 for(cold=1;cold<=2;cold++){
 now=10000;deadline=15000;wake=healthy_read=blocked_read=drops=releases=0;
 get_two_parallel_run(j);
 assert(healthy_read==1&&!blocked_read&&!drops&&releases==1);
 assert(j[cold-1].rc==EFS_ERR_NET&&j[2-cold].rc==EFS_OK&&now<15000&&deadline==15000);
 }
 return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-get-readiness-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('GET pair: CREDIT-only wake never blocks healthy reply, one original5s budget and exact connection ownership PASS')
