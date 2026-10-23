#!/usr/bin/env python3
"""Actual FUSE callbacks: flock follows the kernel open-description release."""
from pathlib import Path
import os, shlex, subprocess, tempfile
import re
root = Path(__file__).resolve().parents[1]
source = Path(os.environ.get('EFS_FUSE_SOURCE', root/'src/client/efs_fuse.c')).read_text()
def function(name):
    match = re.search(r'static int '+name+r'\([^;]*?\)\n\{', source)
    assert match, name
    a = match.start()
    return source[a:source.index('\n}', a)+2]+'\n'
code = r'''
#include "efs/protocol.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
typedef uint64_t fuse_ino_t;
struct fuse_file_info {uint64_t fh,lock_owner;unsigned flock_release;};
static struct {unsigned export_id;uint64_t flock_token;} g_client={1,1234};
static pthread_mutex_t edge=PTHREAD_MUTEX_INITIALIZER;
static unsigned requests,closed,last,holds,evicts,logged,old_takes;
static int reply;
static uint32_t operation;
static uint64_t owner;
static efs_ino_t efs_file_ino(const struct fuse_file_info*f,efs_ino_t fallback){return f?f->fh:fallback;}
static int virt_kind(efs_ino_t i){(void)i;return 0;}
static void efs_file_close(struct fuse_file_info*f){(void)f;closed++;}
static void efs_client_note_meta_change(unsigned i){(void)i;}
static pthread_mutex_t *open_edge_mu(efs_ino_t i){assert(i==4097);return &edge;}
static int efs_close_note(efs_ino_t i,int*leased){assert(i==4097);*leased=1;return last;}
static int efs_open_release_pending(efs_ino_t i){assert(i==4097);holds++;return 0;}
static void efs_client_stage_evict_ino(efs_ino_t i){assert(i==4097);evicts++;}
#define CLOSE_ADMIT_SPINS 250u
static int usleep(unsigned u){(void)u;return 0;}
void efs_client_opid_wait(unsigned max_ms){usleep(max_ms*1000u);}
static int efs_client_rpc_past_deadline(void){return 0;}
static int efs_open_close_unadmitted(efs_ino_t i){(void)i;return 0;}
static int efs_wb_inode_stalled(efs_ino_t i){(void)i;return 0;}
static int efs_append_flush_report(struct fuse_file_info*f,efs_ino_t i){(void)f;(void)i;return EFS_OK;}
static int efs_file_sync_error(struct fuse_file_info*f,efs_ino_t i,int stalled,int rc){(void)f;(void)i;(void)stalled;return rc;}
static void efs_fuse_log_err(const char*w,int rc,efs_ino_t i,uint64_t o,size_t n,const char*p){(void)w;(void)rc;(void)i;(void)o;(void)n;(void)p;logged++;}
static int efs_client_rpc_flock(unsigned ex,efs_ino_t i,uint32_t op,uint64_t who){assert(ex==1 && i==4097);requests++;operation=op;owner=who;return reply;}
static int efs_rc_to_errno(int rc){return rc==EFS_OK?0:-EBUSY;}
/* Only the old source calls these: retain an executable negative control. */
static int flock_armed_take(efs_ino_t i,uint64_t*o){(void)i;*o=555;old_takes++;return 1;}
static void flock_arm(efs_ino_t i,uint64_t o){(void)i;(void)o;}
''' + ''.join(function(n) for n in ('efs_fuse_flush_ino','efs_fuse_release_ino','efs_fuse_flock_ino')) + r'''
int main(void){
 struct fuse_file_info f={.fh=4097,.lock_owner=77};
 assert(!efs_fuse_flush_ino(4097,&f));assert(requests==0 && old_takes==0);
 assert(!efs_fuse_release_ino(4097,&f));assert(requests==0 && closed==1);
 f.flock_release=1;last=0;
 assert(!efs_fuse_release_ino(4097,&f));
 assert(requests==1 && operation==EFS_FLOCK_UN && owner==(1234^77) && closed==2 && !holds);
 last=1;f.lock_owner=99;
 assert(!efs_fuse_release_ino(4097,&f));assert(requests==2 && owner==(1234^99) && holds==1 && evicts==1);
 for(unsigned i=0;i<100;i++) {f.lock_owner=i;assert(!efs_fuse_release_ino(4097,&f));assert(owner==(1234^i));}
 reply=EFS_ERR_BUSY;assert(efs_fuse_release_ino(4097,&f)==-EBUSY && logged==1);
 reply=EFS_OK;f.flock_release=0;
 assert(!efs_fuse_flock_ino(4097,&f,LOCK_EX));assert(operation==(EFS_FLOCK_EX|EFS_FLOCK_WAIT));
 assert(!efs_fuse_flock_ino(4097,&f,LOCK_SH));assert(operation==(EFS_FLOCK_SH|EFS_FLOCK_WAIT));
 assert(!efs_fuse_flock_ino(4097,&f,LOCK_EX|LOCK_NB));assert(operation==(EFS_FLOCK_EX|EFS_FLOCK_NB));
 assert(!efs_fuse_flock_ino(4097,&f,LOCK_UN));assert(operation==EFS_FLOCK_UN);
 reply=EFS_ERR_BUSY;assert(efs_fuse_flock_ino(4097,&f,LOCK_EX|LOCK_NB)==-EAGAIN);
 assert(efs_fuse_flock_ino(4097,NULL,LOCK_EX)==-EBADF);
 puts("flock callbacks: fork/dup FLUSH preserves lock, per-description kernel RELEASE owner, other opens, >64 owners, failure visibility and blocking WAIT flags PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-flock-description-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-pthread','-I'+str(root/'include')]+shlex.split(os.environ.get('EFS_TEST_CFLAGS',''))+[str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
