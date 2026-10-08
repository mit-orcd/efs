"""Reject kernel descriptor replies, then prove their server lease is released."""
import errno
import os
from pathlib import Path
import re
import subprocess
import time

SHIM=r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
static int reject(void *req,const char *key) {
 const char *path=getenv(key);
 if(!path || unlink(path))return 0;
 int (*reply_err)(void *,int)=dlsym(RTLD_NEXT,"fuse_reply_err");
 if(!reply_err)_exit(97);
 /* Complete the request with no descriptor, then simulate libfuse's
  * negative return when an interrupted request rejects a descriptor. */
 reply_err(req,EIO);
 return 1;
}
int fuse_reply_create(void *req,const void *entry,const void *fi) {
 if(reject(req,"EFS_TEST_REJECT_CREATE"))return -ENOENT;
 int (*real)(void *,const void *,const void *)=dlsym(RTLD_NEXT,"fuse_reply_create");
 return real(req,entry,fi);
}
int fuse_reply_open(void *req,const void *fi) {
 if(reject(req,"EFS_TEST_REJECT_OPEN"))return -ENOENT;
 int (*real)(void *,const void *)=dlsym(RTLD_NEXT,"fuse_reply_open");
 return real(req,fi);
}
'''

def prepare(work,env):
    c=work/'reply-failure.c';so=work/'reply-failure.so';c.write_text(SHIM)
    subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror',str(c),'-ldl','-o',str(so)],check=True)
    return dict(env,LD_PRELOAD=str(so),EFS_TEST_REJECT_CREATE=str(work/'reject-create'),EFS_TEST_REJECT_OPEN=str(work/'reject-open'))

def exercise(mount,work,source,port,env):
    evidence=[]
    for operation in ('create','open'):
        path=mount/('rejected-'+operation)
        if operation=='open':
            with path.open('wb') as f:f.write(b'reply ownership');f.flush();os.fsync(f.fileno())
        marker=work/('reject-'+operation);marker.touch()
        try:
            fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_WRONLY if operation=='create' else os.O_RDONLY)
        except OSError as e:
            assert e.errno==errno.EIO,e
        else:
            os.close(fd);raise AssertionError('descriptor reply was not rejected')
        assert not marker.exists(),'fault not consumed'
        ino=path.stat().st_ino
        if operation=='open':assert path.read_bytes()==b'reply ownership'
        path.unlink()
        deadline=time.monotonic()+60
        while True:
            replies=[]
            for node in range(4):
                r=subprocess.run([str(source/'efs-mgmt'),'raft-getattr',f'127.0.0.1:{port+node}',str(ino)],env=env,capture_output=True,text=True,timeout=10)
                if r.returncode==0:
                    m=re.search(r'status=(\d+)',r.stdout)
                    if m:replies.append(int(m.group(1)))
            if 1 in replies:break
            assert time.monotonic()<deadline,('unreleased inode lease',operation,ino,replies)
            time.sleep(.1)
        evidence.append({'operation':operation,'ino':ino,'rejected_errno':errno.EIO,'inode_reclaimed':True})
    return evidence
