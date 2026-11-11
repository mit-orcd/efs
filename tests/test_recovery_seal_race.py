#!/usr/bin/env python3
"""Actual ledger sweep must not unlink a live, held certificate seal (Linux)."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
s = Path(os.environ.get('RECOVERY_SEAL_SOURCE', root/'src/server/recovery.c')).read_text()
a = s.index('static int rv_ledgers(void)')
b = s.index('\n/* ---- local fragments ---- */', a)
code = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static int held,release_seal,sweep_started;
static int held_sync(int fd) {
 char link[64],path[4096];snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
 ssize_t n=readlink(link,path,sizeof(path)-1);if(n>=0){path[n]=0;
 if(strstr(path,"/put-session/")&&strstr(path,".tmp.")){
 pthread_mutex_lock(&gate);held=1;pthread_cond_broadcast(&cv);
 while(!release_seal)pthread_cond_wait(&cv,&gate);pthread_mutex_unlock(&gate);}}
 return fsync(fd);
}
#define fsync held_sync
#include "PUT_FENCE_SOURCE"
#undef fsync
static struct {struct efsd_server *s;} rv;
static int rv_fail(const char *fmt,...) {(void)fmt;return -1;}
#define efs_telemetry_log(...) ((void)0)
static int sweep_at_tmp;
static void sweep_reached(void){pthread_mutex_lock(&gate);sweep_at_tmp=1;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&gate);}
/* Old sweep: unlinks the temp file itself, then reports. */
static int sweep_unlink(const char *path){int rc=unlink(path);if(strstr(path,".tmp."))sweep_reached();return rc;}
#define unlink sweep_unlink
RECOVER_WRAPPER
''' + s[a:b] + r'''
#undef unlink
static uint8_t uuid[16]={1,2,3,4};static int seal_rc,sweep_rc;
static void *seal(void *p){(void)p;seal_rc=server_put_session_fence(rv.s,uuid,2);return NULL;}
static void *sweep(void *p){(void)p;pthread_mutex_lock(&gate);sweep_started=1;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&gate);sweep_rc=rv_ledgers();return NULL;}
int main(int argc,char **argv) {
 assert(argc==2);struct efsd_server *server=calloc(1,sizeof(*server));assert(server);
 server->storage_path_count=1;strcpy(server->storage_paths[0],argv[1]);rv.s=server;
 pthread_t a,b;assert(!pthread_create(&a,NULL,seal,NULL));
 pthread_mutex_lock(&gate);while(!held)pthread_cond_wait(&cv,&gate);pthread_mutex_unlock(&gate);
 assert(!pthread_create(&b,NULL,sweep,NULL));
 /* The seal is released only after the sweep reached its temp file: the old
  * sweep has unlinked it by then, the fixed sweep is about to wait for the seal. */
 pthread_mutex_lock(&gate);while(!sweep_started||!sweep_at_tmp)pthread_cond_wait(&cv,&gate);
 release_seal=1;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&gate);
 assert(!pthread_join(a,NULL)&&!pthread_join(b,NULL));
 assert(seal_rc==EFS_OK && "recovery removed a live seal temporary file");assert(!sweep_rc);
 uint32_t floor=0;assert(!server_put_session_floor(server,uuid,&floor)&&floor==2);
 struct efs_put_storage_guard guard={0};assert(server_put_session_begin(server,uuid,1,&guard)==EFS_ERR_STALE);
 /* A genuinely interrupted seal must still be removed, without changing floor. */
 char path[EFS_MAX_PATH+96];pthread_mutex_t *mu;assert(!storage_fence_path(server,uuid,path,&mu));
 strcat(path,".tmp.orphan");FILE *f=fopen(path,"wb");assert(f);assert(fputs("partial",f)>=0&&!fclose(f));
 assert(!rv_ledgers()&&access(path,F_OK)&&errno==ENOENT);
 assert(!server_put_session_floor(server,uuid,&floor)&&floor==2);free(server);
 puts("recovery seal race: held live seal survives, stale epoch refused, abandoned temporary removed PASS");
}
'''
fence = os.environ.get('PUT_FENCE_SOURCE',str(root/'src/server/put_fence.c'))
# Fixed sweep: reports before it takes the seal's lock. The old put_fence.c has no helper to wrap.
wrapper = r'''static int sweep_recover(struct efsd_server *s,const char *name){sweep_reached();return server_put_session_recover_tmp(s,name);}
#define server_put_session_recover_tmp sweep_recover''' if 'server_put_session_recover_tmp' in Path(fence).read_text() else ''
code = code.replace('RECOVER_WRAPPER',wrapper).replace('PUT_FENCE_SOURCE',fence)
# Keep every failed fixture; successful generated files are disposable.
d = Path(tempfile.mkdtemp(prefix='efs-recovery-seal-'))
p = d/'test.c';p.write_text(code);store=d/'store';store.mkdir()
flags=['-std=gnu11','-D_GNU_SOURCE','-O1','-g','-I'+str(root/'include'),'-I'+str(root/'src/server'),'-I'+str(root/'src/common'),'-I'+str(root/'deps/blake3')]
if os.environ.get('EFS_TEST_SANITIZE')=='1':flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
subprocess.run(shlex.split(os.environ.get('CC','cc'))+flags+[str(p),str(root/'libefs.a'),'-lpthread','-lm','-ldl','-o',str(d/'test')],check=True)
subprocess.run([str(d/'test'),str(store)],check=True,timeout=30)
import shutil
shutil.rmtree(d)
