/* Task C: fixed-seed mutations of actual product decoders, not reimplementations.
 * A rejected input must leave caller-owned output byte-for-byte unchanged. */
#include "efs/membership.h"
#include "efs/member_view.h"
#include "efs/frag_record.h"
#include "efs/put_ticket.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/record_decode.h"
#include "efs/request_receipt.h"
#include "efs/meta_cmd.h"
#include "efs/session.h"
#include "efs/put_upload.h"
#include <sys/socket.h>
#include <unistd.h>
#include "../../src/server/discovery.h"
#include "../../src/server/member_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t rng = UINT64_C(0x20261025c);
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static struct efs_membership members(void)
{
    struct efs_membership m = { .count=3, .formed=3, .cluster=42 };
    for (unsigned i=0;i<3;i++) {
        m.m[i].id=i+1; m.m[i].port=19000+i; m.m[i].token=100+i;
        strcpy(m.m[i].addr,"127.0.0.1");
    }
    return m;
}
static size_t seed(const char *name, uint8_t *b)
{
    size_t n=0; struct efs_membership m=members();
    if (!strcmp(name,"membership")) {
        assert(efs_membership_encode(&m,b,8192,&n)==EFS_OK);
    } else if (!strcmp(name,"member-view")) {
        struct efs_msg_membership_reply r={0};
        assert(efs_membership_encode(&m,r.record,sizeof(r.record),&n)==EFS_OK);
        r.len=n; n+=EFS_MEMBERSHIP_REPLY_HDR; memcpy(b,&r,n);
    } else if (!strcmp(name,"discovery-probe")) {
        efs_disc_probe_encode(b,42); n=EFS_DISC_PROBE_LEN;
    } else if (!strcmp(name,"discovery-answer")) {
        struct efs_disc_answer a={.cluster=42,.count=3,.formed=1,.port=19000};
        strcpy(a.addr,"127.0.0.1"); strcpy(a.build_id,"decoder-check");
        n=efs_disc_answer_encode(b,8192,42,&a); assert(n);
    } else if (!strcmp(name,"boot")) {
        member_store_boot_encode(42,b); n=EFS_BOOT_FILE_LEN;
    } else if ((!strcmp(name,"fragment") || !strcmp(name,"fragment-verify"))) {
        struct efs_frag_ident id={.kind=EFS_FRAG_KIND_CHUNK,.space=1,
            .object=42,.generation=43,.payload_len=4096,.coding_profile=1};
        uint8_t digest[EFS_HASH_SIZE]={1};
        assert(efs_frag_record_encode(&id,digest,b)==EFS_OK); n=EFS_FRAG_RECORD_BYTES;
    } else if (!strcmp(name,"ticket")) {
        struct efs_put_ticket t={.ino=4097,.inode_gen=1,.fragment_len=4096,
            .coding_profile=EFS_META_PROFILE_K2F1,.member_mask=7};
        t.id.client_uuid[0]=1; t.id.session_epoch=1; t.id.seq=1;
        t.object=efs_meta_candidate_gen(t.id.client_uuid,1,1,0,0);
        for(unsigned i=0;i<3;i++)t.nodes[i]=i+1;
        assert(efs_put_ticket_encode(&t,b)==EFS_OK); n=EFS_PUT_TICKET_WIRE_LEN;
    } else if (!strcmp(name,"identity")) {
        struct member_store store={.lockfd=-1,.member_id=1,.token=100,.state=EFS_MEMBER_FORMED,
                                   .cluster=42,.port=19000,.members=m};
        strcpy(store.addr,"127.0.0.1");assert(!member_store_identity_encode(&store,b));n=EFS_IDENTITY_FILE_LEN;
    } else if (!strcmp(name,"ticket-batch") || !strcmp(name,"ticket-settle")) {
        uint8_t ticket[EFS_PUT_TICKET_WIRE_LEN];seed("ticket",ticket);
        struct efs_put_ticket t;assert(!efs_put_ticket_decode(ticket,sizeof(ticket),&t));
        b[0]=!strcmp(name,"ticket-batch")?EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH:EFS_MD_CMD_PUT_TICKET_SETTLE;
        b[1]=0;b[2]=2;memcpy(b+3,ticket,sizeof(ticket));
        t.id.seq=2;t.object=efs_meta_candidate_gen(t.id.client_uuid,1,2,0,0);
        assert(!efs_put_ticket_encode(&t,b+3+sizeof(ticket)));n=3+2*sizeof(ticket);
    } else if (!strcmp(name,"hello")) {
        struct efs_msg_hello h={.version=EFS_VERSION_PACK,.node_id=1,.port=19000,
            .token=100,.cluster=42,.flags=EFS_HELLO_F_FORMED};
        strcpy(h.addr,"127.0.0.1");strcpy(h.build_id,EFS_BUILD_ID);memcpy(b,&h,sizeof(h));n=sizeof(h);
    } else if (!strcmp(name,"recovery-request")) {
        struct efs_msg_recovery_scan q={.group=0,.what=EFS_RECOVERY_SCAN_TICKETS,.node=1};
        memcpy(b,&q,sizeof(q));n=sizeof(q);
    } else if (!strcmp(name,"recovery-reply")) {
        struct efs_msg_recovery_scan_reply r={.n=1};
        memcpy(b,&r,sizeof(r));n=sizeof(r)+seed("ticket",b+sizeof(r));
    } else if (!strcmp(name,"audit-request")) {
        memset(b,0,EFS_SEGMENT_AUDIT_HEADER);b[1]=EFS_SEGMENT_AUDIT_F_COMPLETE;n=EFS_SEGMENT_AUDIT_HEADER;
    } else if (!strcmp(name,"audit-reply")) {
        memset(b,0,EFS_SEGMENT_AUDIT_REPLY);b[7]=1;b[EFS_SEGMENT_AUDIT_REPLY]=EFS_SEGMENT_AUDIT_OK;
        n=EFS_SEGMENT_AUDIT_REPLY+1;
    } else if (!strcmp(name,"degraded")) {
        memset(b,0,EFS_REBUILD_MARK_LEN);b[7]=1;b[23]=1;b[29]=1;
        b[33]=1;b[37]=2;b[41]=3;n=EFS_REBUILD_MARK_LEN;
    } else if (!strcmp(name,"degraded-command")) {
        memset(b,0,EFS_MD_DEGRADED_MARK_LEN);b[0]=EFS_MD_CMD_DEGRADED_MARK;
        b[8]=1;b[16]=1;b[24]=1;b[33]=1;n=EFS_MD_DEGRADED_MARK_LEN;
    } else if (!strcmp(name,"recovery-command")) {
        b[0]=EFS_MD_CMD_RECOVERY_MARK;n=1+seed("degraded",b+1);
    } else if (!strcmp(name,"pack-degraded")) {
        memset(b,0,EFS_MD_PACK_DEGRADED_LEN);b[0]=EFS_MD_CMD_PACK_DEGRADED;
        b[2]=EFS_MD_PACK_DEGRADED_MARK;b[10]=1;b[18]=1;b[19]=1;b[20]=1;n=EFS_MD_PACK_DEGRADED_LEN;
    } else if (!strcmp(name,"ticket-drain")) {
        memset(b,0,EFS_MD_PUT_TICKET_DRAIN_LEN);b[0]=EFS_MD_CMD_PUT_TICKET_DRAIN;
        b[1]=1;b[20]=1;n=EFS_MD_PUT_TICKET_DRAIN_LEN;
    } else if (!strcmp(name,"put-header")) {
        uint8_t ticket[EFS_PUT_TICKET_WIRE_LEN];seed("ticket",ticket);
        struct efs_put_ticket t;assert(!efs_put_ticket_decode(ticket,sizeof(ticket),&t));
        t.fragment_len=EFS_MIN_CHUNK_SIZE/2;assert(!efs_put_upload_header(&t,1,0,b));n=EFS_PUT_UPLOAD_HEADER_LEN;
    } else if (!strcmp(name,"cleanup-probe")) {
        memset(b,0,EFS_MD_SESSION_CLEANUP_PROBE_LEN);b[0]=EFS_MD_REQ_SESSION_CLEANUP_PROBE;
        b[1]=1;b[20]=1;b[32]=1;n=EFS_MD_SESSION_CLEANUP_PROBE_LEN;
    } else if (!strcmp(name,"receipt")) {
        memset(b,0,32);b[0]=1;n=32;
    } else if (!strcmp(name,"declaration")) {
        struct efs_msg_client_hello h={{'E','F','S','P'},EFS_PROTOCOL_VERSION,0};
        memcpy(b,&h,sizeof(h)); n=sizeof(h);
    } else { fprintf(stderr,"unknown decoder: %s\n",name); exit(2); }
    return n;
}
#define UNCHANGED(rc, obj, before) do { if ((rc)!=EFS_OK) assert(!memcmp(&(obj),&(before),sizeof(obj))); } while(0)
static int check(const char *name, const uint8_t *b, size_t n)
{
    int rc;
    if (!strcmp(name,"membership") || !strcmp(name,"member-view")) {
        struct efs_membership out,before; memset(&out,0xa5,sizeof(out)); before=out;
        rc=!strcmp(name,"membership") ? efs_membership_decode(b,n,&out) : efs_member_view_parse_reply(b,n,&out);
        if (!strcmp(name,"membership")) assert(rc==EFS_OK || rc==EFS_ERR_DECODE || rc==EFS_ERR_UNSUPPORTED);
        else assert(rc<=0); /* server error replies carry any negative server rc */
        UNCHANGED(rc,out,before); if(rc==EFS_OK)assert(efs_membership_valid(&out));
    } else if (!strcmp(name,"discovery-probe")) {
        uint64_t out=UINT64_MAX,before=out; rc=efs_disc_probe_decode(b,n,&out);
        assert(rc==0 || rc==-1); UNCHANGED(rc,out,before);
    } else if (!strcmp(name,"discovery-answer")) {
        struct efs_disc_answer out,before; memset(&out,0xa5,sizeof(out)); before=out;
        rc=efs_disc_answer_decode(b,n,42,&out); assert(rc==0 || rc==-1); UNCHANGED(rc,out,before);
    } else if (!strcmp(name,"boot")) {
        uint64_t out=UINT64_MAX,before=out; rc=member_store_boot_decode(b,n,&out);
        assert(rc==EFS_OK || rc==EFS_ERR_DECODE || rc==EFS_ERR_UNSUPPORTED || rc==EFS_ERR_CHECKSUM);
        UNCHANGED(rc,out,before);
    } else if (!strcmp(name,"fragment")) {
        struct efs_frag_ident out,before; uint8_t digest[32],old[32];
        memset(&out,0xa5,sizeof(out)); before=out; memset(digest,0xa5,32); memcpy(old,digest,32);
        rc=efs_frag_record_parse(b,n,&out,digest);
        assert(rc==EFS_OK || rc==EFS_ERR_CHECKSUM || rc==EFS_ERR_FRAG_FORMAT);
        UNCHANGED(rc,out,before); if(rc)assert(!memcmp(digest,old,32));
    } else if (!strcmp(name,"fragment-verify")) {
        struct efs_frag_ident expect={.kind=EFS_FRAG_KIND_CHUNK,.space=1,.object=42,
            .generation=43,.payload_len=4096,.coding_profile=1};
        uint8_t digest[32],before[32];memset(digest,0xa5,32);memcpy(before,digest,32);
        rc=efs_frag_record_verify(b,n,&expect,digest,NULL);
        assert(rc==EFS_OK || rc==EFS_ERR_CHECKSUM || rc==EFS_ERR_FRAG_FORMAT || rc==EFS_ERR_FRAG_IDENTITY);
        if(rc)assert(!memcmp(digest,before,32));
    } else if (!strcmp(name,"ticket")) {
        struct efs_put_ticket out,before; memset(&out,0xa5,sizeof(out)); before=out;
        rc=efs_put_ticket_decode(b,n,&out); assert(rc==EFS_OK || rc==EFS_ERR_INVAL); UNCHANGED(rc,out,before);
    } else if (!strcmp(name,"identity")) {
        struct member_store out,before;memset(&out,0xa5,sizeof(out));before=out;char err[128];
        rc=member_store_identity_decode(b,n,&out,err,sizeof(err));
        assert(rc==EFS_OK || rc==EFS_ERR_UNSUPPORTED);UNCHANGED(rc,out,before);
    } else if (!strcmp(name,"ticket-batch") || !strcmp(name,"ticket-settle")) {
        struct efs_put_ticket out[EFS_PUT_TICKET_BATCH_MAX],before[EFS_PUT_TICKET_BATCH_MAX];
        memset(out,0xa5,sizeof(out));memcpy(before,out,sizeof(out));uint32_t count=123;
        rc=!strcmp(name,"ticket-batch")?efs_put_ticket_batch_decode(b,n,out,&count):efs_put_ticket_settle_decode(b,n,out,&count);
        assert(rc==EFS_OK || rc==EFS_ERR_INVAL);if(rc) {assert(count==123);assert(!memcmp(out,before,sizeof(out)));}
    } else if (!strcmp(name,"hello")) {
        struct efs_msg_hello out,before;memset(&out,0xa5,sizeof(out));before=out;
        rc=efs_peer_hello_decode(b,n,&out);assert(rc==EFS_OK || rc==EFS_ERR_PROTO);UNCHANGED(rc,out,before);
    } else if (!strcmp(name,"recovery-request")) {
        struct efs_msg_recovery_scan out,before;memset(&out,0xa5,sizeof(out));before=out;
        rc=efs_recovery_scan_request_decode(b,n,&out);assert(rc==EFS_OK || rc==EFS_ERR_INVAL);UNCHANGED(rc,out,before);
    } else if (!strcmp(name,"recovery-reply")) {
        struct efs_msg_recovery_scan_reply out,before;memset(&out,0xa5,sizeof(out));before=out;
        uint8_t records[EFS_RECOVERY_SCAN_PAGE*EFS_PUT_TICKET_WIRE_LEN],old[sizeof(records)];
        memset(records,0xa5,sizeof(records));memcpy(old,records,sizeof(old));
        rc=efs_recovery_scan_reply_decode(b,n,EFS_RECOVERY_SCAN_TICKETS,&out,records);
        assert(rc<=0);UNCHANGED(rc,out,before);if(rc)assert(!memcmp(records,old,sizeof(old)));
    } else if (!strcmp(name,"audit-request")) {
        struct efs_audit_request out,before;memset(&out,0xa5,sizeof(out));before=out;
        rc=efs_segment_audit_request_decode(b,n,&out);assert(rc==EFS_OK || rc==EFS_ERR_INVAL);UNCHANGED(rc,out,before);
    } else if (!strcmp(name,"audit-reply")) {
        struct efs_audit_reply out,before;memset(&out,0xa5,sizeof(out));before=out;
        rc=efs_segment_audit_reply_decode(b,n,&out);assert(rc==EFS_OK || rc==EFS_ERR_PROTO);UNCHANGED(rc,out,before);
    } else if (!strcmp(name,"degraded")) {
        struct efs_meta_degraded out,before;memset(&out,0xa5,sizeof(out));before=out;
        rc=efs_degraded_mark_decode(b,n,&out);assert(rc==EFS_OK || rc==EFS_ERR_INVAL);UNCHANGED(rc,out,before);
    } else if (!strcmp(name,"degraded-command")) {
        struct efs_degraded_command out,before;memset(&out,0xa5,sizeof(out));before=out;
        rc=efs_degraded_command_decode(b,n,&out);assert(rc==EFS_OK || rc==EFS_ERR_UNSUPPORTED);UNCHANGED(rc,out,before);
    } else if (!strcmp(name,"recovery-command")) {
        struct efs_meta_degraded out,before;memset(&out,0xa5,sizeof(out));before=out;
        rc=n==EFS_MD_RECOVERY_MARK_LEN && b[0]==EFS_MD_CMD_RECOVERY_MARK?
           efs_degraded_mark_decode(b+1,n-1,&out):EFS_ERR_INVAL;
        assert(rc==EFS_OK || rc==EFS_ERR_INVAL);UNCHANGED(rc,out,before);
    } else if (!strcmp(name,"pack-degraded")) {
        struct efs_pack_degraded_command out,before;memset(&out,0xa5,sizeof(out));before=out;
        rc=efs_pack_degraded_command_decode(b,n,&out);assert(rc==EFS_OK || rc==EFS_ERR_UNSUPPORTED);UNCHANGED(rc,out,before);
    } else if (!strcmp(name,"ticket-drain")) {
        struct efs_put_stream out,before;memset(&out,0xa5,sizeof(out));before=out;
        rc=efs_ticket_drain_decode(b,n,&out);assert(rc==EFS_OK || rc==EFS_ERR_UNSUPPORTED);UNCHANGED(rc,out,before);
    } else if (!strcmp(name,"put-header")) {
        struct efs_put_ticket out,before;memset(&out,0xa5,sizeof(out));before=out;uint32_t fi=123;
        rc=efs_put_upload_header_decode(b,n,&out,&fi);assert(rc==EFS_OK || rc==EFS_ERR_INVAL);
        UNCHANGED(rc,out,before);if(rc)assert(fi==123);
    } else if (!strcmp(name,"cleanup-probe")) {
        struct efs_opid out,before;memset(&out,0xa5,sizeof(out));before=out;
        uint32_t word=123;uint64_t mask=123;rc=efs_cleanup_probe_decode(b,n,&out,&word,&mask);
        assert(rc==EFS_OK || rc==EFS_ERR_INVAL);UNCHANGED(rc,out,before);if(rc)assert(word==123 && mask==123);
    } else if (!strcmp(name,"receipt")) {
        /* This is a bounded in-memory state machine, not a serialized
         * decoder. Exercise its actual key lookup with arbitrary bytes. */
        struct efs_receipt_table table;assert(!efs_receipt_table_init(&table,16));
        struct efs_receipt_key key={0};memcpy(&key,b,n<sizeof(key)?n:sizeof(key));
        struct efs_receipt out,before;memset(&out,0xa5,sizeof(out));before=out;
        assert(!efs_receipt_lookup(&table,&key,&out));assert(!memcmp(&out,&before,sizeof(out)));
        efs_receipt_table_free(&table);rc=EFS_OK;
    } else {
        rc=efs_client_hello_check(b,n); assert(rc==EFS_OK || rc==EFS_ERR_PROTO);
        int sv[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sv));
        struct efs_conn *conn=efs_conn_wrap_tcp(sv[0],1); assert(conn);
        struct efs_decl_gate g={0};
        int v=efs_decl_gate_frame(&g,conn,EFS_MSG_CLIENT_HELLO,b,n);
        assert(v==(rc==EFS_OK?EFS_GATE_ANSWERED:EFS_GATE_REFUSE));
        assert(g.declared==(rc==EFS_OK));
        uint8_t type; void *reply=NULL; uint32_t len;
        assert(!efs_recv_msg(sv[1],&type,&reply,&len));
        assert(type==EFS_MSG_CLIENT_HELLO_REPLY && len==sizeof(struct efs_msg_client_hello_reply));
        free(reply); efs_conn_destroy(conn); close(sv[1]);
    }
    return rc;
}
static void input(const char *name,const uint8_t *b,size_t n,const char *dir)
{
    /* Exact allocation + deliberately unaligned start exposes both bounds and
     * alignment bugs. Persist the current input before entering the decoder. */
    if(dir) { char p[4096]; assert(snprintf(p,sizeof(p),"%s/current.bin",dir)<(int)sizeof(p));
        FILE *f=fopen(p,"wb"); assert(f); assert(fwrite(b,1,n,f)==n); assert(!fclose(f)); }
    uint8_t *p=malloc(n+1); assert(p); memcpy(p+1,b,n); check(name,p+1,n); free(p);
}
static void regressions(void)
{
    /* Exact rejected wire bytes from the partial-output findings. */
    static const uint8_t membership_short[11]={[0]=2,[1]=3,[2]=3,[10]=42};
    static const uint8_t discovery_zero_port[36]={
        'E','F','S','D','I','S','C',1,2,[16]=42,[24]=42,[28]=3,[29]=1,
        [32]=1,[33]='x',[34]=1,[35]='b'};
    assert(check("membership",membership_short,sizeof(membership_short))==EFS_ERR_DECODE);
    assert(check("discovery-answer",discovery_zero_port,sizeof(discovery_zero_port))==-1);
    /* Intel UBSan input: a malformed declaration at an unaligned address. */
    static const uint8_t declaration[16]={0xba,'F','S','P',5};
    uint8_t unaligned[17];memcpy(unaligned+1,declaration,sizeof(declaration));
    assert(check("declaration",unaligned+1,sizeof(declaration))==EFS_ERR_PROTO);
    uint8_t batch[8192];size_t n=seed("ticket-batch",batch);
    /* First ticket is valid; the second's first node is zero. Failure
     * cannot leave the first ticket in the caller's pre-filled array. */
    memset(batch+3+EFS_PUT_TICKET_WIRE_LEN+88,0,4);
    assert(check("ticket-batch",batch,n)==EFS_ERR_INVAL);
}
int main(int argc,char **argv)
{
    if(argc<2 || argc>4) {fprintf(stderr,"usage: %s DECODER [SECONDS [ARTIFACT_DIR]]\n",argv[0]);return 2;}
    regressions();
    const char *name=argv[1],*dir=argc==4?argv[3]:NULL;
    unsigned seconds=argc>2?(unsigned)strtoul(argv[2],NULL,10):0;
    uint8_t valid[8192],b[8192]; size_t n=seed(name,valid);
    assert(check(name,valid,n)==EFS_OK);
    for(size_t i=0;i<n;i++)input(name,valid,i,dir);
    memcpy(b,valid,n); b[n]=0; input(name,b,n+1,dir);
    for(size_t i=0;i<n;i++) {memcpy(b,valid,n);b[i]^=0xff;input(name,b,n,dir);}
    struct timespec start,now; clock_gettime(CLOCK_MONOTONIC,&start);
    uint64_t rounds=0;
    do {
        memcpy(b,valid,n); size_t len=n; unsigned mode=next()%5;
        if(mode==0)len=next()%(n+1);
        else if(mode==1) {len=n+1+next()%32; memset(b+n,0xff,len-n);}
        else if(mode==2) {len=next()%(n+33);for(size_t j=0;j<len;j++)b[j]=(uint8_t)next();}
        else for(unsigned j=0,k=1+next()%8;j<k;j++)b[next()%n]^=(uint8_t)(1+next()%255);
        input(name,b,len,dir); rounds++; clock_gettime(CLOCK_MONOTONIC,&now);
    } while((seconds && (now.tv_sec-start.tv_sec)+(now.tv_nsec-start.tv_nsec)/1e9<seconds) || (!seconds && rounds<2000));
    printf("PASS %s seed=0x20261025c mutations=%llu elapsed=%llds\n",name,(unsigned long long)rounds,(long long)(now.tv_sec-start.tv_sec));
    return 0;
}
