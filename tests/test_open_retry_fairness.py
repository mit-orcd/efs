#!/usr/bin/env python3
"""A blocked lease admission must leave time for the CREATE owning its slot."""
from pathlib import Path
import os,resource,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/efs_fuse.c').read_text()
a=s.index('static int efs_open_retry_pass(');b=s.index('static void *efs_open_retry_worker(',a)
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define EFS_OK 0
#define EFS_ERR_BUSY -13
static uint64_t now, budget;
static int creates, leases, consume, create_rc;
static uint64_t stats_now_ms(void){return now;}
static uint64_t efs_client_rpc_deadline_ms(void){return budget;}
static void efs_client_rpc_set_deadline_ms(uint64_t v){budget=v;}
static int efs_open_drain_pending(void){leases++;if(consume){now=budget;return EFS_ERR_BUSY;}return EFS_OK;}
static int efs_pending_create_drain(void){creates++;assert(now<budget);return create_rc;}
'''+s[a:b]+r'''
int main(void) {
 now=100;budget=0;consume=1;
 assert(efs_open_retry_pass(1100)==EFS_ERR_BUSY);
 assert(now==600 && creates==1 && leases==1 && budget==0);
 now=100;budget=900;consume=1;
 assert(efs_open_retry_pass(2000)==EFS_ERR_BUSY && budget==900 && now==600);
 now=100;budget=0;consume=0;create_rc=EFS_ERR_BUSY;
 assert(efs_open_retry_pass(1100)==EFS_ERR_BUSY);
 create_rc=0;assert(efs_open_retry_pass(1100)==EFS_OK);
 puts("retry fairness: exhausted lease budget leaves CREATE proof time; inherited deadline/result preserved PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-open-retry-fairness-') as directory:
 p=Path(directory)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
 legacy=code.replace('uint64_t split = stats_now_ms() + 500;', '(void)stats_now_ms(); uint64_t split = deadline;')
 p.write_text(legacy)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra',str(p),'-o',str(p.with_suffix(''))],check=True)
 resource.setrlimit(resource.RLIMIT_CORE,(0,0))
 failed=subprocess.run([str(p.with_suffix(''))],capture_output=True)
 assert failed.returncode!=0,'negative control must reject lease starvation'
 print('legacy full-budget lease starvation negative control rejected PASS')
