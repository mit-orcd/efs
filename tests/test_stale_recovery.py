#!/usr/bin/env python3
"""Production STALE classifier must have pull authority before ACK or replay.

K38: a chunk is done only when the host holds the object the round SENT; a
newer PUT that landed after the pull (its own generation in the local table)
is not a commit, and a chunk the round did not send is left to the rebuild.
"""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
current=Path(os.environ.get('OWNED_RETRY_SOURCE',root/'src/client/write.c')).read_text()
w=Path(os.environ.get('STALE_CLASSIFIER_SOURCE',root/'src/client/write.c')).read_text()
def function(name,source=None):
    source=w if source is None else source
    m=re.search(r'^(?:static )?[^\n;]*\b'+name+r'\([^;]+?\)\n\{',source,re.M)
    return source[m.start():source.index('\n}',m.end())+2]
a=w.index('struct stale_pair {');pair=w[a:w.index('\n};',a)+3]
a=w.index('struct stale_sent {');pair+='\n'+w[a:w.index('\n};',a)+3]
a=current.index('struct put_id {');put=current[a:current.index('\n};',a)+3]
a=current.index('struct stale_retry_view {');put+='\n'+current[a:current.index('\n};',a)+3]
source=r'''
#include "efs/protocol.h"
#include "efs/metadata.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fail_bits;
static void *test_calloc(size_t n,size_t z) {return fail_bits?NULL:calloc(n,z);}
#define calloc test_calloc
struct dirty_snap {uint64_t chunk_count;efs_ino_t *chunk_inos;uint32_t *chunk_idxs;};
'''+pair+r'''
static int pull_error, absent, verdict_error, replay_error;
static unsigned verdicts,replays,commits,pulls;
static int mixed;static uint32_t pulled_lo,pulled_hi;
static struct efs_chunk_entry map,observed;
static uint64_t report_mono_ms(void) {return 0;}
static int efs_client_pull_chunks_range_absent(efs_ino_t ino,uint32_t lo,
                                             uint32_t hi,uint8_t *bits) {
    (void)ino;pulled_lo=lo;pulled_hi=hi;pulls++;bits[0]=absent?1:0;return pull_error;
}
static int export_chunk_copy(efs_ino_t ino,uint32_t ci,struct efs_chunk_entry *out) {
    (void)ino;*out=map;if(mixed&&ci==1)out->generation=20;return EFS_OK;
}
/* The round sent object 10 (snapshot 11) for chunk (1,0). */
static struct efs_chunk_rec sent[2]={{.ino=1,.chunk_index=0,.chunk_generation=10}};
static uint64_t sent_seq[2]={11,11};
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
'''+put+r'''
static struct put_id owner;
static pthread_mutex_t g_putid_mu=PTHREAD_MUTEX_INITIALIZER;
static struct efs_msg_inode_writer_view_reply authority;
static int attr_error;static unsigned attr_reads;
static int ticket_live=1;static unsigned ticket_checks;
int efs_client_put_retry_admitted(efs_ino_t ino,uint32_t ci,uint64_t object,uint64_t seq)
{assert(ino==1&&ci==0&&object==10&&seq==81);ticket_checks++;return ticket_live;}
static struct put_id *putid_find(efs_ino_t ino,uint32_t ci) {return owner.ino==ino&&owner.ci==ci?&owner:NULL;}
static int efs_client_rpc_writer_view(efs_ino_t ino,uint64_t gen,uint32_t ci,struct efs_msg_inode_writer_view_reply *out)
{assert(ino==1&&gen==22&&ci==0);attr_reads++;*out=authority;return attr_error;}
'''+function('stale_retry_owned',current)+'\n'+function('stale_pair_cmp')+'\n'+function('stale_sent_cmp')+'\n'+function('stale_repull_replay')+r'''
static void reset(void) {
    memset(&map,0,sizeof(map));map.generation=7;
    verdicts=replays=commits=pulls=0;pull_error=absent=verdict_error=replay_error=fail_bits=0;
}
int main(void) {
    (void)stale_retry_owned; /* baseline classifier may not call this helper */
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
    /* A transiently aborted atomic unit is not proof that its bytes moved.
     * Its fresh mapping and FileID/epoch still match: retain the exact PUT
     * for one new publication, without ACK, release, or physical re-upload. */
    inos[0]=inos[1]=1;sent[0]=(struct efs_chunk_rec){.ino=1,.chunk_index=0,
      .chunk_generation=10,.base_gen=7,.put_seq=81,.file_generation=22,
      .publish_epoch=3,.publish_flags=EFS_CHUNK_REC_F_FRESH_OBJECT|EFS_CHUNK_REC_F_CAPTURED_EPOCH};
    owner=(struct put_id){.ino=1,.ci=0,.gen=10,.seq=11,.put_seq=81,.file_generation=22,.publish_epoch=3};
    authority=(struct efs_msg_inode_writer_view_reply){.generation=22,.authority_epoch=3};attr_reads=0;attr_error=0;
    reset();assert(!stale_repull_replay(&ds,1,sent,sent_seq,1));
    assert(!commits&&!replays&&!verdicts&&inos[0]&&inos[1]&&owner.stale_retry&&attr_reads==1);
    /* Repeated unchanged refusals retain the same ADMITTED body, never ACK. */
    for(unsigned i=0;i<8;i++){reset();assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&!replays&&!commits);}
    assert(ticket_checks==8);
    ticket_live=EFS_ERR_BUSY;reset();assert(stale_repull_replay(&ds,1,sent,sent_seq,1)==EFS_ERR_BUSY&&!replays&&!commits);
    ticket_live=0;reset();assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&replays==1&&!commits);
    ticket_live=1;
    /* A span with the same immutable range/base and unchanged observed map
     * is reusable too; a changed range/base must take ordinary rebase. */
    sent[0].delta_len=16;sent[0].delta_off=7;
    owner.delta_len=16;owner.delta_off=7;owner.span_base_gen=7;
    owner.stale_retry=0;reset();
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&!replays&&!commits);
    reset();assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&!replays&&!commits);
    owner.span_base_gen=8;reset();assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&replays==1);
    owner.span_base_gen=7;owner.delta_off=8;reset();assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&replays==1);
    owner.delta_off=7;reset();map.generation=8;assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&replays==1);
    owner.delta_off=owner.delta_len=0;sent[0].delta_off=sent[0].delta_len=0;
    owner.stale_retry=0;authority.authority_epoch=4;reset();
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&replays==1&&!owner.stale_retry);
    authority.authority_epoch=3;authority.generation=23;reset();
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&replays==1&&!owner.stale_retry);
    authority.generation=22;attr_error=EFS_ERR_BUSY;reset();
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&replays==1&&!owner.stale_retry);
    attr_error=0;owner.seq=12;reset();
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&replays==1&&!owner.stale_retry);
    owner.seq=11;reset();map.ndelta=1;map.delta_seq=4;
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&replays==1&&!owner.stale_retry);
    reset();map.generation=8;
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&replays==1&&!owner.stale_retry);
    /* The staging row naming our own PUT is never an absence proof. */
    reset();map.generation=10;absent=1;sent[0].base_gen=0;
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&!commits&&!replays&&owner.stale_retry&&inos[0]);
    /* No captured current epoch => no reuse of a post-truncate image. */
    owner.stale_retry=0;sent[0].publish_flags=EFS_CHUNK_REC_F_FRESH_OBJECT;
    reset();absent=1;map.generation=10;
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&replays==1&&!owner.stale_retry);
    sent[0].publish_flags|=EFS_CHUNK_REC_F_CAPTURED_EPOCH;sent[0].base_gen=7;
    reset();owner.gen=12;
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&replays==1&&!owner.stale_retry);
    owner.gen=10;owner.nodes[0]=2;reset();
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&replays==1&&!owner.stale_retry);
    owner.nodes[0]=0;reset();sent[0].base_gen=EFS_CHUNK_BASE_UNCOND;
    /* An explicit full overwrite is not conditional on row absence or its
     * base/delta set. It remains the same owned image, never an ACK. */
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&!replays&&!commits&&!pulls&&owner.stale_retry);
    reset();map.generation=8;map.ndelta=1;map.delta_seq=4;
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&!replays&&!commits&&!pulls);
    reset();pull_error=EFS_ERR_BUSY;
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&!pulls&&!commits&&!replays);
    reset();ticket_live=EFS_ERR_BUSY;
    assert(stale_repull_replay(&ds,1,sent,sent_seq,1)==EFS_ERR_BUSY&&!pulls&&!commits&&!replays);
    ticket_live=0;reset();map.generation=10;
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&pulls==1&&commits==1&&!replays);
    inos[0]=inos[1]=1;ticket_live=1;
    reset();authority.authority_epoch=4;owner.stale_retry=0;
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&replays==1&&!owner.stale_retry);
    authority.authority_epoch=3;reset();sent[0].delta_len=16;owner.delta_len=16;
    assert(!stale_repull_replay(&ds,1,sent,sent_seq,1)&&replays==1&&!owner.stale_retry);
    sent[0].delta_len=owner.delta_len=0;
    sent[0].base_gen=7;reset();pull_error=EFS_ERR_BUSY;attr_reads=0;
    assert(stale_repull_replay(&ds,1,sent,sent_seq,1)==EFS_ERR_BUSY&&!owner.stale_retry&&!attr_reads&&!replays);
    /* A mixed unit fetches only its conditional chunk. A failed conditional
     * proof retains both records, including the unconditional owner, no ACK. */
    mixed=1;cis[1]=1;owner.stale_retry=0;
    sent[0].base_gen=EFS_CHUNK_BASE_UNCOND;sent[1]=sent[0];
    sent[1].chunk_index=1;sent[1].chunk_generation=11;sent[1].base_gen=20;
    reset();assert(!stale_repull_replay(&ds,1,sent,sent_seq,2));
    assert(pulls==1&&pulled_lo==1&&pulled_hi==2&&!commits&&replays==1&&inos[0]&&inos[1]);
    reset();pull_error=EFS_ERR_BUSY;
    assert(stale_repull_replay(&ds,1,sent,sent_seq,2)==EFS_ERR_BUSY);
    assert(pulls==1&&!commits&&!replays&&inos[0]&&inos[1]);
    mixed=0;cis[1]=0;
    /* One authority read covers this inode's chunks in this one round. */
    struct stale_retry_view cached={0};reset();attr_reads=0;
    sent[0].base_gen=7;sent[0].publish_flags|=EFS_CHUNK_REC_F_CAPTURED_EPOCH;
    owner.stale_retry=0;owner.ci=0;
    assert(stale_retry_owned(sent,11,&map,0,&cached));
    owner.stale_retry=0;owner.ci=1;owner.gen=11;owner.put_seq=82;
    sent[0].chunk_index=1;sent[0].chunk_generation=11;sent[0].put_seq=82;
    assert(stale_retry_owned(sent,11,&map,0,&cached)&&attr_reads==1);
    puts("STALE recovery: authoritative pull, sent-object identity, owned unchanged-view reuse with fresh ADMITTED-ticket proof; changed epochs/mappings/identities replay and unknown authority refuses PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-stale-recovery-') as directory:
    p=Path(directory)/'test.c';p.write_text(source)
    command=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include')]
    command+=shlex.split(os.environ.get('WB_RUNTIME_CFLAGS',''))
    subprocess.run(command+[str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
