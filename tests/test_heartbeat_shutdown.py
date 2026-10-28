#!/usr/bin/env python3
"""A heartbeat already observing the host must finish before host teardown."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
from sanitize_env import cflags, wrap

root = Path(__file__).resolve().parents[1]
text = (root / 'src/server/cluster.c').read_text()
start = text.index('void server_start_heartbeat(')
end = text.index('/* Rejoin thread:', start)
efsd = (root / 'src/server/efsd.c').read_text()
stop = efsd[efsd.index('static int sd_raft('):efsd.index('static int sd_peers(')]
assert stop.index('server_stop_heartbeat(s)') < stop.index('server_raft_host_stop()')
code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#define EFS_HEARTBEAT_MS 50
struct efsd_server {int running;pthread_t heartbeat_tid;int heartbeat_started;};
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static int entered,release,exited,joined,fail_spawn;
static void *heartbeat_thread(void *arg) {
    (void)arg;pthread_mutex_lock(&mu);entered=1;pthread_cond_broadcast(&cv);
    while(!release)pthread_cond_wait(&cv,&mu);
    exited=1;pthread_mutex_unlock(&mu);return NULL;
}
static int efsd_pthread_create(pthread_t *tid,void *(*fn)(void *),void *arg) {
    return fail_spawn ? 1 : pthread_create(tid,NULL,fn,arg);
}
''' + text[start:end] + r'''
static void *stop(void *arg) {
    server_stop_heartbeat(arg);
    pthread_mutex_lock(&mu);assert(exited);joined=1;pthread_mutex_unlock(&mu);return NULL;
}
int main(void) {
    struct efsd_server server={.running=1};
    server_stop_heartbeat(&server); /* startup did not start a heartbeat */
    fail_spawn=1;server_start_heartbeat(&server);assert(!server.heartbeat_started);
    fail_spawn=0;server.running=1;server_start_heartbeat(&server);assert(server.heartbeat_started);
    pthread_mutex_lock(&mu);while(!entered)pthread_cond_wait(&cv,&mu);pthread_mutex_unlock(&mu);
    pthread_t stopper;assert(!pthread_create(&stopper,NULL,stop,&server));
    while(__atomic_load_n(&server.running,__ATOMIC_ACQUIRE))usleep(100);
    pthread_mutex_lock(&mu);assert(!joined && !exited);release=1;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&mu);
    pthread_join(stopper,NULL);assert(joined && !server.heartbeat_started);
    server_stop_heartbeat(&server);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-heartbeat-stop-') as directory:
    source = Path(directory) / 'stop.c'
    binary = source.with_suffix('')
    source.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + cflags() + [
        '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-pthread', str(source), '-o', str(binary)], check=True)
    subprocess.run(wrap() + [str(binary)], check=True, timeout=15)
print('heartbeat shutdown: in-flight status drained before host free, failed start and repeated stop PASS')
