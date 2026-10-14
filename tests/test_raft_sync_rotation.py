#!/usr/bin/env python3
"""Real disk store: a stalled fdatasync pins rotation and its durable watermark."""
from pathlib import Path
import os,shlex,subprocess,sys,tempfile
if not sys.platform.startswith('linux'):
    print('Raft sync/rotation: Linux linker-wrap gate NOTRUN on this platform');raise SystemExit(0)
root=Path(__file__).resolve().parents[1]
code=r'''
#include "efs/raft.h"
#include "efs/raft_disk.h"
#include "raft_disk_internal.h"
#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static pthread_mutex_t gate_mu=PTHREAD_MUTEX_INITIALIZER;
static int stall,began,release,fail_sync;
int __real_fdatasync(int fd);
int __wrap_fdatasync(int fd){pthread_mutex_lock(&gate_mu);if(stall){began=1;while(!release){pthread_mutex_unlock(&gate_mu);usleep(1000);pthread_mutex_lock(&gate_mu);}}int fail=fail_sync;pthread_mutex_unlock(&gate_mu);if(fail){errno=EIO;return -1;}return __real_fdatasync(fd);}
struct writer {struct efs_raft_store*s;int rc;};
static void *write_one(void*p){struct writer*w=p;w->rc=w->s->append(w->s,1,2,(uint8_t*)"target",6);return NULL;}
static void check(struct efs_raft_store*s,uint64_t index,const uint8_t*want,uint32_t n){uint8_t b[256];uint32_t len=sizeof(b);uint64_t term;assert(s->get(s,index,&term,b,&len)==EFS_OK&&len==n&&!memcmp(b,want,n));}
int main(void){char dir[]="/tmp/efs-sync-rotate-XXXXXX";assert(mkdtemp(dir));struct efs_raft_disk*d=efs_raft_disk_open(dir,EFS_RAFT_DISK_SYNC);assert(d);
 struct efs_raft_store*a=efs_raft_disk_group(d,0),*b=efs_raft_disk_group(d,2);assert(a&&b);uint8_t payload[256];memset(payload,'b',sizeof(payload));
 for(uint64_t i=1;i<=32;i++)assert(b->append(b,i,2,payload,sizeof(payload))==EFS_OK);
 assert(b->truncate_from(b,2)==EFS_OK);
 pthread_mutex_lock(&gate_mu);stall=1;pthread_mutex_unlock(&gate_mu);
 struct writer w={a,999};pthread_t thread;assert(!pthread_create(&thread,NULL,write_one,&w));
 int seen=0;for(int i=0;i<1000;i++){pthread_mutex_lock(&gate_mu);seen=began;pthread_mutex_unlock(&gate_mu);if(seen)break;usleep(1000);}assert(seen);
 pthread_mutex_lock(&d->mu);assert(d->syncing);int original=d->fd;struct stat before;assert(!fstat(original,&before));pthread_mutex_unlock(&d->mu);
 assert(efs_raft_disk_rotate(d)==EFS_ERR_BUSY);
 assert(efs_raft_disk_sync_hold(d)==EFS_OK);
 pthread_mutex_lock(&d->mu);d->check_at=0;pthread_mutex_unlock(&d->mu);
 assert(b->append(b,2,2,payload,sizeof(payload))==EFS_OK);
 pthread_mutex_lock(&d->mu);struct stat after;assert(d->fd==original&&!fstat(d->fd,&after)&&before.st_ino==after.st_ino&&d->check_at==0);d->check_at=UINT64_MAX;pthread_mutex_unlock(&d->mu);
 pthread_mutex_lock(&gate_mu);release=1;pthread_mutex_unlock(&gate_mu);assert(!pthread_join(thread,NULL)&&w.rc==EFS_OK);
 uint64_t first=efs_raft_disk_synced_bytes(d);assert(efs_raft_disk_covered_index(d,0,first)==1&&efs_raft_disk_covered_index(d,2,first)==1);
 pthread_mutex_lock(&gate_mu);stall=0;pthread_mutex_unlock(&gate_mu);
 assert(efs_raft_disk_sync_release_wait(d)==EFS_OK&&efs_raft_disk_covered_index(d,2,efs_raft_disk_synced_bytes(d))==2);
 assert(efs_raft_disk_rotate(d)==EFS_OK);efs_raft_disk_close(d);
 d=efs_raft_disk_open(dir,EFS_RAFT_DISK_SYNC);assert(d);a=efs_raft_disk_group(d,0);b=efs_raft_disk_group(d,2);check(a,1,(uint8_t*)"target",6);check(b,1,payload,sizeof(payload));check(b,2,payload,sizeof(payload));efs_raft_disk_close(d);
 DIR*dp=opendir(dir);assert(dp);struct dirent*e;char path[1024];while((e=readdir(dp))){if(e->d_name[0]=='.')continue;snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);assert(!unlink(path));}closedir(dp);assert(!rmdir(dir));
 /* A real syscall error cannot certify the newly written record, and the
  * prior acknowledged prefix survives reopen. An unacknowledged suffix may
  * survive in page cache: never require its absence after a process crash. */
 char bad[]="/tmp/efs-sync-error-XXXXXX";assert(mkdtemp(bad));d=efs_raft_disk_open(bad,EFS_RAFT_DISK_SYNC);assert(d);a=efs_raft_disk_group(d,0);
 assert(a->append(a,1,2,(uint8_t*)"good",4)==EFS_OK);uint64_t known=efs_raft_disk_synced_bytes(d);
 pthread_mutex_lock(&gate_mu);fail_sync=1;pthread_mutex_unlock(&gate_mu);
 assert(a->append(a,2,2,(uint8_t*)"uncertain",9)==EFS_ERR_IO&&d->io_failed);
 assert(efs_raft_disk_synced_bytes(d)==known&&efs_raft_disk_covered_index(d,0,known)==1);
 assert(a->append(a,2,2,(uint8_t*)"retry",5)==EFS_ERR_IO);efs_raft_disk_close(d);
 pthread_mutex_lock(&gate_mu);fail_sync=0;pthread_mutex_unlock(&gate_mu);
 d=efs_raft_disk_open(bad,EFS_RAFT_DISK_SYNC);assert(d);a=efs_raft_disk_group(d,0);check(a,1,(uint8_t*)"good",4);efs_raft_disk_close(d);
 dp=opendir(bad);assert(dp);while((e=readdir(dp))){if(e->d_name[0]=='.')continue;snprintf(path,sizeof(path),"%s/%s",bad,e->d_name);assert(!unlink(path));}closedir(dp);assert(!rmdir(bad));
 puts("Raft sync/rotation: stalled syscall, rotation exclusion, exact watermarks, cold bytes and failed-sync durable-prefix fencing PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-sync-rotate-build-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    log=Path(os.environ.get('EFS_RAFT_LOG_SOURCE',root/'src/raft/raft_log.c'))
    cmd=shlex.split(os.environ.get('CC','cc'))+['-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/raft'),'-I'+str(root/'deps/blake3'),'-ffunction-sections','-fdata-sections',str(p),str(root/'src/raft/raft_disk.c'),str(log),str(root/'src/common/checksum.c'),str(root/'src/common/batch_obs.c'),'-pthread','-Wl,--gc-sections','-Wl,--wrap=fdatasync','-o',str(p.with_suffix(''))]
    subprocess.run(cmd,check=True);subprocess.run([str(p.with_suffix(''))],check=True,timeout=15)
