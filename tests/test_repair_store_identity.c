#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int fail_file,fail_linked_dir,linked;
static int identity_sync(int fd){struct stat st;assert(!fstat(fd,&st));if((S_ISREG(st.st_mode)&&fail_file)||(S_ISDIR(st.st_mode)&&linked&&fail_linked_dir)){errno=EIO;return -1;}return fsync(fd);}
static int identity_link(int a,const char *b,int c,const char *d,int flags){int rc=linkat(a,b,c,d,flags);if(!rc)linked=1;return rc;}
#define fsync identity_sync
#define linkat identity_link
#include "../src/server/put_fence.c"
#undef fsync
#undef linkat
static struct efsd_server s;
struct result {uint8_t id[16];int rc;};
static void *initialize(void *p){struct result *r=p;r->rc=server_repair_store_initialize(&s,1,r->id);return NULL;}
int main(void)
{
 char root[]="/tmp/efs-repair-identity-XXXXXX";assert(mkdtemp(root));s.storage_path_count=1;strcpy(s.storage_paths[0],root);
 uint8_t id[16],again[16];assert(server_repair_store_identity(&s,id)==EFS_ERR_STALE);
 fail_file=1;assert(server_repair_store_initialize(&s,0,id)==EFS_ERR_IO);assert(!id[0]);fail_file=0;
 assert(server_repair_store_initialize(&s,0,id)==EFS_ERR_STALE); /* nonempty unbound root never initialized implicitly */
 fail_linked_dir=1;linked=0;assert(server_repair_store_initialize(&s,1,id)==EFS_ERR_IO);assert(!id[0]&&linked);
 assert(server_repair_store_initialize(&s,1,id)==EFS_ERR_IO);fail_linked_dir=0;
 assert(!server_repair_store_initialize(&s,1,id));assert(!server_repair_store_identity(&s,again)&&!memcmp(id,again,16));
 struct result a={0},b={0};pthread_t one,two;assert(!pthread_create(&one,NULL,initialize,&a)&&!pthread_create(&two,NULL,initialize,&b));assert(!pthread_join(one,NULL)&&!pthread_join(two,NULL));assert(!a.rc&&!b.rc&&!memcmp(a.id,b.id,16)&&!memcmp(id,a.id,16));
 struct efsd_server cold={.storage_path_count=1};strcpy(cold.storage_paths[0],root);assert(!server_repair_store_matches(&cold,id));
 char fresh[]="/tmp/efs-repair-newdisk-XXXXXX";assert(mkdtemp(fresh));strcpy(cold.storage_paths[0],fresh);assert(server_repair_store_matches(&cold,id)==EFS_ERR_STALE);assert(!server_repair_store_initialize(&cold,0,again));assert(memcmp(id,again,16));assert(server_repair_store_matches(&cold,id)==EFS_ERR_STALE);
 char path[EFS_MAX_PATH+80];assert(!repair_store_path(&s,path));int fd=open(path,O_WRONLY);assert(fd>=0);uint8_t bad=255;assert(pwrite(fd,&bad,1,4)==1&&!close(fd));assert(server_repair_store_identity(&s,again)==EFS_ERR_IO);assert(server_repair_store_initialize(&s,1,again)==EFS_ERR_IO);fd=open(path,O_WRONLY);assert(fd>=0&&!ftruncate(fd,4)&&!close(fd));assert(server_repair_store_identity(&s,again)==EFS_ERR_IO);assert(server_repair_store_initialize(&s,1,again)==EFS_ERR_IO);
 puts("inactive repair store identity: unprivileged durable init, failed file/name sync noACK and barrier retry, concurrent agreement, nonempty missing/corrupt refusal, cold/new disk old-job mismatch PASS");return 0;
}
