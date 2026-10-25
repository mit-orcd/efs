#!/usr/bin/env python3
"""Filesystem capacity deduplicates roots and respects optional quotas."""
import pathlib, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as d:
    src=pathlib.Path(d)/'test.c'
    src.write_text(r'''#include <sys/stat.h>
#include <sys/statvfs.h>
#include <stdint.h>
#include <string.h>
/* Physical inventory remains a real-filesystem check. Quota grouping gets
 * controlled headroom: /tmp can legitimately have less than the 1 GiB reserve. */
static int mock;
static int capacity_stat(const char *path,struct stat *out){
 if(!mock)return stat(path,out);
 memset(out,0,sizeof(*out));out->st_dev=strstr(path,"other-device")?8:7;return 0;
}
static int capacity_statvfs(const char *path,struct statvfs *out){
 if(!mock)return statvfs(path,out);
 memset(out,0,sizeof(*out));out->f_frsize=4096;
 out->f_blocks=(UINT64_C(100)<<30)/4096;
 out->f_bavail=((uint64_t)(mock==2?20:strstr(path,"other-device")?40:60)<<30)/4096;return 0;
}
#define stat(path,out) capacity_stat(path,out)
#define statvfs(path,out) capacity_statvfs(path,out)
#include "efs/storage_capacity.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv){
 (void)argc;uint64_t b, g=UINT64_C(1)<<30;
 assert(!efs_storage_quota_budget(100*g,100*g,0,&b)&&b==70*g);
 assert(!efs_storage_quota_budget(100*g,60*g,10*g,&b)&&b==45*g);
 assert(efs_storage_quota_budget(100*g,20*g,0,&b));
 assert(efs_storage_quota_budget(g,g,0,&b));
 assert(efs_storage_quota_budget(100*g,100*g,71*g,&b));
 assert(efs_storage_quota_budget(100*g,101*g,0,&b));
 assert(!efs_storage_quota_budget(UINT64_MAX,UINT64_MAX,0,&b));
 char roots[2][EFS_MAX_PATH];uint64_t c,a,c2,a2;
 snprintf(roots[0],sizeof(roots[0]),"%s",argv[1]);
 strcpy(roots[1],roots[0]);
 assert(!efs_storage_capacity(roots,1,0,0,&c,&a));assert(c>0&&a<=c);
 assert(!efs_storage_capacity(roots,2,0,0,&c2,&a2));assert(c2==c);
 /* Compare identical inventory inputs. Live free space may change between
  * calls while another test/service writes; that is not deduplication failure.
  * The earlier capacity calls still exercise the real filesystem inventory. */
 uint64_t usage[2]={0,0}, q1,q2;
 mock=1;
 assert(!efs_storage_safe_quota(roots,usage,1,&q1)&&q1==35*g);
 assert(!efs_storage_safe_quota(roots,usage,2,&q2)&&q1==q2);
 usage[0]=g;usage[1]=2*g;
 assert(!efs_storage_safe_quota(roots,usage,2,&q2)&&q2==38*g);
 usage[0]=usage[1]=10*g;
 assert(!efs_storage_safe_quota(roots,usage,2,&q2)&&q2==55*g);
 usage[0]=usage[1]=0;strcpy(roots[1],"/capacity-other-device");
 assert(!efs_storage_safe_quota(roots,usage,2,&q2)&&q2==q1+35*g/2);
 /* The detail rows are the estimate's own inputs: one per filesystem. */
 struct efs_storage_fs_budget fsb[EFS_MAX_STORAGE_PATHS];uint32_t nfs=0;
 assert(!efs_storage_safe_quota_detail(roots,usage,2,&q2,fsb,&nfs)&&nfs==2);
 assert(fsb[0].total==100*g&&fsb[0].available==60*g&&fsb[0].used==0&&fsb[0].budget==35*g);
 assert(fsb[1].available==40*g&&fsb[1].budget==35*g/2&&fsb[0].budget+fsb[1].budget==q2);
 strcpy(roots[1],roots[0]);mock=2;
 assert(efs_storage_safe_quota(roots,usage,1,&q1));
 assert(efs_storage_safe_quota(roots,usage,2,&q2));
 mock=1;
 usage[1]=UINT64_MAX;assert(efs_storage_safe_quota(roots,usage,2,&q2));
 mock=0;
 assert(!efs_storage_capacity(roots,2,1024,1000,&c2,&a2));assert(c2==1024&&a2<=24);
 assert(!efs_storage_capacity(roots,2,1024,2048,&c2,&a2));assert(a2==0);
 strcpy(roots[1],"/path-that-does-not-exist-efs-capacity");
 assert(efs_storage_capacity(roots,2,0,0,&c2,&a2));
 assert(efs_storage_capacity(roots,0,0,0,&c2,&a2));return 0;
}''')
    binary=src.with_suffix('')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(src),'-o',str(binary)],check=True)
    subprocess.run([str(binary),d],check=True)
print('filesystem capacity: duplicate roots, quota bounds, reserves, occupancy and failed inventory PASS')
