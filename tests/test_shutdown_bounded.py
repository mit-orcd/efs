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
4. efsd.c (T2): the deadline counts from the stop signal, the recorders
   stop inside it, and after it the stopping thread does no storage or
   stdio I/O itself: the exit report runs on helper threads waited for at
   most EFS_SHUTDOWN_REPORT_MS, so deadline plus report bound stays at
   least 5 s inside stop.sh's 60 s wait. The live blocked-I/O run is
   tests/test_shutdown_blocked_io.py.
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
report_ms = int(re.search(r'#define EFS_SHUTDOWN_REPORT_MS (\d+)u', efsd).group(1))
assert total_ms + report_ms + 5000 <= stop_s * 1000, (total_ms, report_ms, stop_s)
assert 'return total_ms / 5u * 4u;' in fn(efsd, 'static unsigned shutdown_drain_share(')
main_tail = efsd[efsd.index('uint64_t stop_t0 = __atomic_load_n(&g_stop_signal_ms'):]
assert main_tail.index('int exit_rc = shutdown_bounded(&server, stop_t0);') < main_tail.index('return exit_rc;')
assert 'stop_perf_recorder' not in main_tail, 'recorders must stop inside the deadline'
handler_fn = fn(efsd, 'static void sigint_handler(int sig)')
assert '__atomic_compare_exchange_n(&g_stop_signal_ms, &unset, shutdown_now_ms()' in handler_fn
for stop in ('server_packer_stop', 'server_writer_pool_stop', 'server_put_sync_stop',
             'server_reclaim_shutdown', 'server_usage_flush_dirty', 'server_raft_host_stop',
             'server_peer_pool_shutdown', 'server_storage_roots_close'):
    assert stop + '(' not in main_tail, f'{stop} outside the bounded teardown'
bounded = fn(efsd, 'static int shutdown_bounded(struct efsd_server *s, uint64_t t0)')
order = [bounded.index(x) for x in ('server_handlers_begin_stop();', 'conn_wake_readers();',
                                    'shutdown_drain(t0, t0 + shutdown_drain_share(total_ms))',
                                    'shutdown_forced(s, "handler-drain"',
                                    'efsd_pthread_create(&tid, shutdown_teardown_thread',
                                    'pthread_cond_timedwait(&g_sd.cv, &g_sd.mu, &due)',
                                    'shutdown_forced(s, p->name, p->worker',
                                    'pthread_join(tid, NULL);')]
assert order == sorted(order), order
assert 'const uint64_t end = t0 + total_ms;' in bounded
assert 'pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);' in bounded
forced = fn(efsd, 'static void shutdown_forced(')
assert '_exit(EFS_SHUTDOWN_FORCED_RC);' in forced
# After the deadline the stopping thread never touches stdio or storage: the
# report's I/O is on helper threads, and its wait is bounded.
report = fn(efsd, 'static int shutdown_report(')
for body, name in ((forced, 'shutdown_forced'), (report, 'shutdown_report'),
                   (fn(efsd, 'static int shutdown_drain('), 'shutdown_drain')):
    for call in ('fprintf(', 'fputs(', 'fflush(', 'printf(', 'open(', 'fsync(', 'rename('):
        assert call not in body.replace('snprintf(', '').replace('vsnprintf(', ''), (name, call)
assert 'efsd_pthread_create(&tid, shutdown_record_thread, NULL)' in report
assert 'efsd_pthread_create(&tid, shutdown_line_thread, NULL)' in report
assert 'const uint64_t end = shutdown_now_ms() + EFS_SHUTDOWN_REPORT_MS;' in report
assert 'shutdown_now_ms() < end)' in report
record_park = fn(efsd, 'static void shutdown_fault_record_park(const char *step)')
assert record_park.index('#if EFS_FAULTS') < record_park.index('EFS_FAULT_SHUTDOWN_RECORD_PARK') < record_park.index('#else')
phases = efsd[efsd.index('} g_sd_phases[] = {'):efsd.index('};', efsd.index('} g_sd_phases[] = {'))]
names = re.findall(r'\{ "([a-z-]+)", "[^"]+", sd_\w+, (-1|SD_\w+) \}', phases)
assert names == [('recorders', '-1'), ('rejoin', '-1'), ('recovery', '-1'), ('packer', '-1'), ('writer-pool', '-1'),
                 ('put-sync', 'SD_PUT_SYNC'),
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
static uint64_t shutdown_now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void shutdown_say(const char *fmt, ...){(void)fmt;}
''' + fn(handler, 'static int handler_stop_gate(void)').replace('static int handler_stop_gate', '''static int g_handlers_stopping;
static uint64_t g_handler_stop_closes;
void server_handlers_begin_stop(void)
{
    __atomic_store_n(&g_handlers_stopping, 1, __ATOMIC_RELEASE);
}
static int handler_stop_gate''') + fn(efsd, 'static int shutdown_drain(uint64_t t0, uint64_t deadline)') + r'''
#define ms shutdown_now_ms
#define drain_for(d) shutdown_drain(ms(), ms() + (d))
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
 uint64_t t0=ms();assert(drain_for(5000)==0);uint64_t el=ms()-t0;
 printf("handler released %llu ms after stop (requests served %d)\n",(unsigned long long)el,requests);
 assert(el<500);pthread_join(t,NULL);
 assert(g_handler_stop_closes==1);
 /* Drain returns once handlers finish within the deadline. */
 g_live_conns=1;assert(!pthread_create(&t,NULL,stuck_then_done,NULL));
 t0=ms();assert(drain_for(5000)==0);el=ms()-t0;assert(el>=100&&el<1000);pthread_join(t,NULL);
 /* A handler that never returns: refuse at the deadline, never hang. */
 g_live_conns=1;t0=ms();assert(drain_for(300)==-1);el=ms()-t0;
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

# K33: a stopping leader must release its readers before handler drain closes
# the reply paths needed for fresh quorum proofs. Execute the host helper with
# the actual core step-down/campaign functions, including persistence fencing.
raft_host = (root/'src/server/raft_host.c').read_text()
raft_core = (root/'src/raft/raft.c').read_text()
pump = fn(raft_host, 'static void *host_pump(void *arg)')
stop_call = 'host_stop_leading(h, h->s && server_stop_requested());'
assert pump.index('pthread_mutex_lock(&h->mu);') < pump.index(stop_call) < pump.index('host_pending_reads(h);')
assert pump.index('drain_inbox(h);') < pump.rindex(stop_call) < pump.index('(void)efs_raft_tick(')
code = r'''
#include "efs/raft.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define HOST_NGROUPS 2
struct efs_raft {int recv_persisting,role,leader,allow_campaign,read_in_flight;
 uint64_t current_term,vote_bits,commit_index,applied;unsigned resets,exits;};
struct host_group {int hosted;struct efs_raft *r;unsigned group;};
struct efs_raft_host {struct host_group g[HOST_NGROUPS];unsigned published;};
static void leader_exit_note(struct efs_raft*r,uint64_t t,const char*why){assert(t==r->current_term&&why);r->exits++;}
static void read_proof_forget(struct efs_raft*r){r->read_in_flight=0;}
static void reset_election(struct efs_raft*r){r->resets++;}
static void host_publish_group(struct efs_raft_host*h,unsigned g){h->published|=1u<<g;}
''' + fn(raft_core, 'int efs_raft_role(') + fn(raft_core, 'int efs_raft_step_down(') + fn(raft_core, 'void efs_raft_allow_campaign(') + fn(raft_host, 'static void host_stop_leading(') + r'''
int main(void){
 struct efs_raft a={.role=EFS_RAFT_LEADER,.leader=0,.allow_campaign=1,
  .current_term=7,.read_in_flight=1,.commit_index=19,.applied=18};
 struct efs_raft b={.role=EFS_RAFT_CANDIDATE,.leader=-1,.allow_campaign=1,
  .current_term=9,.recv_persisting=1,.commit_index=25,.applied=24};
 struct efs_raft_host h={.g={{1,&a,0},{1,&b,2}}};
 host_stop_leading(&h,0);assert(a.role==EFS_RAFT_LEADER&&a.allow_campaign&&b.allow_campaign&&!h.published);
 host_stop_leading(&h,1);
 assert(a.role==EFS_RAFT_FOLLOWER&&!a.allow_campaign&&!a.read_in_flight&&a.exits==1&&a.resets==1);
 assert(b.role==EFS_RAFT_CANDIDATE&&!b.allow_campaign&&!b.resets&&h.published==1);
 b.recv_persisting=0;host_stop_leading(&h,1);
 assert(b.role==EFS_RAFT_FOLLOWER&&b.resets==1&&h.published==5);
 host_stop_leading(&h,1);assert(a.exits==1&&a.resets==1&&b.resets==1);
 assert(a.current_term==7&&a.commit_index==19&&a.applied==18);
 assert(b.current_term==9&&b.commit_index==25&&b.applied==24);
 h.g[0].hosted=0;a.role=EFS_RAFT_LEADER;a.allow_campaign=1;
 host_stop_leading(&h,1);assert(a.role==EFS_RAFT_LEADER&&a.allow_campaign);
 h.g[0].hosted=1;h.g[0].r=NULL;host_stop_leading(&h,1);
 puts("shutdown leadership: pump-owned demotion before proof/tick, no campaign, receive-persistence fence retained, committed/apply state unchanged PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-stop-leadership-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror',
                   '-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)

# Execute the real rejoin lifecycle: teardown must wait for a paused
# HELLO/rejoin before the shared owners can be destroyed.
cluster = Path(os.environ.get('CLUSTER_WORKER_SOURCE', root/'src/server/cluster.c')).read_text()
code = r"""
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
struct efsd_server {pthread_t rejoin_tid;int rejoin_started;};
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static unsigned entered,released,departed,join_calls,done,fail_create;
static void *rejoin_thread(void *s){(void)s;pthread_mutex_lock(&mu);entered=1;pthread_cond_broadcast(&cv);
 while(!released)pthread_cond_wait(&cv,&mu);departed=1;pthread_mutex_unlock(&mu);return NULL;}
static int efsd_pthread_create(pthread_t*t,void*(*fn)(void*),void*s){return fail_create?1:pthread_create(t,NULL,fn,s);}
static int observed_join(pthread_t t,void**r){__atomic_add_fetch(&join_calls,1,__ATOMIC_RELEASE);return pthread_join(t,r);}
#define pthread_join observed_join
""" + fn(cluster, 'void server_start_rejoin(') + fn(cluster, 'void server_stop_rejoin(') + r"""
#undef pthread_join
static void *stopper(void*s){server_stop_rejoin(s);__atomic_store_n(&done,1,__ATOMIC_RELEASE);return NULL;}
int main(void){struct efsd_server s={0};
 fail_create=1;server_start_rejoin(&s);assert(!s.rejoin_started);server_stop_rejoin(&s);assert(!join_calls);
 fail_create=0;server_start_rejoin(&s);server_start_rejoin(&s); /* no duplicate owner */
 pthread_mutex_lock(&mu);while(!entered)pthread_cond_wait(&cv,&mu);pthread_mutex_unlock(&mu);
 pthread_t stop;assert(!pthread_create(&stop,NULL,stopper,&s));
 for(unsigned i=0;i<1000&&!__atomic_load_n(&join_calls,__ATOMIC_ACQUIRE);i++)usleep(1000);
 assert(join_calls==1);usleep(20000);assert(!__atomic_load_n(&done,__ATOMIC_ACQUIRE));
 pthread_mutex_lock(&mu);released=1;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&mu);
 assert(!pthread_join(stop,NULL));assert(done&&departed&&!s.rejoin_started);
 server_stop_rejoin(&s);assert(join_calls==1);
 puts("rejoin worker: failed start, duplicate start, paused owner joined before teardown, idempotent stop PASS");
}
"""
with tempfile.TemporaryDirectory(prefix='efs-rejoin-join-') as d:
    p = Path(d)/'test.c'; p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc'))+['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-pthread', str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True, timeout=10)

main = efsd[efsd.index('int main(int argc, char **argv)'):]
assert main.index('__atomic_store_n(&server.running') < main.index('if (rejoin_needed) server_start_rejoin')
assert main.index('if (server_raft_host_start') < main.index('if (rejoin_needed) server_start_rejoin')
assert phases.index('"rejoin"') < phases.index('"raft-host"') < phases.index('"peer-pool"')
