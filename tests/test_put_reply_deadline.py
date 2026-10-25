#!/usr/bin/env python3
"""Actual parallel PUT reply/drain path with stalled descriptors and caller budget."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/write.c').read_text();a=s.index('static int put_fragments_parallel_once(')
f=s[a:s.index('\n}',a)+2]
a=s.index('static int put_recv_reply(');reply_fn=s[a:s.index('\n}',a)+2]
a=s.index('static void put_drain_verdict(');drain_fn=s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/protocol.h"
#include "efs/put_upload.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <stdint.h>
#undef EFS_IO_TIMEOUT_MS
#define EFS_IO_TIMEOUT_MS 300
struct efs_conn {void *rc;int fd;};
static struct efs_conn conns[3];
static struct {uint32_t export_id;} g_client;
static uint64_t deadline;
static unsigned drops,releases,fails,sends,checkouts,mode;
static uint64_t report_clock_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
static int efs_client_rpc_past_deadline(void){return deadline && report_clock_ms()>=deadline;}
static int efs_net_remaining_ms(int cap){if(!deadline)return cap;uint64_t now=report_clock_ms();if(now>=deadline)return 0;return deadline-now<(uint64_t)cap?(int)(deadline-now):cap;}
static int efs_client_node_is_down(efs_node_id_t n){(void)n;return 0;}
static struct efs_conn *efs_client_conn_get(efs_node_id_t n){checkouts++;return &conns[n-1];}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn *c){(void)n;(void)c;drops++;}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn *c){(void)n;(void)c;releases++;}
static void efs_client_node_note_fail(efs_node_id_t n){(void)n;fails++;}
int efs_put_ticket_encode(const struct efs_put_ticket *t,uint8_t out[EFS_PUT_TICKET_WIRE_LEN]){assert(t->ino==1);memset(out,7,EFS_PUT_TICKET_WIRE_LEN);return 0;}
int efs_conn_send_msg_parts(struct efs_conn *c,uint8_t t,const void *a,uint32_t an,const void *b,uint32_t bn){(void)c;(void)b;assert(t==EFS_MSG_PUT_CHUNK&&an==EFS_PUT_UPLOAD_HEADER_LEN&&bn==1&&efs_put_upload_rd32(a)==1);sends++;return 0;}
int efs_conn_reply_watch_quick(struct efs_conn *c){if(mode==12)return c==&conns[2]?-1:EFS_CONN_REPLY_READY;return ((mode==1 && c!=&conns[2]) || (mode==3 && c==&conns[0]) || mode==4 || mode==5 || mode==6 || mode==7 || mode==8 || mode==10 || mode==11)?EFS_CONN_REPLY_READY:c->fd;}
int efs_conn_reply_watch_us(struct efs_conn *c,int budget){(void)budget;return efs_conn_reply_watch_quick(c);}
int efs_conn_put_reply_watch(struct efs_conn *c,int budget){(void)budget;return efs_conn_reply_watch_quick(c);}
static void efs_client_node_note_ok(efs_node_id_t n){(void)n;}
static uint64_t noted;static uint64_t efs_client_commit_note(efs_node_id_t n){(void)n;return ++noted;}
int efs_conn_recv_put_reply(struct efs_conn *c,uint8_t *t,uint8_t *status,uint8_t *path,uint64_t *epoch){(void)c;*epoch=123;*t=EFS_MSG_PUT_CHUNK_REPLY;*status=mode==11 && c==&conns[2]?EFS_PUT_CHUNK_QUOTA_EXCEEDED:(mode==8 && c==&conns[1]) || (mode==10 && c==&conns[2])?EFS_PUT_CHUNK_STALE:mode==6 && c==&conns[2]?EFS_PUT_CHUNK_BUSY:mode==7 && c!=&conns[0]?EFS_PUT_CHUNK_BUSY:mode==4?EFS_PUT_CHUNK_QUOTA_EXCEEDED:mode==5?EFS_PUT_CHUNK_BUSY:EFS_PUT_CHUNK_OK;*path=0xff;return 0;}
'''+reply_fn+drain_fn+f+r'''
int main(void){
 int pp[2];assert(!pipe(pp));
 for(unsigned i=0;i<3;i++){conns[i].rc=(void *)1;conns[i].fd=pp[0];}
 g_client.export_id=1;struct efs_put_ticket ticket={.ino=1,.ci=0,.nodes={1,2,3},.fragment_len=1};
 const uint8_t *fragments[3]={0};int failed[3];
 for(mode=0;mode<12;mode++){
  drops=releases=fails=sends=checkouts=0;
  uint64_t start=report_clock_ms();deadline=mode==2?start:start+15;
  /* mode 9: fragments 0 and 1 were acknowledged by an earlier attempt of
   * this object: only fragment 2 is sent, and the two count (K34). */
  int acked[3]={mode==9,mode==9,0},quota[3]={0,0,0};uint64_t seq[3]={0,0,0},epoch[3]={0,0,0};
  int rc=put_fragments_parallel_once(&ticket,fragments,failed,acked,quota,seq,epoch);
  if(mode==9)assert(sends==1&&checkouts==1&&acked[0]&&acked[1]&&!acked[2]);
  /* mode 6: the third member answers BUSY after the quorum: alive, its
   * fragment not stored, so the caller may complete the stripe (K34). */
  if(mode==6)assert(failed[2]==3&&!acked[2]&&acked[0]&&acked[1]);
  /* W154: an OK names the fragments that were acknowledged (at least two). */
  if(rc==EFS_OK)assert(acked[0]+acked[1]+acked[2]>=2);
  /* K35: each acknowledgement carries its sequence and the verifier. */
  /* mode 9's slots 0 and 1 were acknowledged by an earlier attempt: their
   * sequence and verifier belong to that attempt (checked by the case below). */
  for(unsigned i=0;i<3;i++)if(!(mode==9&&i<2))assert(acked[i]?seq[i]&&seq[i]<=noted&&epoch[i]==123:!seq[i]&&!epoch[i]);
  uint64_t elapsed=report_clock_ms()-start;
  printf("mode=%u rc=%d elapsed_ms=%llu sent=%u drops=%u releases=%u peer_fail=%u\n",mode,rc,(unsigned long long)elapsed,sends,drops,releases,fails);fflush(stdout);
  /* mode 8: one member refuses the ticket; a quorum of ACKs never publishes
   * it; mode 10: nor does a refusal that arrives after the quorum (K34). */
  /* B5: every full member is named, also one refusing after the quorum
   * (mode 11: two ACKs, the third member full). */
  if(mode==4)assert(quota[0]&&quota[1]&&quota[2]);
  else if(mode==11)assert(acked[0]&&acked[1]&&!acked[2]&&!quota[0]&&!quota[1]&&quota[2]);
  else assert(!quota[0]&&!quota[1]&&!quota[2]);
  assert(elapsed<150);assert(rc==((mode==1 || mode==6 || mode==9 || mode==11)?EFS_OK:mode==4?EFS_ERR_QUOTA:(mode==8 || mode==10)?EFS_ERR_STALE:EFS_ERR_BUSY));
  assert(!fails);assert(drops+releases==checkouts);
  if(mode==2)assert(!sends && !checkouts);
 }
 /* K35: an attempt after two acknowledgements sends only the missing slot
  * and waits for its reply within the drain. */
 {
  mode=8;drops=releases=fails=sends=checkouts=0;deadline=report_clock_ms()+100;
  int acked[3]={1,1,0},quota[3]={0,0,0};uint64_t seq[3]={7,8,0},epoch[3]={123,123,0};
  int rc=put_fragments_parallel_once(&ticket,fragments,failed,acked,quota,seq,epoch);
  assert(rc==EFS_OK&&sends==1&&checkouts==1&&acked[2]&&seq[2]&&epoch[2]==123&&seq[0]==7&&seq[1]==8);
 }
 /* K39: one earlier acknowledgement counts once: with the other two
  * members BUSY there is no quorum (it returned OK holding one fragment). */
 {
  mode=7;drops=releases=fails=sends=checkouts=0;deadline=report_clock_ms()+50;
  int acked[3]={1,0,0},quota[3]={0,0,0};uint64_t seq[3]={7,0,0},epoch[3]={123,0,0};
  int rc=put_fragments_parallel_once(&ticket,fragments,failed,acked,quota,seq,epoch);
  assert(rc==EFS_ERR_BUSY&&sends==2&&acked[0]&&!acked[1]&&!acked[2]&&failed[1]==3&&failed[2]==3);
 }
 /* K39: the third member's connection fails after the quorum (it stops):
  * a hard failure of that slot, so the caller resends or places it around. */
 {
  mode=12;drops=releases=fails=sends=checkouts=0;deadline=report_clock_ms()+50;
  int acked[3]={0,0,0},quota[3]={0,0,0};uint64_t seq[3]={0,0,0},epoch[3]={0,0,0};
  int rc=put_fragments_parallel_once(&ticket,fragments,failed,acked,quota,seq,epoch);
  assert(rc==EFS_OK&&acked[0]&&acked[1]&&!acked[2]&&failed[2]==1&&!fails);
 }
 close(pp[0]);close(pp[1]);return 0;
}
'''
# The baseline does not use these budget functions; allow only that diagnostic
# so the red run reaches the stalled real poll rather than failing compilation.
with tempfile.TemporaryDirectory(prefix='efs-put-reply-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('parallel ticketed PUT: caller-bound quorum/drain, expired admission, one-ACK refusal, negative replies, terminal ticket refusal (also after the quorum), earlier acknowledgements not resent, a BUSY third member reported, full members named (also after the quorum), acknowledgement sequence and verifier per fragment, resend of the missing slot only (K35), an earlier acknowledgement counted once and a drain-time loss reported (K39) and no false peer failures PASS')
