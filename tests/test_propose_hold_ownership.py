#!/usr/bin/env python3
"""Exact production submit must own a hold despite concurrent hold closure."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text();a=s.index('static int host_propose_impl(');b=s.index('\n}',a)+2
# The forward-once refusal (flake sweep, Oct 25) is part of the exact production body.
fa=s.index('static int host_forward_refused(');fb=s.index('\n}',fa)+2
code=r'''
#include "efs/raft.h"
#include "efs/raft_disk.h"
#include "efs/protocol.h"
#include <pthread.h>
#include <sched.h>
#include <assert.h>
#include <stdio.h>
#define BO_HIST(a,b) ((void)0)
#define BO_COUNT(a) ((void)0)
struct efs_raft {int role;uint32_t voters;};
struct efs_raft_disk {int depth,hold_rc;};
struct efs_raft_host {pthread_mutex_t mu;struct efs_raft_disk *disk;struct efs_raft *r;int raft_id;};
static __thread int host_serving_forward;
static struct efs_raft_host *test_host;
static int first_depth,appends,locked_sync,held_sync;
int efs_raft_disk_sync_depth(struct efs_raft_disk*d){if(first_depth){first_depth=0;return 1;}return d->depth;}
int efs_raft_disk_sync_hold(struct efs_raft_disk*d){if(d->hold_rc)return d->hold_rc;d->depth++;return 0;}
static int release(struct efs_raft_disk*d){assert(!pthread_mutex_trylock(&test_host->mu));pthread_mutex_unlock(&test_host->mu);assert(d->depth>0);d->depth--;held_sync++;return 0;}
int efs_raft_disk_sync_release(struct efs_raft_disk*d){return release(d);}
int efs_raft_disk_sync_release_wait(struct efs_raft_disk*d){assert(d->depth==1);return release(d);}
enum{WP_HOLD,WP_MU,WP_APPEND,WP_SPIN,WP_PERSIST,WP_MU_AFTER,WP_PROPOSE,WP_COMMIT_APPLY,WP_WAKE,WP_VERDICT,WP_APPLY,WP_COVER,WP_REPORT_MU};__attribute__((unused)) static inline int write_prof_on(void){return 0;}__attribute__((unused)) static inline uint64_t wprof_now(void){return 0;}__attribute__((unused)) static inline void wprof_add(int p,uint64_t us){(void)p;(void)us;}__attribute__((unused)) static inline void wprof_tick(void){}
static void propose_q_add(void){} static void propose_q_sub(void){} static int propose_q_busy(void){return 0;}
static struct efs_raft *group_raft(struct efs_raft_host*h,uint8_t g){(void)g;return h->r;}
uint32_t efs_raft_voters(const struct efs_raft*r){return r->voters;}
int efs_raft_role(const struct efs_raft*r){return r->role;}
int efs_raft_leader(const struct efs_raft*r){(void)r;return 0;}
uint64_t efs_raft_term(const struct efs_raft*r){(void)r;return 8;}
int efs_raft_propose_local(struct efs_raft*r,const uint8_t*c,uint32_t n,uint64_t*i){(void)r;(void)c;(void)n;assert(test_host->disk->depth>0);appends++;*i=17;return 0;}
int efs_raft_propose(struct efs_raft*r,const uint8_t*c,uint32_t n,uint64_t*i){(void)r;(void)c;(void)n;int rc=pthread_mutex_trylock(&test_host->mu);if(rc)locked_sync++;else pthread_mutex_unlock(&test_host->mu);appends++;*i=17;return 0;}
int efs_raft_submit(struct efs_raft*r,uint64_t i){(void)r;assert(i==17);return 0;}
static void host_publish_group(struct efs_raft_host*h,uint8_t g){(void)h;(void)g;}
static void host_pump_kick(struct efs_raft_host*h){(void)h;}
static void host_note_durable_synced(struct efs_raft_host*h){(void)h;}
static int host_mark_durable_covered(struct efs_raft_host*h,uint8_t g,struct efs_raft*r,uint64_t i){(void)h;(void)g;(void)r;assert(i==17);return 0;}
static int host_remote_cmd(struct efs_raft_host*h,uint8_t g,const uint8_t*c,uint32_t n,struct efs_msg_raft_mkfs_reply*r,int lid){(void)h;(void)g;(void)c;(void)n;(void)r;(void)lid;return EFS_ERR_NOT_PRIMARY;}
''' + 'static int host_group_incompatible(struct efs_raft_host *h,uint8_t g){(void)h;(void)g;return 0;}\n' \
  + '/* W140 tag gate: tested in test_submit_unknown_tag.py. */\n' \
  + 'static int host_tag_refused(uint8_t g,const uint8_t *c,uint32_t n){(void)g;(void)c;(void)n;return 0;}\n' \
  + 'static int host_admit(struct efs_raft_host *h,struct efs_raft *r,const uint8_t *c,uint32_t n){(void)h;(void)r;(void)c;(void)n;return 0;}\n' + s[fa:fb] + '\n' + s[a:b] + r'''
int main(void){struct efs_raft r={EFS_RAFT_LEADER,3};struct efs_raft_disk d={0};
 struct efs_raft_host h={.mu=PTHREAD_MUTEX_INITIALIZER,.disk=&d,.r=&r};test_host=&h;
 uint8_t cmd[]={EFS_MD_CMD_CREATE};uint64_t index=0,term=0;int hint=-1;
 /* Another caller's hold disappears between depth observation and host lock. */
 first_depth=1;
 assert(!host_propose_impl(&h,0,cmd,1,&index,&term,&hint,0));
 printf("closure schedule: index=%llu term=%llu appends=%d locked_sync=%d released=%d depth=%d\n",(unsigned long long)index,(unsigned long long)term,appends,locked_sync,held_sync,d.depth);fflush(stdout);
 assert(index==17&&term==8&&appends==1&&!locked_sync&&held_sync==1&&!d.depth);
 /* Failure to own durability is a refusal, never a synchronous fallback. */
 d.hold_rc=EFS_ERR_IO;appends=0;
 assert(host_propose_impl(&h,0,cmd,1,&index,&term,&hint,0)==EFS_ERR_IO&&!appends);
 /* A pre-existing outer hold remains owned by its caller. */
 d.hold_rc=0;d.depth=1;
 assert(!host_propose_impl(&h,0,cmd,1,&index,&term,&hint,0)&&d.depth==1&&!locked_sync);
 puts("submit hold ownership: concurrent closure, failed hold and nested ownership PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-propose-hold-') as work:
 p=Path(work)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
