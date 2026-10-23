/* D38 review: default placement and single-loss quorum arithmetic only. */
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
 for(int n=3;n<=EFS_MAX_NODES;n++) {
  unsigned a=efs_md_group_voters(EFS_RAFT_GROUP_SHARD,n);
  unsigned b=efs_md_group_voters(EFS_RAFT_GROUP_SHARD2,n);
  assert(a==7 && b==(n==3?7:14));
  for(int lost=0;lost<n;lost++) {
   unsigned live=~(1u<<lost);
   assert(__builtin_popcount(a&live)>=2 && __builtin_popcount(b&live)>=2);
  }
 }
 puts("D38 default masks and single-loss quorum arithmetic: N=3..16 PASS");
}
