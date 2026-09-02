#include "efs/opid.h"
#include <string.h>

static int uuid_eq(const uint8_t a[EFS_OPID_UUID_LEN],
                   const uint8_t b[EFS_OPID_UUID_LEN])
{
    return memcmp(a, b, EFS_OPID_UUID_LEN) == 0;
}

static int cache_find(const struct efs_opid_window *w, uint64_t seq,
                      struct efs_opid_reply *out)
{
    int i;

    for (i = 0; i < w->ncache; i++) {
        if (w->cache[i].seq == seq) {
            if (out)
                *out = w->cache[i];
            return 1;
        }
    }
    return 0;
}

static int seq_completed(const struct efs_opid_window *w, uint64_t seq)
{
    uint64_t i;

    if (seq == 0 || seq <= w->highest_contiguous_seq)
        return seq != 0;
    if (seq > w->highest_contiguous_seq + EFS_OPID_BITMAP_BITS)
        return 0;
    i = seq - w->highest_contiguous_seq - 1;
    return (w->bitmap & (1ULL << i)) != 0;
}

void efs_opid_window_init(struct efs_opid_window *w,
                          const uint8_t uuid[EFS_OPID_UUID_LEN],
                          uint32_t epoch)
{
    if (!w)
        return;
    memset(w, 0, sizeof(*w));
    if (uuid)
        memcpy(w->client_uuid, uuid, EFS_OPID_UUID_LEN);
    w->session_epoch = epoch;
    w->inited = 1;
}

int efs_opid_lookup(const struct efs_opid_window *w, const struct efs_opid *id,
                    struct efs_opid_reply *out)
{
    if (!w || !w->inited || !id)
        return EFS_ERR_INVAL;
    if (id->seq == 0)
        return EFS_ERR_INVAL;
    if (id->session_epoch != w->session_epoch ||
        !uuid_eq(id->client_uuid, w->client_uuid))
        return EFS_ERR_STALE;
    if (!seq_completed(w, id->seq))
        return 0;
    if (cache_find(w, id->seq, out))
        return 1;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->seq = id->seq;
        out->rc = EFS_OK; /* acked and reclaimed; client already has the reply */
    }
    return 1;
}

static int cache_put(struct efs_opid_window *w, const struct efs_opid_reply *r)
{
    int i;

    for (i = 0; i < w->ncache; i++) {
        if (w->cache[i].seq == r->seq) {
            w->cache[i] = *r;
            return EFS_OK;
        }
    }
    if (w->ncache >= EFS_OPID_REPLY_CACHE)
        return EFS_ERR_NOMEM;
    w->cache[w->ncache++] = *r;
    return EFS_OK;
}

int efs_opid_complete(struct efs_opid_window *w, const struct efs_opid *id,
                      const struct efs_opid_reply *reply)
{
    struct efs_opid_reply rec;
    uint64_t i;
    int rc;

    if (!w || !w->inited || !id || !reply)
        return EFS_ERR_INVAL;
    if (id->seq == 0)
        return EFS_ERR_INVAL;
    if (id->session_epoch != w->session_epoch ||
        !uuid_eq(id->client_uuid, w->client_uuid))
        return EFS_ERR_STALE;
    rec = *reply;
    rec.seq = id->seq;
    rc = cache_put(w, &rec);
    if (rc != EFS_OK)
        return rc;
    if (id->seq <= w->highest_contiguous_seq)
        return EFS_OK;
    if (id->seq == w->highest_contiguous_seq + 1) {
        w->highest_contiguous_seq = id->seq;
        w->bitmap >>= 1;
        while (w->bitmap & 1ULL) {
            w->highest_contiguous_seq++;
            w->bitmap >>= 1;
        }
        return EFS_OK;
    }
    if (id->seq > w->highest_contiguous_seq + EFS_OPID_BITMAP_BITS)
        return EFS_ERR_INVAL;
    i = id->seq - w->highest_contiguous_seq - 1;
    w->bitmap |= 1ULL << i;
    return EFS_OK;
}

int efs_opid_ack(struct efs_opid_window *w, uint64_t contiguous_ack)
{
    int i, n = 0;

    if (!w || !w->inited)
        return EFS_ERR_INVAL;
    if (contiguous_ack > w->highest_contiguous_seq)
        contiguous_ack = w->highest_contiguous_seq;
    for (i = 0; i < w->ncache; i++) {
        if (w->cache[i].seq <= contiguous_ack)
            continue;
        w->cache[n++] = w->cache[i];
    }
    w->ncache = n;
    return EFS_OK;
}
