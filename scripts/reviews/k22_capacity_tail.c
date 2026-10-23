/* Review counterexample at c71cd1066, K22: preloaded same-term tails,
 * no memory pressure, all nodes campaign-enabled. At cap 128 no node
 * sends even a pre-vote. The send callback counts (does not deliver):
 * zero sends proves the veto happens before networking. Cap 0 is a
 * diagnostic control, not a proposed product setting.
 * cc -std=gnu11 -pthread -Iinclude scripts/reviews/k22_capacity_tail.c \
 *    src/raft/raft.c src/raft/raft_mem.c src/common/batch_obs.c -o /tmp/k22-tail
 * /tmp/k22-tail 128; /tmp/k22-tail 0
 * Prints observations, not a cluster PASS. Uses memory stores only.
 * Fixed on devel-k22b: an uncommitted tail no longer vetoes campaigning
 * (only KV pressure and a committed-but-unapplied backlog do), so at cap
 * 128 the nodes now send pre-votes. The regression runs in `make test` as
 * test_tail_campaigns (this scenario, counting sends) and
 * test_tail_recovery (delivered messages: elect, commit and apply the
 * tail, resume admission) in tests/test_follower_bound.c.
 */
#include "efs/raft.h"
#include <stdio.h>
#include <stdlib.h>
static int sends;
static int send_cb(void *p,const struct efs_raft_msg *m){(void)p;(void)m;sends++;return 0;}
static int apply(void *p,uint64_t i,uint64_t t,const uint8_t *c,uint32_t n){(void)p;(void)i;(void)t;(void)c;(void)n;return 0;}
int main(int argc,char **argv){
 unsigned cap=argc>1?(unsigned)strtoul(argv[1],NULL,10):128;
 struct efs_raft *r[3]; struct efs_raft_store *s[3];
 for(int i=0;i<3;i++){
  s[i]=efs_raft_mem_create(); s[i]->save_hard(s[i],1,-1);
  unsigned char c=0x7a;
  for(int j=1;j<=96;j++)if(s[i]->append(s[i],j,1,&c,1))return 2;
  struct efs_raft_cfg cfg={0};cfg.id=i;cfg.n=3;cfg.election_ticks=10;cfg.heartbeat_ticks=2;cfg.rng_seed=i+7;cfg.boot_id=1;cfg.store=s[i];cfg.store_ctx=s[i];cfg.send=send_cb;cfg.apply=apply;
  r[i]=efs_raft_new(&cfg);if(!r[i])return 3;efs_raft_set_max_unapplied(r[i],cap);
 }
 for(int t=0;t<1000;t++)for(int i=0;i<3;i++)efs_raft_tick(r[i]);
 for(int i=0;i<3;i++){struct efs_raft_log_progress p;efs_raft_log_progress(r[i],&p);printf("node=%d role=%d last=%llu unapplied=%llu low=%llu held=%llu\n",i,efs_raft_role(r[i]),(unsigned long long)p.last_index,(unsigned long long)p.unapplied,(unsigned long long)p.low_capacity,(unsigned long long)p.campaigns_held);efs_raft_free(r[i]);efs_raft_mem_free(s[i]);}
 printf("messages sent=%d\n",sends);return 0;
}
