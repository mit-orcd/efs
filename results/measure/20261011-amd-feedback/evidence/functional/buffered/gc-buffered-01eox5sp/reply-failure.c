
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
