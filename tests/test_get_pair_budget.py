#!/usr/bin/env python3
"""GET-pair wrapper carries and restores one budget through blocking I/O."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/read.c').read_text();a=s.index('static void get_two_parallel(struct frag_get_job *jobs)\n{')
f=s[a:s.index('\n}',a)+2].replace('+ 5000','+ 30')
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
struct frag_get_job {int rc;unsigned len;};
static uint64_t deadline;
static int pipefd[2],definitive;
static uint64_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
static void efs_client_rpc_set_deadline_ms(uint64_t d){deadline=d;}
static int efs_client_rpc_past_deadline(void){return deadline && now_ms()>=deadline;}
static void get_two_parallel_run(struct frag_get_job *jobs){
 assert(deadline);jobs[0]=(struct frag_get_job){EFS_OK,65536};
 jobs[1]=(struct frag_get_job){definitive?EFS_ERR_NOT_FOUND:EFS_ERR_NET,0};
 if(!definitive && !efs_client_rpc_past_deadline()){
  struct pollfd p={.fd=pipefd[0],.events=POLLIN};
  uint64_t now=now_ms();assert(!poll(&p,1,(int)(deadline-now)));
 }
}
'''+f+r'''
int main(void){
 assert(!pipe(pipefd));
 for(unsigned mode=0;mode<4;mode++){
  struct frag_get_job jobs[2];uint64_t start=now_ms();
  deadline=mode==0?0:mode==1?start+10:mode==2?start:start+1000;
  uint64_t previous=deadline;definitive=mode==3;get_two_parallel(jobs);
  assert(deadline==previous && now_ms()-start<150);
  assert(jobs[0].rc==EFS_OK && jobs[0].len==65536);
  assert(jobs[1].rc==(definitive?EFS_ERR_NOT_FOUND:EFS_ERR_BUSY) && !jobs[1].len);
 }
 close(pipefd[0]);close(pipefd[1]);puts("GET pair budget: default/inherited/expired I/O, exact fragment outcome and TLS restoration PASS");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-get-budget-') as d:
    p=Path(d)/'t.c';p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
