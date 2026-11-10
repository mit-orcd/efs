#!/usr/bin/env python3
"""K106 (b): a down mark ends only when the peer answers the protocol declaration.

A stopped process's kernel still accepts TCP, so the old reprobe (a bare connect)
cleared the mark and every PUT placed on the halted member paid the declaration
wait again. Production node_exchange/efs_client_node_reprobe/node_probe_run,
with a peer that accepts and never answers; a caller never waits on a peer last seen mute. Also: shutdown must cut a running
probe short and wait for it before the connection locks are destroyed."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/node_cache.c').read_text()
a=s.find('/* One real exchange')
if a<0:a=s.index('int efs_client_node_reprobe(') # the pre-K106 source: runs, then fails on its assertions
b=s.index('struct efs_conn *efs_client_conn_get(')
probe=s[a:b]
code=r'''
#include "client_internal.h"
#include "efs/network.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
struct efs_client g_client;
enum{MUTE,ANSWER,BLOCK,REFUSE,CUT};static int mode,declares;static uint64_t caller_dl;
#define DECLARES __atomic_load_n(&declares,__ATOMIC_SEQ_CST)
static int peer[2];
int efs_connect_tcp(const char *h,uint16_t p){assert(!strcmp(h,"10.0.0.2")&&p==1234);if(mode==REFUSE)return -1;assert(!socketpair(AF_UNIX,SOCK_STREAM,0,peer));return peer[0];}
int efs_set_recv_timeout(int fd,int ms){(void)fd;(void)ms;return 0;}
int efs_set_send_timeout(int fd,int ms){(void)fd;(void)ms;return 0;}
struct efs_conn *efs_conn_wrap_tcp(int fd,int server){(void)server;return (struct efs_conn *)(intptr_t)(fd+1);}
void efs_conn_destroy(struct efs_conn *c){close((int)(intptr_t)c-1);close(peer[1]);}
/* MUTE: accepted, the whole bound ran out unanswered; CUT: accepted and cut at once (a reset), bound not reached;
 * BLOCK: still waiting until the descriptor is shut down */
int efs_net_remaining_ms(int cap){return mode==MUTE?0:cap;}
int efs_conn_declare(struct efs_conn *c){__atomic_fetch_add(&declares,1,__ATOMIC_SEQ_CST);if(mode==ANSWER)return EFS_OK;if(mode==MUTE||mode==CUT)return EFS_ERR_NET;char x;return recv((int)(intptr_t)c-1,&x,1,0)<=0?EFS_ERR_NET:EFS_OK;}
uint64_t efs_client_rpc_deadline_ms(void){return caller_dl;}
void efs_client_rpc_set_deadline_ms(uint64_t d){(void)d;}
int node_index_for_id(efs_node_id_t id){(void)id;return 0;}
#define EFS_NODE_PROBE_MS 1000
static void node_probe_maybe_start(int idx, int64_t now);
#define EFS_NODE_DOWN_MS 30000
#define EFS_NODE_ADMISSION_MS 5000
#define EFS_NODE_DOWN_FAILS 4
static int64_t monotonic_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
'''+probe+r'''
static void wait_idle(void){for(int i=0;i<5000;i++){pthread_mutex_lock(&g_client.conn_lock[0]);int busy=g_client.node_probe_inflight[0];pthread_mutex_unlock(&g_client.conn_lock[0]);if(!busy)return;usleep(1000);}assert(!"probe did not finish");}
static int64_t mark;
static void down(void){mark=monotonic_ms()+2000;g_client.node_down_until_ms[0]=mark;g_client.node_fail_streak[0]=4;}
static void probe_now(void){pthread_mutex_lock(&g_client.conn_lock[0]);g_client.node_probe_after_ms[0]=0;node_probe_maybe_start(0,10000);pthread_mutex_unlock(&g_client.conn_lock[0]);}
int main(void){
 for(int i=0;i<EFS_MAX_NODES;i++){pthread_mutex_init(&g_client.conn_lock[i],NULL);pthread_cond_init(&g_client.conn_cv[i],NULL);g_client.node_probe_fd[i]=-1;}
 strcpy(g_client.nodes[0].addr,"10.0.0.2");g_client.nodes[0].port=1234;
 /* The halted member: TCP accepted, the declaration unanswered. The background probe needs the answer and holds the mark. */
 mode=MUTE;down();probe_now();wait_idle();
 assert(DECLARES==1&&g_client.node_mute[0]&&g_client.node_fail_streak[0]==4);
 assert(g_client.node_down_until_ms[0]>=monotonic_ms()+EFS_NODE_DOWN_MS-1000);
 /* A caller never waits on a peer last seen mute: it kicks the probe and learns "still down" without I/O. */
 g_client.node_probe_after_ms[0]=0;
 assert(efs_client_node_reprobe(3)==0&&DECLARES==1&&g_client.node_probe_after_ms[0]!=0);
 wait_idle();assert(DECLARES==2);
 assert(efs_client_node_reprobe(3)==0&&DECLARES==2);            /* no second probe inside EFS_NODE_PROBE_MS */
 /* A refusal (a restart in progress) is no silence: not mute, and it does not extend the mark; a stale mute is not cleared by it. */
 mode=REFUSE;g_client.node_mute[0]=0;down();probe_now();wait_idle();
 assert(!g_client.node_mute[0]&&DECLARES==2&&g_client.node_down_until_ms[0]==mark);
 g_client.node_mute[0]=1;down();probe_now();wait_idle();
 assert(g_client.node_mute[0]&&g_client.node_down_until_ms[0]==mark);
 /* An answer cut short before the bound (a reset) is no silence either: no mute, no re-armed mark. */
 mode=CUT;g_client.node_mute[0]=0;down();probe_now();wait_idle();
 assert(DECLARES==3&&!g_client.node_mute[0]&&g_client.node_down_until_ms[0]==mark&&g_client.node_fail_streak[0]==4);
 /* The caller's own short deadline (spare_reprobe's 250 ms) running out is not the member's silence. */
 mode=MUTE;g_client.node_mute[0]=0;down();caller_dl=(uint64_t)monotonic_ms()+250;
 assert(efs_client_node_reprobe(3)==0&&DECLARES==4&&!g_client.node_mute[0]);
 caller_dl=0;
 /* It stops answering after all: the caller waits the local bound once, then the peer is mute. */
 g_client.node_mute[0]=0;down();
 assert(efs_client_node_reprobe(3)==0&&DECLARES==5&&g_client.node_mute[0]&&g_client.node_down_until_ms[0]==mark);
 g_client.node_mute[0]=0;mode=ANSWER;down();                    /* it answers: the caller ends the mark */
 assert(efs_client_node_reprobe(3)==1&&DECLARES==6&&!g_client.node_down_until_ms[0]&&!g_client.node_fail_streak[0]&&!g_client.node_mute[0]);
 /* The background probe ends the mark of a mute peer that answers again. */
 g_client.node_mute[0]=1;down();probe_now();wait_idle();
 assert(!g_client.node_down_until_ms[0]&&!g_client.node_fail_streak[0]&&!g_client.node_mute[0]);
 /* Shutdown cuts a blocked declaration short and waits for the probe before the locks go. */
 mode=BLOCK;down();probe_now();
 for(int i=0;i<5000&&DECLARES<7;i++)usleep(1000);
 assert(DECLARES==7);
 @STOP@
 assert(!g_client.node_probe_inflight[0]&&g_client.node_probe_fd[0]==-1);
 assert(g_client.node_down_until_ms[0]==mark&&g_client.node_fail_streak[0]==4); /* a cancelled probe is no verdict */
 for(int i=0;i<EFS_MAX_NODES;i++){pthread_cond_destroy(&g_client.conn_cv[i]);pthread_mutex_destroy(&g_client.conn_lock[i]);}
 puts("client node probe: the declaration must be answered, shutdown waits for the probe PASS");
}
'''
code=code.replace('@STOP@','node_probes_stop();' if 'node_probes_stop' in probe else '')
with tempfile.TemporaryDirectory(prefix='efs-node-probe-exchange-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
