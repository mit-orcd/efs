#include "efs/raft.h"
#include <stdlib.h>
#include <string.h>

struct efs_raft {
    int id;
    int n;
    int role;
    int leader;
    uint64_t current_term;
    int32_t voted_for;
    uint64_t commit_index;
    uint64_t last_applied;
    uint64_t snap_idx;
    uint64_t snap_term;
    uint64_t next_index[EFS_RAFT_MAX_PEERS];
    uint64_t match_index[EFS_RAFT_MAX_PEERS];
    int votes;
    uint32_t election_elapsed;
    uint32_t election_ticks;
    uint32_t hb_elapsed;
    uint32_t heartbeat_ticks;
    uint64_t read_index;
    unsigned read_acks;
    int read_in_flight;
    struct efs_raft_store *store;
    void *store_ctx;
    efs_raft_send_fn send;
    void *net;
    efs_raft_apply_fn apply;
    void *app;
};

static int quorum(const struct efs_raft *r)
{
    return r->n / 2 + 1;
}

static int save_hard(struct efs_raft *r)
{
    return r->store->save_hard(r->store_ctx, r->current_term, r->voted_for);
}

static int last_log(struct efs_raft *r, uint64_t *index, uint64_t *term)
{
    return r->store->last(r->store_ctx, index, term);
}

static int log_term(struct efs_raft *r, uint64_t index, uint64_t *term)
{
    uint32_t clen = 0;
    int rc;

    if (index == 0) {
        *term = 0;
        return EFS_OK;
    }
    rc = r->store->get(r->store_ctx, index, term, NULL, &clen);
    if (rc == EFS_ERR_INVAL)
        return EFS_OK; /* term filled; buffer was a size probe */
    return rc;
}

static int send_msg(struct efs_raft *r, struct efs_raft_msg *m)
{
    if (!r->send)
        return EFS_ERR_INVAL;
    m->from = r->id;
    m->term = r->current_term;
    return r->send(r->net, m);
}

static int maybe_step_down(struct efs_raft *r, uint64_t term)
{
    if (term <= r->current_term)
        return 0;
    r->current_term = term;
    r->voted_for = -1;
    r->role = EFS_RAFT_FOLLOWER;
    r->leader = -1;
    r->votes = 0;
    r->read_in_flight = 0;
    r->election_elapsed = 0;
    save_hard(r);
    return 1;
}

static int apply_committed(struct efs_raft *r)
{
    while (r->last_applied < r->commit_index) {
        uint64_t idx = r->last_applied + 1;
        uint64_t term = 0;
        uint32_t clen = 0;
        uint8_t *cmd = NULL;
        int rc;

        if (idx <= r->snap_idx) {
            r->last_applied = r->snap_idx;
            continue;
        }
        rc = r->store->get(r->store_ctx, idx, &term, NULL, &clen);
        if (rc == EFS_ERR_INVAL) {
            cmd = clen ? malloc(clen) : NULL;
            if (clen && !cmd)
                return EFS_ERR_NOMEM;
            rc = r->store->get(r->store_ctx, idx, &term, cmd, &clen);
        }
        if (rc != EFS_OK) {
            free(cmd);
            return rc;
        }
        if (r->apply && clen > 0) {
            rc = r->apply(r->app, idx, term, cmd, clen);
            if (rc != EFS_OK) {
                free(cmd);
                return rc;
            }
        }
        free(cmd);
        r->last_applied = idx;
    }
    if (r->read_in_flight && r->last_applied >= r->read_index) {
        int bits = 0, i;
        for (i = 0; i < r->n; i++) {
            if (r->read_acks & (1u << i))
                bits++;
        }
        if (bits >= quorum(r))
            r->read_in_flight = 2; /* ready */
    }
    return EFS_OK;
}

static int append_local(struct efs_raft *r, uint64_t term, const uint8_t *cmd,
                        uint32_t clen, uint64_t *index_out)
{
    uint64_t last_i = 0, last_t = 0;
    uint64_t idx;
    int rc;

    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    idx = last_i + 1;
    rc = r->store->append(r->store_ctx, idx, term, cmd, clen);
    if (rc != EFS_OK)
        return rc;
    r->match_index[r->id] = idx;
    if (index_out)
        *index_out = idx;
    return EFS_OK;
}

static int send_ae(struct efs_raft *r, int to)
{
    struct efs_raft_msg m;
    uint64_t last_i = 0, last_t = 0, prev_t = 0;
    uint64_t ni;
    uint32_t clen = 0;
    uint8_t small[256];
    uint8_t *cmdbuf = small;
    int need_free = 0;
    int rc;

    memset(&m, 0, sizeof(m));
    m.type = EFS_RAFT_MSG_AE_REQ;
    m.to = to;
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    ni = r->next_index[to];
    if (ni == 0)
        ni = 1;
    m.prev_index = ni - 1;
    rc = log_term(r, m.prev_index, &prev_t);
    if (rc != EFS_OK && m.prev_index != 0)
        return rc;
    m.prev_term = prev_t;
    m.leader_commit = r->commit_index;
    if (ni <= last_i) {
        uint64_t eterm = 0;
        clen = sizeof(small);
        rc = r->store->get(r->store_ctx, ni, &eterm, small, &clen);
        if (rc == EFS_ERR_INVAL) {
            cmdbuf = malloc(clen);
            if (!cmdbuf)
                return EFS_ERR_NOMEM;
            need_free = 1;
            rc = r->store->get(r->store_ctx, ni, &eterm, cmdbuf, &clen);
        }
        if (rc != EFS_OK) {
            if (need_free)
                free(cmdbuf);
            return rc;
        }
        m.nentries = 1;
        m.entries[0].term = eterm;
        m.entries[0].clen = clen;
        m.entries[0].cmd = cmdbuf;
    }
    rc = send_msg(r, &m);
    if (need_free)
        free(cmdbuf);
    return rc;
}

static int broadcast_ae(struct efs_raft *r)
{
    int i, rc = EFS_OK;
    for (i = 0; i < r->n; i++) {
        if (i == r->id)
            continue;
        rc = send_ae(r, i);
        if (rc != EFS_OK)
            return rc;
    }
    r->hb_elapsed = 0;
    return EFS_OK;
}

static int try_commit(struct efs_raft *r)
{
    uint64_t last_i = 0, last_t = 0, n;
    int rc;

    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    for (n = last_i; n > r->commit_index; n--) {
        uint64_t t = 0;
        int c = 0, i;
        if (log_term(r, n, &t) != EFS_OK)
            break;
        if (t != r->current_term)
            break;
        for (i = 0; i < r->n; i++) {
            if (r->match_index[i] >= n)
                c++;
        }
        if (c >= quorum(r)) {
            r->commit_index = n;
            break;
        }
    }
    return apply_committed(r);
}

static int become_leader(struct efs_raft *r)
{
    int i;
    uint64_t last_i = 0, last_t = 0;
    int rc;

    r->role = EFS_RAFT_LEADER;
    r->leader = r->id;
    r->election_elapsed = 0;
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    for (i = 0; i < r->n; i++) {
        r->next_index[i] = last_i + 1;
        r->match_index[i] = (i == r->id) ? last_i : 0;
    }
    /* No-op in the current term so ReadIndex / commit can advance (Raft). */
    rc = append_local(r, r->current_term, NULL, 0, NULL);
    if (rc != EFS_OK)
        return rc;
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    r->match_index[r->id] = last_i;
    if (r->n == 1) {
        r->commit_index = last_i;
        apply_committed(r);
    }
    return broadcast_ae(r);
}

static int start_election(struct efs_raft *r)
{
    struct efs_raft_msg m;
    uint64_t last_i = 0, last_t = 0;
    int i, rc;

    r->current_term++;
    r->voted_for = (int32_t)r->id;
    r->role = EFS_RAFT_CANDIDATE;
    r->leader = -1;
    r->votes = 1;
    r->election_elapsed = 0;
    rc = save_hard(r);
    if (rc != EFS_OK)
        return rc;
    if (r->n == 1)
        return become_leader(r);
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    memset(&m, 0, sizeof(m));
    m.type = EFS_RAFT_MSG_VOTE_REQ;
    m.last_log_index = last_i;
    m.last_log_term = last_t;
    for (i = 0; i < r->n; i++) {
        if (i == r->id)
            continue;
        m.to = i;
        rc = send_msg(r, &m);
        if (rc != EFS_OK)
            return rc;
    }
    return EFS_OK;
}

static int on_vote_req(struct efs_raft *r, const struct efs_raft_msg *in)
{
    struct efs_raft_msg m;
    uint64_t last_i = 0, last_t = 0;
    int grant = 0;
    int rc;

    maybe_step_down(r, in->term);
    memset(&m, 0, sizeof(m));
    m.type = EFS_RAFT_MSG_VOTE_REP;
    m.to = in->from;
    if (in->term < r->current_term) {
        m.vote_granted = 0;
        return send_msg(r, &m);
    }
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    if ((r->voted_for == -1 || r->voted_for == in->from) &&
        (in->last_log_term > last_t ||
         (in->last_log_term == last_t && in->last_log_index >= last_i))) {
        grant = 1;
        r->voted_for = in->from;
        r->election_elapsed = 0;
        rc = save_hard(r);
        if (rc != EFS_OK)
            return rc;
    }
    m.vote_granted = grant;
    return send_msg(r, &m);
}

static int on_vote_rep(struct efs_raft *r, const struct efs_raft_msg *in)
{
    maybe_step_down(r, in->term);
    if (r->role != EFS_RAFT_CANDIDATE || in->term != r->current_term)
        return EFS_OK;
    if (in->vote_granted) {
        r->votes++;
        if (r->votes >= quorum(r))
            return become_leader(r);
    }
    return EFS_OK;
}

static int on_ae_req(struct efs_raft *r, const struct efs_raft_msg *in)
{
    struct efs_raft_msg m;
    uint64_t last_i = 0, last_t = 0, pt = 0;
    int rc;

    maybe_step_down(r, in->term);
    memset(&m, 0, sizeof(m));
    m.type = EFS_RAFT_MSG_AE_REP;
    m.to = in->from;
    if (in->term < r->current_term) {
        m.success = 0;
        return send_msg(r, &m);
    }
    r->role = EFS_RAFT_FOLLOWER;
    r->leader = in->from;
    r->election_elapsed = 0;
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    if (in->prev_index > last_i) {
        m.success = 0;
        m.match_index = last_i;
        return send_msg(r, &m);
    }
    rc = log_term(r, in->prev_index, &pt);
    if ((in->prev_index > 0 && rc != EFS_OK) || pt != in->prev_term) {
        if (in->prev_index > r->snap_idx)
            r->store->truncate_from(r->store_ctx, in->prev_index);
        m.success = 0;
        last_log(r, &last_i, &last_t);
        m.match_index = last_i;
        return send_msg(r, &m);
    }
    if (in->nentries) {
        uint64_t idx = in->prev_index + 1;
        uint64_t et = 0;
        rc = log_term(r, idx, &et);
        if (rc == EFS_OK && et != in->entries[0].term)
            r->store->truncate_from(r->store_ctx, idx);
        rc = r->store->append(r->store_ctx, idx, in->entries[0].term,
                              in->entries[0].cmd, in->entries[0].clen);
        if (rc != EFS_OK)
            return rc;
        last_log(r, &last_i, &last_t);
    }
    if (in->leader_commit > r->commit_index) {
        uint64_t cap = last_i;
        r->commit_index = in->leader_commit < cap ? in->leader_commit : cap;
        apply_committed(r);
    }
    last_log(r, &last_i, &last_t);
    m.success = 1;
    m.match_index = last_i;
    return send_msg(r, &m);
}

static int on_ae_rep(struct efs_raft *r, const struct efs_raft_msg *in)
{
    maybe_step_down(r, in->term);
    if (r->role != EFS_RAFT_LEADER || in->term != r->current_term)
        return EFS_OK;
    if (in->success) {
        if (in->match_index > r->match_index[in->from])
            r->match_index[in->from] = in->match_index;
        r->next_index[in->from] = r->match_index[in->from] + 1;
        if (r->read_in_flight == 1)
            r->read_acks |= 1u << in->from;
        try_commit(r);
        apply_committed(r);
        if (r->next_index[in->from] <= r->match_index[r->id])
            return send_ae(r, in->from);
        return EFS_OK;
    }
    if (r->next_index[in->from] > 1)
        r->next_index[in->from]--;
    return send_ae(r, in->from);
}

struct efs_raft *efs_raft_new(const struct efs_raft_cfg *cfg)
{
    struct efs_raft *r;
    int rc;

    if (!cfg || !cfg->store || cfg->n < 1 || cfg->n > EFS_RAFT_MAX_PEERS)
        return NULL;
    if (cfg->id < 0 || cfg->id >= cfg->n)
        return NULL;
    if (!cfg->store->save_hard || !cfg->send)
        return NULL;
    r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->id = cfg->id;
    r->n = cfg->n;
    r->role = EFS_RAFT_FOLLOWER;
    r->leader = -1;
    r->voted_for = -1;
    r->election_ticks = cfg->election_ticks ? cfg->election_ticks : 10;
    r->heartbeat_ticks = cfg->heartbeat_ticks ? cfg->heartbeat_ticks : 1;
    r->store = cfg->store;
    r->store_ctx = cfg->store_ctx ? cfg->store_ctx : cfg->store;
    r->send = cfg->send;
    r->net = cfg->net;
    r->apply = cfg->apply;
    r->app = cfg->app;
    rc = r->store->load_hard(r->store_ctx, &r->current_term, &r->voted_for);
    if (rc != EFS_OK) {
        free(r);
        return NULL;
    }
    if (r->store->load_snap)
        r->store->load_snap(r->store_ctx, &r->snap_idx, &r->snap_term);
    r->last_applied = r->snap_idx;
    r->commit_index = r->snap_idx;
    return r;
}

void efs_raft_free(struct efs_raft *r)
{
    free(r);
}

int efs_raft_tick(struct efs_raft *r)
{
    if (!r)
        return EFS_ERR_INVAL;
    if (r->role == EFS_RAFT_LEADER) {
        r->hb_elapsed++;
        if (r->hb_elapsed >= r->heartbeat_ticks)
            return broadcast_ae(r);
        return EFS_OK;
    }
    r->election_elapsed++;
    if (r->election_elapsed >= r->election_ticks)
        return start_election(r);
    return EFS_OK;
}

int efs_raft_recv(struct efs_raft *r, const struct efs_raft_msg *msg)
{
    if (!r || !msg)
        return EFS_ERR_INVAL;
    if (msg->to != r->id)
        return EFS_ERR_INVAL;
    switch (msg->type) {
    case EFS_RAFT_MSG_VOTE_REQ:
        return on_vote_req(r, msg);
    case EFS_RAFT_MSG_VOTE_REP:
        return on_vote_rep(r, msg);
    case EFS_RAFT_MSG_AE_REQ:
        return on_ae_req(r, msg);
    case EFS_RAFT_MSG_AE_REP:
        return on_ae_rep(r, msg);
    default:
        return EFS_ERR_INVAL;
    }
}

int efs_raft_propose(struct efs_raft *r, const uint8_t *cmd, uint32_t clen,
                     uint64_t *index_out)
{
    int rc;

    if (!r)
        return EFS_ERR_INVAL;
    if (r->role != EFS_RAFT_LEADER)
        return EFS_ERR_NOT_PRIMARY;
    rc = append_local(r, r->current_term, cmd, clen, index_out);
    if (rc != EFS_OK)
        return rc;
    if (r->n == 1) {
        uint64_t last_i = 0, last_t = 0;
        last_log(r, &last_i, &last_t);
        r->commit_index = last_i;
        return apply_committed(r);
    }
    return broadcast_ae(r);
}

int efs_raft_snapshot(struct efs_raft *r)
{
    uint64_t t = 0;
    int rc;

    if (!r || r->last_applied == 0)
        return EFS_OK;
    if (r->last_applied <= r->snap_idx)
        return EFS_OK;
    rc = log_term(r, r->last_applied, &t);
    if (rc != EFS_OK)
        return rc;
    rc = r->store->save_snap(r->store_ctx, r->last_applied, t);
    if (rc != EFS_OK)
        return rc;
    r->snap_idx = r->last_applied;
    r->snap_term = t;
    return EFS_OK;
}

int efs_raft_role(const struct efs_raft *r)
{
    return r ? r->role : -1;
}

int efs_raft_id(const struct efs_raft *r)
{
    return r ? r->id : -1;
}

uint64_t efs_raft_term(const struct efs_raft *r)
{
    return r ? r->current_term : 0;
}

uint64_t efs_raft_commit(const struct efs_raft *r)
{
    return r ? r->commit_index : 0;
}

uint64_t efs_raft_applied(const struct efs_raft *r)
{
    return r ? r->last_applied : 0;
}

int efs_raft_leader(const struct efs_raft *r)
{
    return r ? r->leader : -1;
}

int efs_raft_read_begin(struct efs_raft *r)
{
    if (!r)
        return EFS_ERR_INVAL;
    if (r->role != EFS_RAFT_LEADER)
        return EFS_ERR_NOT_PRIMARY;
    r->read_index = r->commit_index;
    r->read_acks = 1u << r->id;
    r->read_in_flight = 1;
    if (r->n == 1) {
        apply_committed(r);
        if (r->last_applied >= r->read_index)
            r->read_in_flight = 2;
        return EFS_OK;
    }
    return broadcast_ae(r);
}

int efs_raft_read_ready(const struct efs_raft *r)
{
    if (!r)
        return 0;
    return r->read_in_flight == 2 && r->last_applied >= r->read_index;
}
