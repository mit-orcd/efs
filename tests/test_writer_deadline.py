#!/usr/bin/env python3
"""Production retry pause consumes the inherited deadline without extending it."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/inode_rpc.c').read_text();start=s.index('static int rpc_writer_retry_pause(')
function=s[start:s.index('\n}',start)+2]
source=r'''
#include "efs/common.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
static uint64_t now=1000,deadline;static unsigned slept;
uint64_t efs_client_rpc_deadline_ms(void) { return deadline; }
int efs_client_rpc_past_deadline(void) { return deadline && now>=deadline; }
static int fake_clock(int id,struct timespec *ts) {
    assert(id==CLOCK_MONOTONIC);ts->tv_sec=now/1000;ts->tv_nsec=(now%1000)*1000000;return 0;
}
static int fake_sleep(unsigned us) { slept+=us;now+=us/1000;return 0; }
#define clock_gettime fake_clock
#define usleep fake_sleep
''' + function + r'''
int main(void) {
    deadline=1010;
    assert(rpc_writer_retry_pause(7)==EFS_ERR_BUSY && slept==10000 && now==1010);
    slept=0;assert(rpc_writer_retry_pause(0)==EFS_ERR_BUSY && !slept);
    deadline=0;assert(rpc_writer_retry_pause(0)==EFS_OK && slept==50000);
    deadline=now+5000;slept=0;assert(rpc_writer_retry_pause(99)==EFS_OK && slept==800000);
    puts("writer retry deadline: inherited budget, exact short clamp, expired refusal and bounded unlimited backoff PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-writer-deadline-') as directory:
    path=Path(directory)/'test.c';path.write_text(source)
    cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include')]
    subprocess.run(cmd+[str(path),'-o',str(path.with_suffix(''))],check=True)
    subprocess.run([str(path.with_suffix(''))],check=True)
