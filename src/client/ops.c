#include "client_internal.h"
#include "efs/protocol.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

static uint32_t data_chunk_size(void)
{
    uint32_t cs = g_client.export.chunk_size;
    return efs_chunk_size_valid(cs) ? cs : EFS_DEFAULT_CHUNK_SIZE;
}

static uint64_t now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec;
}

void efs_client_ensure_dir_locks(void)
{
    if (g_client.dir_locks_ready)
        return;
    static pthread_mutex_t init_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&init_mu);
    if (!g_client.dir_locks_ready) {
        for (int i = 0; i < EFS_DIR_LOCKS; i++)
            pthread_mutex_init(&g_client.dir_lock[i], NULL);
        pthread_mutex_init(&g_client.dirty_mu, NULL);
        pthread_mutex_init(&g_client.idx_mu, NULL);
        g_client.dir_locks_ready = 1;
    }
    pthread_mutex_unlock(&init_mu);
}

static int dir_stripe(efs_ino_t parent)
{
    return (int)((uint64_t)parent % EFS_DIR_LOCKS);
}

void efs_client_lock_dir(efs_ino_t parent)
{
    efs_client_ensure_dir_locks();
    pthread_mutex_lock(&g_client.dir_lock[dir_stripe(parent)]);
}

void efs_client_unlock_dir(efs_ino_t parent)
{
    pthread_mutex_unlock(&g_client.dir_lock[dir_stripe(parent)]);
}

void efs_client_lock_dirs2(efs_ino_t a, efs_ino_t b)
{
    efs_client_ensure_dir_locks();
    int ia = dir_stripe(a), ib = dir_stripe(b);
    if (ia == ib) {
        pthread_mutex_lock(&g_client.dir_lock[ia]);
        return;
    }
    if (ia < ib) {
        pthread_mutex_lock(&g_client.dir_lock[ia]);
        pthread_mutex_lock(&g_client.dir_lock[ib]);
    } else {
        pthread_mutex_lock(&g_client.dir_lock[ib]);
        pthread_mutex_lock(&g_client.dir_lock[ia]);
    }
}

void efs_client_unlock_dirs2(efs_ino_t a, efs_ino_t b)
{
    int ia = dir_stripe(a), ib = dir_stripe(b);
    if (ia == ib) {
        pthread_mutex_unlock(&g_client.dir_lock[ia]);
        return;
    }
    pthread_mutex_unlock(&g_client.dir_lock[ia]);
    pthread_mutex_unlock(&g_client.dir_lock[ib]);
}

void efs_client_lock_all_dirs(void)
{
    efs_client_ensure_dir_locks();
    for (int i = 0; i < EFS_DIR_LOCKS; i++)
        pthread_mutex_lock(&g_client.dir_lock[i]);
}

void efs_client_unlock_all_dirs(void)
{
    for (int i = EFS_DIR_LOCKS - 1; i >= 0; i--)
        pthread_mutex_unlock(&g_client.dir_lock[i]);
}

void efs_client_table_lock(void)
{
    pthread_mutex_lock(&g_client.lock);
    efs_client_lock_all_dirs();
}

void efs_client_table_unlock(void)
{
    efs_client_unlock_all_dirs();
    pthread_mutex_unlock(&g_client.lock);
}

int efs_client_ensure_meta_room(uint64_t extra_inodes, uint64_t extra_chunks)
{
    if (g_client.write_readonly) {
        g_client.last_err = EFS_ERR_BUSY;
        return EFS_ERR_BUSY;
    }
    if (!efs_export_fits_page_cap(&g_client.export, extra_inodes, extra_chunks)) {
        g_client.last_err = EFS_ERR_QUOTA;
        return EFS_ERR_QUOTA;
    }
    return EFS_OK;
}


static void apply_chunk_recs(efs_ino_t lock_ino, const struct efs_chunk_rec *recs,
                             uint32_t n)
{
    efs_client_lock_dir(lock_ino);
    pthread_mutex_lock(&g_client.idx_mu);
    for (uint32_t i = 0; i < n; i++)
        (void)efs_export_set_chunk(&g_client.export, recs[i].ino,
                                   recs[i].chunk_index, recs[i].nodes,
                                   recs[i].checksums);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(lock_ino);
}

/* Fetch chunk mappings in [start_ci, end_ci). Never walk the whole table. */
static void pull_chunks_range(efs_ino_t ino, uint32_t start_ci, uint32_t end_ci)
{
    if (!ino || start_ci >= end_ci)
        return;
    uint32_t start = start_ci;
    while (start < end_ci) {
        struct efs_chunk_rec recs[EFS_GETCHUNKS_MAX];
        uint32_t n = end_ci - start;
        if (n > EFS_GETCHUNKS_MAX)
            n = EFS_GETCHUNKS_MAX;
        if (efs_client_rpc_getchunks(g_client.export_id, ino, start, recs,
                                     &n) != EFS_OK || n == 0)
            break;
        apply_chunk_recs(ino, recs, n);
        uint32_t next = recs[n - 1].chunk_index + 1;
        if (next <= start)
            break;
        start = next;
        if (n < EFS_GETCHUNKS_MAX)
            break;
    }
}

static void pull_chunks_for_ino(efs_ino_t ino, uint64_t size)
{
    uint32_t cs = data_chunk_size();
    uint32_t nci = 0;
    if (cs && size)
        nci = (uint32_t)((size + cs - 1) / cs);
    if (!nci)
        return;
    pull_chunks_range(ino, 0, nci);
}

static void pull_file_layout(const struct efs_inode *rpc)
{
    if ((rpc->mode & S_IFMT) != S_IFREG)
        return;
    if (rpc->pack_ino && rpc->pack_ino != rpc->ino) {
        uint32_t cs = data_chunk_size();
        uint64_t span = rpc->pack_len ? rpc->pack_len : rpc->size;
        uint32_t c0 = cs ? (uint32_t)(rpc->pack_off / cs) : 0;
        uint32_t c1 = c0 + 1;
        if (cs && span) {
            uint64_t hi = (uint64_t)rpc->pack_off + span;
            c1 = (uint32_t)((hi - 1) / cs) + 1;
        }
        pull_chunks_range(rpc->pack_ino, c0, c1);
    } else {
        pull_chunks_for_ino(rpc->ino, rpc->size);
    }
}

/* Read-miss self-heal (Phase 2b follow-on): the local chunk table is only
 * a cache, filled by adopt-time pulls. A client that never got the adopt
 * trigger (the file's server mtime/size never advanced past its snapshot)
 * would zero-fill real peer data as "holes" forever (mc_stress rwfile).
 * On a read miss inside the file size, pull the mapping range from the
 * owner and let the caller re-check. Rate-limited per ino so genuinely
 * sparse files cost at most one GETCHUNKS per second, not one per read. */
int efs_client_pull_layout_miss(efs_ino_t ino, uint32_t ci0, uint32_t ci1)
{
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static struct {
        efs_ino_t ino;
        uint64_t ns;
    } seen[64];
    static uint32_t next;
    if (!ino || ci0 >= ci1 || efs_ino_is_meta_table(ino))
        return 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    int go;
    pthread_mutex_lock(&mu);
    int slot = -1;
    for (uint32_t i = 0; i < 64; i++) {
        if (seen[i].ino == ino) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        slot = (int)(next++ % 64);
        seen[slot].ino = ino;
        seen[slot].ns = 0;
    }
    go = (now - seen[slot].ns >= 1000000000ull);
    if (go)
        seen[slot].ns = now;
    pthread_mutex_unlock(&mu);
    if (!go)
        return 0;
    pull_chunks_range(ino, ci0, ci1);
    return 1;
}

static void invalidate_file_layout(const struct efs_inode *rpc)
{
    uint32_t cs = data_chunk_size();
    if (rpc->pack_ino && rpc->pack_ino != rpc->ino) {
        uint32_t c0 = cs ? (uint32_t)(rpc->pack_off / cs) : 0;
        efs_rdcache_invalidate(rpc->pack_ino, c0);
        efs_dcache_drop_if_clean(rpc->pack_ino, c0);
    }
    uint32_t nci = 0;
    if (cs && rpc->size)
        nci = (uint32_t)((rpc->size + cs - 1) / cs);
    for (uint32_t ci = 0; ci < nci; ci++) {
        efs_rdcache_invalidate(rpc->ino, ci);
        efs_dcache_drop_if_clean(rpc->ino, ci);
    }
}

/* Adopt an inode the primary just confirmed. A local row that this client
 * dirtied stays (size/pack fresher until REPORT_CHUNKS). A peer's later
 * write is newer on the primary — merge size/pack when remote grew or
 * has a newer mtime (posix2 peer_shared_pwrite). */
static void adopt_rpc_inode(const struct efs_inode *rpc)
{
    if (!rpc || rpc->ino == 0)
        return;
    int is_new = 0;
    int take_remote = 0;
    efs_ino_t lock = rpc->parent ? rpc->parent : rpc->ino;
    efs_client_lock_dir(lock);
    pthread_mutex_lock(&g_client.idx_mu);
    struct efs_inode local;
    if (efs_export_get_inode(&g_client.export, rpc->ino, &local) != 0) {
        (void)efs_export_upsert_inode(&g_client.export, rpc);
        is_new = 1;
    } else if ((rpc->mode & S_IFMT) == S_IFREG) {
        /* Size growth = a peer published more data. mtime-only (chmod)
         * must not clobber the writer's unflushed dcache — that was the
         * POSIX same-fd-read / chmod / unlink-open regression. Shrink
         * only when the primary is newer (peer truncate). */
        int newer = rpc->mtime > local.mtime ||
                    (rpc->mtime == local.mtime &&
                     rpc->mtime_nsec > local.mtime_nsec);
        /* Equal-size with a newer mtime also takes the remote: a peer can
         * change chunk mappings without changing size (writing into a
         * pre-sized file's holes). Size-only growth misses that — the
         * reader then serves zeros/stale mappings forever (mc_stress
         * rwfile). Our own reports echo back with the same mtime, so this
         * does not re-pull on our own writes. */
        if (rpc->size > local.size ||
            (newer && rpc->size <= local.size)) {
            local.size = rpc->size;
            local.pack_ino = rpc->pack_ino;
            local.pack_off = rpc->pack_off;
            local.pack_len = rpc->pack_len;
            local.mtime = rpc->mtime;
            local.mtime_nsec = rpc->mtime_nsec;
            (void)efs_export_upsert_inode(&g_client.export, &local);
            take_remote = 1;
        }
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(lock);
    if (is_new) {
        pull_file_layout(rpc);
        return;
    }
    if (take_remote) {
        invalidate_file_layout(rpc);
        pull_file_layout(rpc);
    }
}

int efs_client_lookup(const char *path, struct efs_inode *out)
{
    if (!path || path[0] != '/')
        return EFS_ERR_INVAL;

    if (strcmp(path, "/") == 0) {
        efs_client_lock_dir(EFS_ROOT_INO);
        int rc = efs_export_get_inode(&g_client.export, EFS_ROOT_INO, out);
        efs_client_unlock_dir(EFS_ROOT_INO);
        return rc;
    }

    /* Copy onto the stack (bounded by PATH_MAX) instead of a per-lookup
     * strdup/free on the getattr/lookup hot path. */
    char pbuf[4096];
    size_t plen = strlen(path + 1);
    if (plen >= sizeof(pbuf))
        return EFS_ERR_INVAL;
    memcpy(pbuf, path + 1, plen + 1);
    char *save = NULL;
    char *part = strtok_r(pbuf, "/", &save);

    efs_client_ensure_dir_locks();
    efs_ino_t parent = EFS_ROOT_INO;
    int rc = EFS_ERR_NOT_FOUND;
    while (part) {
        struct efs_inode child;
        /* Phase 2b reads: ask the primary. The local snapshot is only
         * dual-applied on the mutating client, so a peer walk that stays
         * local misses creates (posix2 / IOR-hard EIO). */
        int lrc = efs_client_rpc_lookup(g_client.export_id, parent, part,
                                        &child);
        if (lrc != EFS_OK) {
            rc = (lrc == EFS_ERR_NOT_FOUND) ? EFS_ERR_NOT_FOUND : lrc;
            break;
        }
        /* After cross-server create the parent owner holds only a
         * dentry stub (size 0). getattr the child owner for the full row
         * so adopt/pull_file_layout see the real size. */
        if (g_client.export.root.shard_bits &&
            g_client.export.root.shard_count > 1 &&
            efs_mode_is_reg(child.mode)) {
            struct efs_inode full;
            if (efs_client_rpc_getattr(g_client.export_id, child.ino,
                                       &full) == EFS_OK)
                child = full;
        }
        adopt_rpc_inode(&child);
        /* Prefer the local row when we already have one: the writer's
         * size/pack fields are newer than the primary until REPORT_CHUNKS. */
        efs_client_lock_dir(child.parent ? child.parent : child.ino);
        pthread_mutex_lock(&g_client.idx_mu);
        struct efs_inode local;
        if (efs_export_get_inode(&g_client.export, child.ino, &local) == 0)
            child = local;
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(child.parent ? child.parent : child.ino);
        parent = child.ino;
        *out = child;
        rc = EFS_OK;
        part = strtok_r(NULL, "/", &save);
    }

    return rc;
}

efs_ino_t efs_client_create(efs_ino_t parent, const char *name, uint32_t mode,
                            uid_t uid, gid_t gid)
{
    g_client.last_err = EFS_OK;
    /* Phase 2b: the mutation runs on the metadata primary (which allocates the
     * ino and persists via the server flush thread). Dual-apply the returned
     * inode to the local snapshot so the data path sees it immediately. */
    struct efs_inode out;
    int rc = efs_client_rpc_create(g_client.export_id, parent, name, mode,
                                   uid, gid, NULL, &out);
    if (rc != EFS_OK) {
        g_client.last_err = rc;
        return 0;
    }
    efs_client_lock_dir(parent);
    pthread_mutex_lock(&g_client.idx_mu);
    /* create_with_ino applies the same side effects as the primary
     * (parent nlink++ for a subdirectory, indexes, rollups). Upsert
     * alone would leave the local parent nlink stale.
     * On a sharded export mirror the server's apply: the full row lives
     * on the child shard's table (so the data path's get_inode finds it),
     * plus a dentry row on the parent's shard when the two differ.
     * bits==0: both are the main table — identical to the old path. */
    struct efs_export *ctab = efs_export_table_for_ino(&g_client.export,
                                                       out.ino);
    struct efs_export *ptab = efs_export_table_for_ino(&g_client.export,
                                                       parent);
    if (!ctab)
        ctab = &g_client.export;
    if (efs_export_create_with_ino(ctab, out.ino, parent, mode,
                                   uid, gid, name) == 0)
        efs_export_upsert_inode(ctab, &out);
    if (ptab && ptab != ctab) {
        if (efs_export_create_with_ino(ptab, out.ino, parent, mode,
                                       uid, gid, name) == 0)
            efs_export_upsert_inode(ptab, &out);
    }
    efs_export_set_mtime(&g_client.export, parent, now());
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(parent);
    return out.ino;
}

/* Phase 2b: setattr via the primary; dual-apply the returned inode. */
static int setattr_rpc_dual_apply(efs_ino_t ino, uint32_t mask, uint32_t mode,
                                  uid_t uid, gid_t gid, uint64_t size,
                                  uint64_t mtime, uint32_t mtime_nsec,
                                  uint64_t atime)
{
    struct efs_inode out;
    int rc = efs_client_rpc_setattr(g_client.export_id, ino, mask, mode,
                                    uid, gid, size, mtime, mtime_nsec, atime,
                                    &out);
    if (rc != EFS_OK) {
        g_client.last_err = rc;
        return rc;
    }
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    /* The primary's returned inode lags the data path: it learns the size only
     * via the async REPORT_CHUNKS flush, so for a non-truncate setattr its
     * size can be stale (0 after a chmod hides the file's data — reads then
     * return empty). Preserve the local data-path size (tracked synchronously
     * by writes) unless this setattr is itself a truncate. */
    if (!(mask & EFS_SETATTR_SIZE)) {
        struct efs_inode cur;
        if (efs_export_get_inode(&g_client.export, ino, &cur) == 0)
            out.size = cur.size;
    }
    /* Apply mode/owner locally so hard-link rows share the new attrs
     * (upsert of the primary's indexed row alone left the other name
     * with a stale mode when the returned nlink was 1). */
    if (mask & EFS_SETATTR_MODE)
        efs_export_set_mode(&g_client.export, ino, mode);
    if (mask & (EFS_SETATTR_UID | EFS_SETATTR_GID))
        efs_export_set_owner(&g_client.export, ino, uid, gid);
    if (!(mask & (EFS_SETATTR_MODE | EFS_SETATTR_UID | EFS_SETATTR_GID)))
        efs_export_upsert_inode(&g_client.export, &out);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
    return EFS_OK;
}

int efs_client_chmod(efs_ino_t ino, uint32_t mode)
{
    return setattr_rpc_dual_apply(ino, EFS_SETATTR_MODE, mode, 0, 0, 0, 0, 0, 0);
}

int efs_client_chown(efs_ino_t ino, uid_t uid, gid_t gid)
{
    uint32_t mask = 0;
    if (uid != (uid_t)-1)
        mask |= EFS_SETATTR_UID;
    if (gid != (gid_t)-1)
        mask |= EFS_SETATTR_GID;
    return setattr_rpc_dual_apply(ino, mask, 0, uid, gid, 0, 0, 0, 0);
}

int efs_client_utime(efs_ino_t ino, uint64_t mtime)
{
    return efs_client_utimens(ino, mtime, 0);
}

int efs_client_utimens(efs_ino_t ino, uint64_t mtime, uint32_t mtime_nsec)
{
    return setattr_rpc_dual_apply(ino, EFS_SETATTR_MTIME, 0, 0, 0, 0,
                                  mtime, mtime_nsec, 0);
}

int efs_client_set_atime(efs_ino_t ino, uint64_t atime)
{
    return setattr_rpc_dual_apply(ino, EFS_SETATTR_ATIME, 0, 0, 0, 0, 0, 0,
                                  atime);
}

int efs_client_truncate(efs_ino_t ino, uint64_t size)
{
    efs_client_lock_dir(ino);
    struct efs_inode inode;
    if (efs_export_get_inode(&g_client.export, ino, &inode) != 0) {
        efs_client_unlock_dir(ino);
        return EFS_ERR_NOT_FOUND;
    }
    uint64_t old_size = inode.size;
    efs_client_unlock_dir(ino);

    if (size == old_size)
        return EFS_OK;

    if (size < old_size && size % data_chunk_size() != 0) {
        /* Rewrite last kept chunk with a zeroed tail so a later truncate-up
         * does not resurrect discarded bytes (data path; reported via RPC). */
        uint32_t chunk_size = data_chunk_size();
        uint32_t ci = (uint32_t)(size / chunk_size);
        uint32_t keep = (uint32_t)(size % chunk_size);
        uint8_t *chunk = efs_buf_alloc(chunk_size);
        if (!chunk)
            return EFS_ERR_NOMEM;
        memset(chunk, 0, chunk_size);
        uint64_t chunk_start = (uint64_t)ci * chunk_size;
        size_t want = (size_t)(old_size - chunk_start);
        if (want > chunk_size)
            want = chunk_size;
        size_t got = 0;
        (void)efs_client_read(ino, chunk_start, want, (char *)chunk, &got);
        memset(chunk + keep, 0, chunk_size - keep);
        int wrc = efs_client_write(ino, chunk_start, chunk_size,
                                   (const char *)chunk);
        efs_buf_free(chunk, chunk_size);
        if (wrc != 0)
            return wrc;
    }

    /* Phase 2b: the size change (and server-side chunk drop on shrink) runs on
     * the primary. Dual-apply: drop the same chunks locally + upsert the
     * returned inode. Grow is a logical sparse hole (no chunk work). */
    struct efs_inode out;
    int rc = efs_client_rpc_setattr(g_client.export_id, ino, EFS_SETATTR_SIZE,
                                    0, 0, 0, size, 0, 0, 0, &out);
    if (rc != EFS_OK) {
        g_client.last_err = rc;
        return rc;
    }
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    if (size < old_size) {
        uint32_t cs = data_chunk_size();
        uint32_t drop = (size == 0) ? 0 : (uint32_t)((size + cs - 1) / cs);
        efs_export_drop_chunks_from(&g_client.export, ino, drop);
    }
    efs_export_upsert_inode(&g_client.export, &out);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
    return EFS_OK;
}

int efs_client_rename(efs_ino_t ino, efs_ino_t new_parent, const char *new_name)
{
    /* Phase 2b: rename on the primary; dual-apply the rebound inode (upsert
     * handles the parent/name index dance). */
    struct efs_inode out;
    int rc = efs_client_rpc_rename(g_client.export_id, ino, new_parent,
                                   new_name, &out);
    if (rc != EFS_OK) {
        g_client.last_err = rc;
        return rc;
    }
    efs_client_lock_dirs2(ino, new_parent);
    pthread_mutex_lock(&g_client.idx_mu);
    /* Dual-apply by renaming the local table too: efs_export_rename only
     * touches name/parent/ctime, preserving the local data-path state (size,
     * pack fields, chunk mappings) which the server may not have yet — the
     * close's metadata flush is batched/async, so the returned inode can carry
     * a stale size=0. Upserting that stale inode would wipe the fresher local
     * state and lose the file's content. Fall back to upsert only if the local
     * table lacks the inode (created elsewhere). */
    if (efs_export_rename(&g_client.export, ino, new_parent, new_name) != EFS_OK)
        efs_export_upsert_inode(&g_client.export, &out);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dirs2(ino, new_parent);
    return EFS_OK;
}

int efs_client_unlink(efs_ino_t parent, const char *name, bool is_dir)
{
    /* Validate type locally (and to surface ENOENT/EISDIR before the RPC). */
    efs_client_lock_dir(parent);
    pthread_mutex_lock(&g_client.idx_mu);
    struct efs_inode ino;
    if (efs_export_lookup(&g_client.export, parent, name, &ino) != 0) {
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(parent);
        return EFS_ERR_NOT_FOUND;
    }
    if (efs_mode_is_dir(ino.mode) != is_dir) {
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(parent);
        return EFS_ERR_INVAL;
    }
    /* rmdir fast-path: refuse a non-empty directory locally (the server also
     * enforces this authoritatively via EFS_INODE_RPC_NOT_EMPTY). */
    if (is_dir && !efs_export_dir_empty(&g_client.export, ino.ino)) {
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(parent);
        return EFS_ERR_NOT_EMPTY;
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(parent);

    /* Phase 2b: unlink on the primary; dual-apply removes the local entry. */
    int rc = efs_client_rpc_unlink(g_client.export_id, parent, name, is_dir);
    if (rc != EFS_OK)
        return rc;
    efs_client_lock_dir(parent);
    pthread_mutex_lock(&g_client.idx_mu);
    efs_export_unlink_name(&g_client.export, parent, name);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(parent);
    return EFS_OK;
}

int efs_client_link(efs_ino_t src_ino, efs_ino_t new_parent, const char *new_name)
{
    /* Phase 2b: link on the primary. */
    struct efs_inode out;
    int rc = efs_client_rpc_link(g_client.export_id, src_ino, new_parent,
                                 new_name, &out);
    if (rc != EFS_OK) {
        g_client.last_err = rc;
        return rc;
    }
    efs_client_lock_dirs2(src_ino, new_parent);
    pthread_mutex_lock(&g_client.idx_mu);
    /* Dual-apply by linking the local table too: efs_export_link only bumps
     * nlink + binds the new name, preserving the local data-path state (size,
     * pack fields, chunk mappings) which the server may not have yet (batched
     * flush). Upserting the returned inode would wipe that fresher local state.
     * Fall back to upsert only if the local table lacks the inode. */
    if (efs_export_link(&g_client.export, src_ino, new_parent, new_name) != EFS_OK)
        efs_export_upsert_inode(&g_client.export, &out);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dirs2(src_ino, new_parent);
    return EFS_OK;
}
