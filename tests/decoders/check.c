/* Task C: fixed-seed mutations of actual product decoders, not reimplementations.
 * A rejected input must leave caller-owned output byte-for-byte unchanged. */
#include "efs/membership.h"
#include "efs/member_view.h"
#include "efs/frag_record.h"
#include "efs/put_ticket.h"
#include "efs/protocol.h"
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
    } else if (!strcmp(name,"fragment")) {
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
    } else if (!strcmp(name,"ticket")) {
        struct efs_put_ticket out,before; memset(&out,0xa5,sizeof(out)); before=out;
        rc=efs_put_ticket_decode(b,n,&out); assert(rc==EFS_OK || rc==EFS_ERR_INVAL); UNCHANGED(rc,out,before);
    } else {
        rc=efs_client_hello_check(b,n); assert(rc==EFS_OK || rc==EFS_ERR_PROTO);
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
int main(int argc,char **argv)
{
    if(argc<2 || argc>4) {fprintf(stderr,"usage: %s DECODER [SECONDS [ARTIFACT_DIR]]\n",argv[0]);return 2;}
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
    } while((seconds && now.tv_sec-start.tv_sec<(time_t)seconds) || (!seconds && rounds<2000));
    printf("PASS %s seed=0x20261025c mutations=%llu elapsed=%llds\n",name,(unsigned long long)rounds,(long long)(now.tv_sec-start.tv_sec));
    return 0;
}
