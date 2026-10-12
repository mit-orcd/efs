#!/usr/bin/env python3
"""Actual repair journal against production LSM/WAL with real fdatasync EIO."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
code=r'''#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int fdatasync(int fd){
 static int (*real)(int);if(!real)real=dlsym(RTLD_NEXT,"fdatasync");
 const char *marker=getenv("EFS_REPAIR_SYNC_MARKER");
 if(marker && !access(marker,F_OK)){
  char link[64],path[8192];snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
  ssize_t n=readlink(link,path,sizeof(path)-1);
  if(n>0){path[n]=0;if(n>=4 && !strcmp(path+n-4,"/wal")){errno=EIO;return -1;}}
 }
 return real(fd);
}
'''
with tempfile.TemporaryDirectory(prefix='efs-repair-fault-') as d:
 p=Path(d)/'shim.c';p.write_text(code);lib=Path(d)/'shim.so'
 subprocess.run(['cc','-shared','-fPIC',p,'-ldl','-o',lib],check=True)
 subprocess.run([root/'tests/test_repair_job'],env=dict(os.environ,LD_PRELOAD=str(lib),EFS_REPAIR_SYNC_MARKER=str(Path(d)/'armed')),check=True,timeout=90)
