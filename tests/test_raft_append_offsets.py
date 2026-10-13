#!/usr/bin/env python3
"""A concurrent append during fsync cannot change another record's offsets."""
from pathlib import Path
import os,shlex,subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
if not sys.platform.startswith('linux'):
    print('Raft concurrent append offset gate requires Linux: SKIP');raise SystemExit(0)
code=r'''
#define _GNU_SOURCE
#include "raft_disk_internal.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready=PTHREAD_COND_INITIALIZER;
static int paused,entered;
int fdatasync(int fd){
 pthread_mutex_lock(&gate);
 if(paused){entered=1;pthread_cond_broadcast(&ready);while(paused)pthread_cond_wait(&ready,&gate);}
 pthread_mutex_unlock(&gate);return syscall(SYS_fdatasync,fd);
}
struct job {struct efs_raft_store *store;const char *body;};
static void *append(void *arg){struct job *j=arg;assert(!j->store->append(j->store,1,7,(const uint8_t *)j->body,strlen(j->body)));return NULL;}
static void verify(struct efs_raft_store *s,const char *expected){uint8_t body[64];uint32_t n=sizeof(body);uint64_t term=0;assert(!s->get(s,1,&term,body,&n));assert(term==7 && n==strlen(expected) && !memcmp(body,expected,n));}
int main(int argc,char **argv){
 assert(argc==2);struct efs_raft_disk *d=efs_raft_disk_open(argv[1],EFS_RAFT_DISK_SYNC);assert(d);
 struct job jobs[2]={{efs_raft_disk_group(d,0),"first-body"},{efs_raft_disk_group(d,2),"second-body-longer"}};
 pthread_mutex_lock(&gate);paused=1;pthread_mutex_unlock(&gate);
 pthread_t a,b;assert(!pthread_create(&a,NULL,append,&jobs[0]));
 pthread_mutex_lock(&gate);while(!entered)pthread_cond_wait(&ready,&gate);pthread_mutex_unlock(&gate);
 assert(!pthread_create(&b,NULL,append,&jobs[1]));
 uint64_t first=8+33+strlen(jobs[0].body),last=first+33+strlen(jobs[1].body);
 int seen=0;
 for(unsigned i=0;i<5000;i++){pthread_mutex_lock(&d->mu);seen=d->bytes==last;pthread_mutex_unlock(&d->mu);if(seen)break;usleep(1000);}
 assert(seen);pthread_mutex_lock(&gate);paused=0;pthread_cond_broadcast(&ready);pthread_mutex_unlock(&gate);
 pthread_join(a,NULL);pthread_join(b,NULL);
 pthread_mutex_lock(&d->mu);
 assert(d->g[0]->log[0].end_off==first && d->g[0]->log[0].payload_off==41);
 assert(d->g[2]->log[0].end_off==last && d->g[2]->log[0].payload_off==first+33);
 for(unsigned i=0;i<2;i++){struct raft_disk_group *g=d->g[i?2:0];g->cache_bytes-=g->log[0].clen;free(g->log[0].cmd);g->log[0].cmd=NULL;}
 pthread_mutex_unlock(&d->mu);
 for(unsigned i=0;i<2;i++)verify(jobs[i].store,jobs[i].body);
 assert(!efs_raft_disk_rotate(d));for(unsigned i=0;i<2;i++)verify(jobs[i].store,jobs[i].body);
 efs_raft_disk_close(d);d=efs_raft_disk_open(argv[1],EFS_RAFT_DISK_SYNC);assert(d);
 for(unsigned i=0;i<2;i++)verify(efs_raft_disk_group(d,i?2:0),jobs[i].body);
 efs_raft_disk_close(d);puts("Raft append offsets: concurrent fsync, evicted CRC reads, rotation and cold replay PASS");return 0;
}
'''
cc=shlex.split(os.environ.get('CC','cc'))
with tempfile.TemporaryDirectory(prefix='efs-append-offsets-') as td:
    p=Path(td);(p/'test.c').write_text(code)
    flags=['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/raft'),'-I'+str(root/'src/common')]
    subprocess.run(cc+flags+[str(p/'test.c'),str(root/'libefs.a'),'-pthread','-lm','-ldl','-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test'),str(p/'positive')],check=True,timeout=15)
    old=(root/'src/raft/raft_disk.c').read_text().replace('g->log[off].end_off = record_end','g->log[off].end_off = g->d->bytes').replace('g->log[off].payload_off=record_end-clen','g->log[off].payload_off=g->d->bytes-clen')
    assert old!=(root/'src/raft/raft_disk.c').read_text()
    (p/'old.c').write_text(old)
    subprocess.run(cc+flags+['-c',str(p/'old.c'),'-o',str(p/'old.o')],check=True)
    subprocess.run(cc+flags+[str(p/'test.c'),str(p/'old.o'),str(root/'libefs.a'),'-pthread','-lm','-ldl','-o',str(p/'negative')],check=True)
    r=subprocess.run([str(p/'negative'),str(p/'negative-store')],capture_output=True,timeout=15)
    assert r.returncode and b'end_off==first' in r.stderr,'late-offset negative did not fail the intended oracle'
    print('late post-fsync offset negative rejected PASS')
