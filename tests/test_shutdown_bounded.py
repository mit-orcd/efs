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
   (logs, returns -1) when one is still live at its share of the deadline.
3. efsd.c (R4): one total deadline, below stop.sh's 60 s, covers the drain
   and every later stop phase; the phases run on one teardown thread that
   main waits for with a timed wait, never an unbounded join, and a missed
   deadline exits rc 3 without teardown. The live stuck-worker run is
   tests/test_shutdown_stuck_worker.py.
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

# 2./3. One total deadline below the stop.sh wait; the drain gets 4/5.
total_ms = int(re.search(r'#define EFS_SHUTDOWN_TOTAL_MS (\d+)u', efsd).group(1))
stop_s = int(re.search(r'def retire\(pid,kind,value,expected=None,timeout=(\d+)\)', procs).group(1))
assert total_ms + 5000 <= stop_s * 1000, (total_ms, stop_s)
assert 'return total_ms / 5u * 4u;' in fn(efsd, 'static unsigned shutdown_drain_share(')
main_tail = efsd[efsd.index('stop_perf_recorder();\n\n'):]
assert main_tail.index('int exit_rc = shutdown_bounded(&server);') < main_tail.index('return exit_rc;')
for stop in ('server_packer_stop', 'server_writer_pool_stop', 'server_put_sync_stop',
             'server_reclaim_shutdown', 'server_usage_flush_dirty', 'server_raft_host_stop',
             'server_peer_pool_shutdown', 'server_storage_roots_close'):
    assert stop + '(' not in main_tail, f'{stop} outside the bounded teardown'
bounded = fn(efsd, 'static int shutdown_bounded(struct efsd_server *s)')
order = [bounded.index(x) for x in ('server_handlers_begin_stop();', 'conn_wake_readers();',
                                    'shutdown_drain(shutdown_drain_share(total_ms))',
                                    'shutdown_forced(s, "handler-drain"',
                                    'efsd_pthread_create(&tid, shutdown_teardown_thread',
                                    'pthread_cond_timedwait(&g_sd.cv, &g_sd.mu, &due)',
                                    'shutdown_forced(s, p->name, p->worker',
                                    'pthread_join(tid, NULL);')]
assert order == sorted(order), order
assert 'const uint64_t t0 = shutdown_now_ms(), end = t0 + total_ms;' in bounded
assert 'pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);' in bounded
assert '_exit(EFS_SHUTDOWN_FORCED_RC);' in fn(efsd, 'static void shutdown_forced(')
phases = efsd[efsd.index('} g_sd_phases[] = {'):efsd.index('};', efsd.index('} g_sd_phases[] = {'))]
names = re.findall(r'\{ "([a-z-]+)", "[^"]+", sd_\w+, (-1|SD_\w+) \}', phases)
assert names == [('packer', '-1'), ('writer-pool', '-1'), ('put-sync', 'SD_PUT_SYNC'),
                 ('reclaim', 'SD_RECLAIM'), ('usage-flush', 'SD_USAGE'), ('raft-host', '-1'),
                 ('peer-pool', '-1'), ('storage-roots', '-1')], names
# Fault hooks exist only in EFS_FAULTS builds.
park = fn(efsd, 'int server_shutdown_fault_park(const char *worker)')
assert park.index('#if EFS_FAULTS') < park.index('getenv("EFS_FAULT_SHUTDOWN_PARK")') < park.index('#else')
total = fn(efsd, 'static unsigned shutdown_total_ms(void)')
assert total.index('#if EFS_FAULTS') < total.index('EFS_FAULT_SHUTDOWN_TOTAL_MS') < total.index('#endif')

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
