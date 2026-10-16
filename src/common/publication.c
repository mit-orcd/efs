#include "efs/publication.h"
#include "efs/checksum.h"
#include <string.h>

static void put(uint8_t **p, uint64_t n, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++)
        *(*p)++ = (uint8_t)(n >> (8 * (bytes - i - 1)));
}

/* Shared immutable object namespace, byte-identical to the metadata helper.
 * Ticket transport validates it before any admission/proposal side effect. */
uint64_t efs_meta_candidate_gen(const uint8_t uuid[16], uint32_t session_epoch,
                                uint64_t seq, uint32_t chunk_index, uint32_t retry)
{
    if (!uuid) return 1;
    uint8_t in[36],hash[EFS_HASH_SIZE],*p=in;
    memcpy(p,uuid,16);p+=16;
    put(&p,session_epoch,4);put(&p,seq,8);put(&p,chunk_index,4);put(&p,retry,4);
    efs_hash(in,sizeof(in),hash);
    uint64_t value=0;
    for (unsigned i=0;i<8;i++) value=(value<<8)|hash[i];
    return value ? value : 1;
}

int efs_publication_digest(const struct efs_meta_pub *r, uint8_t out[EFS_HASH_SIZE])
{
    if (!r || !out || !r->ino || !r->inode_gen || !r->candidate_gen ||
        r->expected_gen == EFS_CHUNK_BASE_UNCOND ||
        r->coding_profile_id != EFS_META_PROFILE_K2F1)
        return EFS_ERR_INVAL;
    unsigned nonzero = 0;
    for (unsigned i = 0; i < EFS_OPID_UUID_LEN; i++)
        nonzero |= r->publication_id.client_uuid[i];
    if (!nonzero || !r->publication_id.seq || !r->publication_id.session_epoch)
        return EFS_ERR_INVAL;
    uint8_t buf[160 + EFS_NUM_FRAGMENTS * (4 + EFS_HASH_SIZE)], *p = buf;
    put(&p, r->ticketed ? 3 : r->fresh_object ? 2 : 1, 4); /* 4 is reserved */
    memcpy(p, r->publication_id.client_uuid, EFS_OPID_UUID_LEN);
    p += EFS_OPID_UUID_LEN;
    put(&p, r->publication_id.session_epoch, 4);
    put(&p, r->publication_id.seq, 8);
    if (r->ticketed) {
        unsigned valid = 0;
        for (unsigned i = 0; i < EFS_OPID_UUID_LEN; i++) valid |= r->put_id.client_uuid[i];
        if (!valid || !r->put_id.session_epoch || !r->put_id.seq || !r->put_fragment_len || !r->put_member_mask) return EFS_ERR_INVAL;
        memcpy(p, r->put_id.client_uuid, EFS_OPID_UUID_LEN); p += EFS_OPID_UUID_LEN;
        put(&p, r->put_id.session_epoch, 4); put(&p, r->put_id.seq, 8);
        put(&p, r->put_fragment_len, 4);
        put(&p, r->put_member_mask, 4);
    }
    put(&p, r->ino, 8);
    put(&p, r->inode_gen, 8);
    put(&p, r->chunk_index, 4);
    put(&p, r->new_size, 8);
    put(&p, r->expected_gen, 8);
    put(&p, r->candidate_gen, 8);
    put(&p, r->content_epoch, 8);
    put(&p, r->coding_profile_id, 4);
    put(&p, r->delta_off, 4);
    put(&p, r->delta_len, 4);
    put(&p, r->delta_base_n, 4);
    put(&p, r->delta_base_seq, 8);
    for (unsigned i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        put(&p, r->ch.nodes[i], 4);
        memcpy(p, r->ch.checksums[i], EFS_HASH_SIZE);
        p += EFS_HASH_SIZE;
    }
    efs_hash(buf, (size_t)(p - buf), out);
    return EFS_OK;
}

int efs_publication_from_rec(const struct efs_chunk_rec *r, uint64_t size,
                             struct efs_meta_pub *p)
{
    uint64_t epoch;
    if (!r || !p) return EFS_ERR_INVAL;
    struct efs_chunk_rec authority = *r;
    if ((r->publish_flags & EFS_CHUNK_REC_F_RESERVED) ||
        !(r->publish_flags & EFS_CHUNK_REC_F_CAPTURED_FILEID) ||
        efs_chunk_publish_authority(&authority, r->file_generation, r->publish_epoch, &epoch) != EFS_OK)
        return EFS_ERR_INVAL;
    memset(p, 0, sizeof(*p));
    p->ino = r->ino;
    p->inode_gen = r->file_generation;
    p->chunk_index = r->chunk_index;
    p->new_size = size;
    p->expected_gen = r->base_gen;
    p->candidate_gen = r->chunk_generation;
    p->fresh_object = !!(r->publish_flags & EFS_CHUNK_REC_F_FRESH_OBJECT);
    p->content_epoch = epoch;
    p->coding_profile_id = EFS_META_PROFILE_K2F1;
    p->delta_off = r->delta_off;
    p->delta_len = r->delta_len;
    p->delta_base_n = r->delta_base_n;
    p->delta_base_seq = r->delta_base_seq;
    memcpy(p->ch.nodes, r->nodes, sizeof(p->ch.nodes));
    memcpy(p->ch.checksums, r->checksums, sizeof(p->ch.checksums));
    return p->ino && p->inode_gen && p->candidate_gen &&
           p->expected_gen != EFS_CHUNK_BASE_UNCOND ? EFS_OK : EFS_ERR_INVAL;
}

int efs_publication_from_request(const struct efs_msg_publication *r,
                                 struct efs_meta_pub *out)
{
    if (!r || !out || r->ticketed>1) return EFS_ERR_INVAL;
    struct efs_meta_pub p;
    int rc=efs_publication_from_rec(&r->rec,r->size,&p);
    if (rc!=EFS_OK) return rc;
    p.publication_id=r->id;
    if (r->ticketed) {
        if (!p.fresh_object || r->put_id.session_epoch!=r->id.session_epoch ||
            memcmp(r->put_id.client_uuid,r->id.client_uuid,EFS_OPID_UUID_LEN) ||
            !r->put_id.seq || !r->put_fragment_len ||
            r->put_fragment_len>EFS_MAX_CHUNK_SIZE/2 ||
            !r->put_member_mask || (r->put_member_mask & ~((1u<<EFS_MAX_NODES)-1u)) ||
            __builtin_popcount(r->put_member_mask)<EFS_NUM_FRAGMENTS ||
            (uint64_t)p.delta_off+p.delta_len>(uint64_t)r->put_fragment_len*2 ||
            p.candidate_gen!=efs_meta_candidate_gen(r->put_id.client_uuid,
                r->put_id.session_epoch,r->put_id.seq,p.chunk_index,0)) return EFS_ERR_INVAL;
        for (unsigned i=0;i<EFS_NUM_FRAGMENTS;i++) {
            if (!p.ch.nodes[i]) return EFS_ERR_INVAL;
            for (unsigned j=0;j<i;j++) if (p.ch.nodes[i]==p.ch.nodes[j]) return EFS_ERR_INVAL;
        }
        p.ticketed=1;p.put_id=r->put_id;p.put_fragment_len=r->put_fragment_len;
        p.put_member_mask=r->put_member_mask;
    } else {
        struct efs_opid zero={0};
        if (r->put_fragment_len || r->put_member_mask ||
            memcmp(r->put_id.client_uuid,zero.client_uuid,EFS_OPID_UUID_LEN) ||
            r->put_id.session_epoch || r->put_id.seq) return EFS_ERR_INVAL;
    }
    uint8_t digest[EFS_HASH_SIZE];
    rc=efs_publication_digest(&p,digest);
    if (rc==EFS_OK) *out=p;
    return rc;
}
