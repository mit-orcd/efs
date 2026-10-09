#include "efs/batch_obs.h"
#include "efs/common.h"
#include "efs/raft_disk.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(void)
{
    char dir[]="/tmp/efs-batch-observer-XXXXXX", path[512];
    assert(mkdtemp(dir));
    struct efs_raft_disk *d=efs_raft_disk_open(dir,EFS_RAFT_DISK_SYNC);
    assert(d);
    struct efs_raft_store *s=efs_raft_disk_group(d,0);
    assert(s);
    assert(efs_raft_disk_sync_hold(d)==EFS_OK);
    for (unsigned i=1;i<=3;i++) assert(s->append(s,i,1,(const uint8_t *)"x",1)==EFS_OK);
    assert(efs_raft_disk_covered_index(d,0,efs_raft_disk_synced_bytes(d))==0);
    assert(efs_raft_disk_sync_release_wait(d)==EFS_OK);
    assert(efs_raft_disk_covered_index(d,0,efs_raft_disk_synced_bytes(d))==3);
    assert(s->append(s,4,1,(const uint8_t *)"x",1)==EFS_OK);
    assert(efs_raft_disk_rotate(d)==EFS_OK);
    assert(s->append(s,5,1,(const uint8_t *)"x",1)==EFS_OK);
    assert(efs_raft_disk_covered_index(d,0,efs_raft_disk_synced_bytes(d))==5);
    efs_batch_obs_dump(efs_batch_obs_now_us());
    efs_raft_disk_close(d);
    snprintf(path,sizeof(path),"%s/raft.log",dir);unlink(path);rmdir(dir);
    puts("observer oracle: covered 3, then 1, rotated, then 1");
    return 0;
}
