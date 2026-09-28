/* Durable efs_raft_store: per-group state in RAM, authority on disk.
 *
 * Every mutating callback logs the record FIRST and installs it into RAM
 * second, and the install is made infallible by reserving memory beforehand.
 * That ordering is deliberate: the log is the authority, so RAM may never
 * hold a mutation the log does not. The reverse — RAM ahead of the log —
 * would let last() report an entry a crash would lose, which is exactly how
 * a Raft store un-commits an acknowledged write.
 *
 * Semantics match raft_mem.c exactly; see raft.h for the contract. */

#include "raft_disk_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REC_OVERHEAD 13u /* crc + paylen + type + group */

static struct raft_disk_group *group_of(void *ctx)
{
    return ctx;
}

/* --- RAM state (also the replay target) ------------------------------ */

int raft_group_reserve(struct raft_disk_group *g)
{
    uint32_t cap;
    struct raft_log_ent *p;

    if (g->n < g->cap)
        return EFS_OK;
    cap = g->cap ? g->cap * 2 : 16;
    p = realloc(g->log, (size_t)cap * sizeof(*p));
    if (!p)
        return EFS_ERR_NOMEM;
    g->log = p;
    g->cap = cap;
    return EFS_OK;
}

void raft_group_install(struct raft_disk_group *g, uint64_t index,
                        uint64_t term, uint8_t *cmd, uint32_t clen)
{
    uint32_t off = (uint32_t)(index - (g->snap_idx + 1));

    if (off < g->n) {
        free(g->log[off].cmd);
    } else {
        off = g->n;
        g->n++;
    }
    g->log[off].term = term;
    g->log[off].end_off = 0;
    g->log[off].clen = clen;
    g->log[off].cmd = cmd;
}

/* True when index is the next slot or an existing one; a gap is invalid. */
static int index_ok(const struct raft_disk_group *g, uint64_t index)
{
    uint64_t first = g->snap_idx + 1;

    if (index == 0 || index < first)
        return 0;
    return index - first <= (uint64_t)g->n;
}

int raft_group_put_entry(struct raft_disk_group *g, uint64_t index,
                         uint64_t term, const uint8_t *cmd, uint32_t clen)
{
    uint8_t *copy = NULL;

    if (!index_ok(g, index))
        return EFS_ERR_INVAL;
    if (clen > 0) {
        if (!cmd)
            return EFS_ERR_INVAL;
        copy = malloc(clen);
        if (!copy)
            return EFS_ERR_NOMEM;
        memcpy(copy, cmd, clen);
    }
    if (raft_group_reserve(g) != EFS_OK) {
        free(copy);
        return EFS_ERR_NOMEM;
    }
    raft_group_install(g, index, term, copy, clen);
    return EFS_OK;
}

int raft_group_truncate(struct raft_disk_group *g, uint64_t index)
{
    uint32_t off, i;

    if (index <= g->snap_idx)
        return EFS_ERR_INVAL;
    off = (uint32_t)(index - (g->snap_idx + 1));
    if (off >= g->n)
        return EFS_OK;
    for (i = off; i < g->n; i++)
        free(g->log[i].cmd);
    g->n = off;
    return EFS_OK;
}

int raft_group_snap(struct raft_disk_group *g, uint64_t last_index,
                    uint64_t last_term)
{
    uint64_t first = g->snap_idx + 1;
    uint32_t drop, i;

    if (last_index < g->snap_idx)
        return EFS_ERR_INVAL;
    if (last_index >= first) {
        drop = (uint32_t)(last_index - first + 1);
        /* Clamped, not rejected: a rotated log writes the snapshot record
         * without the entries it already compacted away, so replay
         * legitimately sees a snapshot ahead of an empty log. Same
         * clamp as raft_mem (InstallSnapshot onto a fresh replica). */
        if (drop > g->n)
            drop = g->n;
        for (i = 0; i < drop; i++)
            free(g->log[i].cmd);
        memmove(g->log, g->log + drop, (size_t)(g->n - drop) * sizeof(*g->log));
        g->n -= drop;
    }
    g->snap_idx = last_index;
    g->snap_term = last_term;
    return EFS_OK;
}

uint64_t raft_group_live_bytes(const struct raft_disk_group *g)
{
    uint64_t n = 0;
    uint32_t i;

    if (g->term || g->voted_for != -1)
        n += REC_OVERHEAD + 12;
    if (g->snap_idx)
        n += REC_OVERHEAD + 16;
    if (g->have_cfg)
        n += REC_OVERHEAD + 8;
    for (i = 0; i < g->n; i++)
        n += REC_OVERHEAD + 20 + g->log[i].clen;
    return n;
}

/* --- store callbacks ------------------------------------------------- */

static int disk_save_hard(void *ctx, uint64_t current_term, int32_t voted_for)
{
    struct raft_disk_group *g = group_of(ctx);
    int rc;

    if (!g)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&g->d->mu);
    rc = raft_log_append(g->d, RAFT_REC_HARD, g, current_term,
                         (uint32_t)voted_for, NULL, 0);
    if (rc == EFS_OK) {
        g->term = current_term;
        g->voted_for = voted_for;
        raft_log_maybe_rotate_locked(g->d);
    }
    pthread_mutex_unlock(&g->d->mu);
    return rc;
}

static int disk_load_hard(void *ctx, uint64_t *current_term, int32_t *voted_for)
{
    struct raft_disk_group *g = group_of(ctx);

    if (!g || !current_term || !voted_for)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&g->d->mu);
    *current_term = g->term;
    *voted_for = g->voted_for;
    pthread_mutex_unlock(&g->d->mu);
    return EFS_OK;
}

static int disk_append(void *ctx, uint64_t index, uint64_t term,
                       const uint8_t *cmd, uint32_t clen)
{
    struct raft_disk_group *g = group_of(ctx);
    uint8_t *copy = NULL;
    int rc;

    if (!g)
        return EFS_ERR_INVAL;
    if (clen > RAFT_CMD_MAX || (clen > 0 && !cmd))
        return EFS_ERR_INVAL;
    if (clen > 0) {
        copy = malloc(clen);
        if (!copy)
            return EFS_ERR_NOMEM;
        memcpy(copy, cmd, clen);
    }
    pthread_mutex_lock(&g->d->mu);
    if (!index_ok(g, index)) {
        rc = EFS_ERR_INVAL;
        goto out;
    }
    rc = raft_group_reserve(g);
    if (rc != EFS_OK)
        goto out;
    rc = raft_log_append(g->d, RAFT_REC_ENTRY, g, index, term, cmd, clen);
    if (rc != EFS_OK)
        goto out;
    raft_group_install(g, index, term, copy, clen);
    {
        uint32_t off = (uint32_t)(index - (g->snap_idx + 1));

        if (off < g->n)
            g->log[off].end_off = g->d->bytes;
    }
    copy = NULL;
    raft_log_maybe_rotate_locked(g->d);
out:
    pthread_mutex_unlock(&g->d->mu);
    free(copy);
    return rc;
}

static int disk_truncate_from(void *ctx, uint64_t index)
{
    struct raft_disk_group *g = group_of(ctx);
    int rc;

    if (!g)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&g->d->mu);
    if (index <= g->snap_idx) {
        pthread_mutex_unlock(&g->d->mu);
        return EFS_ERR_INVAL;
    }
    /* Beyond the end is a no-op, and logging one would only grow the file. */
    if (index - (g->snap_idx + 1) >= (uint64_t)g->n) {
        pthread_mutex_unlock(&g->d->mu);
        return EFS_OK;
    }
    rc = raft_log_append(g->d, RAFT_REC_TRUNC, g, index, 0, NULL, 0);
    if (rc == EFS_OK) {
        rc = raft_group_truncate(g, index);
        raft_log_maybe_rotate_locked(g->d);
    }
    pthread_mutex_unlock(&g->d->mu);
    return rc;
}

static int disk_get(void *ctx, uint64_t index, uint64_t *term, uint8_t *cmd,
                    uint32_t *clen)
{
    struct raft_disk_group *g = group_of(ctx);
    uint64_t first;
    uint32_t off, need;
    int rc = EFS_OK;

    if (!g || !term || !clen || index == 0)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&g->d->mu);
    if (index == g->snap_idx && g->snap_idx > 0) {
        *term = g->snap_term;
        *clen = 0;
        goto out;
    }
    first = g->snap_idx + 1;
    if (index < first) {
        rc = EFS_ERR_NOT_FOUND;
        goto out;
    }
    off = (uint32_t)(index - first);
    if (off >= g->n) {
        rc = EFS_ERR_NOT_FOUND;
        goto out;
    }
    need = g->log[off].clen;
    *term = g->log[off].term;
    if (*clen < need) {
        *clen = need; /* short buffer: report the size the caller needs */
        rc = EFS_ERR_INVAL;
        goto out;
    }
    if (need > 0) {
        if (!cmd) {
            rc = EFS_ERR_INVAL;
            goto out;
        }
        memcpy(cmd, g->log[off].cmd, need);
    }
    *clen = need;
out:
    pthread_mutex_unlock(&g->d->mu);
    return rc;
}

static int disk_last(void *ctx, uint64_t *index, uint64_t *term)
{
    struct raft_disk_group *g = group_of(ctx);

    if (!g || !index || !term)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&g->d->mu);
    if (g->n == 0) {
        *index = g->snap_idx;
        *term = g->snap_term;
    } else {
        *index = g->snap_idx + g->n;
        *term = g->log[g->n - 1].term;
    }
    pthread_mutex_unlock(&g->d->mu);
    return EFS_OK;
}

static int disk_save_snap(void *ctx, uint64_t last_index, uint64_t last_term)
{
    struct raft_disk_group *g = group_of(ctx);
    int rc;

    if (!g)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&g->d->mu);
    /* Only a backwards snapshot is invalid. A snapshot AHEAD of the log end
     * is the whole point of InstallSnapshot onto a behind/fresh follower:
     * raft_group_snap clamps the drop to what the log actually holds (same
     * as raft_mem), and replay of this record applies the same clamp. The
     * old "last_index - snap_idx > n" guard rejected exactly that case,
     * which wedged any follower whose log never held the snapshotted
     * prefix (on_snap_req got INVAL after a successful snap_put and
     * returned without replying — the leader retried forever). */
    if (last_index < g->snap_idx) {
        pthread_mutex_unlock(&g->d->mu);
        return EFS_ERR_INVAL;
    }
    rc = raft_log_append(g->d, RAFT_REC_SNAP, g, last_index, last_term, NULL,
                         0);
    if (rc == EFS_OK) {
        rc = raft_group_snap(g, last_index, last_term);
        /* A snapshot can drop almost the whole file. The usual check
         * waits until the file doubles, which would leave a multi-GB
         * log on disk after the prefix is dead. */
        g->d->check_at = 0;
        raft_log_maybe_rotate_locked(g->d);
    }
    pthread_mutex_unlock(&g->d->mu);
    return rc;
}

static int disk_load_snap(void *ctx, uint64_t *last_index, uint64_t *last_term)
{
    struct raft_disk_group *g = group_of(ctx);

    if (!g || !last_index || !last_term)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&g->d->mu);
    *last_index = g->snap_idx;
    *last_term = g->snap_term;
    pthread_mutex_unlock(&g->d->mu);
    return EFS_OK;
}

static int disk_save_cfg(void *ctx, uint32_t cfg_old, uint32_t cfg_new)
{
    struct raft_disk_group *g = group_of(ctx);
    int rc;

    if (!g)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&g->d->mu);
    rc = raft_log_append(g->d, RAFT_REC_CFG, g, cfg_old, cfg_new, NULL, 0);
    if (rc == EFS_OK) {
        g->cfg_old = cfg_old;
        g->cfg_new = cfg_new;
        g->have_cfg = 1;
        raft_log_maybe_rotate_locked(g->d);
    }
    pthread_mutex_unlock(&g->d->mu);
    return rc;
}

static int disk_load_cfg(void *ctx, uint32_t *cfg_old, uint32_t *cfg_new)
{
    struct raft_disk_group *g = group_of(ctx);
    int rc = EFS_OK;

    if (!g || !cfg_old || !cfg_new)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&g->d->mu);
    if (!g->have_cfg) {
        rc = EFS_ERR_NOT_FOUND;
    } else {
        *cfg_old = g->cfg_old;
        *cfg_new = g->cfg_new;
    }
    pthread_mutex_unlock(&g->d->mu);
    return rc;
}

/* The disk owns every group, so a group cannot be destroyed on its own —
 * a replica that restarts must find its log where it left it. */
static void disk_destroy(void *ctx)
{
    (void)ctx;
}

/* One fsync for a multi-entry AppendEntries. Not the shared sync_hold:
 * the pump calls this while it owns the host lock, and waiting out other
 * threads' holds would deadlock on that lock. */
static int disk_batch_begin(void *ctx)
{
    if (!group_of(ctx))
        return EFS_ERR_INVAL;
    raft_log_defer_begin();
    return EFS_OK;
}

static int disk_batch_end(void *ctx)
{
    struct raft_disk_group *g = group_of(ctx);

    if (!g)
        return EFS_ERR_INVAL;
    return raft_log_defer_end(g->d);
}

static const struct efs_raft_store disk_ops = {
    .save_hard = disk_save_hard,
    .load_hard = disk_load_hard,
    .append = disk_append,
    .truncate_from = disk_truncate_from,
    .get = disk_get,
    .last = disk_last,
    .save_snap = disk_save_snap,
    .load_snap = disk_load_snap,
    .save_cfg = disk_save_cfg,
    .load_cfg = disk_load_cfg,
    .destroy = disk_destroy,
    .batch_begin = disk_batch_begin,
    .batch_end = disk_batch_end,
};

/* --- open / close ---------------------------------------------------- */

struct raft_disk_group *raft_group_get(struct efs_raft_disk *d, uint32_t group)
{
    struct raft_disk_group *g;

    if (group >= RAFT_DISK_MAX_GROUPS)
        return NULL;
    if (d->g[group])
        return d->g[group];
    g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    g->ops = disk_ops;
    g->d = d;
    g->group = group;
    g->voted_for = -1;
    d->g[group] = g;
    return g;
}

struct efs_raft_store *efs_raft_disk_group(struct efs_raft_disk *d,
                                           uint32_t group)
{
    struct raft_disk_group *g;

    if (!d)
        return NULL;
    pthread_mutex_lock(&d->mu);
    g = raft_group_get(d, group);
    pthread_mutex_unlock(&d->mu);
    return g ? &g->ops : NULL;
}

static int mkdir_p(const char *path)
{
    char buf[RAFT_DISK_PATH_MAX];
    size_t i, n = strlen(path);

    if (n == 0 || n >= sizeof(buf))
        return EFS_ERR_INVAL;
    memcpy(buf, path, n + 1);
    for (i = 1; i <= n; i++) {
        char c = buf[i];

        if (c != '/' && c != '\0')
            continue;
        buf[i] = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST)
            return EFS_ERR_IO;
        buf[i] = c;
    }
    return EFS_OK;
}

struct efs_raft_disk *efs_raft_disk_open(const char *dir, int sync_mode)
{
    struct efs_raft_disk *d;
    char path[RAFT_DISK_PATH_MAX + 32];
    struct stat st;
    size_t dlen;

    if (!dir)
        return NULL;
    dlen = strlen(dir);
    if (dlen == 0 || dlen >= RAFT_DISK_PATH_MAX)
        return NULL;
    if (mkdir_p(dir) != EFS_OK)
        return NULL;
    d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->rec = malloc(RAFT_REC_MAX);
    if (!d->rec) {
        free(d);
        return NULL;
    }
    memcpy(d->dir, dir, dlen + 1);
    d->sync_mode = sync_mode;
    d->check_at = RAFT_ROTATE_MIN_BYTES;
    if (pthread_mutex_init(&d->mu, NULL) != 0 ||
        pthread_cond_init(&d->cv, NULL) != 0) {
        free(d->rec);
        free(d);
        return NULL;
    }
    snprintf(path, sizeof(path), "%s/raft.log", dir);
    d->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (d->fd < 0) {
        efs_raft_disk_close(d);
        return NULL;
    }
    if (fstat(d->fd, &st) == 0)
        d->bytes = (uint64_t)st.st_size;
    if (raft_log_replay(d) != EFS_OK) {
        efs_raft_disk_close(d);
        return NULL;
    }
    d->synced_bytes = d->bytes;
    return d;
}

void efs_raft_disk_close(struct efs_raft_disk *d)
{
    uint32_t i, k;

    if (!d)
        return;
    for (i = 0; i < RAFT_DISK_MAX_GROUPS; i++) {
        struct raft_disk_group *g = d->g[i];

        if (!g)
            continue;
        for (k = 0; k < g->n; k++)
            free(g->log[k].cmd);
        free(g->log);
        free(g);
    }
    if (d->fd >= 0)
        close(d->fd);
    pthread_mutex_destroy(&d->mu);
    pthread_cond_destroy(&d->cv);
    free(d->rec);
    free(d);
}

int efs_raft_disk_rotate(struct efs_raft_disk *d)
{
    int rc;

    if (!d)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&d->mu);
    rc = raft_log_rotate_locked(d);
    pthread_mutex_unlock(&d->mu);
    return rc;
}

uint64_t efs_raft_disk_bytes(const struct efs_raft_disk *d)
{
    return d ? d->bytes : 0;
}

uint64_t efs_raft_disk_synced_bytes(struct efs_raft_disk *d)
{
    uint64_t n;

    if (!d)
        return 0;
    pthread_mutex_lock(&d->mu);
    n = d->synced_bytes;
    pthread_mutex_unlock(&d->mu);
    return n;
}

uint64_t efs_raft_disk_covered_index(struct efs_raft_disk *d, uint32_t group,
                                    uint64_t synced)
{
    struct raft_disk_group *g;
    uint64_t best = 0;
    uint32_t i;

    if (!d || group >= RAFT_DISK_MAX_GROUPS)
        return 0;
    pthread_mutex_lock(&d->mu);
    g = d->g[group];
    /* Offsets grow with the file. The tail is the unsynced part, so
     * walk back to the first record the fsync already covers. */
    if (g) {
        i = g->n;
        while (i > 0) {
            uint64_t off = g->log[i - 1].end_off;

            i--;
            if (off == 0 || off <= synced) {
                best = g->snap_idx + 1 + i;
                break;
            }
        }
    }
    pthread_mutex_unlock(&d->mu);
    return best;
}
