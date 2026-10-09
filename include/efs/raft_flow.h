#ifndef EFS_RAFT_FLOW_H
#define EFS_RAFT_FLOW_H
#include <stdint.h>
#include <string.h>
#include "efs/raft.h"

/* Transport admission only. Never a consensus, apply or durability ACK.
 * Eight explicit bytes: EF, version, status, pressure, reserved, retry-ms BE. */
#define EFS_RAFT_FLOW_LEN 8
#define EFS_RAFT_FLOW_OK 0
#define EFS_RAFT_FLOW_BUSY 1
#define EFS_RAFT_FLOW_ERROR 2
#define EFS_RAFT_FLOW_HIGH_N 64u
#define EFS_RAFT_FLOW_LOW_N 32u
#define EFS_RAFT_FLOW_HIGH_BYTES (8ull << 20)
#define EFS_RAFT_FLOW_LOW_BYTES (4ull << 20)
#define EFS_RAFT_FLOW_RESERVE_N 32u
#define EFS_RAFT_FLOW_RESERVE_BYTES (64ull << 10)
#define EFS_RAFT_FLOW_PROBE_US 50000ull
#define EFS_RAFT_FLOW_READ_US 5000ull
struct efs_raft_flow_reply { unsigned status, pressure, retry_ms; };
struct efs_raft_flow_sender {
    uint64_t deadline_us;
    unsigned delay_ms, window, good, sequence, active;
};
static inline void efs_raft_flow_encode(uint8_t out[EFS_RAFT_FLOW_LEN],
                                       const struct efs_raft_flow_reply *r)
{
    out[0]='E'; out[1]='F'; out[2]=1; out[3]=(uint8_t)r->status;
    out[4]=(uint8_t)!!r->pressure; out[5]=0;
    out[6]=(uint8_t)(r->retry_ms >> 8); out[7]=(uint8_t)r->retry_ms;
}
/* 1 = legacy empty reply (admission unknown), 0 = valid status, -1 malformed. */
static inline int efs_raft_flow_decode(const void *buf, uint32_t len,
                                      struct efs_raft_flow_reply *r)
{
    const uint8_t *p=buf;
    memset(r,0,sizeof(*r));
    if (!len) return 1;
    if (!p || len != EFS_RAFT_FLOW_LEN || p[0]!='E' || p[1]!='F' ||
        p[2]!=1 || p[3]>EFS_RAFT_FLOW_ERROR || p[4]>1 || p[5]) return -1;
    r->status=p[3]; r->pressure=p[4]; r->retry_ms=((unsigned)p[6]<<8)|p[7];
    return 0;
}
static inline uint64_t efs_raft_flow_delay_us(struct efs_raft_flow_sender *s)
{
    unsigned span=s->delay_ms*250; /* deterministic bounded jitter, 75..100% */
    s->sequence=s->sequence*1664525u+1013904223u;
    return (uint64_t)s->delay_ms*750 + s->sequence%(span+1);
}
static inline void efs_raft_flow_feedback(struct efs_raft_flow_sender *s,
                                         const struct efs_raft_flow_reply *r,
                                         uint64_t now)
{
    if (r->status==EFS_RAFT_FLOW_BUSY || r->pressure) {
        unsigned was_active=s->active;
        unsigned delay=s->active ? s->delay_ms*2 : 5;
        if (r->retry_ms>delay) delay=r->retry_ms;
        s->delay_ms=delay>50 ? 50 : delay;
        s->active=1; s->window=1; s->good=0;
        /* Control replies must not keep postponing a scheduled data retry. */
        if (!was_active || !s->deadline_us)
            s->deadline_us=now+efs_raft_flow_delay_us(s);
    } else if (r->status==EFS_RAFT_FLOW_OK && s->active && ++s->good>=8) {
        s->good=0;
        s->window=s->window ? s->window*2 : 2;
        if (s->window>=8) { s->window=8; s->active=0; s->deadline_us=0; s->delay_ms=0; }
    }
}
static inline unsigned efs_raft_flow_rd32(const uint8_t *p)
{ return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3]; }
static inline uint64_t efs_raft_flow_rd64(const uint8_t *p)
{ return ((uint64_t)efs_raft_flow_rd32(p)<<32)|efs_raft_flow_rd32(p+4); }
static inline int efs_raft_flow_group(uint8_t group)
{ return group==EFS_RAFT_GROUP_SHARD ? 0 : group==EFS_RAFT_GROUP_SHARD2 ? 1 : -1; }
/* Validate framing without allocation or state-machine/host-lock access. */
static inline int efs_raft_flow_frame(const uint8_t *p, uint32_t len)
{
    uint32_t n,off=80,i;
    if (!p || len<80 || p[0]<EFS_RAFT_MSG_VOTE_REQ || p[0]>EFS_RAFT_MSG_SNAP_REP ||
        efs_raft_flow_group(p[1])<0 || p[2]>1 || p[3]>1 ||
        efs_raft_flow_rd32(p+4)>=EFS_RAFT_MAX_PEERS ||
        efs_raft_flow_rd32(p+8)>=EFS_RAFT_MAX_PEERS) return 0;
    n=efs_raft_flow_rd32(p+76);
    if (n>EFS_RAFT_AE_MAX || (p[0]!=EFS_RAFT_MSG_AE_REQ && p[0]!=EFS_RAFT_MSG_SNAP_REQ && n)) return 0;
    for (i=0;i<n;i++) {
        uint32_t bytes;
        if (len-off<12) return 0;
        bytes=efs_raft_flow_rd32(p+off+8); off+=12;
        if (bytes>4u*1024*1024 || bytes>len-off) return 0;
        off+=bytes;
    }
    return off==len;
}
static inline int efs_raft_flow_empty(const uint8_t *p, uint32_t len)
{ return len==80 && p[0]==EFS_RAFT_MSG_AE_REQ && !efs_raft_flow_rd32(p+76); }
static inline int efs_raft_flow_critical(const uint8_t *p, uint32_t len)
{
    return len==80 && (p[0]==EFS_RAFT_MSG_VOTE_REQ || p[0]==EFS_RAFT_MSG_VOTE_REP ||
                      p[0]==EFS_RAFT_MSG_AE_REP || p[0]==EFS_RAFT_MSG_SNAP_REP);
}
/* Escape probes prove only prefix zero; vote_granted avoids clearing an
 * outstanding data window. Read context, term, group and boot are retained. */
static inline void efs_raft_flow_probe(uint8_t out[80], const uint8_t in[80])
{
    memcpy(out,in,80); out[2]=1; out[3]=0;
    memset(out+36,0,24); /* last_log_term, prev_index, prev_term */
    memset(out+68,0,12); /* match_index, nentries */
}
static inline int efs_raft_flow_probe_due(uint64_t sent, uint64_t old_read,
                                         uint64_t now, uint64_t read)
{
    return !sent || (now>=sent && (now-sent>=EFS_RAFT_FLOW_PROBE_US ||
           (read && read!=old_read && now-sent>=EFS_RAFT_FLOW_READ_US)));
}
#endif
