#include "efs/pack_ledger.h"
#include "efs/checksum.h"
#include <string.h>
#include <stdlib.h>

static void put(uint8_t *p, uint64_t n, unsigned bytes)
{ while (bytes) { p[--bytes]=(uint8_t)n; n>>=8; } }
static uint64_t get(const uint8_t *p, unsigned bytes)
{ uint64_t n=0;while(bytes--)n=(n<<8)|*p++;return n; }
static int valid(const struct efs_pack_ledger *s)
{
    if(!s || !s->segment_id || !s->segment_generation ||
       s->geometry<EFS_SEGMENT_ALIGNMENT || s->geometry>EFS_SEGMENT_MAX_SIZE ||
       s->geometry%EFS_SEGMENT_ALIGNMENT || !s->count || s->count>EFS_PACK_MAX_RECORDS ||
       EFS_PACK_HEADER_BYTES+s->count*(EFS_PACK_RECORD_BYTES+1u)+EFS_HASH_SIZE>s->geometry ||
       (s->flags & ~EFS_PACK_LEDGER_F_COLD) ||
       !s->member_mask || (s->member_mask & ~((1u<<EFS_MAX_NODES)-1u)) ||
       __builtin_popcount(s->member_mask)<EFS_NUM_FRAGMENTS ||
       s->phase<EFS_PACK_ACTIVE || s->phase>EFS_PACK_RETIRED ||
       (s->fence_acks & ~s->member_mask) || (s->delete_acks & ~s->member_mask) ||
       !s->owner.session_epoch || !s->owner.seq) return 0;
    unsigned uuid=0;for(unsigned i=0;i<EFS_OPID_UUID_LEN;i++)uuid|=s->owner.client_uuid[i];
    if(!uuid || (s->phase==EFS_PACK_ACTIVE && (s->fence_acks || s->delete_acks)) ||
       (s->delete_acks && s->fence_acks!=s->member_mask) ||
       (s->phase==EFS_PACK_RETIRED && s->delete_acks!=s->member_mask) ||
       (s->phase==EFS_PACK_RECLAIMING && s->delete_acks==s->member_mask))return 0;
    for(unsigned i=0;i<s->count;i++) {
        if(s->state[i]<EFS_PACK_PENDING || s->state[i]>EFS_PACK_CANCELLED ||
           !s->len[i] || s->len[i]>EFS_PACK_MAX_PAYLOAD ||
           (s->phase!=EFS_PACK_ACTIVE && s->state[i]<=EFS_PACK_LIVE))return 0;
    }
    return 1;
}
static int digest_order(const void *a, const void *b)
{
    const uint8_t *const *x = a, *const *y = b;
    return memcmp(*x, *y, EFS_HASH_SIZE);
}
static int unique(const struct efs_pack_ledger *s)
{
    /* Validation runs on every maintenance read. Pairwise comparison made
     * a full ledger O(count^2), extending KV read-lock holds into seconds.
     * Sort pointers, not records: slot order and the wire format stay intact. */
    const uint8_t *sorted[EFS_PACK_MAX_RECORDS];
    for (unsigned i = 0; i < s->count; i++) sorted[i] = s->reference_digest[i];
    qsort(sorted, s->count, sizeof(sorted[0]), digest_order);
    for (unsigned i = 1; i < s->count; i++)
        if (!memcmp(sorted[i-1], sorted[i], EFS_HASH_SIZE)) return 0;
    return 1;
}
int efs_pack_ledger_init(const uint8_t *image,size_t geometry,
    const struct efs_pack_ref *refs,uint32_t count,const struct efs_opid *owner,
    uint32_t members,uint32_t flags,struct efs_pack_ledger *out)
{
    if(!refs || !owner || !out || !count || count>EFS_PACK_MAX_RECORDS ||
       efs_pack_validate(image,geometry)!=EFS_OK || get(image+24,4)!=count)return EFS_ERR_INVAL;
    struct efs_pack_ledger s={0};
    s.segment_id=get(image+32,8);s.segment_generation=get(image+40,8);
    s.publication_group=(uint32_t)get(image+48,4);s.geometry=(uint32_t)geometry;
    s.count=count;s.member_mask=members;s.phase=EFS_PACK_ACTIVE;s.owner=*owner;s.flags=flags;
    efs_hash(image,geometry,s.body_digest);
    for(unsigned i=0;i<count;i++) {
        const uint8_t *bytes;uint32_t len;uint8_t wire[EFS_PACK_REF_BYTES];
        if(efs_pack_read(image,geometry,&refs[i],&bytes,&len)!=EFS_OK ||
           efs_pack_ref_encode(&refs[i],wire)!=EFS_OK)return EFS_ERR_INVAL;
        if(i && refs[i].offset<=refs[i-1].offset)return EFS_ERR_INVAL;
        for(unsigned j=0;j<i;j++)if(refs[i].ino==refs[j].ino)return EFS_ERR_INVAL;
        efs_hash(wire,sizeof(wire),s.reference_digest[i]);s.state[i]=EFS_PACK_PENDING;s.len[i]=len;
    }
    if(!valid(&s))return EFS_ERR_INVAL;
    *out=s;return EFS_OK;
}
int efs_pack_ledger_encode(const struct efs_pack_ledger *s,uint8_t *out,size_t capacity,size_t *bytes)
{
    if(!out || !bytes || !valid(s) || !unique(s))return EFS_ERR_INVAL;
    size_t n=EFS_PACK_LEDGER_HEADER+EFS_PACK_LEDGER_RECORD*s->count+EFS_HASH_SIZE;
    struct efs_pack_ledger_usage u;
    if(capacity<n)return EFS_ERR_OVERFLOW;
    efs_pack_ledger_usage(s,&u);
    memset(out,0,n);memcpy(out,"EFSLIFE2",8);put(out+8,EFS_PACK_LEDGER_VERSION,4);put(out+12,n,4);
    put(out+16,s->segment_id,8);put(out+24,s->segment_generation,8);
    put(out+32,s->geometry,4);put(out+36,s->publication_group,4);
    put(out+40,s->count,4);put(out+44,s->member_mask,4);put(out+48,s->phase,4);
    put(out+52,s->fence_acks,4);put(out+56,s->delete_acks,4);
    memcpy(out+64,s->owner.client_uuid,16);put(out+80,s->owner.session_epoch,4);
    put(out+84,s->owner.seq,8);memcpy(out+92,s->body_digest,EFS_HASH_SIZE);
    put(out+124,s->flags,4);put(out+128,u.live,4);put(out+132,u.live_bytes,8);
    put(out+140,u.dead,4);put(out+144,u.dead_bytes,8);
    for(unsigned i=0;i<s->count;i++) {
        uint8_t *r=out+EFS_PACK_LEDGER_HEADER+EFS_PACK_LEDGER_RECORD*i;
        memcpy(r,s->reference_digest[i],EFS_HASH_SIZE);r[32]=s->state[i];put(r+33,s->len[i],4);
    }
    efs_hash(out,n-EFS_HASH_SIZE,out+n-EFS_HASH_SIZE);*bytes=n;return EFS_OK;
}
int efs_pack_ledger_decode(const uint8_t *in,size_t bytes,struct efs_pack_ledger *out)
{
    if(!in || !out || bytes<EFS_PACK_LEDGER_HEADER+EFS_PACK_LEDGER_RECORD+EFS_HASH_SIZE ||
       bytes>EFS_PACK_LEDGER_MAX_BYTES || memcmp(in,"EFSLIFE2",8) ||
       get(in+8,4)!=EFS_PACK_LEDGER_VERSION || get(in+12,4)!=bytes || get(in+60,4))return EFS_ERR_PROTO;
    for(unsigned i=152;i<EFS_PACK_LEDGER_HEADER;i++)if(in[i])return EFS_ERR_PROTO;
    uint32_t count=(uint32_t)get(in+40,4);uint8_t sum[EFS_HASH_SIZE];
    if(!count || count>EFS_PACK_MAX_RECORDS || bytes!=EFS_PACK_LEDGER_HEADER+EFS_PACK_LEDGER_RECORD*count+EFS_HASH_SIZE)return EFS_ERR_PROTO;
    efs_hash(in,bytes-EFS_HASH_SIZE,sum);
    if(memcmp(sum,in+bytes-EFS_HASH_SIZE,EFS_HASH_SIZE))return EFS_ERR_CHECKSUM;
    struct efs_pack_ledger s={0};
    s.segment_id=get(in+16,8);s.segment_generation=get(in+24,8);
    s.geometry=(uint32_t)get(in+32,4);s.publication_group=(uint32_t)get(in+36,4);
    s.count=count;s.member_mask=(uint32_t)get(in+44,4);s.phase=(uint32_t)get(in+48,4);
    s.fence_acks=(uint32_t)get(in+52,4);s.delete_acks=(uint32_t)get(in+56,4);
    memcpy(s.owner.client_uuid,in+64,16);s.owner.session_epoch=(uint32_t)get(in+80,4);
    s.owner.seq=get(in+84,8);memcpy(s.body_digest,in+92,EFS_HASH_SIZE);
    s.flags=(uint32_t)get(in+124,4);
    for(unsigned i=0;i<count;i++) {
        const uint8_t *r=in+EFS_PACK_LEDGER_HEADER+EFS_PACK_LEDGER_RECORD*i;
        memcpy(s.reference_digest[i],r,EFS_HASH_SIZE);s.state[i]=r[32];s.len[i]=(uint32_t)get(r+33,4);
    }
    if(!valid(&s) || !unique(&s))return EFS_ERR_PROTO;
    /* The stored totals are redundant with the states: a mismatch is corruption. */
    struct efs_pack_ledger_usage u;efs_pack_ledger_usage(&s,&u);
    if(get(in+128,4)!=u.live || get(in+132,8)!=u.live_bytes ||
       get(in+140,4)!=u.dead || get(in+144,8)!=u.dead_bytes)return EFS_ERR_PROTO;
    *out=s;return EFS_OK;
}
void efs_pack_ledger_usage(const struct efs_pack_ledger *s,struct efs_pack_ledger_usage *u)
{
    memset(u,0,sizeof(*u));
    for(unsigned i=0;i<s->count;i++) {
        u->total_bytes+=s->len[i];
        if(s->state[i]==EFS_PACK_PENDING){u->pending++;u->pending_bytes+=s->len[i];}
        else if(s->state[i]==EFS_PACK_LIVE){u->live++;u->live_bytes+=s->len[i];}
        else {u->dead++;u->dead_bytes+=s->len[i];}
    }
}
int efs_pack_ledger_slot(const struct efs_pack_ledger *s,const struct efs_pack_ref *ref,unsigned *at)
{
    if(!valid(s) || !ref)return EFS_ERR_INVAL;
    uint8_t wire[EFS_PACK_REF_BYTES],digest[EFS_HASH_SIZE];
    if(efs_pack_ref_encode(ref,wire)!=EFS_OK)return EFS_ERR_INVAL;
    efs_hash(wire,sizeof(wire),digest);
    for(unsigned i=0;i<s->count;i++)if(!memcmp(s->reference_digest[i],digest,sizeof(digest))){*at=i;return EFS_OK;}
    return EFS_ERR_STALE;
}
static int transition(struct efs_pack_ledger *s,const struct efs_pack_ref *ref,unsigned from,unsigned to)
{
    unsigned i;int rc=efs_pack_ledger_slot(s,ref,&i);if(rc)return rc;
    if(s->state[i]==to)return EFS_OK;
    if(s->phase!=EFS_PACK_ACTIVE || s->state[i]!=from)return EFS_ERR_STALE;
    s->state[i]=(uint8_t)to;return EFS_OK;
}
int efs_pack_ledger_publish(struct efs_pack_ledger *s,const struct efs_pack_ref *r)
{return transition(s,r,EFS_PACK_PENDING,EFS_PACK_LIVE);}
int efs_pack_ledger_release(struct efs_pack_ledger *s,const struct efs_pack_ref *r)
{return transition(s,r,EFS_PACK_LIVE,EFS_PACK_RELEASED);}
int efs_pack_ledger_cancel(struct efs_pack_ledger *s,const struct efs_pack_ref *r)
{return transition(s,r,EFS_PACK_PENDING,EFS_PACK_CANCELLED);}
int efs_pack_ledger_reclaim_begin(struct efs_pack_ledger *s)
{
    if(!valid(s))return EFS_ERR_INVAL;
    if(s->phase!=EFS_PACK_ACTIVE)return EFS_OK;
    for(unsigned i=0;i<s->count;i++)if(s->state[i]<=EFS_PACK_LIVE)return EFS_ERR_BUSY;
    s->phase=EFS_PACK_RECLAIMING;return EFS_OK;
}
static int member(const struct efs_pack_ledger *s,uint32_t bit)
{return valid(s) && bit && !(bit&(bit-1)) && (bit&s->member_mask);}
int efs_pack_ledger_fence_ack(struct efs_pack_ledger *s,uint32_t bit)
{
    if(!member(s,bit))return EFS_ERR_INVAL;
    if(s->phase==EFS_PACK_ACTIVE)return EFS_ERR_BUSY;
    s->fence_acks|=bit;return EFS_OK;
}
int efs_pack_ledger_delete_ack(struct efs_pack_ledger *s,uint32_t bit)
{
    if(!member(s,bit))return EFS_ERR_INVAL;
    if(s->phase==EFS_PACK_ACTIVE || s->fence_acks!=s->member_mask)return EFS_ERR_BUSY;
    s->delete_acks|=bit;
    if(s->delete_acks==s->member_mask)s->phase=EFS_PACK_RETIRED;
    return EFS_OK;
}
