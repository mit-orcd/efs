/* K13 live ticket probe on a disposable private fixture cluster
 * (tests/live/k13_ticket_cluster.py). Drives the production client ticket
 * ledger, the one ticketed PUT and the ticket-bound REPORT on real daemons:
 * exact retry, lost admission/PUT/REPORT replies, body collision, owner
 * settle, revocation, cold replay and the W88 folded-span host replay. */
#include "../../src/client/client_internal.h"
#include "efs/put_ticket.h"
#include "efs/put_upload.h"
#include "efs/checksum.h"
#include "efs/erasure.h"
#include "efs/placement.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#define CHUNK EFS_DEFAULT_CHUNK_SIZE
#define FRAG (CHUNK / 2)
static void fence(unsigned port,const char *mgmt,const struct efs_opid *id)
{
    char hex[33],address[64];for(unsigned i=0;i<16;i++)snprintf(hex+2*i,3,"%02x",id->client_uuid[i]);
    snprintf(address,sizeof(address),"127.0.0.1:%u",port);pid_t child=fork();assert(child>=0);
    if(!child){execl(mgmt,mgmt,"raft-session",address,"fence",hex,(char *)NULL);_exit(127);}
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
}
/* One fragment upload per member, the real wire shape: EFS_MSG_PUT_CHUNK
 * with the ticket header, decoded by efs_put_reply_decode. Only BUSY (the
 * storage reply for leadership/pressure) is retried, with the same ticket
 * and bytes, inside one 15 s budget. lose_reply drops the first reply
 * unread (an ambiguous PUT) before the exact retry. */
static void fragment_probe(unsigned port,const struct efs_put_ticket *ticket,
                           uint8_t *const frags[EFS_NUM_FRAGMENTS],int cold,int lose_reply,int expected)
{
 for(unsigned fi=0;fi<3;fi++) {
  int fd=efs_connect_declared("127.0.0.1",port+ticket->nodes[fi]-1);assert(fd>=0);
  efs_set_recv_timeout(fd,15000);efs_set_send_timeout(fd,15000);
  if(cold) {
   struct efs_msg_get_chunk get={.export_id=1,.ino=ticket->ino,.chunk_index=ticket->ci,
     .fragment_index=fi,.chunk_generation=ticket->object};
   assert(!efs_send_msg(fd,EFS_MSG_GET_CHUNK,&get,sizeof(get)));
   uint8_t type;void *reply=NULL;uint32_t bytes;
   assert(!efs_recv_msg(fd,&type,&reply,&bytes));
   assert(type==EFS_MSG_GET_CHUNK_REPLY&&bytes==1+32+ticket->fragment_len&&*(uint8_t *)reply==EFS_GET_CHUNK_OK);
   assert(!memcmp((uint8_t *)reply+1,ticket->checksums[fi],32));
   assert(!memcmp((uint8_t *)reply+33,frags[fi],ticket->fragment_len));free(reply);
  }
  uint8_t header[EFS_PUT_UPLOAD_HEADER_LEN];assert(!efs_put_upload_header(ticket,1,fi,header));
  if(lose_reply) {
   assert(!efs_send_msg_parts(fd,EFS_MSG_PUT_CHUNK,header,sizeof(header),frags[fi],ticket->fragment_len));
   close(fd);fd=efs_connect_declared("127.0.0.1",port+ticket->nodes[fi]-1);assert(fd>=0);
  }
  uint64_t previous=efs_net_deadline_ms();struct timespec now;
  assert(!clock_gettime(CLOCK_MONOTONIC,&now));
  uint64_t deadline=(uint64_t)now.tv_sec*1000+now.tv_nsec/1000000+15000;
  if(previous && previous<deadline)deadline=previous;
  efs_net_set_deadline_ms(deadline);
  int rc=EFS_ERR_BUSY;
  for(unsigned attempt=0;attempt<16;attempt++) {
   assert(!clock_gettime(CLOCK_MONOTONIC,&now));
   if((uint64_t)now.tv_sec*1000+now.tv_nsec/1000000>=deadline){if(!attempt)close(fd);break;}
   if(attempt){fd=efs_connect_declared("127.0.0.1",port+ticket->nodes[fi]-1);assert(fd>=0);}
   assert(!efs_send_msg_parts(fd,EFS_MSG_PUT_CHUNK,header,sizeof(header),frags[fi],ticket->fragment_len));
   uint8_t type;void *reply=NULL;uint32_t bytes;
   assert(!efs_recv_msg(fd,&type,&reply,&bytes));close(fd);
   uint8_t path;uint64_t boot;
   rc=efs_put_reply_decode(type,reply,bytes,&path,&boot);free(reply);
   assert(rc!=EFS_ERR_PROTO);
   if(rc!=EFS_ERR_BUSY)break;
   assert(!clock_gettime(CLOCK_MONOTONIC,&now));
   uint64_t current=(uint64_t)now.tv_sec*1000+now.tv_nsec/1000000;
   if(current>=deadline || deadline-current<=100)break;
   usleep(100000);
  }
  efs_net_set_deadline_ms(previous);
  if(rc!=expected)fprintf(stderr,"ticket upload fi=%u rc=%d expected=%d\n",fi,rc,expected);
  assert(rc==expected);
 }
}
/* A full chunk image: fill everywhere, then byte at `at` (if >=0). */
static void image(uint8_t *chunk,uint8_t fill,int at,uint8_t value,uint8_t *frags[3],
                  uint8_t checksums[3][EFS_HASH_SIZE])
{
 memset(chunk,fill,CHUNK);if(at>=0)chunk[at]=value;
 assert(!efs_encode_chunk(chunk,CHUNK,CHUNK,frags));
 for(unsigned i=0;i<3;i++)efs_hash(frags[i],FRAG,checksums[i]);
}
static uint32_t pick_ci(efs_ino_t ino,int cross)
{
 for(uint32_t ci=0;ci<EFS_META_LANES;ci++){
  int same=efs_raft_shard_group(efs_kv_lane_shard(ino,ci%EFS_META_LANES))==efs_raft_shard_group(efs_kv_inode_shard(ino));
  if(same!=cross)return ci;
 }
 assert(0);return 0;
}
/* Admit and upload one object of ino/ci; the ledger entry is released as
 * landed (this probe reports it itself). */
static void put_object(unsigned port,efs_ino_t ino,uint32_t ci,uint8_t *frags[3],
                       uint8_t checksums[3][EFS_HASH_SIZE],uint32_t off,uint32_t len,
                       struct efs_put_ticket *t)
{
 efs_node_id_t nodes[3];efs_place_fragments(g_client.nodes,g_client.node_count,ino,ci,nodes);
 assert(!efs_client_put_admit(ino,ci,nodes,(const uint8_t (*)[EFS_HASH_SIZE])checksums,FRAG,off,len,t));
 fragment_probe(port,t,frags,0,0,EFS_OK);
 efs_client_put_release(t->id.seq,1);
}
static struct efs_chunk_rec record(const struct efs_put_ticket *t,uint64_t base,uint32_t base_n,uint64_t base_seq)
{
 struct efs_chunk_rec r;memset(&r,0,sizeof(r));
 r.ino=t->ino;r.chunk_index=t->ci;memcpy(r.nodes,t->nodes,sizeof(r.nodes));
 memcpy(r.checksums,t->checksums,sizeof(r.checksums));r.base_gen=base;r.chunk_generation=t->object;
 r.file_generation=t->inode_gen;r.publish_flags=EFS_CHUNK_REC_F_FRESH_OBJECT;
 r.put_fragment_len=t->fragment_len;r.put_seq=t->id.seq;r.put_member_mask=t->member_mask;
 r.delta_off=t->delta_off;r.delta_len=t->delta_len;r.delta_base_n=base_n;r.delta_base_seq=base_seq;
 return r;
}
static int report(const struct efs_chunk_rec *r,uint64_t size)
{
 struct efs_ino_size_rec s;memset(&s,0,sizeof(s));s.ino=r->ino;s.size=size;
 return efs_client_rpc_report_dirty_raft(1,r,1,&s,1,1);
}
static void chunk_row(efs_ino_t ino,uint32_t ci,struct efs_chunk_rec *out)
{
 struct efs_chunk_rec recs[1];uint32_t count=1;
 assert(!efs_client_rpc_getchunks(1,ino,ci,recs,&count) && count==1 && recs[0].chunk_index==ci);
 *out=recs[0];
}
static unsigned live_spans(const struct efs_chunk_rec *row,uint64_t gen)
{
 unsigned n=0;
 for(uint32_t i=0;i<row->delta_base_n && i<EFS_CHUNK_DELTA_MAX;i++)
  if(row->deltas[i].generation==gen && row->deltas[i].len)n++;
 return n;
}
static void save(const char *path,const void *p,size_t n){FILE *f=fopen(path,"wb");assert(f && fwrite(p,1,n,f)==n && !fclose(f));}
static void load(const char *path,void *p,size_t n){FILE *f=fopen(path,"rb");assert(f && fread(p,1,n,f)==n && fgetc(f)==EOF);fclose(f);}
/* Saved for the cold phase: ticket wire, the original span record and the
 * expected surviving generation. */
struct w88_state {uint8_t first[EFS_PUT_TICKET_WIRE_LEN];struct efs_chunk_rec rec;uint64_t later;};

/* Usage: port ino mgmt mode [file]. Modes: tickets <cross> <save>,
 * cold <ticket>, w88 <state> (then waits for a stdin line), w88-foreign <state>. */
int main(int argc,char **argv)
{
    assert(argc>=5);unsigned port=atoi(argv[1]);efs_ino_t ino=strtoull(argv[2],0,10);const char *mode=argv[4];
    /* EFS_PROBE_NODES: member count of the fixture (default 3). */
    unsigned members=getenv("EFS_PROBE_NODES")?(unsigned)atoi(getenv("EFS_PROBE_NODES")):3;
    assert(members>=3 && members<=EFS_MAX_NODES);
    g_client.export_id=1;g_client.node_count=members;
    for(unsigned i=0;i<members;i++){g_client.nodes[i].id=i+1;strcpy(g_client.nodes[i].addr,"127.0.0.1");g_client.nodes[i].port=port+i;}
    efs_client_conn_init();
    uint8_t *chunk=malloc(CHUNK),*frag_buf=malloc(3*FRAG),*frags[3]={frag_buf,frag_buf+FRAG,frag_buf+2*FRAG};
    uint8_t checksums[3][EFS_HASH_SIZE];assert(chunk && frag_buf);
    struct efs_put_ticket t;uint32_t state=0;
    if(!strcmp(mode,"tickets")) {
        assert(argc==7);int cross=atoi(argv[5]);uint32_t ci=pick_ci(ino,cross);
        image(chunk,0x3c,(int)ci,0x99,frags,checksums);
        efs_node_id_t nodes[3];efs_place_fragments(g_client.nodes,g_client.node_count,ino,ci,nodes);
        /* Production admission (ledger, FileID, session admission). */
        assert(!efs_client_put_admit(ino,ci,nodes,(const uint8_t (*)[EFS_HASH_SIZE])checksums,FRAG,0,0,&t));
        assert(!efs_client_rpc_put_ticket(&t,1,&state) && state==EFS_PUT_TICKET_ADMITTED);
        assert(!efs_client_rpc_put_ticket(&t,0,&state) && state==EFS_PUT_TICKET_ADMITTED); /* exact re-admission */
        /* Lost admission reply: a raw ADMIT of a fresh ticket is applied and
         * its reply dropped; the retry finds that exact ticket ADMITTED. */
        struct efs_meta_pub vp={.ino=ino,.inode_gen=t.inode_gen,.chunk_index=ci,.coding_profile_id=EFS_META_PROFILE_K2F1,
            .fresh_object=1,.ticketed=1,.put_fragment_len=FRAG,.put_member_mask=t.member_mask};
        assert(!efs_client_put_id(&vp.put_id));vp.publication_id=vp.put_id;
        vp.candidate_gen=efs_meta_candidate_gen(vp.put_id.client_uuid,vp.put_id.session_epoch,vp.put_id.seq,ci,0);
        memcpy(vp.ch.nodes,nodes,sizeof(nodes));memcpy(vp.ch.checksums,checksums,sizeof(checksums));
        struct efs_put_ticket v;assert(!efs_put_ticket_from_pub(&vp,&v));
        uint8_t raw[2+EFS_PUT_TICKET_WIRE_LEN];uint32_t shard=efs_kv_lane_shard(ino,ci%EFS_META_LANES);
        raw[0]=efs_raft_shard_group(shard);raw[1]=EFS_MD_CMD_PUT_TICKET_ADMIT;assert(!efs_put_ticket_encode(&v,raw+2));
        for(unsigned n=0;n<members;n++){int fd=efs_connect_declared("127.0.0.1",port+n);assert(fd>=0);assert(!efs_send_msg(fd,EFS_MSG_RAFT_MKFS,raw,sizeof(raw)));close(fd);}
        for(unsigned tries=0;tries<50 && (efs_client_rpc_put_ticket(&v,1,&state) || state!=EFS_PUT_TICKET_ADMITTED);tries++)usleep(100000);
        assert(state==EFS_PUT_TICKET_ADMITTED);
        assert(!efs_client_rpc_put_ticket(&v,0,&state) && state==EFS_PUT_TICKET_ADMITTED);
        /* Body collision: same identity, different body or file, refused. */
        struct efs_put_ticket changed=t;changed.body_digest[0]^=1;
        assert(efs_client_rpc_put_ticket(&changed,0,&state)==EFS_ERR_PROTO && !state);
        changed=t;changed.ino+=4096;
        assert(efs_client_rpc_put_ticket(&changed,0,&state)==EFS_ERR_PROTO && !state);
        /* An upload of other bytes under this ticket is refused before storage. */
        frags[0][0]^=1;
        { int fd=efs_connect_declared("127.0.0.1",port+t.nodes[0]-1);assert(fd>=0);
          uint8_t h[EFS_PUT_UPLOAD_HEADER_LEN];assert(!efs_put_upload_header(&t,1,0,h));
          assert(!efs_send_msg_parts(fd,EFS_MSG_PUT_CHUNK,h,sizeof(h),frags[0],FRAG));
          uint8_t type;void *reply=NULL;uint32_t bytes;assert(!efs_recv_msg(fd,&type,&reply,&bytes));close(fd);
          assert(efs_put_reply_decode(type,reply,bytes,NULL,NULL)==EFS_ERR_IO);free(reply); }
        frags[0][0]^=1;
        /* Lost PUT reply on every member, then the exact retry. */
        fragment_probe(port,&t,frags,0,1,EFS_OK);
        fragment_probe(port,&t,frags,1,0,EFS_OK);
        /* Lost REPORT reply is exercised by resending: PUBLISHED is a no-op. */
        efs_client_put_release(t.id.seq,1);
        struct efs_chunk_rec r=record(&t,EFS_CHUNK_BASE_UNCOND,0,0);
        assert(!report(&r,(uint64_t)(ci+1)*CHUNK));
        assert(!efs_client_rpc_put_ticket(&t,1,&state) && state==EFS_PUT_TICKET_PUBLISHED);
        assert(!report(&r,(uint64_t)(ci+1)*CHUNK));
        struct efs_chunk_rec row;chunk_row(ino,ci,&row);assert(row.chunk_generation==t.object);
        /* A second admitted body, uploaded and never reported: retained. */
        image(chunk,0x3d,(int)ci,0x9a,frags,checksums);
        struct efs_put_ticket u;put_object(port,ino,ci,frags,checksums,0,0,&u);
        uint8_t wire[EFS_PUT_TICKET_WIRE_LEN];assert(!efs_put_ticket_encode(&u,wire));save(argv[6],wire,sizeof(wire));
        printf("live PUT tickets cross=%d ci=%u shard=%u: lost admission reply, exact re-admission, body/file collision, wrong-bytes refusal, lost PUT replies with exact retry, lost REPORT reply no-op, mapping bound to the ticket PASS\n",cross,ci,shard);
        return 0;
    }
    if(!strcmp(mode,"cold")) {
        assert(argc==6);uint8_t wire[EFS_PUT_TICKET_WIRE_LEN];load(argv[5],wire,sizeof(wire));
        assert(!efs_put_ticket_decode(wire,sizeof(wire),&t) && t.ino==ino);
        image(chunk,0x3d,(int)t.ci,0x9a,frags,checksums);
        assert(!memcmp(checksums,t.checksums,sizeof(checksums)));
        /* After a cold restart: the admitted body and its fragments survive;
         * an exact late PUT is still accepted while the session lives. */
        assert(!efs_client_rpc_put_ticket(&t,1,&state) && state==EFS_PUT_TICKET_ADMITTED);
        fragment_probe(port,&t,frags,1,0,EFS_OK);
        /* Revocation: the completed fence refuses a late PUT and the ticket. */
        fence(port,argv[3],&t.id);
        fragment_probe(port,&t,frags,1,0,EFS_ERR_STALE);
        int rc=efs_client_rpc_put_ticket(&t,1,&state);
        assert(rc!=EFS_OK && !state);
        printf("live PUT ticket cold restart: admitted body and fragments survive, exact late PUT accepted; completed fence refuses late PUT (STALE) and ticket status (rc=%d) PASS\n",rc);
        return 0;
    }
    if(!strcmp(mode,"w88")) {
        assert(argc==6);uint32_t ci=pick_ci(ino,1);struct w88_state st;memset(&st,0,sizeof(st));
        struct efs_put_ticket all[11];unsigned n=0;
        struct efs_chunk_rec first_rec;
        /* Eight one-byte spans. */
        for(unsigned i=0;i<EFS_CHUNK_DELTA_MAX;i++) {
            image(chunk,0,(int)i,(uint8_t)(0x10+i),frags,checksums);
            put_object(port,ino,ci,frags,checksums,i,1,&all[n]);
            struct efs_chunk_rec r=record(&all[n],0,0,0);
            if(!i)first_rec=r;
            assert(!report(&r,(uint64_t)(ci+1)*CHUNK));n++;
        }
        struct efs_chunk_rec row;chunk_row(ino,ci,&row);
        /* Fold: one full image naming the observed span list. */
        memset(chunk,0,CHUNK);for(unsigned i=0;i<EFS_CHUNK_DELTA_MAX;i++)chunk[i]=(uint8_t)(0x10+i);
        assert(!efs_encode_chunk(chunk,CHUNK,CHUNK,frags));for(unsigned i=0;i<3;i++)efs_hash(frags[i],FRAG,checksums[i]);
        put_object(port,ino,ci,frags,checksums,0,0,&all[n]);
        struct efs_chunk_rec r=record(&all[n],0,row.delta_base_n,row.delta_base_seq);
        assert(!report(&r,(uint64_t)(ci+1)*CHUNK));uint64_t fold=all[n++].object;
        chunk_row(ino,ci,&row);assert(row.chunk_generation==fold);
        /* A later full overwrite, then a churn span that evicts the first
         * span's folded identity from the eight-entry trailer. */
        image(chunk,0x5a,-1,0,frags,checksums);
        put_object(port,ino,ci,frags,checksums,0,0,&all[n]);
        r=record(&all[n],fold,row.delta_base_n,row.delta_base_seq);
        assert(!report(&r,(uint64_t)(ci+1)*CHUNK));uint64_t later=all[n++].object;
        image(chunk,0,40,0xc3,frags,checksums);
        put_object(port,ino,ci,frags,checksums,40,1,&all[n]);
        r=record(&all[n],0,0,0);assert(!report(&r,(uint64_t)(ci+1)*CHUNK));n++;
        chunk_row(ino,ci,&row);assert(row.chunk_generation==later);
        /* Lost original ACK: byte-identical retry of the first span. */
        assert(!report(&first_rec,(uint64_t)(ci+1)*CHUNK));
        chunk_row(ino,ci,&row);assert(row.chunk_generation==later && !live_spans(&row,first_rec.chunk_generation));
        /* The owner settles everything in order, then retries once more. */
        uint32_t retired=0;assert(!efs_client_rpc_put_ticket_settle(all,n,&retired) && retired==n);
        assert(report(&first_rec,(uint64_t)(ci+1)*CHUNK)==EFS_ERR_STALE);
        chunk_row(ino,ci,&row);assert(row.chunk_generation==later && !live_spans(&row,first_rec.chunk_generation));
        assert(!efs_put_ticket_encode(&all[0],st.first));st.rec=first_rec;st.later=later;save(argv[5],&st,sizeof(st));
        printf("live W88 ci=%u (cross-group lane): folded span retry after trailer eviction is a no-op while PUBLISHED and STALE once retired; mapping stays %llx PASS\n",ci,(unsigned long long)later);
        /* The same session survives the harness's crash of every daemon. */
        printf("w88 ready\n");fflush(stdout);
        char line[16];assert(fgets(line,sizeof(line),stdin));
        int rc=EFS_ERR_BUSY;
        for(unsigned tries=0;tries<60 && rc!=EFS_ERR_STALE;tries++){rc=report(&first_rec,(uint64_t)(ci+1)*CHUNK);if(rc!=EFS_ERR_STALE)sleep(1);}
        assert(rc==EFS_ERR_STALE);
        chunk_row(ino,ci,&row);assert(row.chunk_generation==later && !live_spans(&row,first_rec.chunk_generation));
        printf("live W88 after cold restart of all daemons, same session: retired span retry refused (STALE), mapping unchanged PASS\n");
        return 0;
    }
    if(!strcmp(mode,"w88-foreign")) {
        /* Another session replays the record: a never-admitted identity. */
        assert(argc==6);struct w88_state st;load(argv[5],&st,sizeof(st));
        assert(!efs_put_ticket_decode(st.first,sizeof(st.first),&t) && t.ino==ino);
        uint32_t ci=st.rec.chunk_index;struct efs_chunk_rec row;
        int rc=report(&st.rec,(uint64_t)(ci+1)*CHUNK);
        assert(rc!=EFS_OK);
        chunk_row(ino,ci,&row);assert(row.chunk_generation==st.later && !live_spans(&row,st.rec.chunk_generation));
        printf("live W88 replay under a foreign session refused (rc=%d), mapping unchanged PASS\n",rc);
        return 0;
    }
    fprintf(stderr,"unknown mode %s\n",mode);return 2;
}
