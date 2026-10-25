#!/usr/bin/env python3
"""W155: a sync REPORT whose reply was lost resolves against its receipt.

Production code under test, extracted from the sources: the client REPORT
builder, the dual-host retry loop, the receipt query and
efs_client_rpc_report_resolve (src/client/inode_rpc.c); the server's
receipt wrapper and query (the W155 block of src/server/raft_host.c, one
copy per simulated node); the receipt table (src/common/request_receipt.c).
The transport is a fake three-node cluster with one shared committed state
(what Raft would replicate) and per-node receipt tables. The server's
publication itself is a model: a record whose chunk already holds its
generation is skipped, one whose base matches is applied, anything else is
STALE (the production idempotence the REPORT path documents).

Faults, each injected deterministically (fake clock, no sleeps):
  A  applied, reply lost; budget spent              -> receipt APPLIED, OK
  B  request lost before the server saw it          -> receipt absent, re-send applies, OK
  C  applied, every reply BUSY until the budget ends -> receipt unknown, re-send skips, OK
  D  applied, reply lost, that node then down       -> another node skips, OK
  E  applied, reply lost, every node down           -> EIO, outcome unknown (only case)
  F  partial (one CAS loser), reply lost             -> STALE (definite), not EIO
  G  first attempt still running when asked          -> PENDING, then APPLIED, OK
  H  reply lost inside the budget                    -> same-identity retry replayed, not re-run
Every case also checks the data was applied exactly once.

Negative control: W155_RESOLVE_MS=0 disables the resolution; the test then
fails (the reply-loss cases end in an error).
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
cli = (root / 'src/client/inode_rpc.c').read_text()
srv = (root / 'src/server/raft_host.c').read_text()


def fn(src, prefix, nth=1):
    a = -1
    for _ in range(nth):
        a = src.index(prefix, a + 1)
    return src[a:src.index('\n}', a) + 2] + '\n'


def span(src, start, end):
    a = src.index(start)
    return src[a:src.index(end, a)]


client = ''.join([
    fn(cli, 'static int rpc_status_to_efs('),
    fn(cli, 'static int rpc_identified('),
    fn(cli, 'static const char *rpc_req_ident('),
    fn(cli, 'static uint64_t rpc_resolve_ms('),
    fn(cli, 'static int rpc_writer_retry_pause(', 2),
    fn(cli, 'static int rpc_type_mutates('),
    fn(cli, 'static int rpc_resolve_identified('),
    fn(cli, 'static int rpc_send_recv_dual_once(', 2),
    span(cli, 'static int rpc_send_recv_dual(uint8_t type, const void *req, uint32_t req_len,\n'
              '                              uint8_t expect, void *reply, uint32_t reply_len,\n'
              '                              int recv_ms)\n{',
         '#if EFS_FAULTS\n/* Private EOF-contract fault'),
    span(cli, 'int efs_client_rpc_report_dirty_raft(', 'int efs_client_rpc_xattr('),
])

block = span(srv, '/* ---- W155 · REPORT receipts', '/* A fresh quorum round proves current leadership')
names = ['g_report_receipts_once', 'g_report_receipts_ok', 'g_report_receipts',
         'g_report_receipt_join_ms', 'g_report_receipt_trace', 'host_report_receipts_init',
         'host_report_receipt_key', 'host_report_call', 'host_report_status_rc',
         'host_report_exec', 'host_receipt_how_name', 'server_raft_host_report_receipted',
         'server_raft_host_report_receipt', 'server_raft_host_report']
nodes = ''
for n in (1, 2, 3):
    b = block
    for name in names:
        b = re.sub(r'\b' + name + r'\b', f'{name}_n{n}', b)
    nodes += f'static void server_raft_host_report_n{n}(const struct efs_chunk_rec *recs, uint32_t count,const struct efs_ino_size_rec *irecs,uint32_t ino_count, uint32_t flags, const struct efs_opid *session,const struct efs_report_pubid *pubid,struct efs_msg_inode_reply *out){{fake_exec({n},recs,count,irecs,ino_count,flags,session,pubid,out);}}\n'
    nodes += b

code = r'''
#include "efs/protocol.h"
#include "efs/opid.h"
#include "efs/request_receipt.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---- fake clock (client side; the receipt table keeps real time) ---- */
static uint64_t now = 1000, deadline;
static int fake_clock(int id, struct timespec *t){(void)id;t->tv_sec=now/1000;t->tv_nsec=(now%1000)*1000000;return 0;}
static int fake_sleep(unsigned us){now+=us/1000;return 0;}
#define clock_gettime fake_clock
#define usleep fake_sleep
uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
static void efs_client_rpc_set_deadline_ms(uint64_t d){deadline=d;}
int efs_client_rpc_past_deadline(void){return deadline && now>=deadline;}
static int rpc_past_deadline(void){return efs_client_rpc_past_deadline();}

/* ---- client environment ---- */
static struct {efs_export_id_t export_id;unsigned node_count;} g_client={1,3};
static int efs_client_session_check(void){return EFS_OK;}
static void efs_client_session_fence(int rc){(void)rc;assert(0);}
static int efs_client_publication_id(uint64_t seq,struct efs_opid *id){memset(id,0,sizeof(*id));id->client_uuid[0]=0x55;id->client_uuid[1]=0x15;id->session_epoch=7;id->seq=seq;return EFS_OK;}
static uint32_t efs_client_degraded_flags(efs_ino_t i,uint32_t c,uint64_t g){(void)i;(void)c;(void)g;return 0;}
static uint64_t efs_client_degraded_pending(void){return 0;}
static void efs_client_degraded_drop(efs_ino_t i,uint32_t c,uint64_t g){(void)i;(void)c;(void)g;}
static int rpc_prof_enabled(void){return 0;}
static unsigned long long rpc_prof_now_us(void){return (unsigned long long)now*1000ull;}
static void rpc_prof_add(int t,unsigned long long a,unsigned long long b,unsigned long long c,unsigned long long d){(void)t;(void)a;(void)b;(void)c;(void)d;}
static void rpc_note_slow_ok(uint8_t t,int a,int b,uint8_t s,unsigned long long n){(void)t;(void)a;(void)b;(void)s;(void)n;}
static int stale_retryable(uint8_t t){return t!=EFS_MSG_REPORT_CHUNKS;}
static void opid_retry_ack(void *p,uint32_t n){(void)p;(void)n;}
static int rpc_send_recv_shard_once(uint32_t s,uint8_t t,const void *q,uint32_t l,uint8_t e,void *r,uint32_t rl,uint32_t c,uint32_t *cp){(void)s;(void)t;(void)q;(void)l;(void)e;(void)r;(void)rl;(void)c;(void)cp;assert(0);return 0;}
static __thread uint64_t rpc_tl_sent;
static __thread unsigned rpc_tl_lost;
static int rpc_send_recv_dual_once(uint8_t,const void*,uint32_t,uint8_t,void*,uint32_t,int);
int efs_client_rpc_report_dirty_pub(efs_export_id_t,const struct efs_chunk_rec*,uint32_t,const struct efs_ino_size_rec*,uint32_t,int,const struct efs_report_pubid*);

/* ---- fake cluster ---- */
struct efs_conn {int node;int have;uint8_t type;uint8_t buf[4096];uint32_t len;};
static struct efs_conn conns[4];
static int up[4]={0,1,1,1};
static uint64_t committed[8];     /* chunk index -> generation (replicated state) */
static unsigned applies, skips, execs[4], queries[4];
static int phase;                 /* 1: the first REPORT call, 2: resolution */
static int drop_request, drop_reply, busy_after_apply, down_after_loss, lose_first, recv_wait=30000;
static int pending_mode;
static pthread_t bg;
static pthread_mutex_t bg_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bg_cv=PTHREAD_COND_INITIALIZER;
static int bg_release, bg_started;
static __thread struct { int proposed; const char *phase; struct efs_receipt_stale stale; } host_report_note;
static void set_inode_rc(struct efs_msg_inode_reply *out,int rc,int hint){
 (void)hint;out->primary_id=0;
 out->status=rc==EFS_OK?EFS_INODE_RPC_OK:rc==EFS_ERR_BUSY?EFS_INODE_RPC_BUSY:rc==EFS_ERR_STALE?EFS_INODE_RPC_STALE:
  rc==EFS_ERR_INVAL?EFS_INODE_RPC_INVAL:EFS_INODE_RPC_ERROR;}

static void fake_exec(int node,const struct efs_chunk_rec *recs,uint32_t count,const struct efs_ino_size_rec *irecs,
                      uint32_t ino_count,uint32_t flags,const struct efs_opid *session,
                      const struct efs_report_pubid *pubid,struct efs_msg_inode_reply *out){
 (void)irecs;(void)ino_count;(void)flags;(void)session;(void)pubid;
 execs[node]++;
 if(pending_mode && phase==1){
  pthread_mutex_lock(&bg_mu);bg_started=1;pthread_cond_broadcast(&bg_cv);
  while(!bg_release)pthread_cond_wait(&bg_cv,&bg_mu);
  pthread_mutex_unlock(&bg_mu);
 }
 int stale=0;
 host_report_note.proposed=1;
 for(uint32_t i=0;i<count;i++){
  uint32_t ci=recs[i].chunk_index;
  if(committed[ci]==recs[i].chunk_generation){skips++;continue;}
  if(committed[ci]==recs[i].base_gen){committed[ci]=recs[i].chunk_generation;applies++;continue;}
  /* the production precheck's first loser (host_pub_stale_note) */
  if(!stale){host_report_note.stale.ino=recs[i].ino;host_report_note.stale.ci=ci;
   host_report_note.stale.want=recs[i].base_gen;host_report_note.stale.have=committed[ci];
   snprintf(host_report_note.stale.why,sizeof(host_report_note.stale.why),"span-base");}
  stale=1;
 }
 set_inode_rc(out,busy_after_apply&&phase==1?EFS_ERR_BUSY:stale?EFS_ERR_STALE:EFS_OK,-1);
}
'''+nodes+r'''
struct bg_arg {int node;struct efs_chunk_rec recs[4];uint32_t count;struct efs_ino_size_rec irec;struct efs_opid session;struct efs_report_pubid pubid;struct efs_msg_inode_reply out;};
static struct bg_arg bga;
static void *bg_run(void *p){struct bg_arg *a=p;server_raft_host_report_receipted_n1(a->recs,a->count,&a->irec,1,EFS_REPORT_F_SYNC|EFS_REPORT_F_SESSION|EFS_REPORT_F_PUBID,&a->session,&a->pubid,&a->out);return NULL;}

/* Serve one request on node n (the handler's REPORT parse, reduced). */
static void serve(int n,struct efs_conn *c,const uint8_t *req,uint32_t len){
 const struct efs_msg_report_chunks *h=(const void *)req;
 uint32_t flags=h->sync;size_t avail=len-sizeof(*h);
 struct efs_opid session;struct efs_report_pubid pubid;
 assert(flags&EFS_REPORT_F_SESSION);avail-=EFS_REPORT_SESSION_LEN;
 assert(efs_report_session_unpack(&session,req+sizeof(*h)+avail));
 assert(flags&EFS_REPORT_F_PUBID);avail-=EFS_REPORT_PUBID_LEN;
 assert(efs_report_pubid_unpack(&pubid,req+sizeof(*h)+avail));
 if(flags&EFS_REPORT_F_RECEIPT){
  struct efs_msg_report_receipt_reply rr;queries[n]++;
  assert(!h->count && !h->ino_count);
  if(pending_mode && queries[n]==2){ /* the running attempt finishes now */
   pthread_mutex_lock(&bg_mu);bg_release=1;pthread_cond_broadcast(&bg_cv);pthread_mutex_unlock(&bg_mu);
   pthread_join(bg,NULL);
  }
  void (*q)(const struct efs_opid*,const struct efs_report_pubid*,struct efs_msg_report_receipt_reply*)=
   n==1?server_raft_host_report_receipt_n1:n==2?server_raft_host_report_receipt_n2:server_raft_host_report_receipt_n3;
  q(&session,&pubid,&rr);
  memcpy(c->buf,&rr,sizeof(rr));c->len=sizeof(rr);c->have=1;return;
 }
 struct efs_chunk_rec recs[4];size_t used=0;
 assert(h->count<=4 && !efs_chunk_recs_unpack(req+sizeof(*h),avail,h->count,recs,&used));
 const struct efs_ino_size_rec *irecs=(const void *)(req+sizeof(*h)+used);
 if(pending_mode && phase==1){ /* the first attempt runs in the background */
  memset(&bga,0,sizeof(bga));bga.node=n;memcpy(bga.recs,recs,sizeof(recs));bga.count=h->count;bga.irec=irecs[0];
  bga.session=session;bga.pubid=pubid;
  assert(!pthread_create(&bg,NULL,bg_run,&bga));
  pthread_mutex_lock(&bg_mu);while(!bg_started)pthread_cond_wait(&bg_cv,&bg_mu);pthread_mutex_unlock(&bg_mu);
  c->have=0;return; /* its reply never comes */
 }
 struct efs_msg_inode_reply r;memset(&r,0,sizeof(r));
 void (*run)(const struct efs_chunk_rec*,uint32_t,const struct efs_ino_size_rec*,uint32_t,uint32_t,const struct efs_opid*,const struct efs_report_pubid*,struct efs_msg_inode_reply*)=
  n==1?server_raft_host_report_receipted_n1:n==2?server_raft_host_report_receipted_n2:server_raft_host_report_receipted_n3;
 run(recs,h->count,irecs,h->ino_count,flags,&session,&pubid,&r);
 memcpy(c->buf,&r,sizeof(r));c->len=sizeof(r);c->have=1;
}

static struct efs_conn *efs_client_conn_get(efs_node_id_t n){return n<=3&&up[n]?&conns[n]:NULL;}
static struct efs_conn *raft_dual_voter_conn(efs_node_id_t *n,efs_node_id_t skip){
 for(efs_node_id_t i=1;i<=3;i++) if(up[i] && i!=skip){*n=i;conns[i].node=(int)i;return &conns[i];}
 *n=0;return NULL;}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn *c){(void)n;c->have=0;}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn *c){(void)n;(void)c;}
static void efs_conn_set_recv_timeout(struct efs_conn *c,int ms){(void)c;(void)ms;}
int efs_conn_send_msg(struct efs_conn *c,uint8_t type,const void *p,uint32_t n){
 assert(type==EFS_MSG_REPORT_CHUNKS);int node=(int)(c-conns);c->node=node;
 const struct efs_msg_report_chunks *h=p;
 if(!(h->sync&EFS_REPORT_F_RECEIPT) && phase==1 && drop_request){c->have=0;return 0;}
 serve(node,c,p,n);
 if(!(h->sync&EFS_REPORT_F_RECEIPT) && phase==1 && (drop_reply || lose_first)){
  c->have=0;
  if(lose_first)lose_first--;
  if(down_after_loss)up[node]=0;
  if(down_after_loss==2)up[1]=up[2]=up[3]=0;
 }
 return 0;}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *t,void **p,uint32_t *n){
 if(!c->have){now+=(uint64_t)recv_wait;return -1;} /* a receive timeout */
 c->have=0;*t=EFS_MSG_REPORT_CHUNKS_REPLY;*p=malloc(c->len);assert(*p);memcpy(*p,c->buf,c->len);*n=c->len;return 0;}
'''+client+r'''
static void reset(void){
 now=1000;deadline=0;phase=0;drop_request=drop_reply=busy_after_apply=down_after_loss=lose_first=pending_mode=0;recv_wait=30000;
 bg_release=bg_started=0;
 up[1]=up[2]=up[3]=1;memset(conns,0,sizeof(conns));memset(committed,0,sizeof(committed));
 applies=skips=0;memset(execs,0,sizeof(execs));memset(queries,0,sizeof(queries));
}
static struct efs_report_pubid pid;
/* One sync REPORT as report_dirty_ino_run sends it: an 8 s budget, then
 * (outer deadline 0 again) the W155 resolution when the outcome is open. */
static int report(struct efs_chunk_rec *recs,uint32_t n){
 struct efs_ino_size_rec irec={.ino=4242,.size=(uint64_t)n<<20};
 pid.mint_ns++;pid.seq++;
 phase=1;deadline=now+8000;
 int rc=efs_client_rpc_report_dirty_pub(1,recs,n,&irec,1,1,&pid);
 uint64_t sent=efs_client_rpc_last_sent();
 deadline=0;phase=2;
 fprintf(stderr,"case first_rc=%d sent=%llx\n",rc,(unsigned long long)sent);
 if(sent && n) rc=efs_client_rpc_report_resolve(1,recs,n,&irec,1,1,&pid,sent,rc,4242);
 return rc;
}
static void recs2(struct efs_chunk_rec *r){
 memset(r,0,2*sizeof(*r));
 for(int i=0;i<2;i++){r[i].ino=4242;r[i].chunk_index=(uint32_t)i;r[i].base_gen=0;r[i].chunk_generation=100+(uint64_t)i;
  r[i].nodes[0]=1;r[i].nodes[1]=2;r[i].nodes[2]=3;}
}
int main(void){
 struct efs_chunk_rec r[2];int rc;
 pid.mint_ns=1700000000000000000ull;pid.seq=0;
 /* A: applied, reply lost, the 8 s budget spent in the 30 s receive. */
 reset();recs2(r);drop_reply=1;rc=report(r,2);
 assert(rc==EFS_OK && applies==2 && execs[1]==1 && queries[1]==1 && !execs[2] && !execs[3]);
 /* B: the request never reached a server. */
 reset();recs2(r);drop_request=1;rc=report(r,2);
 assert(rc==EFS_OK && applies==2 && execs[1]==1 && queries[1]==1);
 /* C: applied, and every reply in the budget was BUSY (verdict unknown on
  * the server): the receipt says unknown, the same identity is sent again
  * and its records are skipped as committed. */
 reset();recs2(r);busy_after_apply=1;rc=report(r,2);
 assert(rc==EFS_OK && applies==2 && skips>=2);
 /* D: applied on n1, reply lost, n1 down: n2 answers the re-send. */
 reset();recs2(r);drop_reply=1;down_after_loss=1;rc=report(r,2);
 assert(rc==EFS_OK && applies==2 && execs[1]==1 && execs[2]==1 && skips==2 && !queries[1]);
 /* E: applied, reply lost, the whole cluster unreachable for the bound:
  * the only case that stays EIO, after the bound, with the outcome unknown. */
 reset();recs2(r);drop_reply=1;down_after_loss=2;uint64_t t0;
 { struct efs_ino_size_rec irec={.ino=4242,.size=2u<<20};pid.mint_ns++;pid.seq++;phase=1;deadline=now+8000;
   rc=efs_client_rpc_report_dirty_pub(1,r,2,&irec,1,1,&pid);uint64_t sent=efs_client_rpc_last_sent();
   deadline=0;phase=2;t0=now;rc=efs_client_rpc_report_resolve(1,r,2,&irec,1,1,&pid,sent,rc,4242); }
 assert(rc==EFS_ERR_IO && applies==2 && now-t0>=8000 && now-t0<8000+1000);
 /* F: one record lost its CAS (another writer moved chunk 1), reply lost:
  * the receipt says partial, a definite STALE (replay the loser), not EIO. */
 reset();recs2(r);committed[1]=77;r[1].base_gen=5;drop_reply=1;rc=report(r,2);
 assert(rc==EFS_ERR_STALE && applies==1 && committed[0]==100 && committed[1]==77);
 /* G: the first attempt is still running on n1 when the client asks. */
 reset();recs2(r);pending_mode=1;rc=report(r,2);
 assert(rc==EFS_OK && applies==2 && execs[1]==1 && queries[1]==2);
 /* H: the reply is lost inside the budget: the loop's own same-identity
  * re-send is answered from n1's APPLIED receipt, not run again. */
 reset();recs2(r);lose_first=1;recv_wait=100;
 { struct efs_ino_size_rec irec={.ino=4242,.size=2u<<20};pid.mint_ns++;pid.seq++;phase=1;deadline=now+8000;
   rc=efs_client_rpc_report_dirty_pub(1,r,2,&irec,1,1,&pid);phase=2;
   if(rc!=EFS_OK){uint64_t sent=efs_client_rpc_last_sent();deadline=0;rc=efs_client_rpc_report_resolve(1,r,2,&irec,1,1,&pid,sent,rc,4242);} }
 assert(rc==EFS_OK && applies==2 && execs[1]==1);
 { struct efs_receipt_key k;struct efs_receipt rec;memset(&k,0,sizeof(k));k.uuid[0]=0x55;k.uuid[1]=0x15;k.epoch=7;k.mint_ns=pid.mint_ns;k.seq=pid.seq;
   assert(efs_receipt_lookup(&g_report_receipts_n1,&k,&rec) && rec.attempts==1 && rec.replayed>=1 && rec.outcome==EFS_RECEIPT_OUT_APPLIED); }
 puts("W155 REPORT: applied+lost reply, lost request, BUSY-unknown, failed node, all-down EIO, partial STALE, pending and replayed duplicate PASS");
 return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='efs-w155-report-') as d:
    p = Path(d) / 't.c'
    p.write_text(code)
    cc = shlex.split(os.environ.get('CC', 'cc'))
    extra = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.environ.get('EFS_SANITIZE') == '1' else []
    subprocess.run(cc + ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                         '-I' + str(root / 'include'), *extra, str(p),
                         str(root / 'src/common/request_receipt.c'), '-pthread',
                         '-o', str(p.with_suffix(''))], check=True)
    out = subprocess.run([str(p.with_suffix(''))], check=True, timeout=60,
                         capture_output=True, text=True,
                         env=dict(os.environ, EFS_RESOLVE_MS=os.environ.get('W155_RESOLVE_MS', '8000'), EFS_REPORT_RECEIPT_JOIN_MS='50'))
    log = out.stderr
    # The lines carry the request identity on both sides and join on it.
    assert re.search(r'report-resolve id=5515\w*/7/\w+:\d+ ino=4242 outcome=applied rc=0', log), log
    assert 'outcome=resent rc=0' in log, log
    assert re.search(r'outcome=unknown rc=-?\d+ .* -> EIO, outcome unknown after the bound', log), log
    assert 'outcome=partial rc=-14' in log, log
    # K38: the PARTIAL verdict names the record that stayed behind
    assert re.search(r'outcome=partial rc=-14 .* stuck=\d+:1 gen=[0-9a-f]+ span=\d+\+\d+ base=5 committed=4d why=span-base', log), log
    assert re.search(r'report-receipt query id=5515\w*/7/\w+:\d+ state=2 outcome=applied', log), log
    assert re.search(r'report-receipt query id=\S+ state=1 ', log), log
    assert re.search(r'report-receipt id=\S+ ino=4242 nrec=2 how=replay rc=0 outcome=applied', log), log
    assert re.search(r'inode-rpc: retry type=67 attempt=0 why=recv rc=-?\d+ req_len=\d+ recv_ms=\d+ wait_ms=\d+ cut=\w+ nid=1 id=5515', log), log
    # EIO comes from exactly one case: the all-down E.
    assert log.count('-> EIO') == 1, log
    print(out.stdout.strip())

# The production REPORT loop calls the resolver before failing a sync REPORT.
w = (root / 'src/client/write.c').read_text()
run = w[w.index('static int report_dirty_ino_run('):w.index('\nint efs_client_report_dirty_ino(')]
assert run.count('report_sent |= efs_client_rpc_last_sent();') == 1
# every mint starts a new identity with no nodes reached
assert len(re.findall(r'report_pubid_mint\(&pubid\);\n\s+report_sent = 0;', run)) == run.count('report_pubid_mint(&pubid);') == 2
a = run.index('efs_client_rpc_set_deadline_ms(outer);')
b = run.index('rc = efs_client_rpc_report_resolve(g_client.export_id, crecs, cn, irecs, in, sync,')
c = run.index('if (rc == EFS_OK) {\n        /* Every rec committed')
assert a < b < c, 'resolution must run after the budget and before the commit bookkeeping'
assert 'if (sync && report_sent && cn)' in run[a:b]
print('W155 REPORT: write.c resolves an open sync REPORT before EIO PASS')
