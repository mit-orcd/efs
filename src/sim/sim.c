/* Deterministic discrete-event simulator. Metadata is RF=3 Raft + KV apply.
 * Data plane is mem store + loop transport. */
#include "sim_internal.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/placement.h"
#include "efs/protocol.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

int sim_enqueue(struct efs_sim *sim, struct sim_ev *in)
{
    uint32_t i, j;
    uint64_t delay;

    if (sim->nev >= SIM_MAX_EV)
        return EFS_ERR_NOMEM;
    delay = 0;
    if (sim->delay_max)
        delay = efs_sim_rng(sim) % (sim->delay_max + 1);
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

static int schedule(struct efs_sim *sim, struct sim_ev *in)
{
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
    return sim_enqueue(sim, in);
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
    efs_ino_t ino = 0;
    int rc;

    rc = sim_raft_create(sim, (int)e->client, e->has_op,
                         e->has_op ? &e->op : NULL, e->parent, e->mode, e->name,
                         &ino);
    if (rc != EFS_OK)
        return rc;
    e->ino = ino;
    hist(sim, EV_CREATE, ino, e->parent);
    return EFS_OK;
}

static int apply_lookup(struct efs_sim *sim, struct sim_ev *e)
{
    struct efs_meta_dentry dent;
    int rc;

    rc = sim_raft_lookup(sim, e->parent, e->name, &dent);
    if (rc != EFS_OK)
        return rc;
    e->ino = dent.ino;
    hist(sim, EV_LOOKUP, dent.ino, e->parent);
    return EFS_OK;
}

static int apply_unlink(struct efs_sim *sim, struct sim_ev *e)
{
    int rc;

    rc = sim_raft_unlink(sim, (int)e->client, e->parent, e->name);
    if (rc != EFS_OK)
        return rc;
    hist(sim, EV_UNLINK, e->parent, 0);
    return EFS_OK;
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
    uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
    efs_node_id_t ranks[EFS_NUM_FRAGMENTS];
    struct efs_meta_chunk ch;
    int n;
    int rc;
    int i;

    n = durable_count(sim, e->ino, e->chunk_index, sums, ranks);
    if (n < 0)
        return EFS_ERR_NOMEM;
    /* I14: healthy publish needs all k+f = 3 fragments durable. */
    if (n < EFS_NUM_FRAGMENTS)
        return EFS_ERR_IO;
    memset(&ch, 0, sizeof(ch));
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        ch.nodes[i] = ranks[i];
        memcpy(ch.checksums[i], sums[i], EFS_HASH_SIZE);
    }
    rc = sim_raft_publish(sim, (int)e->client, e->ino, e->chunk_index,
                          e->new_size, &ch);
    if (rc != EFS_OK)
        return rc;
    hist(sim, EV_PUBLISH, e->ino, e->chunk_index);
    return EFS_OK;
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
    case EV_RAFT:
        rc = sim_raft_deliver(sim, e);
        break;
    default:
        rc = EFS_ERR_INVAL;
        break;
    }
    free(e->payload);
    e->payload = NULL;
    sim->last_rc = rc;
    if (rc == EFS_OK && e->kind != EV_RAFT)
        sim->last_ino = e->ino;
    return rc;
}

int efs_sim_check(struct efs_sim *sim)
{
    if (!sim)
        return EFS_ERR_INVAL;
    return sim_raft_check(sim);
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
    if (sim_raft_boot(sim) != EFS_OK) {
        efs_sim_free(sim);
        return NULL;
    }
    for (c = 0; c < sim->nclients; c++) {
        uint8_t uuid[EFS_OPID_UUID_LEN];
        memset(uuid, 0, sizeof(uuid));
        uuid[15] = (uint8_t)(c + 1);
        efs_opid_window_init(&sim->cli[c].win, uuid, 1);
    }
    if (sim_sess_boot(sim) != EFS_OK) {
        efs_sim_free(sim);
        return NULL;
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
    sim_raft_free_all(sim);
    for (i = 0; i < EFS_SIM_MAX_SERVERS; i++) {
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
    sim->last_ino = 0;
    rc = schedule(sim, &e);
    if (rc != EFS_OK)
        return rc;
    rc = maybe_drain(sim);
    if (rc != EFS_OK)
        return rc;
    if (out)
        *out = (sim->last_rc == EFS_OK) ? sim->last_ino : 0;
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
    struct sim_ev e;
    int hit, rc;

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
    memset(&e, 0, sizeof(e));
    e.kind = EV_CREATE;
    e.client = (uint8_t)client;
    e.parent = parent;
    e.mode = mode;
    strncpy(e.name, name, EFS_MAX_NAME - 1);
    e.has_op = 1;
    e.op = *op;
    sim->last_rc = EFS_OK;
    sim->last_ino = 0;
    rc = schedule(sim, &e);
    if (rc != EFS_OK)
        return rc;
    rc = maybe_drain(sim);
    if (rc != EFS_OK)
        return rc;
    rep.rc = sim->last_rc;
    rep.ino = (sim->last_rc == EFS_OK) ? sim->last_ino : 0;
    if (out)
        *out = rep.ino;
    efs_opid_complete(&sim->cli[client].win, op, &rep);
    return sim->last_rc;
}

int efs_sim_opid_forget(struct efs_sim *sim, int client)
{
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t epoch;

    if (!sim || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    memcpy(uuid, sim->cli[client].win.client_uuid, EFS_OPID_UUID_LEN);
    epoch = sim->cli[client].win.session_epoch;
    efs_opid_window_init(&sim->cli[client].win, uuid, epoch);
    return EFS_OK;
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
    sim->last_ino = 0;
    rc = schedule(sim, &e);
    if (rc != EFS_OK)
        return rc;
    rc = maybe_drain(sim);
    if (rc != EFS_OK)
        return rc;
    if (out)
        *out = (sim->last_rc == EFS_OK) ? sim->last_ino : 0;
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

int efs_sim_publish(struct efs_sim *sim, int client, efs_ino_t ino,
                    uint32_t chunk_index, uint64_t new_size)
{
    struct sim_ev e;
    int rc;

    if (!sim || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    memset(&e, 0, sizeof(e));
    e.kind = EV_PUBLISH;
    e.client = (uint8_t)client;
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
    struct efs_meta_chunk ch;
    uint8_t *block = NULL;
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    int present[EFS_NUM_FRAGMENTS];
    size_t fl = (size_t)EFS_SIM_CHUNK / 2;
    uint8_t *full;
    int i, a = -1, b = -1, miss = -1, n = 0;
    int rc;

    if (!sim || !out || chunk_len > EFS_SIM_CHUNK)
        return EFS_ERR_INVAL;
    if (sim_raft_get_chunk(sim, ino, chunk_index, &ch) != EFS_OK)
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
    s->alive = 0;
    sim_raft_halt(sim, server);
    hist(sim, 0x10, (uint64_t)server, 0);
    return EFS_OK;
}

int efs_sim_restart(struct efs_sim *sim, int server)
{
    struct sim_server *s;
    int rc;

    if (!sim || server < 0 || server >= sim->nservers)
        return EFS_ERR_INVAL;
    s = &sim->srv[server];
    s->alive = 1;
    s->partitioned = 0;
    hist(sim, 0x11, (uint64_t)server, 0);
    rc = sim_raft_restart(sim, server);
    if (rc != EFS_OK)
        return rc;
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
    uint64_t i, n;

    if (!sim)
        return EFS_ERR_INVAL;
    sim->now += delta;
    n = delta > 32 ? 32 : delta;
    for (i = 0; i < n; i++) {
        if (sim_raft_tick_reachable(sim) != EFS_OK)
            break;
    }
    hist(sim, 0x14, delta, 0);
    return EFS_OK;
}
