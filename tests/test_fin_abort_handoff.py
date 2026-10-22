#!/usr/bin/env python3
"""W64: a coordinator whose DECIDE ABORT fails across a leader change hands the
transaction to the finisher, which re-proposes ABORT at the new leader and
RESOLVEs it promptly instead of leaving PREPAREs for the 5 s recovery scan.
COMMIT decisions are never converted; a BUSY COMMIT keeps the read-only finish."""
import os, shlex, subprocess, tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[1]
s = (root / 'src/server/raft_host.c').read_text()
def function(head):
    a = s.index(head)
    return s[a:s.index('\n}', a) + 2]
assert '#define HOST_FIN_ABORT 2' in s
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define EFS_OK 0
#define EFS_ERR_NOT_FOUND -2
#define EFS_ERR_IO -6
#define EFS_ERR_PROTO -7
#define EFS_ERR_BUSY -13
#define EFS_ERR_STALE -14
#define EFS_ERR_NOT_PRIMARY -15
#define EFS_TXN_COMMIT 1
#define EFS_TXN_ABORT 2
#define EFS_TXN_MAX_PART 4
#define HOST_DECIDE_MAX 64
#define HOST_FIN_ABORT 2
#define HOST_FIN_MAX_NS (10ull*1000000000ull)
#define HOST_FIN_POLL_US 5000
#define EFS_MD_CMD_DECIDE 9
#define EFS_MD_CMD_RESOLVE 10
struct efs_txid {uint8_t bytes[16];};
struct efs_txn_parts {uint8_t n; uint32_t shard[EFS_TXN_MAX_PART];};
struct host_fin_item {struct efs_txid t; struct efs_txn_parts parts; uint32_t coord; uint64_t born_ns; int retire_only;};
struct efs_raft_host {int fin_running; uint64_t fin_dropped, fin_done, fin_aborted; void *kv;};
struct host_resize_ctx {struct efs_raft_host *host; int *hint;};
static uint64_t clock_ns;
static uint64_t now_ns(void){return clock_ns;}
static int usleep(unsigned us){clock_ns += (uint64_t)us*1000; return 0;}
static uint8_t efs_raft_shard_group(uint32_t s){return (s&1)?0:2;}
static uint32_t efs_txn_coordinator(const struct efs_txid *t,const struct efs_txn_parts *p){(void)t;return p->shard[0];}
static uint32_t pack_decide(uint8_t *o,const struct efs_txid *t,uint32_t c,int d,const struct efs_txn_parts *p){(void)t;(void)c;(void)p;o[0]=EFS_MD_CMD_DECIDE;o[1]=(uint8_t)d;return 23;}
static void pack_resolve(uint8_t *o,const struct efs_txid *t,uint32_t sh,int d){(void)t;o[0]=EFS_MD_CMD_RESOLVE;o[1]=(uint8_t)d;o[2]=(uint8_t)sh;}
/* Scripted cluster: the coordinator group has no leader for `election` polls. */
static int election, decided, resolved[8], propose_calls, finished_kind=-1, retired;
static int forced_rc;
static int host_bg_propose(struct efs_raft_host *h,uint8_t g,const uint8_t *cmd,uint32_t n,int verdict){
 (void)h;(void)g;(void)n;assert(verdict==1);propose_calls++;
 if(forced_rc)return forced_rc;
 if(election>0){election--;return EFS_ERR_NOT_PRIMARY;}
 if(cmd[0]==EFS_MD_CMD_DECIDE){if(decided && decided!=cmd[1])return EFS_ERR_PROTO;decided=cmd[1];return EFS_OK;}
 assert(cmd[0]==EFS_MD_CMD_RESOLVE && decided && cmd[1]==decided);resolved[cmd[2]]++;return EFS_OK;
}
static int host_hosts(struct efs_raft_host *h,uint8_t g){(void)h;(void)g;return 1;}
static int host_read_index(struct efs_raft_host *h,uint8_t g,int *hint){(void)h;(void)g;(void)hint;return EFS_OK;}
static int efs_txn_decision_get(void *kv,uint32_t c,const struct efs_txid *t,int *d){(void)kv;(void)c;(void)t;if(!decided)return EFS_ERR_NOT_FOUND;*d=decided;return EFS_OK;}
static void host_txn_retire(struct efs_raft_host *h,const struct efs_txid *t,uint32_t c){(void)h;(void)t;(void)c;retired++;}
static int propose_wait_rc;
static int host_propose_wait(struct efs_raft_host *h,uint8_t g,const uint8_t *cmd,uint32_t n,int *hint){(void)h;(void)g;(void)cmd;(void)n;(void)hint;return propose_wait_rc;}
static void host_fin_add(struct efs_raft_host *h,const struct efs_txid *t,const struct efs_txn_parts *p,uint32_t c,int mode){(void)h;(void)t;(void)p;(void)c;finished_kind=mode;}
''' + function('static void host_fin_one(') + '\n' + function('static int host_resize_decide(') + r'''
static void reset(void){election=decided=propose_calls=retired=forced_rc=0;memset(resolved,0,sizeof(resolved));finished_kind=-1;clock_ns=0;}
int main(void){
 struct efs_raft_host h={.fin_running=1};int hint=-1;
 struct efs_txn_parts parts={3,{1,2,5}};struct efs_txid t={{0}};
 struct host_resize_ctx ctx={&h,&hint};
 /* Coordinator's ABORT hits the step-down: handed off, never a plain finish. */
 for(int rc=-15;rc>=-15;rc--){reset();propose_wait_rc=EFS_ERR_NOT_PRIMARY;
  assert(host_resize_decide(&ctx,&t,&parts,EFS_TXN_ABORT)==EFS_ERR_NOT_PRIMARY&&finished_kind==HOST_FIN_ABORT);}
 reset();propose_wait_rc=EFS_ERR_BUSY;assert(host_resize_decide(&ctx,&t,&parts,EFS_TXN_ABORT)==EFS_ERR_BUSY&&finished_kind==HOST_FIN_ABORT);
 reset();propose_wait_rc=EFS_ERR_IO;assert(host_resize_decide(&ctx,&t,&parts,EFS_TXN_ABORT)==EFS_ERR_IO&&finished_kind==HOST_FIN_ABORT);
 /* Decided elsewhere / reclaimed: nothing to hand off. */
 reset();propose_wait_rc=EFS_ERR_PROTO;host_resize_decide(&ctx,&t,&parts,EFS_TXN_ABORT);assert(finished_kind==-1);
 reset();propose_wait_rc=EFS_ERR_STALE;host_resize_decide(&ctx,&t,&parts,EFS_TXN_ABORT);assert(finished_kind==-1);
 /* COMMIT is never turned into ABORT: BUSY keeps the read-only finish, other errors nothing. */
 reset();propose_wait_rc=EFS_ERR_BUSY;host_resize_decide(&ctx,&t,&parts,EFS_TXN_COMMIT);assert(finished_kind==0);
 reset();propose_wait_rc=EFS_ERR_NOT_PRIMARY;host_resize_decide(&ctx,&t,&parts,EFS_TXN_COMMIT);assert(finished_kind==-1);
 reset();propose_wait_rc=EFS_OK;host_resize_decide(&ctx,&t,&parts,EFS_TXN_ABORT);assert(finished_kind==-1);
 /* Finisher: ABORT survives a ~1 s election and resolves every participant well inside 5 s. */
 struct host_fin_item it={.t=t,.parts=parts,.coord=1,.born_ns=0,.retire_only=HOST_FIN_ABORT};
 reset();election=50;host_fin_one(&h,&it);
 assert(decided==EFS_TXN_ABORT&&resolved[1]==1&&resolved[2]==1&&resolved[5]==1&&retired==1);
 assert(clock_ns<2000000000ull);
 /* Already decided ABORT by recovery: idempotent, still resolves. */
 reset();decided=EFS_TXN_ABORT;host_fin_one(&h,&it);assert(resolved[5]==1&&retired==1);
 /* Already decided COMMIT (impossible from this coordinator): resolve COMMIT, never ABORT. */
 reset();decided=EFS_TXN_COMMIT;host_fin_one(&h,&it);assert(decided==EFS_TXN_COMMIT&&resolved[1]==1);
 /* Reclaimed: stop without resolving. */
 reset();forced_rc=EFS_ERR_STALE;host_fin_one(&h,&it);assert(!retired&&!resolved[1]);
 /* No leader within the finisher deadline: drop to recovery, no resolve. */
 reset();election=1000000;unsigned long long dropped=h.fin_dropped;host_fin_one(&h,&it);
 assert(h.fin_dropped==dropped+1&&!decided&&!retired);
 /* Retire-only and ordinary modes unchanged. */
 reset();it.retire_only=1;host_fin_one(&h,&it);assert(retired==1&&!propose_calls);
 reset();it.retire_only=0;decided=EFS_TXN_COMMIT;host_fin_one(&h,&it);assert(resolved[2]==1&&retired==1);
 puts("finisher ABORT handoff: coordinator leader change aborts promptly, COMMIT never converted, idempotent with recovery PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-fin-abort-') as d:
    src = Path(d) / 't.c'; src.write_text(code); exe = src.with_suffix('')
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=c99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=60)
