#include "efs/record_decode.h"
#include "efs/meta_cmd.h"
#include "efs/packed_segment.h"
#include "efs/session.h"
#include "efs/erasure.h"
#include <string.h>
static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p)<<32 | be32(p+4); }
static int printable(const char *p,size_t n)
{ if(!n)return 0; for(size_t i=0;i<n;i++)if((unsigned char)p[i]<=32 || (unsigned char)p[i]>=127)return 0;return 1; }
int efs_peer_hello_decode(const void *bytes,uint32_t len,struct efs_msg_hello *out)
{
    struct efs_msg_hello h;
    if(!bytes || !out || len!=sizeof(h))return EFS_ERR_PROTO;
    memcpy(&h,bytes,sizeof(h));
    size_t n=strnlen(h.addr,sizeof(h.addr));
    if(h.version!=EFS_VERSION_PACK || strnlen(h.build_id,sizeof(h.build_id))==sizeof(h.build_id) ||
       strcmp(h.build_id,EFS_BUILD_ID) || n==sizeof(h.addr) || !printable(h.addr,n) ||
       !h.port || !h.token || h.node_id>EFS_MAX_NODES || (h.flags&~EFS_HELLO_F_FORMED) ||
       ((h.flags&EFS_HELLO_F_FORMED) && (!h.node_id || !h.cluster)))return EFS_ERR_PROTO;
    *out=h;return EFS_OK;
}
int efs_recovery_scan_request_decode(const void *bytes,uint32_t len,struct efs_msg_recovery_scan *out)
{
    struct efs_msg_recovery_scan q;
    if(!bytes || !out || len!=sizeof(q))return EFS_ERR_INVAL;
    memcpy(&q,bytes,sizeof(q));
    if(q.group>2 || (q.group&1) || q.what<1 || q.what>3 || !q.node || q.node>EFS_MAX_NODES ||
       q.reserved || q.at.len>EFS_RECOVERY_CURSOR_MAX || q.at.shard>EFS_RECOVERY_SHARD_END ||
       (q.at.shard==EFS_RECOVERY_SHARD_END && q.at.len))return EFS_ERR_INVAL;
    *out=q;return EFS_OK;
}
static uint32_t record_len(uint32_t what)
{
    return what==EFS_RECOVERY_SCAN_CHUNKS?EFS_REBUILD_MARK_LEN:
           what==EFS_RECOVERY_SCAN_TICKETS?EFS_PUT_TICKET_WIRE_LEN:
           what==EFS_RECOVERY_SCAN_SEGMENTS?EFS_RECOVERY_SEGMENT_REC_LEN:0;
}
int efs_recovery_scan_reply_decode(const void *bytes,uint32_t len,uint32_t what,
                                  struct efs_msg_recovery_scan_reply *out,uint8_t *records)
{
    struct efs_msg_recovery_scan_reply r;
    uint32_t size=record_len(what);
    if(!bytes || !out || !records || !size || len<sizeof(r))return EFS_ERR_PROTO;
    memcpy(&r,bytes,sizeof(r));
    if(r.rc>0 || r.n>EFS_RECOVERY_SCAN_PAGE || len!=sizeof(r)+(size_t)r.n*size ||
       r.next.len>EFS_RECOVERY_CURSOR_MAX || r.next.shard>EFS_RECOVERY_SHARD_END ||
       (r.next.shard==EFS_RECOVERY_SHARD_END && r.next.len) || (r.rc && r.n))return EFS_ERR_PROTO;
    if(r.rc)return r.rc;
    memcpy(records,(const uint8_t *)bytes+sizeof(r),(size_t)r.n*size);
    *out=r;return EFS_OK;
}
int efs_degraded_mark_decode(const uint8_t *q,uint32_t len,struct efs_meta_degraded *out)
{
    struct efs_meta_degraded d={0};
    if(!q || !out || len!=EFS_REBUILD_MARK_LEN)return EFS_ERR_INVAL;
    d.ino=be64(q);d.inode_gen=be64(q+8);d.object_gen=be64(q+16);
    d.chunk_index=be32(q+24);d.lane=q[28];d.missing_mask=q[29];
    for(unsigned i=0;i<EFS_NUM_FRAGMENTS;i++)d.nodes[i]=be32(q+30+i*4);
    memcpy(d.checksums,q+30+4*EFS_NUM_FRAGMENTS,sizeof(d.checksums));
    if(!d.ino || !d.object_gen || d.lane!=d.chunk_index%EFS_META_LANES ||
       !d.missing_mask || !efs_chunk_missing_valid(d.missing_mask))return EFS_ERR_INVAL;
    *out=d;return EFS_OK;
}
int efs_cleanup_probe_decode(const uint8_t *p,uint32_t len,struct efs_opid *out,uint32_t *word,uint64_t *mask)
{
    struct efs_opid id={0};
    if(!p || !out || !word || !mask || len!=EFS_MD_SESSION_CLEANUP_PROBE_LEN ||
       p[0]!=EFS_MD_REQ_SESSION_CLEANUP_PROBE)return EFS_ERR_INVAL;
    memcpy(id.client_uuid,p+1,16);id.session_epoch=be32(p+17);
    uint32_t w=be32(p+21);uint64_t m=be64(p+25);
    if(!id.session_epoch || w>=EFS_SESSION_BITS/64 || !m)return EFS_ERR_INVAL;
    *out=id;*word=w;*mask=m;return EFS_OK;
}
int efs_segment_audit_request_decode(const uint8_t *p,uint32_t len,struct efs_audit_request *out)
{
    if(!p || !out || len<EFS_SEGMENT_AUDIT_HEADER)return EFS_ERR_INVAL;
    struct efs_audit_request q={.group=p[0],.flags=p[1],.count=be32(p+4),.incarnation=be64(p+8)};
    if((q.group!=0 && q.group!=2) || (q.flags&~EFS_SEGMENT_AUDIT_F_COMPLETE) || p[2] || p[3] ||
       q.count>EFS_SEGMENT_AUDIT_MAX || len!=EFS_SEGMENT_AUDIT_HEADER+(size_t)q.count*EFS_SEGMENT_AUDIT_ITEM ||
       ((q.flags&EFS_SEGMENT_AUDIT_F_COMPLETE) && q.count))return EFS_ERR_INVAL;
    for(uint32_t i=0;i<q.count;i++) {
        const uint8_t *it=p+EFS_SEGMENT_AUDIT_HEADER+(size_t)i*EFS_SEGMENT_AUDIT_ITEM;
        uint32_t geometry=be32(it+20);
        if(!be64(it) || !be64(it+8) || it[16]>=EFS_NUM_FRAGMENTS || it[17] || it[18] || it[19] ||
           geometry<EFS_SEGMENT_ALIGNMENT || geometry>EFS_SEGMENT_MAX_SIZE || geometry%EFS_SEGMENT_ALIGNMENT)
            return EFS_ERR_INVAL;
    }
    *out=q;return EFS_OK;
}
int efs_segment_audit_reply_decode(const uint8_t *p,uint32_t len,struct efs_audit_reply *out)
{
    if(!p || !out || len<EFS_SEGMENT_AUDIT_REPLY)return EFS_ERR_PROTO;
    struct efs_audit_reply r={.rc=(int32_t)be32(p),.count=be32(p+4),.incarnation=be64(p+8),
                             .audited=be32(p+16),.settled=be32(p+20)};
    if(r.rc>0 || r.count>EFS_SEGMENT_AUDIT_MAX || len!=EFS_SEGMENT_AUDIT_REPLY+r.count ||
       (r.rc && r.count) || (r.audited&~3u) || r.settled>1)return EFS_ERR_PROTO;
    for(unsigned i=0;i<r.count;i++) {
        if(p[EFS_SEGMENT_AUDIT_REPLY+i]>EFS_SEGMENT_AUDIT_ERROR)return EFS_ERR_PROTO;
        r.status[i]=p[EFS_SEGMENT_AUDIT_REPLY+i];
    }
    *out=r;return EFS_OK;
}
