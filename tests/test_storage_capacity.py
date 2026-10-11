#!/usr/bin/env python3
"""Filesystem capacity deduplicates roots and respects optional quotas."""
import pathlib, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as d:
    src=pathlib.Path(d)/'test.c'
    src.write_text(r'''#include "efs/storage_capacity.h"
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
 uint64_t usage[2]={0,0}, q1,q2;
 assert(!efs_storage_safe_quota(roots,usage,1,&q1));
 assert(!efs_storage_safe_quota(roots,usage,2,&q2)&&q1==q2);
 usage[1]=UINT64_MAX;assert(efs_storage_safe_quota(roots,usage,2,&q2));
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
