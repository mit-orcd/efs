#include "efs/raft.h"
#include <stdlib.h>
#include <string.h>

struct log_ent {
    uint64_t term;
    uint32_t clen;
    uint8_t *cmd;
};

struct raft_mem_box {
    struct efs_raft_store ops;
    uint64_t term;
    int32_t voted_for;
    uint64_t snap_idx;
    uint64_t snap_term;
    uint32_t cfg_old;
    uint32_t cfg_new;
    int have_cfg;
    struct log_ent *log;
    uint32_t n;
    uint32_t cap;
};

static struct raft_mem_box *box_of(void *ctx)
{
    return ctx;
}

static int mem_save_hard(void *ctx, uint64_t current_term, int32_t voted_for)
{
    struct raft_mem_box *m = box_of(ctx);
    if (!m)
        return EFS_ERR_INVAL;
    m->term = current_term;
    m->voted_for = voted_for;
    return EFS_OK;
}

static int mem_load_hard(void *ctx, uint64_t *current_term, int32_t *voted_for)
{
    struct raft_mem_box *m = box_of(ctx);
    if (!m || !current_term || !voted_for)
        return EFS_ERR_INVAL;
    *current_term = m->term;
    *voted_for = m->voted_for;
    return EFS_OK;
}

static int grow(struct raft_mem_box *m)
{
    uint32_t cap = m->cap ? m->cap * 2 : 16;
    struct log_ent *p = realloc(m->log, (size_t)cap * sizeof(*p));
    if (!p)
        return EFS_ERR_NOMEM;
    m->log = p;
    m->cap = cap;
    return EFS_OK;
}

static int mem_append(void *ctx, uint64_t index, uint64_t term,
                      const uint8_t *cmd, uint32_t clen)
{
    struct raft_mem_box *m = box_of(ctx);
    uint64_t first;
    uint32_t off;
    uint8_t *copy = NULL;

    if (!m || index == 0)
        return EFS_ERR_INVAL;
    first = m->snap_idx + 1;
    if (index < first)
        return EFS_ERR_INVAL;
    off = (uint32_t)(index - first);
    if (off > m->n)
        return EFS_ERR_INVAL;
    if (clen > 0) {
        if (!cmd)
            return EFS_ERR_INVAL;
        copy = malloc(clen);
        if (!copy)
            return EFS_ERR_NOMEM;
        memcpy(copy, cmd, clen);
    }
    if (off < m->n) {
        free(m->log[off].cmd);
        m->log[off].term = term;
        m->log[off].clen = clen;
        m->log[off].cmd = copy;
        return EFS_OK;
    }
    if (m->n == m->cap && grow(m) != EFS_OK) {
        free(copy);
        return EFS_ERR_NOMEM;
    }
    m->log[m->n].term = term;
    m->log[m->n].clen = clen;
    m->log[m->n].cmd = copy;
    m->n++;
    return EFS_OK;
}

static int mem_truncate_from(void *ctx, uint64_t index)
{
    struct raft_mem_box *m = box_of(ctx);
    uint64_t first;
    uint32_t off, i;

    if (!m)
        return EFS_ERR_INVAL;
    first = m->snap_idx + 1;
    if (index <= m->snap_idx)
        return EFS_ERR_INVAL;
    off = (uint32_t)(index - first);
    if (off >= m->n)
        return EFS_OK;
    for (i = off; i < m->n; i++)
        free(m->log[i].cmd);
    m->n = off;
    return EFS_OK;
}

static int mem_get(void *ctx, uint64_t index, uint64_t *term, uint8_t *cmd,
                   uint32_t *clen)
{
    struct raft_mem_box *m = box_of(ctx);
    uint64_t first;
    uint32_t off, need;

    if (!m || !term || !clen || index == 0)
        return EFS_ERR_INVAL;
    if (index == m->snap_idx && m->snap_idx > 0) {
        *term = m->snap_term;
        *clen = 0;
        return EFS_OK;
    }
    first = m->snap_idx + 1;
    if (index < first)
        return EFS_ERR_NOT_FOUND;
    off = (uint32_t)(index - first);
    if (off >= m->n)
        return EFS_ERR_NOT_FOUND;
    need = m->log[off].clen;
    *term = m->log[off].term;
    if (*clen < need) {
        *clen = need;
        return EFS_ERR_INVAL;
    }
    if (need > 0) {
        if (!cmd)
            return EFS_ERR_INVAL;
        memcpy(cmd, m->log[off].cmd, need);
    }
    *clen = need;
    return EFS_OK;
}

static int mem_last(void *ctx, uint64_t *index, uint64_t *term)
{
    struct raft_mem_box *m = box_of(ctx);

    if (!m || !index || !term)
        return EFS_ERR_INVAL;
    if (m->n == 0) {
        *index = m->snap_idx;
        *term = m->snap_term;
        return EFS_OK;
    }
    *index = m->snap_idx + m->n;
    *term = m->log[m->n - 1].term;
    return EFS_OK;
}

static int mem_save_snap(void *ctx, uint64_t last_index, uint64_t last_term)
{
    struct raft_mem_box *m = box_of(ctx);
    uint64_t first;
    uint32_t drop, i;

    if (!m || last_index < m->snap_idx)
        return EFS_ERR_INVAL;
    first = m->snap_idx + 1;
    if (last_index >= first) {
        drop = (uint32_t)(last_index - first + 1);
        if (drop > m->n)
            return EFS_ERR_INVAL;
        for (i = 0; i < drop; i++)
            free(m->log[i].cmd);
        memmove(m->log, m->log + drop, (size_t)(m->n - drop) * sizeof(*m->log));
        m->n -= drop;
    }
    m->snap_idx = last_index;
    m->snap_term = last_term;
    return EFS_OK;
}

static int mem_load_snap(void *ctx, uint64_t *last_index, uint64_t *last_term)
{
    struct raft_mem_box *m = box_of(ctx);
    if (!m || !last_index || !last_term)
        return EFS_ERR_INVAL;
    *last_index = m->snap_idx;
    *last_term = m->snap_term;
    return EFS_OK;
}

static int mem_save_cfg(void *ctx, uint32_t cfg_old, uint32_t cfg_new)
{
    struct raft_mem_box *m = box_of(ctx);
    if (!m)
        return EFS_ERR_INVAL;
    m->cfg_old = cfg_old;
    m->cfg_new = cfg_new;
    m->have_cfg = 1;
    return EFS_OK;
}

static int mem_load_cfg(void *ctx, uint32_t *cfg_old, uint32_t *cfg_new)
{
    struct raft_mem_box *m = box_of(ctx);
    if (!m || !cfg_old || !cfg_new)
        return EFS_ERR_INVAL;
    if (!m->have_cfg)
        return EFS_ERR_NOT_FOUND;
    *cfg_old = m->cfg_old;
    *cfg_new = m->cfg_new;
    return EFS_OK;
}

static void mem_destroy(void *ctx)
{
    struct raft_mem_box *m = box_of(ctx);
    uint32_t i;

    if (!m)
        return;
    for (i = 0; i < m->n; i++)
        free(m->log[i].cmd);
    free(m->log);
    free(m);
}

static const struct efs_raft_store mem_ops = {
    .save_hard = mem_save_hard,
    .load_hard = mem_load_hard,
    .append = mem_append,
    .truncate_from = mem_truncate_from,
    .get = mem_get,
    .last = mem_last,
    .save_snap = mem_save_snap,
    .load_snap = mem_load_snap,
    .save_cfg = mem_save_cfg,
    .load_cfg = mem_load_cfg,
    .destroy = mem_destroy,
};

struct efs_raft_store *efs_raft_mem_create(void)
{
    struct raft_mem_box *m = calloc(1, sizeof(*m));

    if (!m)
        return NULL;
    m->ops = mem_ops;
    m->voted_for = -1;
    return &m->ops;
}

void efs_raft_mem_free(struct efs_raft_store *st)
{
    if (!st)
        return;
    mem_destroy(st);
}
