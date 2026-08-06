#include "client_internal.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>

static uint64_t now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec;
}

int efs_client_lookup(const char *path, struct efs_inode *out)
{
    if (!path || path[0] != '/')
        return EFS_ERR_INVAL;

    efs_ino_t parent = EFS_ROOT_INO;
    if (strcmp(path, "/") == 0) {
        pthread_mutex_lock(&g_client.lock);
        int rc = efs_export_get_inode(&g_client.export, EFS_ROOT_INO, out);
        pthread_mutex_unlock(&g_client.lock);
        return rc;
    }

    char *p = strdup(path + 1);
    char *save = NULL;
    char *part = strtok_r(p, "/", &save);

    pthread_mutex_lock(&g_client.lock);
    int rc = EFS_ERR_NOT_FOUND;
    while (part) {
        struct efs_inode child;
        if (efs_export_lookup(&g_client.export, parent, part, &child) != 0) {
            rc = EFS_ERR_NOT_FOUND;
            break;
        }
        parent = child.ino;
        *out = child;
        rc = EFS_OK;
        part = strtok_r(NULL, "/", &save);
    }
    pthread_mutex_unlock(&g_client.lock);

    free(p);
    return rc;
}

efs_ino_t efs_client_create(efs_ino_t parent, const char *name, uint32_t mode,
                            uid_t uid, gid_t gid)
{
    pthread_mutex_lock(&g_client.lock);
    efs_ino_t ino;
    if (g_client.ino_namespace != 0) {
        /* Allocate from this client's private namespace so concurrent clients
         * can never assign the same ino to different files. */
        efs_ino_t candidate = g_client.ino_namespace | (g_client.ino_counter++);
        ino = efs_export_create_with_ino(&g_client.export, candidate, parent,
                                         mode, uid, gid, name);
    } else {
        ino = efs_export_create(&g_client.export, parent, mode, uid, gid, name);
    }
    if (ino != 0) {
        efs_export_set_mtime(&g_client.export, parent, now());
        efs_client_mark_ino_dirty(ino);
        efs_client_mark_ino_dirty(parent);
    }
    pthread_mutex_unlock(&g_client.lock);
    if (ino != 0)
        efs_client_note_meta_change(0);
    return ino;
}

int efs_client_chmod(efs_ino_t ino, uint32_t mode)
{
    pthread_mutex_lock(&g_client.lock);
    int rc = efs_export_set_mode(&g_client.export, ino, mode);
    if (rc == 0)
        efs_client_mark_ino_dirty(ino);
    pthread_mutex_unlock(&g_client.lock);
    if (rc == 0)
        rc = efs_client_note_meta_change(0);
    return rc;
}

int efs_client_chown(efs_ino_t ino, uid_t uid, gid_t gid)
{
    pthread_mutex_lock(&g_client.lock);
    int rc = efs_export_set_owner(&g_client.export, ino, uid, gid);
    if (rc == 0)
        efs_client_mark_ino_dirty(ino);
    pthread_mutex_unlock(&g_client.lock);
    if (rc == 0)
        rc = efs_client_note_meta_change(0);
    return rc;
}

int efs_client_utime(efs_ino_t ino, uint64_t mtime)
{
    pthread_mutex_lock(&g_client.lock);
    int rc = efs_export_set_mtime(&g_client.export, ino, mtime);
    if (rc == 0)
        efs_client_mark_ino_dirty(ino);
    pthread_mutex_unlock(&g_client.lock);
    if (rc == 0)
        rc = efs_client_note_meta_change(0);
    return rc;
}

int efs_client_truncate(efs_ino_t ino, uint64_t size)
{
    pthread_mutex_lock(&g_client.lock);
    int rc = efs_export_set_size(&g_client.export, ino, size);
    if (rc == 0)
        efs_client_mark_ino_dirty(ino);
    pthread_mutex_unlock(&g_client.lock);
    if (rc == 0)
        rc = efs_client_note_meta_change(0);
    return rc;
}

int efs_client_rename(efs_ino_t ino, efs_ino_t new_parent, const char *new_name)
{
    pthread_mutex_lock(&g_client.lock);
    int rc = efs_export_rename(&g_client.export, ino, new_parent, new_name);
    if (rc == 0) {
        efs_client_mark_ino_dirty(ino);
        efs_client_mark_ino_dirty(new_parent);
    }
    pthread_mutex_unlock(&g_client.lock);
    if (rc == 0)
        rc = efs_client_note_meta_change(0);
    return rc;
}

int efs_client_unlink(efs_ino_t parent, const char *name, bool is_dir)
{
    pthread_mutex_lock(&g_client.lock);
    struct efs_inode ino;
    if (efs_export_lookup(&g_client.export, parent, name, &ino) != 0) {
        pthread_mutex_unlock(&g_client.lock);
        return EFS_ERR_NOT_FOUND;
    }
    if (efs_mode_is_dir(ino.mode) != is_dir) {
        pthread_mutex_unlock(&g_client.lock);
        return EFS_ERR_INVAL;
    }

    efs_ino_t removed = ino.ino;
    int rc = efs_export_unlink_name(&g_client.export, parent, name);
    /* Merge cannot delete; full replicate when not batched. When batched,
     * mark parent (and remaining hard-link names) dirty. Tombstones TBD. */
    if (rc == 0) {
        efs_client_mark_ino_dirty(parent);
        efs_client_mark_ino_dirty(removed);
    }
    pthread_mutex_unlock(&g_client.lock);
    if (rc == 0)
        efs_client_note_meta_change(0);
    return rc;
}

int efs_client_link(efs_ino_t src_ino, efs_ino_t new_parent, const char *new_name)
{
    pthread_mutex_lock(&g_client.lock);
    int rc = efs_export_link(&g_client.export, src_ino, new_parent, new_name);
    if (rc == 0) {
        efs_client_mark_ino_dirty(src_ino);
        efs_client_mark_ino_dirty(new_parent);
    }
    pthread_mutex_unlock(&g_client.lock);
    if (rc == 0)
        rc = efs_client_note_meta_change(0);
    return rc;
}
