#!/usr/bin/env python3
"""Production STALE classifier must have pull authority before ACK or replay.

K38: a chunk is done only when the host holds the object the round SENT; a
newer PUT that landed after the pull (its own generation in the local table)
is not a commit, and a chunk the round did not send is left to the rebuild.
"""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
w=(root/'src/client/write.c').read_text()
def function(name):
    m=re.search(r'^(?:static )?[^\n;]*\b'+name+r'\([^;]+?\)\n\{',w,re.M)
    return w[m.start():w.index('\n}',m.end())+2]
a=w.index('struct stale_pair {');pair=w[a:w.index('\n};',a)+3]
a=w.index('struct stale_sent {');pair+='\n'+w[a:w.index('\n};',a)+3]
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
/* The round sent object 10 (snapshot 11) for chunk (1,0). */
static struct efs_chunk_rec sent[1]={{.ino=1,.chunk_index=0,.chunk_generation=10}};
static uint64_t sent_seq[1]={11};
static uint32_t nsent=1;
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
'''+function('stale_pair_cmp')+'\n'+function('stale_sent_cmp')+'\n'+function('stale_repull_replay')+r'''
static void reset(void) {
    memset(&map,0,sizeof(map));map.generation=7;
    verdicts=replays=commits=0;pull_error=absent=verdict_error=replay_error=fail_bits=0;
}
int main(void) {
    efs_ino_t inos[2]={1,1};uint32_t cis[2]={0,0};
    struct dirty_snap ds={2,inos,cis};
    reset();assert(!stale_repull_replay(&ds,1,sent,sent_seq,nsent));assert(verdicts==1&&replays==1&&!commits);
    reset();map.generation=10;pull_error=EFS_ERR_BUSY;
    assert(stale_repull_replay(&ds,1,sent,sent_seq,nsent)==EFS_ERR_BUSY);
    assert(!commits&&!verdicts&&!replays&&inos[0]&&inos[1]);
    reset();fail_bits=1;
    assert(stale_repull_replay(&ds,1,sent,sent_seq,nsent)==EFS_ERR_NOMEM);
    assert(!commits&&!verdicts&&!replays&&inos[0]&&inos[1]);
    reset();map.generation=10;absent=1;
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,nsent));
    assert(!commits&&verdicts==1&&replays==1&&!observed.generation&&inos[0]);
    reset();verdict_error=1;
    assert(stale_repull_replay(&ds,1,sent,sent_seq,nsent)==EFS_ERR_IO);
    assert(verdicts==1&&!replays&&!commits&&inos[0]);
    reset();replay_error=EFS_ERR_NO_QUORUM;
    assert(stale_repull_replay(&ds,1,sent,sent_seq,nsent)==EFS_ERR_NO_QUORUM);
    assert(verdicts==1&&replays==1&&!commits&&inos[0]);
    reset();map.generation=10;
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,nsent));
    assert(commits==1&&!verdicts&&!replays&&!inos[0]&&!inos[1]);
    /* K38: a newer PUT (object 12) landed after the pull and set the table to
     * its own generation: the host does not hold what the round sent. */
    inos[0]=inos[1]=1;
    reset();map.generation=12;sent[0].chunk_generation=10;
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,nsent));
    assert(!commits&&verdicts==1&&replays==1&&inos[0]&&inos[1]);
    /* the same for a span: our newer span in the table's list is not the sent one */
    reset();map.generation=7;map.ndelta=1;map.deltas[0].generation=12;map.deltas[0].off=0;map.deltas[0].len=8;
    sent[0].chunk_generation=10;sent[0].delta_len=8;
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,nsent)&&!commits&&replays==1);
    map.deltas[0].generation=10;reset();map.generation=7;map.ndelta=1;map.deltas[0].generation=10;map.deltas[0].len=8;
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,nsent)&&commits==1&&!replays&&!inos[0]);
    /* a chunk the round did not send: neither done nor replayed */
    inos[0]=inos[1]=1;sent[0].delta_len=0;
    reset();map.generation=10;sent[0].ino=2;
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,nsent)&&!commits&&!verdicts&&!replays&&inos[0]);
    puts("STALE recovery: authoritative pull, absence, retained failures, dedupe, prefix ACK, sent-object identity (K38) PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-stale-recovery-') as directory:
    p=Path(directory)/'test.c';p.write_text(source)
    command=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-I'+str(root/'include')]
    command+=shlex.split(os.environ.get('WB_RUNTIME_CFLAGS',''))
    subprocess.run(command+[str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
