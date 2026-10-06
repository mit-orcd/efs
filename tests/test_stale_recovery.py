#!/usr/bin/env python3
"""Production STALE classifier must have pull authority before ACK or replay."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
w=(root/'src/client/write.c').read_text()
def function(name):
    m=re.search(r'^(?:static )?[^\n;]*\b'+name+r'\([^;]+?\)\n\{',w,re.M)
    return w[m.start():w.index('\n}',m.end())+2]
a=w.index('struct stale_pair {');pair=w[a:w.index('\n};',a)+3]
source=r'''
#include "efs/protocol.h"
#include "efs/metadata.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fail_bits;
static void *test_calloc(size_t n,size_t z) {return fail_bits?NULL:calloc(n,z);}
#define calloc test_calloc
struct dirty_snap {uint64_t chunk_count;efs_ino_t *chunk_inos;uint32_t *chunk_idxs;};
'''+pair+r'''
static int pull_error, absent, verdict_error, replay_error;
static unsigned verdicts,replays,commits;
static struct efs_chunk_entry map,observed;
static uint64_t report_mono_ms(void) {return 0;}
static int efs_client_pull_chunks_range_absent(efs_ino_t ino,uint32_t lo,
                                             uint32_t hi,uint8_t *bits) {
    (void)ino;(void)lo;(void)hi;bits[0]=absent?1:0;return pull_error;
}
static int export_chunk_copy(efs_ino_t ino,uint32_t ci,struct efs_chunk_entry *out) {
    (void)ino;(void)ci;*out=map;return EFS_OK;
}
static int putid_fill(efs_ino_t ino,uint32_t ci,struct efs_chunk_rec *out,uint64_t *seq) {
    (void)ino;(void)ci;out->chunk_generation=10;*seq=11;return 1;
}
static int dcache_object_of(efs_ino_t ino,uint32_t ci,struct efs_chunk_rec *out,uint64_t *seq) {
    (void)ino;(void)ci;(void)out;(void)seq;return 0;
}
static int dcache_note_committed(efs_ino_t ino,uint32_t ci,uint64_t gen,uint64_t obj,uint64_t seq) {
    (void)ino;(void)ci;(void)gen;(void)obj;(void)seq;commits++;return 1;
}
static void putid_drop_snapshot(efs_ino_t ino,uint32_t ci,uint64_t obj,uint64_t seq) {
    (void)ino;(void)ci;(void)obj;(void)seq;
}
static int dcache_trace_on(void) {return 0;}
static uint64_t dcache_base_gen_of(efs_ino_t ino,uint32_t ci,uint64_t g) {
    (void)ino;(void)ci;return g;
}
static int dcache_cycle_verdict(efs_ino_t ino,uint32_t ci,const struct efs_chunk_entry *ce,uint64_t seq) {
    (void)ino;(void)ci;assert(seq==11);verdicts++;observed=*ce;return verdict_error;
}
static int replay_fan_run(struct stale_pair *rp,uint64_t n) {
    if(!n)return EFS_OK;
    assert(n==1&&rp[0].absent==absent);replays++;return replay_error;
}
'''+function('stale_pair_cmp')+'\n'+function('stale_repull_replay')+r'''
static void reset(void) {
    memset(&map,0,sizeof(map));map.generation=7;
    verdicts=replays=commits=0;pull_error=absent=verdict_error=replay_error=fail_bits=0;
}
int main(void) {
    efs_ino_t inos[2]={1,1};uint32_t cis[2]={0,0};
    struct dirty_snap ds={2,inos,cis};
    reset();assert(!stale_repull_replay(&ds,1));assert(verdicts==1&&replays==1&&!commits);
    reset();map.generation=10;pull_error=EFS_ERR_BUSY;
    assert(stale_repull_replay(&ds,1)==EFS_ERR_BUSY);
    assert(!commits&&!verdicts&&!replays&&inos[0]&&inos[1]);
    reset();fail_bits=1;
    assert(stale_repull_replay(&ds,1)==EFS_ERR_NOMEM);
    assert(!commits&&!verdicts&&!replays&&inos[0]&&inos[1]);
    reset();map.generation=10;absent=1;
    assert(!stale_repull_replay(&ds,1));
    assert(!commits&&verdicts==1&&replays==1&&!observed.generation&&inos[0]);
    reset();verdict_error=1;
    assert(stale_repull_replay(&ds,1)==EFS_ERR_IO);
    assert(verdicts==1&&!replays&&!commits&&inos[0]);
    reset();replay_error=EFS_ERR_NO_QUORUM;
    assert(stale_repull_replay(&ds,1)==EFS_ERR_NO_QUORUM);
    assert(verdicts==1&&replays==1&&!commits&&inos[0]);
    reset();map.generation=10;
    assert(!stale_repull_replay(&ds,1));
    assert(commits==1&&!verdicts&&!replays&&!inos[0]&&!inos[1]);
    puts("STALE recovery: authoritative pull, absence, retained failures, dedupe and prefix ACK PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-stale-recovery-') as directory:
    p=Path(directory)/'test.c';p.write_text(source)
    command=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-I'+str(root/'include')]
    command+=shlex.split(os.environ.get('WB_RUNTIME_CFLAGS',''))
    subprocess.run(command+[str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
