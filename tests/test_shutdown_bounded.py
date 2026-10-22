#!/usr/bin/env python3
"""K1/W145 part 4: an efsd stop is bounded.

orcd-mini n2's shutdown waited minutes for a handler that kept serving a
pooled RDMA client's retried group read (each a 400 ms ReadIndex wait that
could no longer get a quorum); the TCP SHUT_RD wake never reaches the RDMA
channel. stop.sh (scripts/server_processes.py stop-file) gives up after
60 s without SIGKILL and left the daemon running.

Checks, on slices of the real sources:
1. handler.c: once server_handlers_begin_stop runs, the request loop closes
   the connection before waiting for the next request, so a client's
   retries cannot keep a handler alive.
2. efsd.c: shutdown_drain returns as soon as handlers are gone, and refuses
   (logs, returns -1) when one is still live at the deadline; main exits
   without teardown then, and the deadline is below stop.sh's 60 s.
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile, time

root = Path(__file__).resolve().parents[1]
handler = (root/'src/server/handler.c').read_text()
efsd = (root/'src/server/efsd.c').read_text()
procs = (root/'scripts/server_processes.py').read_text()

def fn(src, sig):
    a = src.index(sig)
    return src[a:src.index('\n}', a)+2]

# 1. Handler gate: checked at the top of the loop, before the next wait.
loop = fn(handler, 'void server_handle_conn(')
gate = loop.index('if (handler_stop_gate())\n            break;')
wait = loop.index('efs_conn_wait_request(conn)')
assert gate < wait, 'stop gate must run before waiting for the next request'
assert loop.index('while (!close_conn) {') < gate

# 2. Drain bound below the stop.sh wait; main refuses and exits untorn.
drain_ms = int(re.search(r'#define EFS_SHUTDOWN_DRAIN_MS (\d+)u', efsd).group(1))
stop_s = int(re.search(r'def retire\(pid,kind,value,expected=None,timeout=(\d+)\)', procs).group(1))
assert drain_ms + 10000 <= stop_s * 1000, (drain_ms, stop_s)
main_tail = efsd[efsd.index('stop_perf_recorder();\n\n'):]
order = [main_tail.index(x) for x in ('server_handlers_begin_stop();', 'conn_wake_readers();',
                                      'shutdown_drain(EFS_SHUTDOWN_DRAIN_MS)',
                                      '_exit(EFS_SHUTDOWN_FORCED_RC);', 'server_packer_stop();')]
assert order == sorted(order), order

code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#define EFS_SHUTDOWN_FORCED_RC 3
static volatile int g_live_conns;
''' + fn(handler, 'static int handler_stop_gate(void)').replace('static int handler_stop_gate', '''static int g_handlers_stopping;
static uint64_t g_handler_stop_closes;
void server_handlers_begin_stop(void)
{
    __atomic_store_n(&g_handlers_stopping, 1, __ATOMIC_RELEASE);
}
static int handler_stop_gate''') + fn(efsd, 'static int shutdown_drain(unsigned deadline_ms)') + r'''
static uint64_t ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
/* A client that retries forever on one pooled connection. */
static int requests;
static void *handler_thread(void *arg){
 (void)arg;
 for(;;){ if(handler_stop_gate())break; requests++; usleep(20000); }
 __sync_fetch_and_sub(&g_live_conns,1);return NULL;
}
static void *stuck_then_done(void *arg){(void)arg;usleep(150000);__sync_fetch_and_sub(&g_live_conns,1);return NULL;}
int main(void){
 pthread_t t;
 /* A retried request stream keeps the handler busy until the stop. */
 g_live_conns=1;assert(!pthread_create(&t,NULL,handler_thread,NULL));
 usleep(100000);assert(requests>=3 && g_live_conns==1);
 server_handlers_begin_stop();
 uint64_t t0=ms();assert(shutdown_drain(5000)==0);uint64_t el=ms()-t0;
 printf("handler released %llu ms after stop (requests served %d)\n",(unsigned long long)el,requests);
 assert(el<500);pthread_join(t,NULL);
 assert(g_handler_stop_closes==1);
 /* Drain returns once handlers finish within the deadline. */
 g_live_conns=1;assert(!pthread_create(&t,NULL,stuck_then_done,NULL));
 t0=ms();assert(shutdown_drain(5000)==0);el=ms()-t0;assert(el>=100&&el<1000);pthread_join(t,NULL);
 /* A handler that never returns: refuse at the deadline, never hang. */
 g_live_conns=1;t0=ms();assert(shutdown_drain(300)==-1);el=ms()-t0;
 printf("stuck handler refused after %llu ms\n",(unsigned long long)el);
 assert(el>=300&&el<1500);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-shutdown-') as d:
    path = Path(d)/'test.c'; path.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc'))+['-std=gnu11', '-Wall', '-Wextra', '-Werror',
                   '-pthread', str(path), '-o', str(path.with_suffix(''))], check=True)
    out = subprocess.run([str(path.with_suffix(''))], check=True, capture_output=True, text=True)
    print(out.stdout.strip())
print('test_shutdown_bounded: OK')
