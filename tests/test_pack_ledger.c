#include "efs/pack_ledger.h"
#include "efs/checksum.h"
#include "efs/kv.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct efs_pack_ledger ledger, saved, decoded;
static uint8_t wire[EFS_PACK_LEDGER_MAX_BYTES], copy[EFS_PACK_LEDGER_MAX_BYTES];
static struct efs_pack_ref refs[EFS_PACK_MAX_RECORDS];
static void roundtrip(void)
{
    size_t bytes=0;assert(!efs_pack_ledger_encode(&ledger,wire,sizeof(wire),&bytes));
    assert(!efs_pack_ledger_decode(wire,bytes,&decoded));
    assert(!memcmp(&ledger,&decoded,sizeof(ledger)));
}
static void resign(uint8_t *p,size_t n)
{efs_hash(p,n-EFS_HASH_SIZE,p+n-EFS_HASH_SIZE);}
int main(void)
{
    uint8_t *image=malloc(EFS_SEGMENT_MAX_SIZE);assert(image);
    uint8_t payload[17];memset(payload,0x17,sizeof(payload));
    struct efs_pack_record records[EFS_PACK_MAX_RECORDS];
    for(unsigned i=0;i<EFS_PACK_MAX_RECORDS;i++)records[i]=(struct efs_pack_record){
        .ino=100+i,.inode_generation=7,.record_generation=9+i,.content_epoch=11,
        .len=sizeof(payload),.data=payload};
    struct efs_opid owner={.client_uuid={13},.session_epoch=17,.seq=19};size_t geometry;
    assert(!efs_pack_build(23,29,2,records,16,image,EFS_SEGMENT_MAX_SIZE,&geometry,refs));
    assert(!efs_pack_ledger_init(image,geometry,refs,16,&owner,15,0,&ledger));
    saved=ledger;struct efs_pack_ref swapped=refs[0];refs[0]=refs[1];refs[1]=swapped;
    assert(efs_pack_ledger_init(image,geometry,refs,16,&owner,15,0,&ledger)==EFS_ERR_INVAL);
    assert(!memcmp(&ledger,&saved,sizeof(saved)));
    swapped=refs[0];refs[0]=refs[1];refs[1]=swapped;
    roundtrip();
    /* No collection while even one prospective publication is unresolved. */
    assert(efs_pack_ledger_reclaim_begin(&ledger)==EFS_ERR_BUSY);
    assert(efs_pack_ledger_fence_ack(&ledger,1)==EFS_ERR_BUSY);
    assert(efs_pack_ledger_release(&ledger,&refs[0])==EFS_ERR_STALE);
    assert(!efs_pack_ledger_publish(&ledger,&refs[0]));
    assert(!efs_pack_ledger_publish(&ledger,&refs[0]));
    assert(efs_pack_ledger_cancel(&ledger,&refs[0])==EFS_ERR_STALE);
    saved=ledger;struct efs_pack_ref bad=refs[0];bad.content_epoch++;
    assert(efs_pack_ledger_release(&ledger,&bad)==EFS_ERR_STALE);
    assert(!memcmp(&ledger,&saved,sizeof(ledger)));
    for(unsigned i=1;i<16;i++)assert(!efs_pack_ledger_publish(&ledger,&refs[i]));
    for(unsigned i=0;i<15;i++) {
        assert(!efs_pack_ledger_release(&ledger,&refs[i]));
        assert(!efs_pack_ledger_release(&ledger,&refs[i]));
        assert(efs_pack_ledger_publish(&ledger,&refs[i])==EFS_ERR_STALE);
        assert(efs_pack_ledger_reclaim_begin(&ledger)==EFS_ERR_BUSY);
    }
    {   /* Counters follow the states: one live 17-byte record, 15 dead. */
        struct efs_pack_ledger_usage u;efs_pack_ledger_usage(&ledger,&u);
        assert(u.live==1 && u.live_bytes==17 && u.dead==15 && u.dead_bytes==15*17 &&
               !u.pending && u.total_bytes==16*17);
    }
    roundtrip();assert(!efs_pack_ledger_release(&ledger,&refs[15]));
    assert(!efs_pack_ledger_reclaim_begin(&ledger));roundtrip();
    /* Fence all captured members, including a fourth possible stale uploader,
     * before the first physical deletion; ACKs must be single captured bits. */
    assert(efs_pack_ledger_fence_ack(&ledger,3)==EFS_ERR_INVAL);
    assert(efs_pack_ledger_fence_ack(&ledger,16)==EFS_ERR_INVAL);
    for(unsigned bit=1;bit<8;bit<<=1) {
        assert(!efs_pack_ledger_fence_ack(&ledger,bit));
        assert(!efs_pack_ledger_fence_ack(&ledger,bit));
        assert(efs_pack_ledger_delete_ack(&ledger,1)==EFS_ERR_BUSY);roundtrip();
    }
    assert(!efs_pack_ledger_fence_ack(&ledger,8));
    for(unsigned bit=1;bit<=8;bit<<=1) {
        assert(!efs_pack_ledger_delete_ack(&ledger,bit));roundtrip();
        assert(!efs_pack_ledger_delete_ack(&ledger,bit));
    }
    assert(ledger.phase==EFS_PACK_RETIRED);
    assert(efs_pack_ledger_publish(&ledger,&refs[0])==EFS_ERR_STALE);
    assert(!efs_pack_ledger_reclaim_begin(&ledger));
    /* Corruption and checksum-valid malformed states must not change output. */
    size_t n;assert(!efs_pack_ledger_encode(&ledger,wire,sizeof(wire),&n));
    for(size_t i=0;i<n;i+=37) {
        memcpy(copy,wire,n);copy[i]^=1;decoded=saved;
        assert(efs_pack_ledger_decode(copy,n,&decoded)!=EFS_OK);
        assert(!memcmp(&decoded,&saved,sizeof(saved)));
    }
    unsigned offsets[]={8,40,48,52,56,60,124,128,132,140,144,152,EFS_PACK_LEDGER_HEADER+32,EFS_PACK_LEDGER_HEADER+33};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
        memcpy(copy,wire,n);copy[offsets[i]]=0xff;resign(copy,n);
        assert(efs_pack_ledger_decode(copy,n,&decoded)==EFS_ERR_PROTO);
    }
    memcpy(copy,wire,n);memcpy(copy+EFS_PACK_LEDGER_HEADER+EFS_PACK_LEDGER_RECORD,copy+EFS_PACK_LEDGER_HEADER,32);resign(copy,n);
    assert(efs_pack_ledger_decode(copy,n,&decoded)==EFS_ERR_PROTO);
    assert(efs_pack_ledger_decode(wire,n-1,&decoded)==EFS_ERR_PROTO);
    assert(efs_pack_ledger_decode(wire,n+1,&decoded)==EFS_ERR_PROTO);
    memset(copy,0x5a,sizeof(copy));size_t unchanged=117;
    assert(efs_pack_ledger_encode(&ledger,copy,n-1,&unchanged)==EFS_ERR_OVERFLOW);
    assert(unchanged==117 && copy[0]==0x5a && copy[n-1]==0x5a);
    /* Cancelled admissions cannot later publish or masquerade as released. */
    assert(!efs_pack_ledger_init(image,geometry,refs,16,&owner,7,0,&ledger));
    for(unsigned i=0;i<16;i++) {
        assert(!efs_pack_ledger_cancel(&ledger,&refs[i]));
        assert(!efs_pack_ledger_cancel(&ledger,&refs[i]));
        assert(efs_pack_ledger_publish(&ledger,&refs[i])==EFS_ERR_STALE);
        assert(efs_pack_ledger_release(&ledger,&refs[i])==EFS_ERR_STALE);
    }
    assert(!efs_pack_ledger_reclaim_begin(&ledger));roundtrip();
    /* The caller plans several records from one loaded ledger, then commits
     * ledger + file mapping together. An atomic-batch failure changes neither. */
    assert(!efs_pack_ledger_init(image,geometry,refs,16,&owner,7,0,&ledger));
    struct efs_kv *kv=efs_kv_mem_create();assert(kv);
    uint8_t key[]={0,1,36},mapping[]={0,1,4},mapval[EFS_PACK_REF_BYTES];
    assert(!efs_pack_ledger_encode(&ledger,wire,sizeof(wire),&n));
    assert(!efs_kv_put(kv,key,sizeof(key),wire,(uint32_t)n));
    saved=ledger;
    assert(!efs_pack_ledger_publish(&ledger,&refs[0]));
    assert(!efs_pack_ledger_publish(&ledger,&refs[1]));
    assert(!efs_pack_ledger_encode(&ledger,wire,sizeof(wire),&n));
    assert(!efs_pack_ref_encode(&refs[0],mapval));
    struct efs_kv_item items[]={
        {EFS_KV_PUT,key,sizeof(key),wire,(uint32_t)n},
        {EFS_KV_PUT,mapping,sizeof(mapping),mapval,sizeof(mapval)}};
    assert(!efs_kv_mem_fail_next_batch(kv));assert(efs_kv_batch(kv,items,2)==EFS_ERR_IO);
    uint32_t bytes=sizeof(copy);assert(!efs_kv_get(kv,key,sizeof(key),copy,&bytes));
    assert(!efs_pack_ledger_decode(copy,bytes,&decoded));assert(!memcmp(&decoded,&saved,sizeof(saved)));
    bytes=sizeof(copy);assert(efs_kv_get(kv,mapping,sizeof(mapping),copy,&bytes)==EFS_ERR_NOT_FOUND);
    assert(!efs_kv_batch(kv,items,2));bytes=sizeof(copy);
    assert(!efs_kv_get(kv,key,sizeof(key),copy,&bytes));assert(!efs_pack_ledger_decode(copy,bytes,&decoded));
    assert(decoded.state[0]==EFS_PACK_LIVE && decoded.state[1]==EFS_PACK_LIVE && decoded.state[2]==EFS_PACK_PENDING);
    efs_kv_mem_free(kv);
    /* Maximum occupancy has bounded state and serialized size. */
    assert(!efs_pack_build(23,29,2,records,EFS_PACK_MAX_RECORDS,image,EFS_SEGMENT_MAX_SIZE,&geometry,refs));
    assert(!efs_pack_ledger_init(image,geometry,refs,EFS_PACK_MAX_RECORDS,&owner,15,0,&ledger));
    roundtrip();assert(!efs_pack_ledger_encode(&ledger,wire,sizeof(wire),&n));
    assert(n==EFS_PACK_LEDGER_MAX_BYTES);
    /* Nonadjacent duplicates at maximum occupancy still fail atomically. */
    saved=decoded;
    memcpy(copy,wire,n);
    memcpy(copy+EFS_PACK_LEDGER_HEADER+(EFS_PACK_MAX_RECORDS-1)*EFS_PACK_LEDGER_RECORD,
           copy+EFS_PACK_LEDGER_HEADER,EFS_HASH_SIZE);
    resign(copy,n);
    assert(efs_pack_ledger_decode(copy,n,&decoded)==EFS_ERR_PROTO);
    assert(!memcmp(&decoded,&saved,sizeof(saved)));
    saved=ledger;refs[1]=refs[0];
    assert(efs_pack_ledger_init(image,geometry,refs,EFS_PACK_MAX_RECORDS,&owner,15,0,&ledger)==EFS_ERR_INVAL);
    assert(!memcmp(&ledger,&saved,sizeof(saved)));
    {   /* The cold flag round-trips; unknown flags are refused. */
        assert(!efs_pack_build(23,29,2,records,EFS_PACK_MAX_RECORDS,image,EFS_SEGMENT_MAX_SIZE,&geometry,refs));
        assert(!efs_pack_ledger_init(image,geometry,refs,EFS_PACK_MAX_RECORDS,&owner,15,EFS_PACK_LEDGER_F_COLD,&ledger));
        roundtrip();assert(decoded.flags==EFS_PACK_LEDGER_F_COLD);
        assert(efs_pack_ledger_init(image,geometry,refs,EFS_PACK_MAX_RECORDS,&owner,15,2,&ledger)==EFS_ERR_INVAL);
    }
    free(image);puts("pack ledger: live/dead counters, cold flag, neighbors, pending retention, replay fences, captured-member GC, corruption and atomic mapping batch PASS");
}
