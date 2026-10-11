/* Read decoded APPEND lane fanout from an offline copied KV only. */
#include "efs/kv_lsm.h"
#include "efs/meta_apply.h"
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv) {
 if(argc!=2 || !strstr(argv[1],"/offline-append-fanout/")) {
  fprintf(stderr,"Use a stopped private fixture KV COPY under offline-append-fanout/.\n");return 2;
 }
 struct efs_kv_lsm_cfg cfg={0};struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);
 if(!kv)return 3;
 struct efs_meta_dentry dent={0};struct efs_meta_row row={0};
 int rc=efs_meta_apply_lookup(kv,1,"append-burst",&dent);
 if(rc==0)rc=efs_meta_apply_get_inode(kv,dent.ino,&row);
 printf("{\"rc\":%d,\"ino\":%llu,\"generation\":%llu,\"base_size\":%llu,\"active_lanes_mask\":%llu,\"active_lane_count\":%u,\"capture_image_count_if_current\":%u}\n",rc,
  (unsigned long long)dent.ino,(unsigned long long)row.generation,
  (unsigned long long)row.base_size,(unsigned long long)row.active_lanes,
  (unsigned)__builtin_popcountll(row.active_lanes),4+(unsigned)__builtin_popcountll(row.active_lanes));
 efs_kv_lsm_close(kv);return rc?1:0;
}
