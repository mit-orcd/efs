#!/usr/bin/env python3
"""Raft log rotation must not hold the disk lock for the bulk rewrite.

orcd-mini 2026-10-17: rotating a 5.8 GB log (1.5 GB live) held d->mu (and,
through save_snap, the host pump's h->mu) for 12.7-14.0 s on all three
replicas; clients ran out of retries and saw EIO. This rotates ~96 MiB of
live log while another thread takes d->mu in a loop and appends, and bounds
both the rotation's longest lock hold and the prober's longest wait. It also
checks what an off-lock rotation can get wrong: concurrent appends and a
truncation inside the captured range (EFS_ERR_STALE, then a clean retry),
reads through the swap, a second rotation over imaged entries, durable
watermarks that must not move backwards across the swap, the background
rotator in SYNC mode, and a cold replay of every entry."""
from pathlib import Path
import os, shlex, subprocess, sys, tempfile
root = Path(__file__).resolve().parents[1]
code = r'''
#include "efs/raft.h"
#include "efs/raft_disk.h"
#include "raft_disk_internal.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BODY 4096u
#define FILL 12000u /* per group: 2 groups x 12000 x 4 KiB = ~96 MiB live */
#define HOLD_BOUND_US 100000u

static uint64_t now_us(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;}
static void body(uint8_t *b,uint32_t group,uint64_t index,uint64_t term){memset(b,(int)(index*7+group),BODY);memcpy(b,&group,4);memcpy(b+4,&index,8);memcpy(b+12,&term,8);}
static void check(struct efs_raft_store *s,uint32_t group,uint64_t index,uint64_t term){
 static uint8_t want[BODY],got[BODY];uint32_t n=BODY;uint64_t t=0;body(want,group,index,term);
 int rc=s->get(s,index,&t,got,&n);
 if(rc!=EFS_OK||n!=BODY||t!=term||memcmp(want,got,BODY)){
  struct raft_disk_group *g=(struct raft_disk_group *)s;struct raft_log_ent *e=&g->log[index-g->log_base-1];
  fprintf(stderr,"bad entry g=%u i=%llu rc=%d n=%u t=%llu payload_off=%llu img_off=%llx end_off=%llu base=%llu gen=%u bytes=%llu phys=%llu\n",group,(unsigned long long)index,rc,n,(unsigned long long)t,
   (unsigned long long)e->payload_off,(unsigned long long)e->img_off,(unsigned long long)e->end_off,(unsigned long long)g->d->base,g->d->gen,(unsigned long long)g->d->bytes,(unsigned long long)raft_ent_phys(g->d,e));abort();}
}
static uint64_t last(struct efs_raft_store *s){uint64_t i=0,t=0;assert(s->last(s,&i,&t)==EFS_OK);return i;}

struct prober {struct efs_raft_disk *d;struct efs_raft_store *s;volatile int stop;uint64_t max_wait_us,probes,appended,next;};
static void *probe(void *arg){
 struct prober *p=arg;uint8_t b[BODY];
 while(!p->stop){
  uint64_t t0=now_us();pthread_mutex_lock(&p->d->mu);uint64_t w=now_us()-t0;pthread_mutex_unlock(&p->d->mu);
  if(w>p->max_wait_us)p->max_wait_us=w;
  p->probes++;
  if(p->probes%8==0){body(b,2,p->next,3);assert(p->s->append(p->s,p->next,3,b,BODY)==EFS_OK);p->next++;p->appended++;}
  usleep(200);
 }
 return NULL;
}

static struct efs_raft_store *g0;static uint64_t trunc_at;static int fired;
/* Runs on the rotating thread, unlocked: truncate inside the range the
 * attempt has not imaged yet, then rewrite it with a new term. */
static void hook(struct efs_raft_disk *d,int step){
 (void)d;uint8_t b[BODY];
 if(step!=1||fired)return;
 fired=1;
 assert(g0->truncate_from(g0,trunc_at)==EFS_OK);
 for(uint64_t i=trunc_at;i<trunc_at+10;i++){body(b,0,i,9);assert(g0->append(g0,i,9,b,BODY)==EFS_OK);}
}
static uint64_t term_of0(uint64_t i){return i>=trunc_at?9:3;}

static void verify_all(struct efs_raft_disk *d,uint64_t last0,uint64_t last2){
 struct efs_raft_store *a=efs_raft_disk_group(d,0),*b=efs_raft_disk_group(d,2);
 assert(last(a)==last0&&last(b)==last2);
 /* Drop the bounded command cache so reads come from the file. */
 pthread_mutex_lock(&d->mu);
 for(int k=0;k<3;k+=2){struct raft_disk_group *g=d->g[k];for(uint32_t i=0;i<g->n;i++)if(g->log[i].cmd){g->cache_bytes-=g->log[i].clen;free(g->log[i].cmd);g->log[i].cmd=NULL;}}
 pthread_mutex_unlock(&d->mu);
 for(uint64_t i=1;i<=last0;i++)check(a,0,i,term_of0(i));
 for(uint64_t i=1;i<=last2;i++)check(b,2,i,3);
}

int main(int argc,char **argv){
 assert(argc==2);const char *dir=argv[1];uint8_t b[BODY];setvbuf(stdout,NULL,_IONBF,0);
 struct efs_raft_disk *d=efs_raft_disk_open(dir,EFS_RAFT_DISK_SYNC);assert(d);
 g0=efs_raft_disk_group(d,0);struct efs_raft_store *g2=efs_raft_disk_group(d,2);
 pthread_mutex_lock(&d->mu);d->check_at=UINT64_MAX;pthread_mutex_unlock(&d->mu); /* explicit rotations only */
 assert(g0->save_hard(g0,3,1)==EFS_OK&&g0->save_cfg(g0,7,7)==EFS_OK);
 assert(efs_raft_disk_sync_hold(d)==EFS_OK);
 /* Dead prefix: entries a truncation removes, so rotation has work. */
 for(uint64_t i=1;i<=FILL/4;i++){body(b,0,i,1);assert(g0->append(g0,i,1,b,BODY)==EFS_OK);}
 assert(g0->truncate_from(g0,1)==EFS_OK);
 for(uint64_t i=1;i<=FILL;i++){body(b,0,i,3);assert(g0->append(g0,i,3,b,BODY)==EFS_OK);body(b,2,i,3);assert(g2->append(g2,i,3,b,BODY)==EFS_OK);}
 assert(efs_raft_disk_sync_release(d)==EFS_OK);
 assert(g0->save_snap(g0,FILL/2,3)==EFS_OK); /* inside the retained window: entries stay */
 trunc_at=FILL-50;raft_log_rotate_hook=hook;
 uint64_t synced_before=efs_raft_disk_synced_bytes(d),before=efs_raft_disk_bytes(d);

 struct prober p={d,g2,0,0,0,0,FILL+1};pthread_t th;assert(!pthread_create(&th,NULL,probe,&p));
 usleep(20000);
 /* BUSY: the prober's fdatasync is in flight (the API's existing contract). */
 uint64_t t0=now_us();int rc;while((rc=efs_raft_disk_rotate(d))==EFS_ERR_BUSY)usleep(100);uint64_t elapsed=now_us()-t0;
 p.stop=1;pthread_join(th,NULL);raft_log_rotate_hook=NULL;
 pthread_mutex_lock(&d->mu);uint64_t hold=d->rot_hold_max_us,fin=d->rot_final_us,rot=d->rotations;pthread_mutex_unlock(&d->mu);
 printf("rotate: rc=%d elapsed_us=%llu hold_max_us=%llu final_us=%llu probe_max_wait_us=%llu probes=%llu appended=%llu bytes %llu->%llu\n",
        rc,(unsigned long long)elapsed,(unsigned long long)hold,(unsigned long long)fin,(unsigned long long)p.max_wait_us,
        (unsigned long long)p.probes,(unsigned long long)p.appended,(unsigned long long)before,(unsigned long long)efs_raft_disk_bytes(d));
 assert(rc==EFS_OK&&fired&&rot==1);
 assert(hold<HOLD_BOUND_US&&"rotation held d->mu too long");
 assert(p.max_wait_us<HOLD_BOUND_US&&"a d->mu waiter stalled behind rotation");
 assert(p.appended>0&&efs_raft_disk_bytes(d)<before);
 uint64_t last2=p.next-1,last0=trunc_at+9;
 verify_all(d,last0,last2);

 /* Watermarks are logical: one read before the swap cannot cover a record
  * appended after it, even though the new file is shorter. */
 assert(efs_raft_disk_synced_bytes(d)>=synced_before);
 assert(efs_raft_disk_sync_hold(d)==EFS_OK);
 body(b,2,last2+1,3);assert(g2->append(g2,last2+1,3,b,BODY)==EFS_OK);
 assert(efs_raft_disk_covered_index(d,2,synced_before)<=last2);
 assert(efs_raft_disk_covered_index(d,2,efs_raft_disk_synced_bytes(d))==last2);
 assert(efs_raft_disk_sync_release_wait(d)==EFS_OK);
 assert(efs_raft_disk_covered_index(d,2,efs_raft_disk_synced_bytes(d))==++last2);

 /* Second rotation reads entries through the first image's tags. */
 assert(efs_raft_disk_rotate(d)==EFS_OK);verify_all(d,last0,last2);
 efs_raft_disk_close(d);
 d=efs_raft_disk_open(dir,EFS_RAFT_DISK_SYNC);assert(d);verify_all(d,last0,last2);

 /* Background rotator: a snapshot that kills most of the file schedules
  * it, and the caller returns without waiting for the rewrite. */
 struct efs_raft_store *a=efs_raft_disk_group(d,0);
 assert(a->truncate_from(a,FILL/2+1)==EFS_OK);last0=FILL/2;
 struct efs_raft_store *c=efs_raft_disk_group(d,2);assert(c->truncate_from(c,2)==EFS_OK);last2=1;
 pthread_mutex_lock(&d->mu);uint64_t r0=d->rotations;pthread_mutex_unlock(&d->mu);
 t0=now_us();assert(a->save_snap(a,FILL/2,3)==EFS_OK);uint64_t snap_us=now_us()-t0;
 int done=0;for(int i=0;i<3000&&!done;i++){pthread_mutex_lock(&d->mu);done=d->rotations>r0&&!d->rotating;pthread_mutex_unlock(&d->mu);if(!done)usleep(10000);}
 printf("background: save_snap_us=%llu rotated=%d bytes=%llu\n",(unsigned long long)snap_us,done,(unsigned long long)efs_raft_disk_bytes(d));
 assert(done&&snap_us<HOLD_BOUND_US);
 verify_all(d,last0,last2);efs_raft_disk_close(d);
 d=efs_raft_disk_open(dir,EFS_RAFT_DISK_NOSYNC);assert(d);verify_all(d,last0,last2);efs_raft_disk_close(d);
 puts("Raft rotation: bounded lock hold, concurrent appends, stale capture retry, logical watermarks, background rotator and cold replay PASS");
 return 0;
}
'''
cc = shlex.split(os.environ.get('CC', 'cc'))
gc = '-Wl,--gc-sections' if sys.platform.startswith('linux') else '-Wl,-dead_strip'
with tempfile.TemporaryDirectory(prefix='efs-rotate-stall-') as td:
    p = Path(td)
    (p / 'test.c').write_text(code)
    flags = ['-std=gnu11', '-D_GNU_SOURCE', '-O2', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'include'),
             '-I' + str(root / 'src/raft'), '-I' + str(root / 'src/common'),
             '-I' + str(root / 'deps/blake3'), '-ffunction-sections', '-fdata-sections']
    srcs = [str(root / 'src/raft/raft_disk.c'), str(root / 'src/raft/raft_log.c'),
            str(root / 'src/common/checksum.c'), str(root / 'src/common/batch_obs.c')]
    subprocess.run(cc + flags + [str(p / 'test.c')] + srcs + ['-pthread', '-lm', gc, '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test'), str(p / 'log')], check=True, timeout=300)
