#include "client_internal.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>

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

static void grow_inodes_exclusive(void)
{
    efs_client_table_lock();
    (void)efs_export_reserve_inodes(&g_client.export, 65536);
    efs_client_table_unlock();
}


int efs_client_lookup(const char *path, struct efs_inode *out)
{
    if (!path || path[0] != '/')
        return EFS_ERR_INVAL;

    efs_ino_t parent = EFS_ROOT_INO;
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
    int rc = EFS_ERR_NOT_FOUND;
    while (part) {
        efs_client_lock_dir(parent);
        pthread_mutex_lock(&g_client.idx_mu);
        struct efs_inode child;
        int lrc = efs_export_lookup(&g_client.export, parent, part, &child);
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(parent);
        if (lrc != 0) {
            rc = EFS_ERR_NOT_FOUND;
            break;
        }
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
    efs_ino_t ino = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        if (efs_export_needs_inode_grow(&g_client.export))
            grow_inodes_exclusive();
        efs_client_lock_dir(parent);
        pthread_mutex_lock(&g_client.idx_mu);
        if (efs_export_needs_inode_grow(&g_client.export)) {
            pthread_mutex_unlock(&g_client.idx_mu);
            efs_client_unlock_dir(parent);
            continue;
        }
        if (efs_export_lookup(&g_client.export, parent, name, NULL) == EFS_OK) {
            pthread_mutex_unlock(&g_client.idx_mu);
            efs_client_unlock_dir(parent);
            return 0;
        }
        if (g_client.ino_namespace != 0) {
            efs_ino_t candidate = g_client.ino_namespace | (g_client.ino_counter++);
            ino = efs_export_create_with_ino(&g_client.export, candidate, parent,
                                             mode, uid, gid, name);
        } else {
            ino = efs_export_create(&g_client.export, parent, mode, uid, gid, name);
        }
        if (ino != 0)
            efs_export_set_mtime(&g_client.export, parent, now());
        pthread_mutex_unlock(&g_client.idx_mu);
        if (ino != 0) {
            efs_client_mark_ino_dirty(ino);
            efs_client_mark_ino_dirty(parent);
            efs_client_unlock_dir(parent);
            break;
        }
        efs_client_unlock_dir(parent);
        /* ino collision or a failed idx insert: try the next candidate */
    }
    if (ino != 0)
        efs_client_note_meta_change(0);
    return ino;
}

int efs_client_chmod(efs_ino_t ino, uint32_t mode)
{
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    int rc = efs_export_set_mode(&g_client.export, ino, mode);
    pthread_mutex_unlock(&g_client.idx_mu);
    if (rc == 0)
        efs_client_mark_ino_dirty(ino);
    efs_client_unlock_dir(ino);
    /* Mode is already applied locally; a later meta flush failure must not
     * surface as chmod EINVAL (ImageNet-scale batches can temporarily exceed
     * caps or hit transient quorum). */
    if (rc == 0)
        (void)efs_client_note_meta_change(0);
    return rc;
}

int efs_client_chown(efs_ino_t ino, uid_t uid, gid_t gid)
{
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    int rc = efs_export_set_owner(&g_client.export, ino, uid, gid);
    pthread_mutex_unlock(&g_client.idx_mu);
    if (rc == 0)
        efs_client_mark_ino_dirty(ino);
    efs_client_unlock_dir(ino);
    if (rc == 0)
        rc = efs_client_note_meta_change(0);
    return rc;
}

int efs_client_utime(efs_ino_t ino, uint64_t mtime)
{
    return efs_client_utimens(ino, mtime, 0);
}

int efs_client_utimens(efs_ino_t ino, uint64_t mtime, uint32_t mtime_nsec)
{
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    int rc = efs_export_set_mtime_ns(&g_client.export, ino, mtime, mtime_nsec);
    pthread_mutex_unlock(&g_client.idx_mu);
    if (rc == 0)
        efs_client_mark_ino_dirty(ino);
    efs_client_unlock_dir(ino);
    if (rc == 0)
        rc = efs_client_note_meta_change(0);
    return rc;
}

int efs_client_set_atime(efs_ino_t ino, uint64_t atime)
{
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    int rc = efs_export_set_atime(&g_client.export, ino, atime);
    pthread_mutex_unlock(&g_client.idx_mu);
    if (rc == 0)
        efs_client_mark_ino_dirty(ino);
    efs_client_unlock_dir(ino);
    if (rc == 0)
        rc = efs_client_note_meta_change(0);
    return rc;
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

    if (size < old_size) {
        uint32_t chunk_size = data_chunk_size();
        uint32_t first_drop;
        if (size == 0) {
            first_drop = 0;
        } else {
            uint32_t ci = (uint32_t)(size / chunk_size);
            uint32_t keep = (uint32_t)(size % chunk_size);
            if (keep != 0) {
                /* Rewrite last kept chunk with a zeroed tail so a later
                 * truncate-up does not resurrect discarded bytes. */
                uint8_t *chunk = malloc(chunk_size);
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
                free(chunk);
                if (wrc != 0)
                    return wrc;
                first_drop = ci + 1;
            } else {
                first_drop = ci;
            }
        }
        efs_client_lock_dir(ino);
        pthread_mutex_lock(&g_client.idx_mu);
        efs_export_drop_chunks_from(&g_client.export, ino, first_drop);
        int rc = efs_export_set_size(&g_client.export, ino, size);
        pthread_mutex_unlock(&g_client.idx_mu);
        if (rc == 0)
            efs_client_mark_ino_dirty(ino);
        efs_client_unlock_dir(ino);
        if (rc == 0)
            rc = efs_client_note_meta_change(0);
        return rc;
    }

    /* Grow: logical sparse hole; reads of unmapped chunks return zeros. */
    efs_client_lock_dir(ino);
    int rc = efs_export_set_size(&g_client.export, ino, size);
    if (rc == 0)
        efs_client_mark_ino_dirty(ino);
    efs_client_unlock_dir(ino);
    if (rc == 0)
        rc = efs_client_note_meta_change(0);
    return rc;
}

int efs_client_rename(efs_ino_t ino, efs_ino_t new_parent, const char *new_name)
{
    efs_client_lock_dirs2(ino, new_parent);
    pthread_mutex_lock(&g_client.idx_mu);
    int rc = efs_export_rename(&g_client.export, ino, new_parent, new_name);
    pthread_mutex_unlock(&g_client.idx_mu);
    if (rc == 0) {
        efs_client_mark_ino_dirty(ino);
        efs_client_mark_ino_dirty(new_parent);
    }
    efs_client_unlock_dirs2(ino, new_parent);
    if (rc == 0)
        rc = efs_client_note_meta_change(0);
    return rc;
}

int efs_client_unlink(efs_ino_t parent, const char *name, bool is_dir)
{
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

    efs_ino_t removed = ino.ino;
    int rc = efs_export_unlink_name(&g_client.export, parent, name);
    pthread_mutex_unlock(&g_client.idx_mu);
    /* Merge cannot delete; full replicate when not batched. When batched,
     * mark parent (and remaining hard-link names) dirty. Tombstones TBD. */
    if (rc == 0) {
        efs_client_mark_ino_dirty(parent);
        efs_client_mark_ino_dirty(removed);
    }
    efs_client_unlock_dir(parent);
    if (rc == 0)
        efs_client_note_meta_change(0);
    return rc;
}

int efs_client_link(efs_ino_t src_ino, efs_ino_t new_parent, const char *new_name)
{
    if (efs_export_needs_inode_grow(&g_client.export))
        grow_inodes_exclusive();
    efs_client_lock_dirs2(src_ino, new_parent);
    pthread_mutex_lock(&g_client.idx_mu);
    int rc = efs_export_link(&g_client.export, src_ino, new_parent, new_name);
    pthread_mutex_unlock(&g_client.idx_mu);
    if (rc == 0) {
        efs_client_mark_ino_dirty(src_ino);
        efs_client_mark_ino_dirty(new_parent);
    }
    efs_client_unlock_dirs2(src_ino, new_parent);
    if (rc == 0)
        rc = efs_client_note_meta_change(0);
    return rc;
}
