/* Actual certificates and guarded writes; errors never acknowledge a fence. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
static unsigned sync_calls,fail_sync;
static int storage_fsync(int fd){sync_calls++;if(sync_calls==fail_sync){errno=EIO;return -1;}return fsync(fd);}
#define fsync storage_fsync
#include "../src/server/put_fence.c"
#include "test_tmpdir.h"
#undef fsync
static struct efsd_server server;
static uint8_t uuid[16]={1,2,3,4};
static pthread_mutex_t progress_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t progress_cv=PTHREAD_COND_INITIALIZER;
static int started,finished,fence_rc;
static void *fencer(void *unused)
{
    (void)unused;pthread_mutex_lock(&progress_mu);started=1;pthread_cond_broadcast(&progress_cv);pthread_mutex_unlock(&progress_mu);
    int rc=server_put_session_fence(&server,uuid,2);
    pthread_mutex_lock(&progress_mu);fence_rc=rc;finished=1;pthread_cond_broadcast(&progress_cv);pthread_mutex_unlock(&progress_mu);return NULL;
}
int main(void)
{
    char root[EFS_TEST_PATH];efs_test_path(root,sizeof(root),"efs-put-session-XXXXXX");assert(efs_test_mkdtemp(root));
    server.storage_path_count=1;strcpy(server.storage_paths[0],root);
    struct efs_put_storage_guard guard={0};uint8_t zero[16]={0};
    assert(server_put_session_begin(&server,zero,1,&guard)==EFS_ERR_INVAL);
    assert(server_put_session_begin(&server,uuid,0,&guard)==EFS_ERR_INVAL);
    assert(!server_put_session_begin(&server,uuid,1,&guard));
    assert(server_put_session_begin(&server,uuid,1,&guard)==EFS_ERR_INVAL);
    pthread_t thread;assert(!pthread_create(&thread,NULL,fencer,NULL));
    pthread_mutex_lock(&progress_mu);while(!started)pthread_cond_wait(&progress_cv,&progress_mu);assert(!finished);pthread_mutex_unlock(&progress_mu);
    /* A fence cannot seal while the old write owns the guard. */
    char path[EFS_MAX_PATH+80];pthread_mutex_t *mu;uint32_t floor=99;
    assert(!storage_fence_path(&server,uuid,path,&mu));assert(!storage_fence_read(path,uuid,&floor)&&!floor);
    server_put_session_end(&guard);server_put_session_end(&guard);assert(!pthread_join(thread,NULL));assert(finished&&!fence_rc);
    assert(server_put_session_begin(&server,uuid,1,&guard)==EFS_ERR_STALE&&!guard.locked);
    assert(!server_put_session_begin(&server,uuid,2,&guard));server_put_session_end(&guard);
    assert(!server_put_session_fence(&server,uuid,1)); /* never move backwards */
    assert(server_put_session_begin(&server,uuid,1,&guard)==EFS_ERR_STALE);
    struct efsd_server cold={.storage_path_count=1};strcpy(cold.storage_paths[0],root);
    assert(server_put_session_begin(&cold,uuid,1,&guard)==EFS_ERR_STALE);
    /* Failed certificate-file sync leaves the old floor, and cannot ACK. */
    fail_sync=sync_calls+2;assert(server_put_session_fence(&server,uuid,3)==EFS_ERR_IO);fail_sync=0;
    assert(!storage_fence_read(path,uuid,&floor)&&floor==2);
    /* Rename may succeed before directory sync fails. Repeat must seal it. */
    fail_sync=sync_calls+3;assert(server_put_session_fence(&server,uuid,3)==EFS_ERR_IO);fail_sync=0;
    assert(!storage_fence_read(path,uuid,&floor)&&floor==3);
    assert(!server_put_session_fence(&server,uuid,3));
    /* A11: once synced in this process, a repeat at or below that floor
     * (every FENCE_LOC of one revoke) needs no further fsync. */
    unsigned synced=sync_calls;
    assert(!server_put_session_fence(&server,uuid,3)&&!server_put_session_fence(&server,uuid,2));
    assert(sync_calls==synced);
    assert(server_put_session_begin(&cold,uuid,2,&guard)==EFS_ERR_STALE);
    /* Configured roots may deliberately be symlinks to a storage volume. */
    char alias[256];snprintf(alias,sizeof(alias),"%s-alias",root);assert(!symlink(root,alias));
    struct efsd_server linked={.storage_path_count=1};strcpy(linked.storage_paths[0],alias);
    synced=sync_calls;assert(!server_put_session_fence(&linked,uuid,3));
    assert(sync_calls>synced); /* another certificate path is synced on its own */
    assert(server_put_session_begin(&linked,uuid,2,&guard)==EFS_ERR_STALE);
    assert(!unlink(alias));
    /* Full UUID identity and checksum, rather than epoch alone, are sealed. */
    int fd=open(path,O_WRONLY);assert(fd>=0);uint8_t corrupt=255;assert(pwrite(fd,&corrupt,1,4)==1&&!close(fd));
    assert(server_put_session_begin(&cold,uuid,3,&guard)==EFS_ERR_IO&&!guard.locked);
    assert(server_put_session_fence(&server,uuid,4)==EFS_ERR_IO);
    assert(!unlink(path));char dir[EFS_MAX_PATH+80];strcpy(dir,path);*strrchr(dir,'/')=0;assert(!rmdir(dir));*strrchr(dir,'/')=0;assert(!rmdir(dir));assert(!rmdir(root));
    puts("storage session fence: full UUID, monotonic cold floor, guarded-write ordering, file/directory sync failures and corrupt-certificate rejection PASS");return 0;
}
