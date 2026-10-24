/* K37 rebuild on return for packed segments (W154 for segments).
 *
 * A packed segment is one 2+1 stripe; every packed file in it is lost with
 * a second fragment. A segment with fewer than three good fragments carries
 * a degraded mark (EFS_KV_KIND_PACK_DEGRADED) on its group's anchor shard,
 * written through EFS_MD_CMD_PACK_DEGRADED. Marks come from:
 *   - the packer: a segment whose third fragment PUT was not acknowledged
 *     (member down before the heartbeat says so, or refusing) is marked
 *     before any file switches into it (packer.c), also for compaction;
 *   - this audit: a member that returns with a new incarnation is asked
 *     to read and verify every fragment of every published segment placed
 *     on it (EFS_MSG_SEGMENT_AUDIT); a missing or bad copy is marked. A
 *     store whose recovery after an unclean stop is uncertain answers only
 *     once it is settled, so a fragment that verified recovery (K36) drops
 *     is found by the audit that follows;
 *   - the rebuild itself: a source fragment its holder proves lost adds a
 *     second bit, and the segment is reported unrecoverable.
 *
 * This pass runs on each group leader inside the GC loop (about 1 s):
 *   1. audit: each available member is probed at most every
 *      SR_PROBE_US; a member whose incarnation this group has not swept
 *      gets a sweep of the group's segment index from a cursor, at most
 *      EFS_SEGMENT_AUDIT_MAX fragments per member per pass; the member
 *      records the completed sweep for its incarnation, so a new leader
 *      does not repeat it. Any unproven answer (I/O error) keeps the sweep
 *      open and it starts again later: a member is never called audited
 *      without a verdict for every fragment;
 *   2. rebuild: marks are walked from a cursor; a mark whose segment is no
 *      longer live is retired; a mark with two bits is reported and kept;
 *      otherwise the two present fragments are read and checked against
 *      the segment's checksums, the third is rebuilt (XOR) and checked,
 *      PUT to its placement node with EFS_SEGMENT_PUT_F_REBUILD (fsynced
 *      before the acknowledgement), read back and verified there by an
 *      audit of that one fragment, and only then is the mark cleared.
 * At most SR_REBUILD_PER_PASS rebuilds and SR_BUDGET_US per group per pass;
 * a node whose I/O failed is skipped for SR_NODE_BACKOFF_US. */
#include "server_internal.h"
#include "efs/checksum.h"
#include "efs/erasure.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include "efs/meta_cmd.h"
#include "efs/network.h"
#include "efs/pack_mapping.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SR_PROBE_US 2000000ull
#define SR_BUDGET_US 1000000ull
#define SR_NODE_BACKOFF_US 10000000ull
#define SR_RETRY_US 30000000ull /* a sweep that met an unproven answer */
#define SR_IO_MS 5000
#define SR_REBUILD_PER_PASS 64u
#define SR_MARKS_MAX 256u
#define SR_SCAN_KEYS 1024u /* merged index keys per audit page */
#define SR_KEY EFS_PACK_OBJECT_KEY_BYTES

struct sr_audit {
    int sweeping, unproven;
    uint64_t inc, probe_us, retry_us;
    uint8_t cursor[SR_KEY];
    uint32_t cursor_len;
    uint64_t checked, marked;
};
static struct {
    struct sr_audit node[EFS_MAX_NODES + 1];
    uint8_t mark_cursor[SR_KEY];
    uint32_t mark_cursor_len;
    uint64_t said_unrecoverable_us;
} sr[2];
static uint64_t sr_backoff_us[EFS_MAX_NODES + 1];

static uint64_t sr_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
}
static void sr_put32(uint8_t *p, uint32_t v) { for (int i = 3; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; } }
static void sr_put64(uint8_t *p, uint64_t v) { for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; } }
static uint32_t sr_get32(const uint8_t *p) { uint32_t v = 0; for (int i = 0; i < 4; i++) v = (v << 8) | p[i]; return v; }
static uint64_t sr_get64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v; }
static int sr_gi(uint8_t group) { return group == EFS_RAFT_GROUP_SHARD ? 0 : 1; }
#define SR_ADD(f, n) __atomic_fetch_add(&s->f, (n), __ATOMIC_RELAXED)

void server_segment_rebuild_reset(uint8_t group)
{
    memset(&sr[sr_gi(group)], 0, sizeof(sr[0]));
}

static const struct efs_node *sr_node(const struct efs_node *nodes, uint32_t count, uint32_t id)
{
    for (uint32_t i = 0; i < count; i++)
        if (nodes[i].id == id) return &nodes[i];
    return NULL;
}
/* Available: known, not marked down by the heartbeat (this node always),
 * and not in a backoff after failed I/O. */
static int sr_available(struct efsd_server *s, const struct efs_node *nodes, uint32_t count,
                        uint32_t id, uint64_t now)
{
    const struct efs_node *n = sr_node(nodes, count, id);
    return n && (id == s->id || !n->down_until_ms) && sr_backoff_us[id] <= now;
}

/* One request/reply with a peer, bounded at SR_IO_MS: a hung member costs
 * one short timeout and a backoff, not the pool's 30 s per call. */
static int sr_rpc(const struct efs_node *n, uint8_t type, const void *a, uint32_t alen,
                  const void *b, uint32_t blen, uint8_t want, uint8_t **reply, uint32_t *rlen)
{
    struct efs_conn *pc = server_peer_conn_get(n->addr, n->port);
    uint8_t got = 0;
    void *r = NULL;
    *reply = NULL;
    *rlen = 0;
    if (!pc) return EFS_ERR_NET;
    if (pc->kind == EFS_CONN_TCP) {
        efs_set_recv_timeout(pc->fd, SR_IO_MS);
        efs_set_send_timeout(pc->fd, SR_IO_MS);
    }
    int bad = blen ? efs_conn_send_msg_parts(pc, type, a, alen, b, blen)
                   : efs_conn_send_msg(pc, type, a, alen);
    if (bad || efs_conn_recv_msg(pc, &got, &r, rlen)) {
        server_peer_conn_drop(n->addr, n->port, pc);
        free(r);
        *rlen = 0;
        return EFS_ERR_NET;
    }
    if (pc->kind == EFS_CONN_TCP) {
        efs_set_recv_timeout(pc->fd, EFS_IO_TIMEOUT_MS);
        efs_set_send_timeout(pc->fd, EFS_IO_TIMEOUT_MS);
    }
    server_peer_conn_release(n->addr, n->port, pc);
    if (got != want) {
        free(r);
        *rlen = 0;
        return EFS_ERR_PROTO;
    }
    *reply = r;
    return EFS_OK;
}

/* ---- audit ---- */
struct sr_item { uint64_t id, gen; uint32_t fi, geometry; uint8_t sum[EFS_HASH_SIZE]; };
struct sr_reply { int32_t rc; uint32_t count, audited, settled; uint64_t inc; uint8_t v[EFS_SEGMENT_AUDIT_MAX]; };

/* One audit exchange with member `id` (local call for this node). */
static int sr_audit_call(struct efsd_server *s, const struct efs_node *nodes, uint32_t count,
                         uint32_t id, uint8_t group, uint32_t flags, uint64_t inc,
                         const struct sr_item *items, uint32_t n, struct sr_reply *out)
{
    uint32_t len = EFS_SEGMENT_AUDIT_HEADER + n * EFS_SEGMENT_AUDIT_ITEM;
    uint8_t *req = calloc(1, len), *rep = NULL;
    uint32_t rlen = 0;
    int rc = req ? EFS_OK : EFS_ERR_NOMEM;
    if (rc != EFS_OK) return rc;
    req[0] = group;
    req[1] = (uint8_t)flags;
    sr_put32(req + 4, n);
    sr_put64(req + 8, inc);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *it = req + EFS_SEGMENT_AUDIT_HEADER + (size_t)i * EFS_SEGMENT_AUDIT_ITEM;
        sr_put64(it, items[i].id);
        sr_put64(it + 8, items[i].gen);
        it[16] = (uint8_t)items[i].fi;
        sr_put32(it + 20, items[i].geometry);
        memcpy(it + 24, items[i].sum, EFS_HASH_SIZE);
    }
    if (id == s->id) {
        server_segment_audit(s, req, len, &rep, &rlen);
        if (!rep) rc = EFS_ERR_NOMEM;
    } else {
        const struct efs_node *nd = sr_node(nodes, count, id);
        rc = nd ? sr_rpc(nd, EFS_MSG_SEGMENT_AUDIT, req, len, NULL, 0, EFS_MSG_SEGMENT_AUDIT_REPLY,
                         &rep, &rlen)
                : EFS_ERR_IO;
    }
    free(req);
    if (rc == EFS_OK && (rlen < EFS_SEGMENT_AUDIT_REPLY ||
                         sr_get32(rep + 4) > EFS_SEGMENT_AUDIT_MAX ||
                         rlen != EFS_SEGMENT_AUDIT_REPLY + sr_get32(rep + 4)))
        rc = EFS_ERR_PROTO;
    if (rc == EFS_OK) {
        out->rc = (int32_t)sr_get32(rep);
        out->count = sr_get32(rep + 4);
        out->inc = sr_get64(rep + 8);
        out->audited = sr_get32(rep + 16);
        out->settled = sr_get32(rep + 20);
        memcpy(out->v, rep + EFS_SEGMENT_AUDIT_REPLY, out->count);
        /* A verdict list must answer exactly the items asked. */
        if (out->rc == EFS_OK && out->count != n) rc = EFS_ERR_PROTO;
    }
    free(rep);
    return rc;
}

static int sr_propose(uint8_t group, uint32_t op, uint64_t id, uint64_t gen, uint32_t mask,
                      uint32_t reason)
{
    uint8_t cmd[EFS_MD_PACK_DEGRADED_LEN] = {EFS_MD_CMD_PACK_DEGRADED, group, (uint8_t)op};
    sr_put64(cmd + 3, id);
    sr_put64(cmd + 11, gen);
    cmd[19] = (uint8_t)mask;
    cmd[20] = (uint8_t)reason;
    return server_raft_host_pack_propose(group, cmd, sizeof(cmd));
}

/* Mark fragment fi of a segment; the mark must be in the local replica
 * afterwards (the apply's verdict is the row, not the proposal's rc). */
int server_segment_mark(uint8_t group, uint64_t id, uint64_t gen, uint32_t fi, uint32_t reason)
{
    struct efs_kv *kv = server_raft_host_kv();
    uint8_t have = 0;
    int rc = sr_propose(group, EFS_MD_PACK_DEGRADED_MARK, id, gen, 1u << fi, reason);
    if (rc == EFS_OK && kv) rc = efs_pack_degraded_get(kv, group, id, gen, &have, NULL);
    if (rc == EFS_OK && !(have & (1u << fi))) rc = EFS_ERR_STALE;
    return rc;
}

/* Collect published fragments placed on `node` after the sweep cursor. */
struct sr_collect {
    uint8_t group;
    uint32_t node, n, max;
    int full;
    const uint8_t *skip;
    uint32_t skip_len;
    uint8_t last[SR_KEY];
    uint32_t last_len;
    struct efs_kv *kv;
    struct sr_item *items;
    struct efs_pack_object o;
};
static int sr_collect_cb(void *u, const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl)
{
    struct sr_collect *c = u;
    if (c->skip_len && kl == c->skip_len && !memcmp(k, c->skip, kl)) return 0;
    if (c->n + 1 > c->max) { c->full = 1; return 1; }
    if (kl == SR_KEY) { memcpy(c->last, k, kl); c->last_len = kl; }
    if (kl != SR_KEY || k[2] != EFS_KV_KIND_PACK_SEGMENT) return 0;
    if (efs_pack_object_decode(v, vl, &c->o) != EFS_OK) return 0; /* the packer reports it */
    struct efs_pack_ledger_usage u2;
    efs_pack_ledger_usage(&c->o.ledger, &u2);
    /* Pending records: a copy in flight, whose fragments may not exist yet. */
    if (c->o.ledger.phase != EFS_PACK_ACTIVE || u2.pending || !u2.live) return 0;
    for (uint32_t fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        if (c->o.nodes[fi] != c->node) continue;
        uint8_t mask = 0;
        if (efs_pack_degraded_get(c->kv, c->group, c->o.ledger.segment_id,
                                  c->o.ledger.segment_generation, &mask, NULL) == EFS_OK &&
            (mask & (1u << fi)))
            continue; /* already known missing */
        struct sr_item *it = &c->items[c->n++];
        it->id = c->o.ledger.segment_id;
        it->gen = c->o.ledger.segment_generation;
        it->fi = fi;
        it->geometry = c->o.ledger.geometry;
        memcpy(it->sum, c->o.checksums[fi], EFS_HASH_SIZE);
    }
    return 0;
}

static void sr_audit_node(struct efsd_server *s, struct efs_kv *kv, uint8_t group,
                          const struct efs_node *nodes, uint32_t count, uint32_t id, uint64_t now)
{
    struct sr_audit *a = &sr[sr_gi(group)].node[id];
    struct sr_reply rep;
    uint32_t bit = group == EFS_RAFT_GROUP_SHARD ? 1u : 2u;
    if (!a->sweeping) {
        if (now < a->probe_us + SR_PROBE_US || now < a->retry_us) return;
        a->probe_us = now;
        memset(&rep, 0, sizeof(rep));
        if (sr_audit_call(s, nodes, count, id, group, 0, 0, NULL, 0, &rep) != EFS_OK ||
            rep.rc != EFS_OK) {
            sr_backoff_us[id] = now + SR_NODE_BACKOFF_US;
            return;
        }
        if (!rep.settled || (rep.audited & bit)) return;
        memset(a, 0, sizeof(*a));
        a->sweeping = 1;
        a->inc = rep.inc;
        a->probe_us = now;
        fprintf(stderr, "segment-rebuild: audit node %u group %u incarnation %llu: sweep started\n",
                id, group, (unsigned long long)rep.inc);
    }
    struct sr_collect *c = calloc(1, sizeof(*c));
    struct sr_item *items = calloc(EFS_SEGMENT_AUDIT_MAX, sizeof(*items));
    if (!c || !items) { free(c); free(items); return; }
    uint32_t anchor = efs_pack_group_anchor(group);
    uint8_t prefix[3] = {(uint8_t)(anchor >> 8), (uint8_t)anchor, EFS_KV_KIND_PACK_SEGMENT};
    c->group = group; c->node = id; c->max = EFS_SEGMENT_AUDIT_MAX; c->kv = kv; c->items = items;
    c->skip = a->cursor; c->skip_len = a->cursor_len;
    /* One bounded page of the index per pass: the KV read lock is never
     * held for a whole large index (apply waits on it). */
    uint8_t resume[EFS_KV_KEY_MAX];
    uint32_t resume_len = 0;
    int src = efs_kv_lsm_scan_work(kv, prefix, sizeof(prefix), a->cursor_len ? a->cursor : NULL,
                                   a->cursor_len, SR_SCAN_KEYS, sr_collect_cb, c, resume, &resume_len);
    if (src < 0)
        goto out;
    if (resume_len == SR_KEY && !c->full) {
        /* Stopped by the page bound: resume after the last merged key. */
        memcpy(c->last, resume, SR_KEY);
        c->last_len = SR_KEY;
        c->full = 1;
    }
    if (c->n) {
        memset(&rep, 0, sizeof(rep));
        int rc = sr_audit_call(s, nodes, count, id, group, 0, a->inc, items, c->n, &rep);
        if (rc != EFS_OK) {
            sr_backoff_us[id] = now + SR_NODE_BACKOFF_US;
            goto out; /* the same window again after the backoff */
        }
        if (rep.rc == EFS_ERR_STALE || rep.rc == EFS_ERR_BUSY) {
            /* Restarted, or recovery uncertain again: probe and start over. */
            a->sweeping = 0;
            goto out;
        }
        if (rep.rc != EFS_OK) {
            sr_backoff_us[id] = now + SR_NODE_BACKOFF_US;
            goto out;
        }
        for (uint32_t i = 0; i < c->n; i++) {
            uint32_t v = rep.v[i];
            if (!server_raft_host_gc_running()) {
                a->unproven = 1; /* stopping: this sweep cannot complete */
                break;
            }
            a->checked++;
            SR_ADD(segment_audit_checked, 1);
            if (v == EFS_SEGMENT_AUDIT_OK) continue;
            if (v != EFS_SEGMENT_AUDIT_MISSING && v != EFS_SEGMENT_AUDIT_BAD) {
                a->unproven = 1; /* proves nothing: this sweep cannot complete */
                continue;
            }
            int mrc = server_segment_mark(group, items[i].id, items[i].gen, items[i].fi,
                              v == EFS_SEGMENT_AUDIT_MISSING ? EFS_PACK_DEGRADED_R_MISSING
                                                             : EFS_PACK_DEGRADED_R_BAD);
            if (mrc == EFS_OK) {
                a->marked++;
                SR_ADD(segment_audit_marked, 1);
                fprintf(stderr, "segment-rebuild: audit node %u group %u: g%u id=%016llx gen=%llx "
                        "fragment %u %s; marked degraded for rebuild\n", id, group, group,
                        (unsigned long long)items[i].id, (unsigned long long)items[i].gen,
                        items[i].fi, v == EFS_SEGMENT_AUDIT_MISSING ? "missing" : "torn or bad");
            } else if (mrc != EFS_ERR_STALE) {
                a->unproven = 1; /* not recorded: look again next sweep */
                SR_ADD(segment_rebuild_failures, 1);
            }
        }
    }
    if (c->last_len) {
        memcpy(a->cursor, c->last, c->last_len);
        a->cursor_len = c->last_len;
    }
    if (c->full) goto out;
    /* End of the index: the member is audited for this incarnation only
     * when every fragment got a verdict. */
    if (a->unproven) {
        fprintf(stderr, "segment-rebuild: audit node %u group %u: sweep had unproven answers "
                "(checked=%llu marked=%llu); starting over later\n", id, group,
                (unsigned long long)a->checked, (unsigned long long)a->marked);
        a->sweeping = 0;
        a->retry_us = now + SR_RETRY_US;
        goto out;
    }
    memset(&rep, 0, sizeof(rep));
    if (sr_audit_call(s, nodes, count, id, group, EFS_SEGMENT_AUDIT_F_COMPLETE, a->inc, NULL, 0,
                      &rep) == EFS_OK && rep.rc == EFS_OK && (rep.audited & bit)) {
        fprintf(stderr, "segment-rebuild: audit node %u group %u incarnation %llu: complete "
                "checked=%llu marked=%llu\n", id, group, (unsigned long long)a->inc,
                (unsigned long long)a->checked, (unsigned long long)a->marked);
    }
    a->sweeping = 0; /* any refusal: the next probe decides */
out:
    free(c);
    free(items);
}

/* ---- rebuild ---- */
/* One fragment, checksum-verified against the segment object. */
static int sr_get_fragment(struct efsd_server *s, const struct efs_node *nodes, uint32_t count,
                           const struct efs_pack_object *o, uint32_t fi, uint8_t *out)
{
    uint32_t id = o->nodes[fi], group = o->ledger.publication_group, len = o->ledger.geometry / 2;
    uint8_t sum[EFS_HASH_SIZE], h[EFS_HASH_SIZE];
    int rc;
    if (id == s->id) {
        rc = server_segment_read(s, group, o->ledger.segment_id, o->ledger.segment_generation, fi,
                                 len, out, sum);
    } else {
        const struct efs_node *n = sr_node(nodes, count, id);
        uint8_t req[EFS_SEGMENT_GET_HEADER] = {0}, *rep = NULL;
        uint32_t rlen = 0;
        if (!n) return EFS_ERR_IO;
        req[0] = (uint8_t)group;
        req[1] = (uint8_t)fi;
        sr_put32(req + 4, o->ledger.geometry);
        sr_put64(req + 8, o->ledger.segment_id);
        sr_put64(req + 16, o->ledger.segment_generation);
        rc = sr_rpc(n, EFS_MSG_GET_SEGMENT, req, sizeof(req), NULL, 0, EFS_MSG_GET_SEGMENT_REPLY,
                    &rep, &rlen);
        if (rc == EFS_OK && rlen < EFS_SEGMENT_GET_REPLY) rc = EFS_ERR_PROTO;
        if (rc == EFS_OK) rc = (int32_t)sr_get32(rep);
        if (rc == EFS_OK && (sr_get32(rep + 4) != len || rlen != EFS_SEGMENT_GET_REPLY + len))
            rc = EFS_ERR_PROTO;
        if (rc == EFS_OK) {
            memcpy(sum, rep + 8, EFS_HASH_SIZE);
            memcpy(out, rep + EFS_SEGMENT_GET_REPLY, len);
        }
        free(rep);
    }
    if (rc != EFS_OK) return rc;
    efs_hash(out, len, h);
    return memcmp(h, o->checksums[fi], EFS_HASH_SIZE) || memcmp(sum, o->checksums[fi], EFS_HASH_SIZE)
           ? EFS_ERR_CHECKSUM : EFS_OK;
}

/* The holder's own verdict on one fragment (an audit of one item). */
static uint32_t sr_check_one(struct efsd_server *s, const struct efs_node *nodes, uint32_t count,
                             const struct efs_pack_object *o, uint32_t fi)
{
    struct sr_item it = {o->ledger.segment_id, o->ledger.segment_generation, fi, o->ledger.geometry, {0}};
    struct sr_reply rep;
    memcpy(it.sum, o->checksums[fi], EFS_HASH_SIZE);
    memset(&rep, 0, sizeof(rep));
    if (sr_audit_call(s, nodes, count, o->nodes[fi], (uint8_t)o->ledger.publication_group, 0, 0,
                      &it, 1, &rep) != EFS_OK || rep.rc != EFS_OK)
        return EFS_SEGMENT_AUDIT_ERROR;
    return rep.v[0];
}

static int sr_put_rebuilt(struct efsd_server *s, const struct efs_node *nodes, uint32_t count,
                          const struct efs_pack_object *o, uint32_t fi, const uint8_t *data)
{
    uint32_t len = o->ledger.geometry / 2;
    uint8_t *body = malloc(EFS_SEGMENT_PUT_HEADER + len), *rep = NULL;
    uint32_t rlen = 0;
    int rc;
    if (!body) return EFS_ERR_NOMEM;
    memset(body, 0, EFS_SEGMENT_PUT_HEADER);
    body[0] = (uint8_t)o->ledger.publication_group;
    body[1] = (uint8_t)fi;
    body[2] = EFS_SEGMENT_PUT_F_REBUILD;
    sr_put32(body + 4, o->ledger.geometry);
    sr_put64(body + 8, o->ledger.segment_id);
    sr_put64(body + 16, o->ledger.segment_generation);
    memcpy(body + 24, o->checksums[fi], EFS_HASH_SIZE);
    memcpy(body + EFS_SEGMENT_PUT_HEADER, data, len);
    if (o->nodes[fi] == s->id) {
        rc = server_segment_put(s, body, EFS_SEGMENT_PUT_HEADER + len);
    } else {
        const struct efs_node *n = sr_node(nodes, count, o->nodes[fi]);
        rc = n ? sr_rpc(n, EFS_MSG_PUT_SEGMENT, body, EFS_SEGMENT_PUT_HEADER + len, NULL, 0,
                        EFS_MSG_PUT_SEGMENT_REPLY, &rep, &rlen)
               : EFS_ERR_IO;
        if (rc == EFS_OK)
            rc = rlen == sizeof(struct efs_msg_segment_put_reply)
                 ? ((struct efs_msg_segment_put_reply *)rep)->rc : EFS_ERR_PROTO;
        free(rep);
    }
    free(body);
    return rc;
}

static void sr_say_unrecoverable(uint8_t group, uint64_t id, uint64_t gen, uint32_t mask,
                                 uint32_t reasons)
{
    fprintf(stderr, "segment-rebuild: g%u id=%016llx gen=%llx UNRECOVERABLE: fragments mask=%x "
            "lost (reasons=%x); its files read EIO until rewritten or removed; mark kept\n",
            group, (unsigned long long)id, (unsigned long long)gen, mask, reasons);
}

/* One mark. Returns 1 when a fragment was rebuilt or the mark retired. */
static int sr_one(struct efsd_server *s, struct efs_kv *kv, uint8_t group,
                  const struct efs_node *nodes, uint32_t count, uint64_t id, uint64_t gen,
                  uint32_t mask, uint32_t reasons, uint8_t *buf[EFS_NUM_FRAGMENTS], int report)
{
    struct efs_pack_object *o = malloc(sizeof(*o));
    uint64_t now = sr_now_us();
    int done = 0;
    if (!o) return 0;
    int rc = efs_pack_object_get(kv, group, id, gen, o);
    struct efs_pack_ledger_usage u = {0};
    if (rc == EFS_OK) efs_pack_ledger_usage(&o->ledger, &u);
    if (rc == EFS_ERR_NOT_FOUND || (rc == EFS_OK && (o->ledger.phase != EFS_PACK_ACTIVE ||
                                                     (!u.live && !u.pending)))) {
        /* Rewritten, removed, compacted or cancelled: nothing to protect. */
        if (sr_propose(group, EFS_MD_PACK_DEGRADED_RETIRE, id, gen, mask, 0) == EFS_OK) {
            SR_ADD(segment_marks_retired, 1);
            fprintf(stderr, "segment-rebuild: g%u id=%016llx gen=%llx mask=%x: segment no longer "
                    "live, mark retired\n", group, (unsigned long long)id, (unsigned long long)gen,
                    mask);
            done = 1;
        }
        goto out;
    }
    if (rc != EFS_OK) {
        SR_ADD(segment_rebuild_failures, 1);
        fprintf(stderr, "segment-rebuild: g%u id=%016llx gen=%llx: segment object unreadable "
                "(rc=%d); mark kept\n", group, (unsigned long long)id, (unsigned long long)gen, rc);
        goto out;
    }
    if (mask & (mask - 1u)) {
        if (report) sr_say_unrecoverable(group, id, gen, mask, reasons);
        goto out;
    }
    if (!u.live) goto out; /* the packer's switch is still in flight */
#if EFS_FAULTS
    /* K37 tests: EFS_FAULT_SEGMENT_REBUILD_HOLD=<path> keeps every mark
     * waiting while the file exists, so the degraded state is observable
     * with all members up. */
    {
        const char *hold = getenv("EFS_FAULT_SEGMENT_REBUILD_HOLD");
        if (hold && !access(hold, F_OK)) goto out;
    }
#endif
    uint32_t fi = (uint32_t)__builtin_ctz(mask), a = (fi + 1) % EFS_NUM_FRAGMENTS,
             b = (fi + 2) % EFS_NUM_FRAGMENTS, len = o->ledger.geometry / 2;
    if (!sr_available(s, nodes, count, o->nodes[fi], now) ||
        !sr_available(s, nodes, count, o->nodes[a], now) ||
        !sr_available(s, nodes, count, o->nodes[b], now))
        goto out; /* still away: wait for the heartbeat to see it */
    uint32_t src[2] = {a, b};
    for (int k = 0; k < 2; k++) {
        rc = sr_get_fragment(s, nodes, count, o, src[k], buf[k]);
        if (rc == EFS_OK) continue;
        if (rc != EFS_ERR_NET) {
            /* Not a transport failure: ask the holder for a verdict. */
            uint32_t v = sr_check_one(s, nodes, count, o, src[k]);
            if (v == EFS_SEGMENT_AUDIT_MISSING || v == EFS_SEGMENT_AUDIT_BAD) {
                if (server_segment_mark((uint8_t)group, id, gen, src[k], EFS_PACK_DEGRADED_R_SOURCE) == EFS_OK) {
                    sr_say_unrecoverable(group, id, gen, mask | (1u << src[k]),
                                         reasons | EFS_PACK_DEGRADED_R_SOURCE);
                    goto out;
                }
            }
        }
        sr_backoff_us[o->nodes[src[k]]] = now + SR_NODE_BACKOFF_US;
        SR_ADD(segment_rebuild_failures, 1);
        fprintf(stderr, "segment-rebuild: g%u id=%016llx gen=%llx fragment %u: source fragment %u "
                "on node %u unavailable (rc=%d); mark kept\n", group, (unsigned long long)id,
                (unsigned long long)gen, fi, src[k], o->nodes[src[k]], rc);
        goto out;
    }
    uint8_t sum[EFS_HASH_SIZE];
    rc = efs_rebuild_fragment(buf[0], buf[1], buf[2], len);
    if (rc == EFS_OK) {
        efs_hash(buf[2], len, sum);
        if (memcmp(sum, o->checksums[fi], EFS_HASH_SIZE)) rc = EFS_ERR_CHECKSUM;
    }
    if (rc != EFS_OK) {
        SR_ADD(segment_rebuild_failures, 1);
        fprintf(stderr, "segment-rebuild: g%u id=%016llx gen=%llx fragment %u: rebuilt bytes do "
                "not match the segment checksum (rc=%d); mark kept\n", group,
                (unsigned long long)id, (unsigned long long)gen, fi, rc);
        goto out;
    }
    rc = sr_put_rebuilt(s, nodes, count, o, fi, buf[2]);
    if (rc != EFS_OK) {
        sr_backoff_us[o->nodes[fi]] = now + SR_NODE_BACKOFF_US;
        SR_ADD(segment_rebuild_failures, 1);
        fprintf(stderr, "segment-rebuild: g%u id=%016llx gen=%llx fragment %u node %u: PUT failed "
                "(rc=%d); mark kept\n", group, (unsigned long long)id, (unsigned long long)gen, fi,
                o->nodes[fi], rc);
        goto out;
    }
    /* Proof: the placement node reads the stored copy back and verifies it. */
    uint32_t v = sr_check_one(s, nodes, count, o, fi);
    if (v != EFS_SEGMENT_AUDIT_OK) {
        SR_ADD(segment_rebuild_failures, 1);
        fprintf(stderr, "segment-rebuild: g%u id=%016llx gen=%llx fragment %u node %u: rebuilt copy "
                "not verified (audit=%u); mark kept\n", group, (unsigned long long)id,
                (unsigned long long)gen, fi, o->nodes[fi], v);
        goto out;
    }
    if (sr_propose(group, EFS_MD_PACK_DEGRADED_CLEAR, id, gen, mask, 0) != EFS_OK)
        goto out; /* the copy stays good; the next pass clears it */
    SR_ADD(segments_rebuilt, 1);
    fprintf(stderr, "segment-rebuild: rebuilt g%u id=%016llx gen=%llx fragment %u on node %u "
            "(from nodes %u,%u)\n", group, (unsigned long long)id, (unsigned long long)gen, fi,
            o->nodes[fi], o->nodes[a], o->nodes[b]);
    done = 1;
out:
    free(o);
    return done;
}

struct sr_mark_scan {
    const uint8_t *skip;
    uint32_t skip_len, n;
    int full;
    uint8_t keys[SR_MARKS_MAX][SR_KEY];
    uint8_t vals[SR_MARKS_MAX][EFS_PACK_DEGRADED_VAL];
    uint8_t ok[SR_MARKS_MAX];
};
static int sr_mark_cb(void *u, const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl)
{
    struct sr_mark_scan *c = u;
    if (c->skip_len && kl == c->skip_len && !memcmp(k, c->skip, kl)) return 0;
    if (c->n == SR_MARKS_MAX) { c->full = 1; return 1; }
    memset(c->keys[c->n], 0, SR_KEY);
    memcpy(c->keys[c->n], k, kl < SR_KEY ? kl : SR_KEY);
    memset(c->vals[c->n], 0, EFS_PACK_DEGRADED_VAL);
    memcpy(c->vals[c->n], v, vl < EFS_PACK_DEGRADED_VAL ? vl : EFS_PACK_DEGRADED_VAL);
    c->ok[c->n] = kl == SR_KEY && vl == EFS_PACK_DEGRADED_VAL && v[0] && v[0] <= 7 && !v[2] && !v[3];
    c->n++;
    return 0;
}
struct sr_count { uint64_t total, lost, bad; };
static int sr_count_cb(void *u, const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl)
{
    struct sr_count *c = u;
    (void)k;
    c->total++;
    if (kl != SR_KEY || vl != EFS_PACK_DEGRADED_VAL || !v[0] || v[0] > 7) c->bad++;
    else if (v[0] & (v[0] - 1u)) c->lost++;
    return 0;
}

void server_segment_rebuild_pass(struct efsd_server *s, uint8_t group)
{
    struct efs_kv *kv = server_raft_host_kv();
    uint32_t anchor = efs_pack_group_anchor(group), count;
    int gi = sr_gi(group);
    uint64_t t0 = sr_now_us();
    EFS_NODE_TABLE(nodes);
    if (!s || !kv || !anchor || !nodes) return;
    pthread_mutex_lock(&s->lock);
    count = s->node_count > EFS_MAX_NODES ? EFS_MAX_NODES : s->node_count;
    memcpy(nodes, s->nodes, count * sizeof(nodes[0]));
    pthread_mutex_unlock(&s->lock);

    /* 1. audit members that returned. */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = nodes[i].id;
        if (!id || id > EFS_MAX_NODES || !sr_available(s, nodes, count, id, sr_now_us())) continue;
        if (sr_now_us() - t0 > SR_BUDGET_US || !server_raft_host_gc_running()) break;
        sr_audit_node(s, kv, group, nodes, count, id, sr_now_us());
    }

    /* 2. rebuild marked segments. */
    uint8_t prefix[3] = {(uint8_t)(anchor >> 8), (uint8_t)anchor, EFS_KV_KIND_PACK_DEGRADED};
    struct sr_count cnt = {0};
    if (efs_kv_scan_prefix(kv, prefix, sizeof(prefix), sr_count_cb, &cnt) < 0) return;
    __atomic_store_n(&s->degraded_segments[gi], cnt.total, __ATOMIC_RELAXED);
    __atomic_store_n(&s->unrecoverable_segments[gi], cnt.lost, __ATOMIC_RELAXED);
    __atomic_fetch_or(&s->segment_sampled_mask, 1u << gi, __ATOMIC_RELAXED);
    if (!cnt.total) {
        sr[gi].mark_cursor_len = 0;
        return;
    }
    /* Unrecoverable marks are reported again at most once a minute. */
    int report = t0 >= sr[gi].said_unrecoverable_us + 60000000ull;
    if (report) sr[gi].said_unrecoverable_us = t0;
    struct sr_mark_scan *c = calloc(1, sizeof(*c));
    uint8_t *buf[EFS_NUM_FRAGMENTS] = {0};
    uint32_t done = 0;
    int wrapped = 0;
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) buf[i] = malloc(EFS_SEGMENT_MAX_SIZE / 2);
    if (!c || !buf[0] || !buf[1] || !buf[2]) goto out;
    for (;;) {
        memset(c, 0, sizeof(*c));
        c->skip = sr[gi].mark_cursor;
        c->skip_len = sr[gi].mark_cursor_len;
        if (efs_kv_scan_from(kv, prefix, sizeof(prefix),
                             sr[gi].mark_cursor_len ? sr[gi].mark_cursor : NULL,
                             sr[gi].mark_cursor_len, sr_mark_cb, c) < 0)
            break;
        if (!c->n) {
            if (sr[gi].mark_cursor_len && !wrapped) {
                sr[gi].mark_cursor_len = 0;
                wrapped = 1;
                continue;
            }
            break;
        }
        uint32_t i;
        for (i = 0; i < c->n; i++) {
            if (done >= SR_REBUILD_PER_PASS || sr_now_us() - t0 > SR_BUDGET_US ||
                !server_raft_host_gc_running())
                break;
            if (!c->ok[i]) {
                if (report)
                    fprintf(stderr, "segment-rebuild: group %u: malformed segment mark; kept "
                            "(fail closed)\n", group);
                SR_ADD(segment_rebuild_failures, 1);
                continue;
            }
            done += (uint32_t)sr_one(s, kv, group, nodes, count, sr_get64(c->keys[i] + 3),
                                     sr_get64(c->keys[i] + 11), c->vals[i][0], c->vals[i][1], buf,
                                     report);
        }
        if (i > 0) {
            memcpy(sr[gi].mark_cursor, c->keys[i - 1], SR_KEY);
            sr[gi].mark_cursor_len = SR_KEY;
        }
        if (i < c->n || !c->full) break;
    }
out:
    free(c);
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) free(buf[i]);
    cnt = (struct sr_count){0};
    if (efs_kv_scan_prefix(kv, prefix, sizeof(prefix), sr_count_cb, &cnt) >= 0) {
        __atomic_store_n(&s->degraded_segments[gi], cnt.total, __ATOMIC_RELAXED);
        __atomic_store_n(&s->unrecoverable_segments[gi], cnt.lost, __ATOMIC_RELAXED);
    }
}
