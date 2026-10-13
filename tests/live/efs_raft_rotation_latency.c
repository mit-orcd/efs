/* Isolate synchronous Raft rotation on the same storage filesystem.
 * Never point DIR at a live Raft directory: mkdir must create a new directory.
 * Retains the fixture for inspection; remove DIR only after the run finishes. */
#include "efs/raft_disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
static double seconds(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9; }
int main(int argc,char **argv) {
 if(argc!=3) {fprintf(stderr,"usage: %s NEW-DIR RECORDS\n",argv[0]);return 2;}
 unsigned long n=strtoul(argv[2],NULL,10);if(!n||mkdir(argv[1],0700))return 2;
 struct efs_raft_disk *d=efs_raft_disk_open(argv[1],EFS_RAFT_DISK_SYNC);if(!d)return 1;
 struct efs_raft_store *s=efs_raft_disk_group(d,0);unsigned char cmd[512];memset(cmd,0x5a,sizeof(cmd));
 if(efs_raft_disk_sync_hold(d)!=EFS_OK)return 1;
 double t=seconds();
 for(unsigned long i=1;i<=n;i++) {memcpy(cmd,&i,sizeof(i));if(s->append(s,i,1,cmd,sizeof(cmd))!=EFS_OK)return 1;}
 if(efs_raft_disk_sync_release_wait(d)!=EFS_OK)return 1;
 printf("append_seconds=%.6f records=%lu bytes=%llu\n",seconds()-t,n,(unsigned long long)efs_raft_disk_bytes(d));fflush(stdout);
 for(int k=0;k<3;k++) {t=seconds();int rc=efs_raft_disk_rotate(d);printf("rotation=%d seconds=%.6f rc=%d\n",k,seconds()-t,rc);fflush(stdout);if(rc!=EFS_OK)return 1;}
 efs_raft_disk_close(d);d=efs_raft_disk_open(argv[1],EFS_RAFT_DISK_SYNC);if(!d)return 1;s=efs_raft_disk_group(d,0);
 for(unsigned long i=1;i<=n;i++) {unsigned char actual[512];uint32_t len=sizeof(actual);uint64_t term;memcpy(cmd,&i,sizeof(i));if(s->get(s,i,&term,actual,&len)!=EFS_OK||term!=1||len!=sizeof(cmd)||memcmp(cmd,actual,len))return 1;}
 efs_raft_disk_close(d);puts("all replayed records verified PASS");return 0;
}
