#!/usr/bin/env python3
"""K38/W161: a STALE span whose dcache body is gone must not be re-sent on its old base.

Executes the production putid table, STALE classifier (stale_repull_replay),
STALE replay (dcache_replay_stale_ex_budgeted) and REPORT record builder
(report_fill_owned) from src/client/write.c. Fetch, PUT, pull and the dcache
slot lookup are adapters. K38_WRITE_SOURCE selects another write.c (the
pre-fix source) for the negative control: there the first case fails with
the livelock signature, an unchanged record on the stale base.

Cases, each one REPORT round after a STALE reply (committed base 244b, the
span's PUT based on c289, the span's dcache body gone):
  superseded  a newer snapshot of the chunk committed (it carried the span's
              bytes); the span's putid was re-inserted by its late PUT
  rebuild     the span is the newest publication of its bytes: rebuild it on
              the committed image from its own landed object
  lost        the span's own object cannot be read: fail closed (writeback
              error on the inode), release the identity, REPORT goes on
  transient   the committed image fetch is BUSY: retained, retried later
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
src = Path(os.environ.get('K38_WRITE_SOURCE', root/'src/client/write.c')).read_text()
SENT = 'struct stale_sent {' in src  # the classifier takes the round's sent records


def fn(name, optional=False):
    m = re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{', src, re.M)
    if not m:
        assert optional, name
        return ''
    return src[m.start():src.index('\n}', m.end())+2]


def block(start, end):
    a = src.index(start)
    return src[a:src.index(end, a)+len(end)]


code = r'''
#include "client_internal.h"
#include "efs/put_ticket.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DCACHE_NR 32
#define DTRACE(e, ...) do { (void)(e); } while (0)
''' + block('struct put_id {', 'static pthread_mutex_t g_putid_mu = PTHREAD_MUTEX_INITIALIZER;') + '\n' + \
    block('struct dcache_ent {', '\n};') + '\n' + block('struct stale_pair {', '\n};') + \
    (block('struct stale_sent {', '\n};') if SENT else '') + r'''
struct dirty_snap {uint64_t chunk_count;efs_ino_t *chunk_inos;uint32_t *chunk_idxs;};
#define INO 4642
#define CI 2033
#define LEN 1024u
#define SPAN_OFF 400u
#define SPAN_LEN 300u
#define OLD_BASE 0xc28987f070b5f01aull
#define NEW_BASE 0x244b546dfa67ace7ull
#define SPAN_A 0xa5ull
static struct dcache_ent slot;
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER, io=PTHREAD_MUTEX_INITIALIZER;
static struct efs_chunk_entry row;
static efs_node_id_t nodes[3]={1,2,3};
static uint8_t cks[3][EFS_HASH_SIZE];
static unsigned settled, fetches, overlays, nput, wb_errors, commits;
static int fetch_rc, overlay_rc;
static uint64_t put_bg, put_seq, next_object=0x1000, seqctr=10;
static uint32_t put_off, put_len;
static uint8_t put_img[LEN];
static struct efs_chunk_delta overlay_d;
void efs_client_put_done(efs_ino_t ino,uint32_t ci,uint64_t o){(void)ino;(void)ci;if(o)settled++;}
static void putid_miss_note(efs_ino_t ino,uint32_t ci,const efs_node_id_t *n){(void)ino;(void)ci;(void)n;}
static void report_retry_without_identity(efs_ino_t ino,uint32_t ci){(void)ino;(void)ci;}
static void dcache_ensure(void){}
static uint32_t dcache_slot(efs_ino_t ino,uint32_t ci){(void)ino;(void)ci;return 0;}
static pthread_mutex_t *dcache_mu(uint32_t s){(void)s;return &mu;}
static pthread_mutex_t *dcache_io_mu(uint32_t s){(void)s;return &io;}
static struct dcache_ent *dcache_find_meta(uint32_t s,efs_ino_t ino,uint32_t ci)
{(void)s;return slot.ino==ino&&slot.ci==ci?&slot:NULL;}
static struct dcache_ent *dcache_find(uint32_t s,efs_ino_t ino,uint32_t ci)
{struct dcache_ent *e=dcache_find_meta(s,ino,ci);return e&&e->data?e:NULL;}
static void dcache_pin_release(struct dcache_ent *e){e->pin_held=0;}
void *efs_buf_alloc(uint32_t len){return malloc(len);}
void efs_buf_free(void *p,uint32_t len){(void)len;free(p);}
static uint64_t dcache_seq_next(struct dcache_ent *e){e->snap_seq=++seqctr;return seqctr;}
static void export_forget_unreported(efs_ino_t ino,uint32_t ci){(void)ino;(void)ci;}
static int export_chunk_copy(efs_ino_t ino,uint32_t ci,struct efs_chunk_entry *out)
{(void)ino;(void)ci;*out=row;return 0;}
int efs_client_fetch_published_chunk_obs(efs_ino_t ino,uint32_t ci,uint8_t *buf,uint32_t len,
                                         struct efs_chunk_entry *obs,int *have_obs)
{(void)ino;(void)ci;fetches++;if(fetch_rc){return fetch_rc;}
 memset(buf,'P',len);if(obs){*obs=row;}if(have_obs){*have_obs=1;}return EFS_OK;}
int efs_client_overlay_span(efs_ino_t ino,uint32_t ci,uint8_t *buf,uint32_t len,
                            const struct efs_chunk_delta *d)
{(void)ino;(void)ci;overlays++;overlay_d=*d;if(overlay_rc){return overlay_rc;}
 assert((uint64_t)d->off+d->len<=len);memset(buf+d->off,'A',d->len);return EFS_OK;}
static int dcache_trace_on(void){return 0;}
static uint32_t data_chunk_size(void){return LEN;}
static void dcache_install_image(struct dcache_ent *e,uint8_t *img,uint32_t len,uint64_t seq)
{(void)e;(void)img;(void)len;(void)seq;}
uint64_t efs_client_epoch_get(efs_ino_t ino){(void)ino;return 0;}
int efs_client_pull_chunks_range_absent(efs_ino_t ino,uint32_t lo,uint32_t hi,uint8_t *bits)
{(void)ino;(void)lo;(void)hi;bits[0]=0;return EFS_OK;}
static int dcache_note_committed(efs_ino_t ino,uint32_t ci,uint64_t g,uint64_t o,uint64_t s)
{(void)ino;(void)ci;(void)g;(void)o;(void)s;commits++;return 1;}
static int dcache_cycle_verdict(efs_ino_t ino,uint32_t ci,const struct efs_chunk_entry *ce,uint64_t s)
{(void)ino;(void)ci;(void)ce;(void)s;return 0;}
static uint64_t dcache_base_gen_of(efs_ino_t ino,uint32_t ci,uint64_t fb)
{return slot.ino==ino&&slot.ci==ci?slot.base_gen:fb;}
static int wb_error_record_add(efs_ino_t ino){assert(ino==INO);wb_errors++;return 1;}
static int dcache_replay_stale_ex_budgeted(efs_ino_t ino, uint32_t ci, int absent);
static int replay_fan_run(struct stale_pair *rp,uint64_t n)
{int error=EFS_OK;for(uint64_t i=0;i<n;i++){int rc=dcache_replay_stale_ex_budgeted(rp[i].ino,rp[i].ci,rp[i].absent);
 if(rc!=EFS_OK&&!error)error=rc;}return error;}
static uint64_t report_mono_ms(void){return 0;}
static uint64_t dcache_object_seq_of(efs_ino_t ino,uint32_t ci);
static void putid_note(efs_ino_t ino, uint32_t ci, uint64_t gen, uint64_t seq,
                      const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                      const uint8_t cks[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE],
                      uint32_t delta_off, uint32_t delta_len,
                      uint32_t delta_base_n, uint64_t delta_base_seq,
                      const struct efs_chunk_entry *obs, uint64_t span_base_gen,
                      const struct efs_put_ticket *ticket);
/* The PUT lands: putid_note and the slot record, as dcache_put_now_budgeted. */
static int dcache_put_now(efs_ino_t ino,uint32_t ci,const uint8_t *chunk,uint32_t len,uint64_t bg,
                          uint64_t seq,uint32_t doff,uint32_t dlen,const struct efs_chunk_entry *obs)
{
 assert(len==LEN);nput++;put_bg=bg;put_seq=seq;put_off=doff;put_len=dlen;memcpy(put_img,chunk,len);
 uint64_t object=++next_object;
 struct efs_put_ticket t={.object=object,.id={.seq=object},.inode_gen=1,.fragment_len=LEN/2,.member_mask=7};
 putid_note(ino,ci,object,seq,nodes,cks,doff,dlen,0,0,obs,bg,&t);
 if(slot.ino==ino&&slot.ci==ci&&seq>=slot.object_seq){slot.object_gen=object;slot.object_seq=seq;
  slot.object_span_base_gen=bg;slot.object_delta_off=doff;slot.object_delta_len=dlen;}
 return EFS_OK;
}
''' + '\n'.join(fn(n) for n in ['putid_hash','putid_find','putid_rehash','putid_insert_slot']) + '\n' + \
    fn('putid_note') + fn('rec_ticket') + fn('putid_fill') + fn('putid_drop_snapshot') + \
    fn('dcache_unpublished') + fn('dcache_object_of') + fn('report_fill_owned') + \
    fn('chunk_obs_has_live_spans') + fn('chunk_obs_masked') + fn('stale_pair_cmp') + \
    (fn('stale_sent_cmp') if SENT else '') + \
    fn('stale_repull_replay') + fn('dcache_identity_abandon', True) + \
    fn('replay_span_without_body', True) + fn('dcache_replay_stale_ex_budgeted') + \
    (fn('dcache_object_seq_of', True) or
     'static uint64_t dcache_object_seq_of(efs_ino_t i,uint32_t c){(void)i;(void)c;return 0;}') + r'''
static void reset(void)
{
 for(uint64_t i=0;g_putid&&i<=g_putid_mask;i++)g_putid[i].ino=0;
 g_putid_live=g_putid_used=0;
 memset(&slot,0,sizeof(slot));memset(&row,0,sizeof(row));
 row.generation=NEW_BASE;memcpy(row.fragment_nodes,nodes,sizeof(nodes));
 settled=fetches=overlays=nput=wb_errors=commits=0;fetch_rc=overlay_rc=0;put_bg=put_seq=0;
}
/* Span A's PUT (snapshot 5) on base c289 lands. */
static void land_span_a(void)
{
 struct efs_put_ticket t={.object=SPAN_A,.id={.seq=105},.inode_gen=1,.fragment_len=LEN/2,.member_mask=7};
 putid_note(INO,CI,SPAN_A,5,nodes,cks,SPAN_OFF,SPAN_LEN,0,0,NULL,OLD_BASE,&t);
}
/* One STALE round: classify, replay, rebuild the record. 1 = a record is built. */
static int round_after_stale(int *replay_rc,struct efs_chunk_rec *rec)
{
 efs_ino_t inos[1]={INO};uint32_t cis[1]={CI};struct dirty_snap ds={1,inos,cis};
 uint64_t seq=0,sent_seq=0;
 /* the record the STALE round sent, built as report_dirty_ino_run builds it */
 struct efs_chunk_rec sent={.ino=INO,.chunk_index=CI,.base_gen=dcache_base_gen_of(INO,CI,0)};
 if(!report_fill_owned(INO,CI,&sent,&sent_seq,nodes)){*replay_rc=EFS_OK;return 0;}
 *replay_rc=''' + ('stale_repull_replay(&ds,INO,&sent,&sent_seq,1)' if SENT else 'stale_repull_replay(&ds,INO)') + r''';
 memset(rec,0,sizeof(*rec));rec->ino=INO;rec->chunk_index=CI;rec->base_gen=dcache_base_gen_of(INO,CI,0);
 return report_fill_owned(INO,CI,rec,&seq,nodes);
}
static void no_stale_resend(int built,const struct efs_chunk_rec *rec,const char *what)
{
 if(built&&rec->delta_len&&rec->base_gen==OLD_BASE){
  fprintf(stderr,"K38 %s: the rebuilt REPORT re-sends span %llx+%u+%u on base %llx against "
          "committed %llx (replay returned without a body; livelock)\n",what,
          (unsigned long long)rec->chunk_generation,rec->delta_off,rec->delta_len,
          (unsigned long long)rec->base_gen,(unsigned long long)NEW_BASE);
  exit(1);
 }
}
int main(void)
{
 struct efs_chunk_rec rec;int rc,built;

 /* superseded: snapshot 6 (full image B = 244b) landed, its REPORT committed and
  * freed the body; A's slower PUT lands afterwards and re-inserts its putid. */
 reset();slot.ino=INO;slot.ci=CI;
 land_span_a();
 slot.object_gen=0xb6;slot.object_seq=6;slot.committed_object=0xb6;slot.committed_seq=6;
 slot.snap_seq=6;slot.base_gen=NEW_BASE;slot.data=NULL;
 built=round_after_stale(&rc,&rec);
 no_stale_resend(built,&rec,"superseded");
 assert(rc==EFS_OK&&!built&&!nput&&!wb_errors);
 assert(settled==1); /* A's ticket: no REPORT carries it any more */
 /* the guard: the same late landing no longer re-inserts the putid */
 reset();slot.ino=INO;slot.ci=CI;slot.object_gen=0xb6;slot.object_seq=6;
 slot.committed_object=0xb6;slot.committed_seq=6;slot.snap_seq=6;slot.base_gen=NEW_BASE;
 land_span_a();
 { struct efs_chunk_rec r={.ino=INO};uint64_t s=0;assert(!putid_fill(INO,CI,&r,&s)&&settled==1); }

 /* rebuild: A is the slot's newest object, its body gone (no slot body). */
 reset();slot.ino=INO;slot.ci=CI;slot.object_gen=SPAN_A;slot.object_seq=5;slot.snap_seq=5;
 slot.object_delta_off=SPAN_OFF;slot.object_delta_len=SPAN_LEN;slot.object_span_base_gen=OLD_BASE;
 slot.base_gen=OLD_BASE;
 land_span_a();
 built=round_after_stale(&rc,&rec);
 no_stale_resend(built,&rec,"rebuild");
 assert(rc==EFS_OK&&fetches==1&&overlays==1&&nput==1&&!wb_errors);
 assert(overlay_d.generation==SPAN_A&&overlay_d.off==SPAN_OFF&&overlay_d.len==SPAN_LEN&&
        overlay_d.nodes[0]==1&&overlay_d.nodes[2]==3);
 assert(put_bg==NEW_BASE&&put_off==SPAN_OFF&&put_len==SPAN_LEN&&put_seq>=5);
 for(uint32_t i=0;i<LEN;i++)assert(put_img[i]==(i>=SPAN_OFF&&i<SPAN_OFF+SPAN_LEN?'A':'P'));
 assert(built&&rec.base_gen==NEW_BASE&&rec.chunk_generation==next_object&&
        rec.delta_off==SPAN_OFF&&rec.delta_len==SPAN_LEN);
 assert(settled==1); /* the replaced object A */
 /* the same rebuild with no slot node at all (reused after a reclaim) */
 reset();land_span_a();
 built=round_after_stale(&rc,&rec);
 no_stale_resend(built,&rec,"rebuild-noslot");
 assert(rc==EFS_OK&&nput==1&&put_bg==NEW_BASE&&built&&rec.base_gen==NEW_BASE);

 /* lost: A's own object is gone. Fail closed, release the identity. */
 reset();slot.ino=INO;slot.ci=CI;slot.object_gen=SPAN_A;slot.object_seq=5;slot.snap_seq=5;
 slot.object_delta_off=SPAN_OFF;slot.object_delta_len=SPAN_LEN;slot.object_span_base_gen=OLD_BASE;
 land_span_a();overlay_rc=EFS_ERR_NOT_FOUND;
 built=round_after_stale(&rc,&rec);
 no_stale_resend(built,&rec,"lost");
 assert(rc==EFS_OK&&!built&&!nput&&wb_errors==1);
 assert(slot.committed_object==SPAN_A&&slot.committed_seq==5&&!dcache_unpublished(&slot));
 assert(settled==1);

 /* transient: the committed image fetch is BUSY. Retain, no verdict. */
 reset();land_span_a();fetch_rc=EFS_ERR_BUSY;
 built=round_after_stale(&rc,&rec);
 assert(rc==EFS_ERR_BUSY&&!nput&&!wb_errors&&built&&rec.chunk_generation==SPAN_A);
 puts("K38 STALE span without body: superseded drop + late-PUT guard, rebuild on committed base "
      "from own object, fail-closed lost object, retained transient PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-k38-replay-') as d:
    p = Path(d)/'test.c'; p.write_text(code)
    cmd = shlex.split(os.getenv('CC', 'cc'))+['-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function', '-pthread', '-I'+str(root/'include'), '-I'+str(root/'src/client')]
    cmd += shlex.split(os.environ.get('WB_RUNTIME_CFLAGS', ''))
    subprocess.run(cmd+[str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True)
