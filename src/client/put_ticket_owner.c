#include "client_internal.h"
#include "efs/put_ticket.h"
#include "efs/kv_key.h"
#include <string.h>

struct efs_client_put_request {
    struct efs_put_ticket ticket;
    efs_node_id_t members[EFS_MAX_NODES];
    uint32_t count;
    int attempted, published, retired;
};

/* Mount membership is immutable while a request is outstanding. Addresses
 * may change; the captured mask always refers to these exact ordered IDs. */
static int put_members(const struct efs_client_put_request *r)
{
    if (r->count!=g_client.node_count) return EFS_ERR_STALE;
    for (uint32_t i=0;i<r->count;i++)
        if (r->members[i]!=g_client.nodes[i].id) return EFS_ERR_STALE;
    return EFS_OK;
}

int efs_client_put_begin(struct efs_client_put_request **slot,
                         const struct efs_meta_pub *pub)
{
    if (!slot || *slot || !pub || g_client.node_count<EFS_NUM_FRAGMENTS ||
        g_client.node_count>EFS_MAX_NODES) return EFS_ERR_INVAL;
    struct efs_opid mount;
    int rc=efs_client_publication_id(pub->put_id.seq,&mount);
    if (rc!=EFS_OK) return rc;
    if (mount.session_epoch!=pub->put_id.session_epoch ||
        memcmp(mount.client_uuid,pub->put_id.client_uuid,EFS_OPID_UUID_LEN))
        return EFS_ERR_STALE;
    struct efs_put_ticket ticket;
    rc=efs_put_ticket_from_pub(pub,&ticket);
    uint8_t wire[EFS_PUT_TICKET_WIRE_LEN];
    if (rc==EFS_OK) rc=efs_put_ticket_encode(&ticket,wire);
    if (rc!=EFS_OK) return rc;
    if (ticket.member_mask!=((1u<<g_client.node_count)-1u)) return EFS_ERR_INVAL;
    for (uint32_t i=0;i<g_client.node_count;i++) {
        if (!g_client.nodes[i].id) return EFS_ERR_INVAL;
        for (uint32_t j=0;j<i;j++)
            if (g_client.nodes[i].id==g_client.nodes[j].id) return EFS_ERR_INVAL;
    }
    for (unsigned i=0;i<EFS_NUM_FRAGMENTS;i++) {
        uint32_t j=0;
        while (j<g_client.node_count && g_client.nodes[j].id!=ticket.nodes[i]) j++;
        if (j==g_client.node_count) return EFS_ERR_INVAL;
    }
    struct efs_client_put_request *r=efs_buf_metadata_alloc(sizeof(*r));
    if (!r) return EFS_ERR_NOMEM;
    memset(r,0,sizeof(*r));r->ticket=ticket;r->count=g_client.node_count;
    for (uint32_t i=0;i<r->count;i++) r->members[i]=g_client.nodes[i].id;
    *slot=r;return EFS_OK;
}

int efs_client_put_manifest(const struct efs_client_put_request *r,
                            struct efs_put_ticket *out)
{
    if (!r || !out) return EFS_ERR_INVAL;
    *out=r->ticket;return EFS_OK;
}

int efs_client_put_resume(struct efs_client_put_request *r,int probe_only,
                          uint32_t *state)
{
    if (!state) return EFS_ERR_INVAL;
    *state=0;
    if (!r || probe_only<0 || probe_only>1) return EFS_ERR_INVAL;
    int rc=put_members(r);
    if (rc!=EFS_OK) return rc;
    uint32_t observed=0;
    rc=efs_client_rpc_put_ticket(&r->ticket,1,&observed);
    /* UNREGISTERED: this lane shard has not admitted the session yet (K5). */
    if (!probe_only && (rc==EFS_ERR_NOT_FOUND || rc==EFS_ERR_BUSY || rc==EFS_ERR_UNREGISTERED)) {
        uint32_t shard=efs_kv_lane_shard(r->ticket.ino,r->ticket.ci%EFS_META_LANES);
        rc=efs_client_session_admit(shard,&r->ticket.id);
        if (rc!=EFS_OK) return rc;
        /* Set before send: even a failed send may have admitted this body. */
        r->attempted=1;
        rc=efs_client_rpc_put_ticket(&r->ticket,0,&observed);
    }
    if (rc!=EFS_OK) return rc;
    if (observed<EFS_PUT_TICKET_ADMITTED || observed>EFS_PUT_TICKET_RECLAIMING)
        return EFS_ERR_PROTO;
    /* Finding durable state also forbids treating this as a cancellable
     * unsent request, including after process-local lost-reply recovery. */
    r->attempted=1;
    if (observed==EFS_PUT_TICKET_PUBLISHED) r->published=1;
    *state=observed;
    return !probe_only && observed!=EFS_PUT_TICKET_ADMITTED ? EFS_ERR_STALE : EFS_OK;
}

int efs_client_put_free(struct efs_client_put_request *r)
{
    if (!r) return EFS_OK;
    if (r->attempted && (!r->published || !r->retired)) return EFS_ERR_BUSY;
    efs_buf_metadata_free(r,sizeof(*r));return EFS_OK;
}

int efs_client_put_retire(struct efs_client_put_request *r)
{
    if (!r) return EFS_ERR_INVAL;
    if (!r->published) return EFS_ERR_BUSY;
    if (r->retired) return EFS_OK;
    int rc=put_members(r);
    if (rc!=EFS_OK) return rc;
    rc=efs_client_rpc_put_ticket_retire(&r->ticket);
    if (rc==EFS_OK)r->retired=1;
    return rc;
}
