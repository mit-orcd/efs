#!/usr/bin/env python3
"""Production REPORT retry pauses never sleep beyond the inherited budget."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/write.c').read_text();a=s.index('static int report_retry_pause(');f=s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/common.h"
#include <assert.h>
static uint64_t now=1000,deadline;static unsigned slept;
static uint64_t report_clock_ms(void) {return now;}
static uint64_t efs_client_rpc_deadline_ms(void) {return deadline;}
static int efs_client_rpc_past_deadline(void) {return deadline&&now>=deadline;}
static int fake_sleep(unsigned us) {slept+=us;now+=us/1000;return 0;}
#define usleep fake_sleep
'''+f+r'''
int main(void) {
 deadline=1010;assert(report_retry_pause(400000)==EFS_ERR_BUSY&&slept==10000&&now==1010);
 slept=0;assert(report_retry_pause(20000)==EFS_ERR_BUSY&&!slept);
 deadline=0;assert(report_retry_pause(20000)==EFS_OK&&slept==20000);
 deadline=now+1000;slept=0;assert(report_retry_pause(50000)==EFS_OK&&slept==50000);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-report-retry-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
print('REPORT retry budget: short clamp, expiry refusal and normal backoff PASS')
