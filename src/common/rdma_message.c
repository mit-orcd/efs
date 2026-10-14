#include "efs/rdma_message.h"
#include <stdlib.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t v)
{ p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static uint32_t get32(const uint8_t *p)
{ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static int setup_valid(const struct efs_rmsg_setup *s)
{
    if (s->capabilities!=EFS_RMSG_CAP_SEGMENTED ||
        s->physical_cap<EFS_RMSG_ENVELOPE+5 || s->physical_cap>86016 ||
        s->logical_cap<5 || s->logical_cap>EFS_RMSG_LOGICAL_MAX ||
        s->receives<2 || s->receives>64 || !s->outstanding ||
        s->outstanding>EFS_RMSG_QUEUE_MAX || s->address_version>1 ||
        s->lid>65535 || !s->qpn || s->qpn>0xffffff || s->psn>0xffffff ||
        s->mtu<1 || s->mtu>5) return 0;
    unsigned nonzero=0;
    for (unsigned i=0;i<16;i++) nonzero|=s->gid[i];
    return s->address_version ? !!nonzero : (!nonzero && s->lid);
}
int efs_rmsg_setup_encode(uint8_t out[EFS_RMSG_SETUP_BYTES], const struct efs_rmsg_setup *s)
{
    if (!out || !s || !setup_valid(s)) return -1;
    memset(out,0,EFS_RMSG_SETUP_BYTES); memcpy(out,"EFSS",4);
    const uint32_t v[]={EFS_RMSG_VERSION,s->capabilities,s->physical_cap,
        s->logical_cap,s->receives,s->outstanding,s->address_version,
        s->lid,s->qpn,s->psn,s->mtu};
    for (unsigned i=0;i<11;i++) put32(out+4+i*4,v[i]);
    memcpy(out+48,s->gid,16); return 0;
}
int efs_rmsg_setup_decode(struct efs_rmsg_setup *s, const void *wire, size_t len)
{
    const uint8_t *p=wire;
    if (!s || !p || len!=EFS_RMSG_SETUP_BYTES || memcmp(p,"EFSS",4) ||
        get32(p+4)!=EFS_RMSG_VERSION || get32(p+64) || get32(p+68)) return -1;
    s->capabilities=get32(p+8); s->physical_cap=get32(p+12);
    s->logical_cap=get32(p+16); s->receives=get32(p+20);
    s->outstanding=get32(p+24); s->address_version=get32(p+28);
    s->lid=get32(p+32); s->qpn=get32(p+36); s->psn=get32(p+40);
    s->mtu=get32(p+44); memcpy(s->gid,p+48,16);
    return setup_valid(s) ? 0 : -1;
}
int efs_rmsg_negotiate(struct efs_rmsg_setup *limits,
                       const struct efs_rmsg_setup *local,
                       const struct efs_rmsg_setup *peer)
{
    if (!limits || !local || !peer || !setup_valid(local) ||
        !setup_valid(peer) || local->address_version!=peer->address_version) return -1;
    *limits=*peer;
    if (local->physical_cap<limits->physical_cap) limits->physical_cap=local->physical_cap;
    if (local->logical_cap<limits->logical_cap) limits->logical_cap=local->logical_cap;
    if (local->outstanding<limits->outstanding) limits->outstanding=local->outstanding;
    if (local->mtu<limits->mtu) limits->mtu=local->mtu;
    /* receives remains the actual peer count, not a symmetric assumption. */
    return 0;
}
static int valid(const struct efs_rmsg_header *h)
{
    if (!h->id) return 0;
    if (h->kind == EFS_RMSG_CREDIT)
        return !h->flags && !h->total && !h->offset && !h->length;
    if (h->kind != EFS_RMSG_DATA || (h->flags & ~EFS_RMSG_FINAL) ||
        h->total < 5 || !h->length || h->offset > h->total ||
        h->length > h->total-h->offset) return 0;
    return !!(h->flags & EFS_RMSG_FINAL) ==
           (h->length == h->total-h->offset);
}
int efs_rmsg_encode(uint8_t out[EFS_RMSG_ENVELOPE], const struct efs_rmsg_header *h)
{
    if (!out || !h || !valid(h)) return -1;
    memset(out,0,EFS_RMSG_ENVELOPE);
    memcpy(out,"EFSG",4); out[4]=EFS_RMSG_VERSION; out[5]=h->kind;
    out[6]=h->flags>>8; out[7]=h->flags;
    put32(out+8,h->id>>32); put32(out+12,(uint32_t)h->id);
    put32(out+16,h->total); put32(out+20,h->offset); put32(out+24,h->length);
    return 0;
}
int efs_rmsg_decode(struct efs_rmsg_header *h, const void *frame, size_t len)
{
    const uint8_t *p=frame;
    if (!h || !p || len<EFS_RMSG_ENVELOPE || memcmp(p,"EFSG",4) ||
        p[4]!=EFS_RMSG_VERSION || get32(p+28)) return -1;
    h->kind=p[5]; h->flags=((uint16_t)p[6]<<8)|p[7];
    h->id=((uint64_t)get32(p+8)<<32)|get32(p+12);
    h->total=get32(p+16); h->offset=get32(p+20); h->length=get32(p+24);
    return valid(h) && len-EFS_RMSG_ENVELOPE==h->length ? 0 : -1;
}
static int reserve(struct efs_rmsg_rx *r, uint32_t n)
{
    if (n>r->limit-r->used) return -1;
    uint64_t old=__atomic_load_n(&r->global->used,__ATOMIC_RELAXED);
    do {
        if (old>r->global->limit || n>r->global->limit-old) return -1;
    } while (!__atomic_compare_exchange_n(&r->global->used,&old,old+n,0,
                                          __ATOMIC_RELAXED,__ATOMIC_RELAXED));
    r->used+=n;
    return 0;
}
static void unreserve(struct efs_rmsg_rx *r, uint32_t n)
{
    r->used-=n;
    __atomic_fetch_sub(&r->global->used,n,__ATOMIC_RELAXED);
}
void efs_rmsg_rx_release(struct efs_rmsg_rx *r)
{
    if (!r->count) return;
    struct efs_rmsg_view *v=&r->queue[r->head];
    r->dealloc(v->frame); unreserve(r,v->len); memset(v,0,sizeof(*v));
    r->head=(r->head+1)%r->capacity; r->count--;
}
void efs_rmsg_rx_destroy(struct efs_rmsg_rx *r)
{
    if (r->active) {
        r->dealloc(r->active); unreserve(r,r->active_total); r->active=NULL;
    }
    while (r->count) efs_rmsg_rx_release(r);
    r->active_total=r->received=0; r->broken=1;
}
static int fail(struct efs_rmsg_rx *r)
{
    /* A synchronous handler may borrow the FIFO head while send-side
     * progress encounters a corrupt later message. Preserve complete views
     * until explicit release/destruction; never free beneath that borrower. */
    if (r->active) {
        r->dealloc(r->active); unreserve(r,r->active_total); r->active=NULL;
    }
    r->active_total=r->received=0; r->broken=1; return -1;
}
int efs_rmsg_rx_init(struct efs_rmsg_rx *r, struct efs_rmsg_budget *global,
                     uint64_t limit, uint32_t physical_cap,
                     uint32_t logical_cap, unsigned capacity)
{
    if (!r || !global || !limit || physical_cap<EFS_RMSG_ENVELOPE+5 ||
        logical_cap<5 || logical_cap>limit || !capacity ||
        capacity>EFS_RMSG_QUEUE_MAX) return -1;
    memset(r,0,sizeof(*r)); r->global=global; r->limit=limit;
    r->physical_cap=physical_cap; r->logical_cap=logical_cap;
    r->capacity=capacity; r->alloc=malloc; r->dealloc=free;
    return 0;
}
int efs_rmsg_rx_feed(struct efs_rmsg_rx *r, const void *frame, size_t len)
{
    struct efs_rmsg_header h;
    if (r->broken) return -1;
    if (len>r->physical_cap || efs_rmsg_decode(&h,frame,len) ||
        h.kind!=EFS_RMSG_DATA || h.total>r->logical_cap) return fail(r);
    const uint8_t *p=(const uint8_t *)frame+EFS_RMSG_ENVELOPE;
    if (!r->active) {
        /* Validate the original wire length before reserving/malloc. */
        if (r->last_id==UINT64_MAX || h.id!=r->last_id+1 || h.offset ||
            h.length<5 || get32(p)!=h.total-4 || r->count==r->capacity ||
            reserve(r,h.total)) return fail(r);
        r->active=r->alloc(h.total);
        if (!r->active) { unreserve(r,h.total); return fail(r); }
        r->active_total=h.total; r->received=0;
    } else if (h.id!=r->last_id+1 || h.total!=r->active_total) return fail(r);
    if (h.offset!=r->received) return fail(r);
    memcpy(r->active+r->received,p,h.length); r->received+=h.length;
    if (!(h.flags&EFS_RMSG_FINAL)) return 0;
    unsigned tail=(r->head+r->count)%r->capacity;
    r->queue[tail].frame=r->active; r->queue[tail].len=r->active_total;
    r->active=NULL; r->active_total=r->received=0; r->last_id=h.id; r->count++;
    return 1;
}
const struct efs_rmsg_view *efs_rmsg_rx_peek(const struct efs_rmsg_rx *r)
{ return r->count ? &r->queue[r->head] : NULL; }
int efs_rmsg_credit_init(struct efs_rmsg_credits *c, uint32_t peer_receives)
{
    if (!c || peer_receives<2 || peer_receives>64) return -1;
    c->sent=c->returned=0; c->capacity=peer_receives-1; return 0;
}
int efs_rmsg_credit_take(struct efs_rmsg_credits *c)
{
    if (c->sent==UINT64_MAX) return -1;
    if (c->sent-c->returned==c->capacity) return 0;
    c->sent++; return 1;
}
int efs_rmsg_credit_return(struct efs_rmsg_credits *c, uint64_t cumulative)
{
    if (cumulative<=c->returned || cumulative>c->sent) return -1;
    c->returned=cumulative; return 0;
}
