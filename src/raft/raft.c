#include "efs/raft.h"
#include <stdlib.h>
#include <string.h>

struct efs_raft {
    int id;
    int role;
    int leader;
    uint8_t group;
    uint64_t boot_id;
    uint64_t current_term;
    int32_t voted_for;
    uint64_t commit_index;
    uint64_t last_applied;
    uint64_t snap_idx;
    uint64_t snap_term;
    uint64_t next_index[EFS_RAFT_MAX_PEERS];
    uint64_t match_index[EFS_RAFT_MAX_PEERS];
    uint64_t peer_boot[EFS_RAFT_MAX_PEERS];
    unsigned vote_bits;
    uint32_t election_elapsed;
    uint32_t election_ticks;
    uint32_t hb_elapsed;
    uint32_t heartbeat_ticks;
    uint64_t read_index;
    unsigned read_acks;
    int read_in_flight;
    uint32_t log_old;
    uint32_t log_new;
    uint32_t app_old;
    uint32_t app_new;
    uint32_t learners;
    uint64_t joint_idx;
    uint64_t cold_idx;
    struct efs_raft_store *store;
    void *store_ctx;
    efs_raft_send_fn send;
    void *net;
    efs_raft_apply_fn apply;
    void *app;
};

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int popc(uint32_t x)
{
    int n = 0;
    while (x) {
        n += (int)(x & 1u);
        x >>= 1;
    }
    return n;
}

static int majority(uint32_t set, unsigned bits)
{
    int need = popc(set) / 2 + 1;
    return popc(set & (uint32_t)bits) >= need;
}

static int quorum_ok(const struct efs_raft *r, unsigned bits)
{
    if (!majority(r->log_old, bits))
        return 0;
    if (r->log_new && !majority(r->log_new, bits))
        return 0;
    return 1;
}

static int solo(const struct efs_raft *r)
{
    return popc(r->log_old) == 1 && r->log_new == 0;
}

static int is_voter(const struct efs_raft *r, int id)
{
    uint32_t bit;

    if (id < 0 || id >= EFS_RAFT_MAX_PEERS)
        return 0;
    bit = 1u << id;
    if (r->log_new)
        return (r->log_old & bit) || (r->log_new & bit);
    return (r->log_old & bit) != 0;
}

static uint32_t peer_mask(const struct efs_raft *r)
{
    /* Include the applied config so a COLD entry still reaches members
     * who are leaving: otherwise they campaign on C_old and livelock
     * elections (I18 disruption). */
    return r->log_old | r->log_new | r->learners | r->app_old | r->app_new;
}

static int in_app_cfg(const struct efs_raft *r, int id)
{
    uint32_t bit;

    if (id < 0 || id >= EFS_RAFT_MAX_PEERS)
        return 0;
    bit = 1u << id;
    if (r->app_new)
        return (r->app_old & bit) || (r->app_new & bit);
    return (r->app_old & bit) != 0;
}

static int is_cfg_cmd(const uint8_t *cmd, uint32_t clen)
{
    if (!cmd || clen < 5)
        return 0;
    if (cmd[0] == EFS_RAFT_CMD_JOINT)
        return clen >= 9;
    if (cmd[0] == EFS_RAFT_CMD_COLD)
        return 1;
    return 0;
}

static int save_hard(struct efs_raft *r)
{
    return r->store->save_hard(r->store_ctx, r->current_term, r->voted_for);
}

static int save_cfg(struct efs_raft *r)
{
    if (!r->store->save_cfg)
        return EFS_OK;
    return r->store->save_cfg(r->store_ctx, r->app_old, r->app_new);
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
        return EFS_OK;
    return rc;
}

static void install_log_cfg(struct efs_raft *r, const uint8_t *cmd,
                            uint32_t clen, uint64_t idx)
{
    uint32_t a, b;

    if (!is_cfg_cmd(cmd, clen))
        return;
    a = rd32(cmd + 1);
    if (cmd[0] == EFS_RAFT_CMD_JOINT) {
        b = rd32(cmd + 5);
        r->log_old = a;
        r->log_new = b;
        r->joint_idx = idx;
        r->learners &= ~(a | b);
    } else {
        r->log_old = a;
        r->log_new = 0;
        r->cold_idx = idx;
        r->learners &= ~a;
    }
}

static int reload_cfg_from_log(struct efs_raft *r)
{
    uint64_t last_i = 0, last_t = 0, idx;

    r->log_old = r->app_old;
    r->log_new = r->app_new;
    r->joint_idx = 0;
    r->cold_idx = 0;
    if (last_log(r, &last_i, &last_t) != EFS_OK)
        return EFS_ERR_IO;
    for (idx = r->snap_idx + 1; idx <= last_i; idx++) {
        uint8_t buf[16];
        uint32_t clen = sizeof(buf);
        uint64_t term = 0;
        int rc = r->store->get(r->store_ctx, idx, &term, buf, &clen);
        if (rc == EFS_ERR_INVAL)
            continue;
        if (rc != EFS_OK)
            return rc;
        install_log_cfg(r, buf, clen, idx);
    }
    return EFS_OK;
}

static int send_msg(struct efs_raft *r, struct efs_raft_msg *m)
{
    if (!r->send)
        return EFS_ERR_INVAL;
    m->from = r->id;
    m->term = r->current_term;
    m->group = r->group;
    m->boot_id = r->boot_id;
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
    r->vote_bits = 0;
    r->read_in_flight = 0;
    r->election_elapsed = 0;
    save_hard(r);
    return 1;
}

static void maybe_leave(struct efs_raft *r)
{
    if (r->role == EFS_RAFT_LEADER && !in_app_cfg(r, r->id)) {
        r->role = EFS_RAFT_FOLLOWER;
        r->leader = -1;
        r->vote_bits = 0;
    }
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
        if (is_cfg_cmd(cmd, clen)) {
            if (cmd[0] == EFS_RAFT_CMD_JOINT) {
                r->app_old = rd32(cmd + 1);
                r->app_new = rd32(cmd + 5);
            } else {
                r->app_old = rd32(cmd + 1);
                r->app_new = 0;
            }
            save_cfg(r);
            maybe_leave(r);
        } else if (r->apply && clen > 0) {
            rc = r->apply(r->app, idx, term, cmd, clen);
            if (rc != EFS_OK) {
                free(cmd);
                return rc;
            }
        }
        free(cmd);
        r->last_applied = idx;
    }
    if (r->read_in_flight && r->last_applied >= r->read_index &&
        quorum_ok(r, r->read_acks))
        r->read_in_flight = 2;
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
    install_log_cfg(r, cmd, clen, idx);
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
    if (ni <= r->snap_idx) {
        /* Prefix was compacted; InstallSnapshot is not in this cut.
         * Skip this peer rather than failing the tick. */
        r->next_index[to] = r->snap_idx + 1;
        return EFS_OK;
    }
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
    uint32_t mask = peer_mask(r);

    for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
        if (i == r->id || !(mask & (1u << i)))
            continue;
        rc = send_ae(r, i);
        if (rc != EFS_OK)
            return rc;
    }
    r->hb_elapsed = 0;
    return EFS_OK;
}

static int maybe_append_cold(struct efs_raft *r)
{
    uint8_t cmd[5];
    int rc;

    if (r->role != EFS_RAFT_LEADER || !r->log_new)
        return EFS_OK;
    if (r->commit_index < r->joint_idx)
        return EFS_OK;
    if (r->cold_idx > r->joint_idx)
        return EFS_OK;
    cmd[0] = EFS_RAFT_CMD_COLD;
    wr32(cmd + 1, r->log_new);
    rc = append_local(r, r->current_term, cmd, 5, NULL);
    if (rc != EFS_OK)
        return rc;
    return broadcast_ae(r);
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
        unsigned bits = 0;
        int i;
        if (log_term(r, n, &t) != EFS_OK)
            break;
        if (t != r->current_term)
            break;
        for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
            if (r->match_index[i] >= n)
                bits |= 1u << i;
        }
        if (quorum_ok(r, bits)) {
            r->commit_index = n;
            break;
        }
    }
    rc = apply_committed(r);
    if (rc != EFS_OK)
        return rc;
    return maybe_append_cold(r);
}

static int become_leader(struct efs_raft *r)
{
    int i;
    uint64_t last_i = 0, last_t = 0;
    uint32_t mask;
    int rc;

    r->role = EFS_RAFT_LEADER;
    r->leader = r->id;
    r->election_elapsed = 0;
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    mask = peer_mask(r) | (1u << r->id);
    for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
        if (!(mask & (1u << i)))
            continue;
        r->next_index[i] = last_i + 1;
        r->match_index[i] = (i == r->id) ? last_i : 0;
    }
    rc = append_local(r, r->current_term, NULL, 0, NULL);
    if (rc != EFS_OK)
        return rc;
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    r->match_index[r->id] = last_i;
    if (solo(r)) {
        r->commit_index = last_i;
        apply_committed(r);
        return maybe_append_cold(r);
    }
    rc = maybe_append_cold(r);
    if (rc != EFS_OK)
        return rc;
    return broadcast_ae(r);
}

static int start_election(struct efs_raft *r)
{
    struct efs_raft_msg m;
    uint64_t last_i = 0, last_t = 0;
    int i, rc;

    if (!is_voter(r, r->id)) {
        r->election_elapsed = 0;
        return EFS_OK;
    }
    r->current_term++;
    r->voted_for = (int32_t)r->id;
    r->role = EFS_RAFT_CANDIDATE;
    r->leader = -1;
    r->vote_bits = 1u << r->id;
    r->election_elapsed = 0;
    rc = save_hard(r);
    if (rc != EFS_OK)
        return rc;
    if (solo(r))
        return become_leader(r);
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    memset(&m, 0, sizeof(m));
    m.type = EFS_RAFT_MSG_VOTE_REQ;
    m.last_log_index = last_i;
    m.last_log_term = last_t;
    for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
        if (i == r->id || !is_voter(r, i))
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
    if (in->term < r->current_term || !is_voter(r, r->id)) {
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
        if (in->from >= 0 && in->from < EFS_RAFT_MAX_PEERS)
            r->vote_bits |= 1u << in->from;
        if (quorum_ok(r, r->vote_bits))
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
        if (in->prev_index > r->snap_idx) {
            r->store->truncate_from(r->store_ctx, in->prev_index);
            reload_cfg_from_log(r);
        }
        m.success = 0;
        last_log(r, &last_i, &last_t);
        m.match_index = last_i;
        return send_msg(r, &m);
    }
    if (in->nentries) {
        uint64_t idx = in->prev_index + 1;
        uint64_t et = 0;
        rc = log_term(r, idx, &et);
        if (rc == EFS_OK && et != in->entries[0].term) {
            r->store->truncate_from(r->store_ctx, idx);
            reload_cfg_from_log(r);
        }
        rc = r->store->append(r->store_ctx, idx, in->entries[0].term,
                              in->entries[0].cmd, in->entries[0].clen);
        if (rc != EFS_OK)
            return rc;
        install_log_cfg(r, in->entries[0].cmd, in->entries[0].clen, idx);
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
    if (in->from < 0 || in->from >= EFS_RAFT_MAX_PEERS)
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

static int valid_voters(uint32_t v)
{
    int n, i;

    if (v == 0 || (v >> EFS_RAFT_MAX_PEERS))
        return 0;
    n = popc(v);
    if (n < 1 || (n % 2) == 0)
        return 0;
    for (i = EFS_RAFT_MAX_PEERS; i < 32; i++) {
        if (v & (1u << i))
            return 0;
    }
    return 1;
}

struct efs_raft *efs_raft_new(const struct efs_raft_cfg *cfg)
{
    struct efs_raft *r;
    uint32_t voters;
    int rc;

    if (!cfg || !cfg->store || cfg->n < 1 || cfg->n > EFS_RAFT_MAX_PEERS)
        return NULL;
    if (cfg->id < 0 || cfg->id >= EFS_RAFT_MAX_PEERS)
        return NULL;
    if (!cfg->store->save_hard || !cfg->send)
        return NULL;
    voters = cfg->voters ? cfg->voters : ((1u << cfg->n) - 1);
    if (!valid_voters(voters))
        return NULL;
    r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->id = cfg->id;
    r->group = cfg->group;
    r->boot_id = cfg->boot_id ? cfg->boot_id : 1;
    r->role = EFS_RAFT_FOLLOWER;
    r->leader = -1;
    r->voted_for = -1;
    r->log_old = r->app_old = voters;
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
    if (r->store->load_cfg) {
        uint32_t o = 0, n = 0;
        rc = r->store->load_cfg(r->store_ctx, &o, &n);
        if (rc == EFS_OK) {
            r->app_old = r->log_old = o;
            r->app_new = r->log_new = n;
        }
    }
    r->last_applied = r->snap_idx;
    r->commit_index = r->snap_idx;
    if (reload_cfg_from_log(r) != EFS_OK) {
        free(r);
        return NULL;
    }
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
    uint64_t boot;

    if (!r || !msg)
        return EFS_ERR_INVAL;
    if (msg->to != r->id)
        return EFS_ERR_INVAL;
    if (msg->from < 0 || msg->from >= EFS_RAFT_MAX_PEERS)
        return EFS_ERR_INVAL;
    boot = msg->boot_id ? msg->boot_id : 1;
    if (r->peer_boot[msg->from] && boot < r->peer_boot[msg->from])
        return EFS_OK;
    if (boot > r->peer_boot[msg->from])
        r->peer_boot[msg->from] = boot;
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
    if (is_cfg_cmd(cmd, clen))
        return EFS_ERR_INVAL;
    rc = append_local(r, r->current_term, cmd, clen, index_out);
    if (rc != EFS_OK)
        return rc;
    if (solo(r)) {
        uint64_t last_i = 0, last_t = 0;
        last_log(r, &last_i, &last_t);
        r->commit_index = last_i;
        rc = apply_committed(r);
        if (rc != EFS_OK)
            return rc;
        return maybe_append_cold(r);
    }
    return broadcast_ae(r);
}

int efs_raft_change(struct efs_raft *r, uint32_t new_voters)
{
    uint32_t add;
    uint8_t cmd[9];
    uint64_t last_i = 0, last_t = 0;
    int i, rc;

    if (!r)
        return EFS_ERR_INVAL;
    if (r->role != EFS_RAFT_LEADER)
        return EFS_ERR_NOT_PRIMARY;
    if (!valid_voters(new_voters))
        return EFS_ERR_INVAL;
    if (r->log_new)
        return EFS_ERR_BUSY;
    if (r->log_old == new_voters && r->app_old == new_voters)
        return EFS_OK;
    add = new_voters & ~r->log_old;
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
        if (!(add & (1u << i)))
            continue;
        r->learners |= 1u << i;
        if (r->next_index[i] == 0)
            r->next_index[i] = last_i + 1;
    }
    for (i = 0; i < EFS_RAFT_MAX_PEERS; i++) {
        if (!(add & (1u << i)))
            continue;
        if (r->match_index[i] < r->commit_index) {
            broadcast_ae(r);
            return EFS_ERR_BUSY;
        }
    }
    cmd[0] = EFS_RAFT_CMD_JOINT;
    wr32(cmd + 1, r->log_old);
    wr32(cmd + 5, new_voters);
    rc = append_local(r, r->current_term, cmd, 9, NULL);
    if (rc != EFS_OK)
        return rc;
    if (solo(r)) {
        last_log(r, &last_i, &last_t);
        r->commit_index = last_i;
        apply_committed(r);
        return maybe_append_cold(r);
    }
    rc = broadcast_ae(r);
    if (rc != EFS_OK)
        return rc;
    return try_commit(r);
}

int efs_raft_restore_applied(struct efs_raft *r, uint64_t idx)
{
    uint64_t last_i = 0, last_t = 0;
    int rc;

    if (!r)
        return EFS_ERR_INVAL;
    if (idx == 0)
        return EFS_OK;
    rc = last_log(r, &last_i, &last_t);
    if (rc != EFS_OK)
        return rc;
    if (idx > last_i)
        idx = last_i;
    if (idx < r->snap_idx)
        idx = r->snap_idx;
    r->last_applied = idx;
    if (r->commit_index < idx)
        r->commit_index = idx;
    return EFS_OK;
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
    return save_cfg(r);
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

uint32_t efs_raft_voters(const struct efs_raft *r)
{
    return r ? r->app_old : 0;
}

int efs_raft_joint(const struct efs_raft *r)
{
    return r && r->app_new != 0;
}

int efs_raft_learner_ready(const struct efs_raft *r, int id)
{
    if (!r || id < 0 || id >= EFS_RAFT_MAX_PEERS)
        return 0;
    if (!(r->learners & (1u << id)))
        return 0;
    return r->match_index[id] >= r->commit_index;
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
    if (solo(r)) {
        apply_committed(r);
        if (r->last_applied >= r->read_index && quorum_ok(r, r->read_acks))
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
