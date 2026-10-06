#!/usr/bin/env python3
"""Exercise production REPORT admission/accounting with deterministic drain outcomes."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
w=(root/'src/client/write.c').read_text()
def function(name):
    m=re.search(r'^(?:static )?[^\n;]*\b'+name+r'\([^;]+?\)\n\{',w,re.M)
    return w[m.start():w.index('\n}',m.end())+2]
def structure(name):
    a=w.index('struct '+name+' {'); return w[a:w.index('\n};',a)+3]
a=w.index('#define REPORT_PRESSURE_RECORD_BYTES')
b=w.index('\nint efs_client_ino_is_dirty(',a)
source=r'''
#include "client_internal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
struct efs_client g_client;
#define IDIRTY_BUCKETS 4096
#define PUB_SLOTS 16
'''+structure('ino_dirty')+'\n'+structure('pub_slot')+r'''
static struct ino_dirty *g_idirty[IDIRTY_BUCKETS];
static struct pub_slot pub_slots[PUB_SLOTS];
static pthread_cond_t pub_cv=PTHREAD_COND_INITIALIZER;
static __thread uint64_t deadline;
static int drain_mode, kicks;
void efs_client_ensure_dir_locks(void) {}
uint64_t efs_client_rpc_deadline_ms(void) { return deadline; }
'''+w[a:b]+'\n'+function('pub_release')+'\n'+structure('dirty_snap')+r'''
static struct ino_dirty restored;
void efs_client_mark_ino_dirty(efs_ino_t ino) {
    restored.ino=ino; restored.ino_marked=1;
    g_client.dirty_ino_count=1; g_idirty[ino%IDIRTY_BUCKETS]=&restored;
}
void efs_client_mark_chunk_dirty(efs_ino_t ino, uint32_t ci) {
    (void)ci; restored.ino=ino; restored.chunk_count=1;
    g_client.dirty_chunk_count=1; g_idirty[ino%IDIRTY_BUCKETS]=&restored;
}
'''+function('idirty_find')+'\n'+function('dirty_snap_remark_locked')+r'''
static void meta_flush_enqueue(efs_ino_t ino) {
    (void)ino;
    pthread_mutex_lock(&g_client.dirty_mu);
    ++kicks;
    if (drain_mode == 1) { /* Detach is not an acknowledgement. */
        pub_slots[0].used=1;
        pub_slots[0].pressure_records=g_client.dirty_ino_count;
        pub_slots[0].since_ms=report_clock_ms()-100;
        g_client.dirty_ino_count=0;
        drain_mode=0;
    } else if (drain_mode == 2) {
        g_client.dirty_ino_count=0;
        pthread_mutex_unlock(&g_client.dirty_mu);
        pub_release(0);
        return;
    }
    pthread_mutex_unlock(&g_client.dirty_mu);
}
void efs_client_rpc_set_deadline_ms(uint64_t value) { deadline=value; }
static int report_dirty_ino_run(efs_ino_t ino, int sync) {
    (void)ino;
    assert(!sync || (deadline && deadline<=report_clock_ms()+8000));
    return EFS_ERR_BUSY;
}
'''+function('efs_client_report_dirty_ino')+r'''
static void *request(void *arg) {
    (void)arg;
    assert(efs_client_report_admit(2)==EFS_OK);
    struct efs_report_pressure s;
    efs_client_report_pressure_stats(&s);
    assert(s.reserved_records+s.pending_records<=8);
    usleep(1000);
    efs_client_report_unreserve();
    return NULL;
}
int main(void) {
    assert(!pthread_mutex_init(&g_client.dirty_mu,NULL));
    setenv("EFS_REPORT_MAX_RECORDS","8",1);
    setenv("EFS_REPORT_MAX_BYTES","16384",1);
    setenv("EFS_REPORT_ADMIT_MS","40",1);
    g_client.meta_batch=1;
    assert(efs_client_report_admit(9)==EFS_ERR_BUSY);
    assert(!report_reserved);
    /* A snapshot in flight stays charged; failed draining changes no marks. */
    g_client.dirty_ino_count=7; drain_mode=1;
    uint64_t start=report_clock_ms();
    assert(efs_client_report_admit(2)==EFS_ERR_BUSY);
    assert(report_clock_ms()-start<250);
    struct efs_report_pressure s;
    efs_client_report_pressure_stats(&s);
    assert(s.pending_records==7 && s.inflight_records==7 &&
           s.pending_bytes==14336 && s.oldest_ms>=100 && !s.reserved_records &&
           s.admission_waits==1 && s.admission_timeouts==1);
    /* Only successful completion releases detached records. */
    drain_mode=2;
    assert(efs_client_report_admit(2)==EFS_OK);
    assert(report_reserved==2);
    efs_client_report_unreserve();
    assert(!report_reserved);
    /* Existing request deadlines take precedence over the admission budget. */
    drain_mode=0; g_client.dirty_ino_count=8;
    deadline=report_clock_ms()-1;
    assert(efs_client_report_admit(1)==EFS_ERR_BUSY);
    assert(g_client.dirty_ino_count==8 && !report_reserved);
    deadline=0; g_client.dirty_ino_count=0;
    assert(efs_client_report_dirty_ino(1,1)==EFS_ERR_BUSY && deadline==0);
    deadline=report_clock_ms()+500;
    uint64_t saved=deadline;
    assert(efs_client_report_dirty_ino(1,1)==EFS_ERR_BUSY && deadline==saved);
    deadline=0;
    /* Byte limit can be tighter than the record limit. */
    report_limit_bytes=2048;
    assert(efs_client_report_admit(2)==EFS_ERR_BUSY);
    report_limit_bytes=16384;
    pthread_t t[16]; drain_mode=2;
    for(int i=0;i<16;++i) assert(!pthread_create(&t[i],NULL,request,NULL));
    for(int i=0;i<16;++i) assert(!pthread_join(t[i],NULL));
    assert(!report_reserved);
    /* Live chunk marks include their companion inode payload. */
    g_client.dirty_ino_count=1; g_client.dirty_chunk_count=2;
    efs_client_report_pressure_stats(&s);
    assert(s.pending_records==5 && s.pending_bytes==10240);
    uint64_t keys[2]={0,9};
    efs_ino_t inos[1]={9}; uint32_t indices[1]={0};
    struct dirty_snap ds={.ino_keys=keys,.ino_mask=1,.chunk_inos=inos,
        .chunk_idxs=indices,.chunk_count=1,.since_ms=report_clock_ms()-1000};
    restored.since_ms=report_clock_ms();
    pub_slots[0].used=1; pub_slots[0].pressure_records=3;
    dirty_snap_remark_locked(&ds); /* Actual failed REPORT restore path. */
    pub_release(0);
    efs_client_report_pressure_stats(&s);
    assert(s.pending_records==3 && !s.inflight_records && s.oldest_ms>=1000);
    g_client.meta_batch=0;
    assert(efs_client_report_admit(1000)==EFS_OK && !report_reserved);
    puts("REPORT pressure: live/inflight accounting, limits, bounded timeout, drain and concurrency PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-report-pressure-') as directory:
    p=Path(directory)/'test.c';p.write_text(source)
    cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client')]
    cmd+=shlex.split(os.environ.get('REPORT_TEST_CFLAGS',''))
    subprocess.run(cmd+[str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
