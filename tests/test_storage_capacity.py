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
 (void)argc;char roots[2][EFS_MAX_PATH];uint64_t c,a,c2,a2;
 snprintf(roots[0],sizeof(roots[0]),"%s",argv[1]);
 strcpy(roots[1],roots[0]);
 assert(!efs_storage_capacity(roots,1,0,0,&c,&a));assert(c>0&&a<=c);
 assert(!efs_storage_capacity(roots,2,0,0,&c2,&a2));assert(c2==c);
 assert(!efs_storage_capacity(roots,2,1024,1000,&c2,&a2));assert(c2==1024&&a2<=24);
 assert(!efs_storage_capacity(roots,2,1024,2048,&c2,&a2));assert(a2==0);
 strcpy(roots[1],"/path-that-does-not-exist-efs-capacity");
 assert(efs_storage_capacity(roots,2,0,0,&c2,&a2));
 assert(efs_storage_capacity(roots,0,0,0,&c2,&a2));return 0;
}''')
    binary=src.with_suffix('')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(src),'-o',str(binary)],check=True)
    subprocess.run([str(binary),d],check=True)
print('filesystem capacity: unlimited, duplicate roots, quota room and failed inventory PASS')
