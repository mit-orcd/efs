#include "efs/publication.h"
#include "efs/checksum.h"
#include <string.h>

static void put(uint8_t **p, uint64_t n, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++)
        *(*p)++ = (uint8_t)(n >> (8 * (bytes - i - 1)));
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
    put(&p, r->ticketed ? 3 : r->fresh_object ? 2 : 1, 4);
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
    if (!r || !p || !(r->publish_flags & EFS_CHUNK_REC_F_CAPTURED_FILEID) ||
        efs_chunk_publish_authority(r, r->file_generation, r->publish_epoch, &epoch) != EFS_OK)
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
