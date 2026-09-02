/* Deterministic discrete-event simulator. Drives the CURRENT efs_export
 * table SM against mem store + mem kv + loop transport. */
#include "efs/sim.h"
#include "efs/metadata.h"
#include "efs/kv.h"
#include "efs/transport.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/placement.h"
#include "efs/protocol.h"
#include "efs/opid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SIM_MAX_EV 128
#define TAB_KEY    ((const uint8_t *)"tab")
#define TLEN_KEY   ((const uint8_t *)"tlen")

enum {
    EV_CREATE = 1,
    EV_LOOKUP,
    EV_UNLINK,
    EV_PUT_FRAG,
    EV_PUBLISH
};

struct sim_ev {
    uint64_t tick;
    uint32_t seq;
    uint8_t kind;
    uint8_t client;
    efs_ino_t parent;
    efs_ino_t ino;
    uint32_t mode;
    uint32_t chunk_index;
    uint32_t frag_index;
    uint32_t plen;
    uint64_t new_size;
    char name[EFS_MAX_NAME];
    uint8_t *payload;
};

struct sim_server {
    int alive;
    int partitioned;
    struct efs_export *ex; /* only primary */
    struct efs_store *store;
    struct efs_kv *disk;
    struct efs_transport *rx[EFS_SIM_MAX_CLIENTS];
};

struct sim_client {
    struct efs_transport *tx[EFS_SIM_MAX_SERVERS];
    struct efs_opid_window win;
};

struct efs_sim {
    uint64_t rng;
    uint64_t now;
    uint64_t history;
    uint32_t seq;
    uint32_t delay_max;
    uint32_t drop_per_mille;
    int nservers;
    int nclients;
    int hold;
    int last_rc;
    uint32_t nev;
    struct sim_ev ev[SIM_MAX_EV];
    struct sim_server srv[EFS_SIM_MAX_SERVERS];
    struct sim_client cli[EFS_SIM_MAX_CLIENTS];
};

static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void hist(struct efs_sim *sim, uint32_t kind, uint64_t a, uint64_t b)
{
    uint64_t x = sim->history ^ (sim->now + 1) * 0x9E3779B97F4A7C15ULL;
    x ^= ((uint64_t)kind << 48) ^ (a << 16) ^ b;
    sim->history = splitmix64(&x);
}

static void frag_sum(const struct efs_frag_id *id, const uint8_t *data,
                     uint32_t len, uint8_t out[EFS_HASH_SIZE])
{
    uint8_t hdr[32];
    uint8_t *tmp;

    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, &id->export_id, sizeof(id->export_id));
    memcpy(hdr + 4, &id->ino, sizeof(id->ino));
    memcpy(hdr + 12, &id->chunk_index, sizeof(id->chunk_index));
    memcpy(hdr + 16, &id->fragment_index, sizeof(id->fragment_index));
    tmp = malloc(32 + (size_t)len);
    if (!tmp) {
        efs_hash(hdr, sizeof(hdr), out);
        return;
    }
    memcpy(tmp, hdr, 32);
    if (len)
        memcpy(tmp + 32, data, len);
    efs_hash(tmp, 32 + (size_t)len, out);
    free(tmp);
}

static struct efs_export *primary(struct efs_sim *sim)
{
    if (!sim->srv[EFS_SIM_META].alive || sim->srv[EFS_SIM_META].partitioned)
        return NULL;
    return sim->srv[EFS_SIM_META].ex;
}

static int persist_tab(struct efs_sim *sim)
{
    struct sim_server *s = &sim->srv[EFS_SIM_META];
    char *blob = NULL;
    size_t blen = 0;
    uint32_t n;
    int rc;

    if (!s->ex || !s->disk)
        return EFS_ERR_INVAL;
    rc = efs_export_serialize(s->ex, &blob, &blen);
    if (rc != EFS_OK)
        return rc;
    n = (uint32_t)blen;
    rc = efs_kv_put(s->disk, TLEN_KEY, 4, (const uint8_t *)&n, 4);
    if (rc == EFS_OK)
        rc = efs_kv_put(s->disk, TAB_KEY, 3, (const uint8_t *)blob, n);
    free(blob);
    return rc;
}

static int load_tab(struct sim_server *s)
{
    uint32_t n = 0, slen = 4;
    uint8_t *blob;
    int rc;

    if (!s->ex || !s->disk)
        return EFS_ERR_INVAL;
    rc = efs_kv_get(s->disk, TLEN_KEY, 4, (uint8_t *)&n, &slen);
    if (rc != EFS_OK)
        return rc;
    blob = malloc(n ? n : 1);
    if (!blob)
        return EFS_ERR_NOMEM;
    slen = n;
    rc = efs_kv_get(s->disk, TAB_KEY, 3, blob, &slen);
    if (rc == EFS_OK)
        rc = efs_export_deserialize(s->ex, (const char *)blob, slen);
    free(blob);
    return rc;
}

static int ev_cmp(const struct sim_ev *a, const struct sim_ev *b)
{
    if (a->tick < b->tick)
        return -1;
    if (a->tick > b->tick)
        return 1;
    if (a->seq < b->seq)
        return -1;
    if (a->seq > b->seq)
        return 1;
    return 0;
}

static int schedule(struct efs_sim *sim, struct sim_ev *in)
{
    uint32_t i, j;
    uint64_t delay;

    if (sim->nev >= SIM_MAX_EV)
        return EFS_ERR_NOMEM;
    delay = 0;
    if (sim->delay_max)
        delay = efs_sim_rng(sim) % (sim->delay_max + 1);
    if (sim->drop_per_mille) {
        uint64_t r = efs_sim_rng(sim) % 1000;
        if (r < sim->drop_per_mille) {
            hist(sim, 0xff, in->kind, in->ino);
            free(in->payload);
            in->payload = NULL;
            sim->last_rc = EFS_ERR_AGAIN;
            return EFS_OK;
        }
    }
    in->tick = sim->now + delay;
    in->seq = sim->seq++;
    i = sim->nev;
    while (i > 0 && ev_cmp(in, &sim->ev[i - 1]) < 0)
        i--;
    for (j = sim->nev; j > i; j--)
        sim->ev[j] = sim->ev[j - 1];
    sim->ev[i] = *in;
    sim->nev++;
    in->payload = NULL;
    return EFS_OK;
}

static int maybe_drain(struct efs_sim *sim)
{
    if (sim->hold)
        return EFS_OK;
    return efs_sim_drain(sim);
}

static int place_rank(struct efs_sim *sim, efs_ino_t ino, uint32_t ci,
                      efs_node_id_t ranks[EFS_NUM_FRAGMENTS])
{
    efs_get_placement((uint32_t)sim->nservers, ino, ci, ranks);
    return EFS_OK;
}

static int frag_ok(struct efs_sim *sim, const struct efs_frag_id *id,
                   int *server_out, uint8_t *buf, uint32_t *len,
                   uint8_t sum[EFS_HASH_SIZE])
{
    efs_node_id_t ranks[EFS_NUM_FRAGMENTS];
    int si;
    uint8_t got[EFS_HASH_SIZE];
    uint8_t expect[EFS_HASH_SIZE];
    int sum_ok = 0;
    int rc;

    place_rank(sim, id->ino, id->chunk_index, ranks);
    if (id->fragment_index >= EFS_NUM_FRAGMENTS)
        return EFS_ERR_INVAL;
    si = (int)ranks[id->fragment_index] - 1;
    if (si < 0 || si >= sim->nservers)
        return EFS_ERR_INVAL;
    if (server_out)
        *server_out = si;
    rc = efs_store_get(sim->srv[si].store, id, buf, len, got, &sum_ok);
    if (rc != EFS_OK)
        return rc;
    if (!sum_ok)
        return EFS_ERR_IO;
    frag_sum(id, buf, *len, expect);
    if (memcmp(got, expect, EFS_HASH_SIZE) != 0)
        return EFS_ERR_IO; /* I25: corrupt / identity mismatch → unavailable */
    if (sum)
        memcpy(sum, got, EFS_HASH_SIZE);
    return EFS_OK;
}

static int durable_count(struct efs_sim *sim, efs_ino_t ino, uint32_t ci,
                         uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE],
                         efs_node_id_t ranks[EFS_NUM_FRAGMENTS])
{
    int n = 0;
    int i;
    uint32_t flen_cap = (uint32_t)EFS_SIM_CHUNK / 2;
    uint8_t *buf = malloc(flen_cap);
    uint32_t flen;

    if (!buf)
        return -1;
    place_rank(sim, ino, ci, ranks);
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        struct efs_frag_id id = { .export_id = 1, .ino = ino,
                                  .chunk_index = ci,
                                  .fragment_index = (uint32_t)i };
        flen = flen_cap;
        if (frag_ok(sim, &id, NULL, buf, &flen, sums[i]) == EFS_OK)
            n++;
    }
    free(buf);
    return n;
}

static int apply_create(struct efs_sim *sim, struct sim_ev *e)
{
    struct efs_export *ex = primary(sim);
    efs_ino_t ino;

    if (!ex)
        return EFS_ERR_BUSY;
    ino = efs_export_create(ex, e->parent, e->mode, 0, 0, e->name);
    if (!ino)
        return EFS_ERR_EXIST;
    hist(sim, EV_CREATE, ino, e->parent);
    return persist_tab(sim);
}

static int apply_lookup(struct efs_sim *sim, struct sim_ev *e)
{
    struct efs_export *ex = primary(sim);
    struct efs_inode rec;

    if (!ex)
        return EFS_ERR_BUSY;
    if (efs_export_lookup(ex, e->parent, e->name, &rec) != 0)
        return EFS_ERR_NOT_FOUND;
    e->ino = rec.ino;
    hist(sim, EV_LOOKUP, rec.ino, e->parent);
    return EFS_OK;
}

static int apply_unlink(struct efs_sim *sim, struct sim_ev *e)
{
    struct efs_export *ex = primary(sim);
    int rc;

    if (!ex)
        return EFS_ERR_BUSY;
    rc = efs_export_unlink_name(ex, e->parent, e->name);
    if (rc != 0)
        return rc;
    hist(sim, EV_UNLINK, e->parent, 0);
    return persist_tab(sim);
}

static int apply_put(struct efs_sim *sim, struct sim_ev *e)
{
    struct efs_frag_id id = { .export_id = 1, .ino = e->ino,
                              .chunk_index = e->chunk_index,
                              .fragment_index = e->frag_index };
    efs_node_id_t ranks[EFS_NUM_FRAGMENTS];
    int si;
    uint8_t sum[EFS_HASH_SIZE];
    uint8_t type = 0;
    void *pl = NULL;
    uint32_t plen = 0;
    int rc;

    place_rank(sim, e->ino, e->chunk_index, ranks);
    si = (int)ranks[e->frag_index] - 1;
    if (si < 0 || si >= sim->nservers)
        return EFS_ERR_INVAL;
    if (!sim->srv[si].alive || sim->srv[si].partitioned)
        return EFS_ERR_BUSY;
    if (e->client >= (uint8_t)sim->nclients)
        return EFS_ERR_INVAL;
    frag_sum(&id, e->payload, e->plen, sum);
    rc = efs_transport_send_parts(sim->cli[e->client].tx[si], EFS_MSG_PUT_CHUNK,
                                  &id, (uint32_t)sizeof(id),
                                  e->payload, e->plen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_transport_recv(sim->srv[si].rx[e->client], &type, &pl, &plen);
    if (rc != EFS_OK)
        return rc;
    free(pl);
    if (type != EFS_MSG_PUT_CHUNK)
        return EFS_ERR_PROTO;
    rc = efs_store_put(sim->srv[si].store, &id, e->payload, e->plen, sum);
    if (rc == EFS_OK)
        hist(sim, EV_PUT_FRAG, e->ino, e->frag_index);
    return rc;
}

static int apply_publish(struct efs_sim *sim, struct sim_ev *e)
{
    struct efs_export *ex = primary(sim);
    uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
    efs_node_id_t ranks[EFS_NUM_FRAGMENTS];
    int n;
    int rc;

    if (!ex)
        return EFS_ERR_BUSY;
    n = durable_count(sim, e->ino, e->chunk_index, sums, ranks);
    if (n < 0)
        return EFS_ERR_NOMEM;
    /* I14: healthy publish needs all k+f = 3 fragments durable. */
    if (n < EFS_NUM_FRAGMENTS)
        return EFS_ERR_IO;
    rc = efs_export_set_chunk(ex, e->ino, e->chunk_index, ranks, sums);
    if (rc != 0)
        return rc;
    rc = efs_export_set_size(ex, e->ino, e->new_size);
    if (rc != 0)
        return rc;
    hist(sim, EV_PUBLISH, e->ino, e->chunk_index);
    return persist_tab(sim);
}

static int apply_one(struct efs_sim *sim, struct sim_ev *e)
{
    int rc;

    sim->now = e->tick;
    switch (e->kind) {
    case EV_CREATE:
        rc = apply_create(sim, e);
        break;
    case EV_LOOKUP:
        rc = apply_lookup(sim, e);
        break;
    case EV_UNLINK:
        rc = apply_unlink(sim, e);
        break;
    case EV_PUT_FRAG:
        rc = apply_put(sim, e);
        break;
    case EV_PUBLISH:
        rc = apply_publish(sim, e);
        break;
    default:
        rc = EFS_ERR_INVAL;
        break;
    }
    free(e->payload);
    e->payload = NULL;
    sim->last_rc = rc;
    return rc;
}

int efs_sim_check(struct efs_sim *sim)
{
    struct efs_export *ex;
    uint64_t i, j;

    if (!sim)
        return EFS_ERR_INVAL;
    ex = sim->srv[EFS_SIM_META].ex;
    if (!sim->srv[EFS_SIM_META].alive || !ex)
        return EFS_OK;
    for (i = 0; i < ex->inode_count; i++) {
        struct efs_inode_mem *a = efs_export_inode_at(ex, i);
        if (!a || a->ino == 0)
            continue;
        for (j = i + 1; j < ex->inode_count; j++) {
            struct efs_inode_mem *b = efs_export_inode_at(ex, j);
            if (b && b->ino == a->ino) {
                fprintf(stderr, "sim: duplicate ino %llu\n",
                        (unsigned long long)a->ino);
                return EFS_ERR_PROTO;
            }
        }
    }
    return EFS_OK;
}

int efs_sim_drain(struct efs_sim *sim)
{
    int rc = EFS_OK;

    if (!sim)
        return EFS_ERR_INVAL;
    while (sim->nev) {
        struct sim_ev e = sim->ev[0];
        uint32_t i;

        for (i = 1; i < sim->nev; i++)
            sim->ev[i - 1] = sim->ev[i];
        sim->nev--;
        rc = apply_one(sim, &e);
        if (efs_sim_check(sim) != EFS_OK)
            return EFS_ERR_PROTO;
        /* BUSY/AGAIN from crash/drop is a legal outcome, not a drain abort. */
        (void)rc;
    }
    return efs_sim_check(sim);
}

uint64_t efs_sim_rng(struct efs_sim *sim)
{
    return splitmix64(&sim->rng);
}

uint64_t efs_sim_now(const struct efs_sim *sim)
{
    return sim ? sim->now : 0;
}

uint64_t efs_sim_history(const struct efs_sim *sim)
{
    return sim ? sim->history : 0;
}

void efs_sim_hold(struct efs_sim *sim, int hold)
{
    if (sim)
        sim->hold = hold ? 1 : 0;
}

struct efs_sim *efs_sim_new(const struct efs_sim_cfg *cfg)
{
    struct efs_sim *sim;
    int i, c;

    if (!cfg || cfg->nservers < 3 || cfg->nservers > EFS_SIM_MAX_SERVERS)
        return NULL;
    if (cfg->nclients < 1 || cfg->nclients > EFS_SIM_MAX_CLIENTS)
        return NULL;
    sim = calloc(1, sizeof(*sim));
    if (!sim)
        return NULL;
    sim->rng = cfg->seed ? cfg->seed : 1;
    sim->nservers = cfg->nservers;
    sim->nclients = cfg->nclients;
    sim->delay_max = cfg->delay_max;
    sim->drop_per_mille = cfg->drop_per_mille;
    for (i = 0; i < sim->nservers; i++) {
        sim->srv[i].alive = 1;
        sim->srv[i].store = efs_store_mem_create();
        sim->srv[i].disk = efs_kv_mem_create();
        if (!sim->srv[i].store || !sim->srv[i].disk) {
            efs_sim_free(sim);
            return NULL;
        }
        for (c = 0; c < sim->nclients; c++) {
            if (efs_transport_loop_pair(&sim->cli[c].tx[i],
                                        &sim->srv[i].rx[c]) != EFS_OK) {
                efs_sim_free(sim);
                return NULL;
            }
        }
    }
    sim->srv[EFS_SIM_META].ex = calloc(1, sizeof(*sim->srv[EFS_SIM_META].ex));
    if (!sim->srv[EFS_SIM_META].ex) {
        efs_sim_free(sim);
        return NULL;
    }
    efs_export_init(sim->srv[EFS_SIM_META].ex, 1, "sim");
    if (persist_tab(sim) != EFS_OK) {
        efs_sim_free(sim);
        return NULL;
    }
    for (c = 0; c < sim->nclients; c++) {
        uint8_t uuid[EFS_OPID_UUID_LEN];
        memset(uuid, 0, sizeof(uuid));
        uuid[15] = (uint8_t)(c + 1);
        efs_opid_window_init(&sim->cli[c].win, uuid, 1);
    }
    return sim;
}

void efs_sim_free(struct efs_sim *sim)
{
    int i, c;
    uint32_t k;

    if (!sim)
        return;
    for (k = 0; k < sim->nev; k++)
        free(sim->ev[k].payload);
    for (i = 0; i < EFS_SIM_MAX_SERVERS; i++) {
        if (sim->srv[i].ex) {
            efs_export_free(sim->srv[i].ex);
            free(sim->srv[i].ex);
        }
        efs_store_mem_free(sim->srv[i].store);
        efs_kv_mem_free(sim->srv[i].disk);
        for (c = 0; c < EFS_SIM_MAX_CLIENTS; c++) {
            efs_transport_loop_free(sim->srv[i].rx[c]);
            efs_transport_loop_free(sim->cli[c].tx[i]);
        }
    }
    free(sim);
}

int efs_sim_create(struct efs_sim *sim, int client, efs_ino_t parent,
                   uint32_t mode, const char *name, efs_ino_t *out)
{
    struct sim_ev e;
    int rc;

    if (!sim || !name || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    memset(&e, 0, sizeof(e));
    e.kind = EV_CREATE;
    e.client = (uint8_t)client;
    e.parent = parent;
    e.mode = mode;
    strncpy(e.name, name, EFS_MAX_NAME - 1);
    sim->last_rc = EFS_OK;
    rc = schedule(sim, &e);
    if (rc != EFS_OK)
        return rc;
    rc = maybe_drain(sim);
    if (rc != EFS_OK)
        return rc;
    if (out) {
        struct efs_inode rec;
        struct efs_export *ex = primary(sim);
        *out = 0;
        if (ex && efs_export_lookup(ex, parent, name, &rec) == 0)
            *out = rec.ino;
    }
    return sim->last_rc;
}

void efs_sim_opid_for(struct efs_sim *sim, int client, uint64_t seq,
                      struct efs_opid *out)
{
    if (!sim || !out || client < 0 || client >= sim->nclients)
        return;
    memset(out, 0, sizeof(*out));
    memcpy(out->client_uuid, sim->cli[client].win.client_uuid,
           EFS_OPID_UUID_LEN);
    out->session_epoch = sim->cli[client].win.session_epoch;
    out->seq = seq;
}

int efs_sim_opid_ack(struct efs_sim *sim, int client, uint64_t contiguous_ack)
{
    if (!sim || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    return efs_opid_ack(&sim->cli[client].win, contiguous_ack);
}

int efs_sim_create_op(struct efs_sim *sim, int client, const struct efs_opid *op,
                      efs_ino_t parent, uint32_t mode, const char *name,
                      efs_ino_t *out)
{
    struct efs_opid_reply rep;
    int hit;

    if (!sim || !op || !name || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    memset(&rep, 0, sizeof(rep));
    hit = efs_opid_lookup(&sim->cli[client].win, op, &rep);
    if (hit < 0)
        return hit;
    if (hit) {
        if (out)
            *out = rep.ino;
        hist(sim, EV_CREATE, rep.ino, 0x16);
        return rep.rc;
    }
    hit = efs_sim_create(sim, client, parent, mode, name, out);
    rep.rc = hit;
    rep.ino = out ? *out : 0;
    efs_opid_complete(&sim->cli[client].win, op, &rep);
    return hit;
}

int efs_sim_lookup(struct efs_sim *sim, int client, efs_ino_t parent,
                   const char *name, efs_ino_t *out)
{
    struct sim_ev e;
    int rc;

    if (!sim || !name || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    memset(&e, 0, sizeof(e));
    e.kind = EV_LOOKUP;
    e.client = (uint8_t)client;
    e.parent = parent;
    strncpy(e.name, name, EFS_MAX_NAME - 1);
    sim->last_rc = EFS_OK;
    rc = schedule(sim, &e);
    if (rc != EFS_OK)
        return rc;
    rc = maybe_drain(sim);
    if (rc != EFS_OK)
        return rc;
    if (out) {
        struct efs_inode rec;
        struct efs_export *ex = primary(sim);
        *out = 0;
        if (ex && efs_export_lookup(ex, parent, name, &rec) == 0)
            *out = rec.ino;
    }
    return sim->last_rc;
}

int efs_sim_unlink(struct efs_sim *sim, int client, efs_ino_t parent,
                   const char *name)
{
    struct sim_ev e;
    int rc;

    if (!sim || !name || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    memset(&e, 0, sizeof(e));
    e.kind = EV_UNLINK;
    e.client = (uint8_t)client;
    e.parent = parent;
    strncpy(e.name, name, EFS_MAX_NAME - 1);
    sim->last_rc = EFS_OK;
    rc = schedule(sim, &e);
    if (rc != EFS_OK)
        return rc;
    rc = maybe_drain(sim);
    if (rc != EFS_OK)
        return rc;
    return sim->last_rc;
}

int efs_sim_put_stripe(struct efs_sim *sim, int client, efs_ino_t ino,
                       uint32_t chunk_index, const uint8_t *chunk,
                       uint32_t chunk_len, int skip_frag)
{
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    uint8_t *block = NULL;
    size_t fl = (size_t)EFS_SIM_CHUNK / 2;
    int i, rc = EFS_OK;

    if (!sim || !chunk || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    if (chunk_len > EFS_SIM_CHUNK)
        return EFS_ERR_INVAL;
    block = calloc(EFS_NUM_FRAGMENTS, fl);
    if (!block)
        return EFS_ERR_NOMEM;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        frags[i] = block + (size_t)i * fl;
    rc = efs_encode_chunk(chunk, chunk_len, EFS_SIM_CHUNK, frags);
    if (rc != EFS_OK) {
        free(block);
        return rc;
    }
    sim->last_rc = EFS_OK;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        struct sim_ev e;
        if (skip_frag >= 0 && i == skip_frag)
            continue;
        memset(&e, 0, sizeof(e));
        e.kind = EV_PUT_FRAG;
        e.client = (uint8_t)client;
        e.ino = ino;
        e.chunk_index = chunk_index;
        e.frag_index = (uint32_t)i;
        e.plen = (uint32_t)fl;
        e.payload = malloc(fl);
        if (!e.payload) {
            free(block);
            return EFS_ERR_NOMEM;
        }
        memcpy(e.payload, frags[i], fl);
        rc = schedule(sim, &e);
        if (rc != EFS_OK) {
            free(e.payload);
            free(block);
            return rc;
        }
    }
    free(block);
    rc = maybe_drain(sim);
    if (rc != EFS_OK)
        return rc;
    return sim->last_rc;
}

int efs_sim_publish(struct efs_sim *sim, efs_ino_t ino, uint32_t chunk_index,
                    uint64_t new_size)
{
    struct sim_ev e;
    int rc;

    if (!sim)
        return EFS_ERR_INVAL;
    memset(&e, 0, sizeof(e));
    e.kind = EV_PUBLISH;
    e.ino = ino;
    e.chunk_index = chunk_index;
    e.new_size = new_size;
    sim->last_rc = EFS_OK;
    rc = schedule(sim, &e);
    if (rc != EFS_OK)
        return rc;
    rc = maybe_drain(sim);
    if (rc != EFS_OK)
        return rc;
    return sim->last_rc;
}

int efs_sim_read_chunk(struct efs_sim *sim, efs_ino_t ino, uint32_t chunk_index,
                       uint8_t *out, uint32_t chunk_len)
{
    struct efs_export *ex;
    struct efs_chunk_entry ce;
    uint8_t *block = NULL;
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    int present[EFS_NUM_FRAGMENTS];
    size_t fl = (size_t)EFS_SIM_CHUNK / 2;
    uint8_t *full;
    int i, a = -1, b = -1, miss = -1, n = 0;
    int rc;

    if (!sim || !out || chunk_len > EFS_SIM_CHUNK)
        return EFS_ERR_INVAL;
    ex = primary(sim);
    if (!ex)
        return EFS_ERR_BUSY;
    if (efs_export_get_chunk(ex, ino, chunk_index, &ce) != 0)
        return EFS_ERR_NOT_FOUND; /* unpublished: I15, not served */
    block = calloc(EFS_NUM_FRAGMENTS, fl);
    full = malloc(EFS_SIM_CHUNK);
    if (!block || !full) {
        free(block);
        free(full);
        return EFS_ERR_NOMEM;
    }
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        struct efs_frag_id id = { .export_id = 1, .ino = ino,
                                  .chunk_index = chunk_index,
                                  .fragment_index = (uint32_t)i };
        uint32_t len = (uint32_t)fl;
        frags[i] = block + (size_t)i * fl;
        present[i] = 0;
        if (frag_ok(sim, &id, NULL, frags[i], &len, NULL) == EFS_OK) {
            present[i] = 1;
            n++;
        }
    }
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (present[i]) {
            if (a < 0)
                a = i;
            else if (b < 0)
                b = i;
        } else if (miss < 0) {
            miss = i;
        }
    }
    if (n < 2) {
        free(block);
        free(full);
        return EFS_ERR_IO;
    }
    if (miss < 0)
        miss = 0; /* all present: decode with 0 missing, have 1+2 */
    if (n == 3) {
        a = 1;
        b = 2;
        miss = 0;
    }
    rc = efs_decode_chunk(frags, EFS_SIM_CHUNK, a, b, miss, full, EFS_SIM_CHUNK);
    if (rc == EFS_OK)
        memcpy(out, full, chunk_len);
    free(block);
    free(full);
    return rc;
}

int efs_sim_frag_present(struct efs_sim *sim, const struct efs_frag_id *id)
{
    efs_node_id_t ranks[EFS_NUM_FRAGMENTS];
    uint8_t buf[16];
    uint32_t len = sizeof(buf);
    int si;
    int rc;

    if (!sim || !id || id->fragment_index >= EFS_NUM_FRAGMENTS)
        return 0;
    place_rank(sim, id->ino, id->chunk_index, ranks);
    si = (int)ranks[id->fragment_index] - 1;
    if (si < 0 || si >= sim->nservers)
        return 0;
    rc = efs_store_get(sim->srv[si].store, id, buf, &len, NULL, NULL);
    if (rc == EFS_ERR_INVAL) {
        /* buffer too small means present */
        return 1;
    }
    return rc == EFS_OK;
}

int efs_sim_crash(struct efs_sim *sim, int server)
{
    struct sim_server *s;

    if (!sim || server < 0 || server >= sim->nservers)
        return EFS_ERR_INVAL;
    s = &sim->srv[server];
    if (server == EFS_SIM_META && s->ex) {
        persist_tab(sim);
        efs_export_free(s->ex);
        free(s->ex);
        s->ex = NULL;
    }
    s->alive = 0;
    hist(sim, 0x10, (uint64_t)server, 0);
    return EFS_OK;
}

int efs_sim_restart(struct efs_sim *sim, int server)
{
    struct sim_server *s;
    int rc = EFS_OK;

    if (!sim || server < 0 || server >= sim->nservers)
        return EFS_ERR_INVAL;
    s = &sim->srv[server];
    if (server == EFS_SIM_META) {
        if (!s->ex) {
            s->ex = calloc(1, sizeof(*s->ex));
            if (!s->ex)
                return EFS_ERR_NOMEM;
        } else {
            efs_export_free(s->ex);
        }
        efs_export_init(s->ex, 1, "sim");
        rc = load_tab(s);
        if (rc != EFS_OK)
            return rc;
    }
    s->alive = 1;
    s->partitioned = 0;
    hist(sim, 0x11, (uint64_t)server, 0);
    return efs_sim_check(sim);
}

int efs_sim_corrupt(struct efs_sim *sim, int server, const struct efs_frag_id *id)
{
    efs_node_id_t ranks[EFS_NUM_FRAGMENTS];
    int si;
    int rc;

    if (!sim || !id)
        return EFS_ERR_INVAL;
    place_rank(sim, id->ino, id->chunk_index, ranks);
    si = (int)ranks[id->fragment_index] - 1;
    if (si < 0 || si >= sim->nservers)
        return EFS_ERR_INVAL;
    (void)server;
    rc = efs_store_mem_corrupt(sim->srv[si].store, id);
    if (rc == EFS_OK)
        hist(sim, 0x12, id->ino, id->fragment_index);
    return rc;
}

int efs_sim_partition(struct efs_sim *sim, int server, int on)
{
    if (!sim || server < 0 || server >= sim->nservers)
        return EFS_ERR_INVAL;
    sim->srv[server].partitioned = on ? 1 : 0;
    hist(sim, 0x13, (uint64_t)server, (uint64_t)on);
    return EFS_OK;
}

int efs_sim_clock_step(struct efs_sim *sim, uint64_t delta)
{
    if (!sim)
        return EFS_ERR_INVAL;
    sim->now += delta;
    hist(sim, 0x14, delta, 0);
    return EFS_OK;
}
