from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_remote_cmd_impl(');b=s.index('\nstatic int host_wait_applied(',a)
code=r'''
#include "efs/protocol.h"
#include "efs/network.h"
#include <assert.h>
#include <time.h>
#include <stdio.h>
#define HOST_READ_TRIES 80
#define HOST_TICK_US 5000
struct efs_raft_host {int raft_id;};
static uint64_t now_ms=10000,deadline;
static unsigned calls,expired_calls;
static int no_peer;
uint64_t efs_net_deadline_ms(void){return deadline;}
void efs_net_set_deadline_ms(uint64_t n){deadline=n;}
int efs_net_remaining_ms(int cap){return !deadline?cap:now_ms>=deadline?0:(deadline-now_ms<(uint64_t)cap?(int)(deadline-now_ms):cap);}
static void host_deadline_us(struct timespec *end,long us){uint64_t ms=now_ms+us/1000;end->tv_sec=ms/1000;end->tv_nsec=(ms%1000)*1000000;}
static int host_past_deadline(const struct timespec *end){return now_ms>=(uint64_t)end->tv_sec*1000+end->tv_nsec/1000000;}
static int host_local_leader(struct efs_raft_host *h,uint8_t g){(void)h;(void)g;return no_peer?-1:1;}
static int host_pick_peer(struct efs_raft_host *h,const uint8_t *g,int n,int skip){(void)h;(void)g;(void)n;return no_peer?-1:skip==1?2:1;}
static int host_rpc_submit(struct efs_raft_host *h,int rid,uint8_t g,const uint8_t *cmd,uint32_t n,struct efs_msg_raft_mkfs_reply *rep,int timeout){(void)h;(void)rid;(void)g;(void)cmd;(void)n;(void)rep;(void)timeout;calls++;if(!efs_net_remaining_ms(1))expired_calls++;return EFS_ERR_IO;}
static int controlled_sleep(unsigned us){now_ms+=us/1000;return 0;}
static int controlled_clock(clockid_t id,struct timespec *out){assert(id==CLOCK_MONOTONIC);out->tv_sec=now_ms/1000;out->tv_nsec=(now_ms%1000)*1000000;return 0;}
#define usleep controlled_sleep
#define clock_gettime controlled_clock
'''+s[a:b]+r'''
int main(void){struct efs_raft_host h={0};struct efs_msg_raft_mkfs_reply reply;
 deadline=10100;int rc=host_remote_cmd(&h,2,NULL,0,&reply,-1);
 printf("inherited100ms: elapsed=%llu expired_RPC_attempts=%u status=%d\n",(unsigned long long)(now_ms-10000),expired_calls,rc);
 assert(rc==EFS_ERR_BUSY&&now_ms==10100&&!expired_calls&&deadline==10100);
 now_ms=10000;deadline=10100;calls=expired_calls=0;no_peer=1;
 assert(host_remote_cmd(&h,2,NULL,0,&reply,-1)==EFS_ERR_BUSY&&now_ms==10100&&!calls&&deadline==10100);
 now_ms=10000;deadline=9000;no_peer=0;
 assert(host_remote_cmd(&h,2,NULL,0,&reply,-1)==EFS_ERR_BUSY&&now_ms==10000&&!calls&&deadline==9000);
 now_ms=10000;deadline=0;
 assert(host_remote_cmd(&h,2,NULL,0,&reply,-1)==EFS_ERR_BUSY&&now_ms==10400&&!deadline);
 now_ms=10000;deadline=10100;calls=expired_calls=0;uint8_t mutation=1;
 assert(host_remote_cmd(&h,2,&mutation,1,&reply,-1)==EFS_ERR_IO&&now_ms==10400&&expired_calls&&deadline==10100);
 puts("read retry loop: inherited/no-peer/already-expired budgets stop without expired RPC attempts; unscoped400ms and scope restoration PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-retry-') as d:
 p=Path(d)/'test.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=10)
