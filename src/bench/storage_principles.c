#include "efs/cli_help.h"
/* Independent, finite EFS-shaped I/O experiments. No production engine calls. */
#include "storage_principles.h"
#include "principles_index.h"
#include "perf_control.h"
#include "latency.h"
#include "efs/checksum.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct experiment;
struct worker {
    struct experiment *e;
    unsigned id, count;
    int fd, dfd, error, owns_dir;
    char dir[4096], path[4096];
    uint8_t *buf, *expected;
    uint64_t *pending_start;
    uint64_t ops, syncs, start_us;
    struct principles_index *index;
    uint64_t index_start_syncs,index_start_written;
    struct lat_vec lat;
};
struct experiment {
    const char *roots[32], *rw, *policy, *persist;
    unsigned nroots, qd, objects, size, logical_size, stride, batch, container_alignment;
    int container, direct, sync_ancestors, retain, recover, indexed;
    uint32_t sealed_mask;
    pthread_mutex_t mu;
    pthread_cond_t ready_cv, go_cv;
    unsigned ready;
    int go, abort;
    uint64_t release;
};
static uint64_t clock_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static int sync_fd(int fd)
{
    int rc; do { rc = fsync(fd); } while (rc && errno == EINTR); return rc;
}
static int io_record(int fd, uint8_t *buf, unsigned size, off_t off, int read_op)
{
    unsigned done = 0;
    while (done < size) {
        ssize_t n = read_op ? pread(fd, buf + done, size - done, off + done)
                           : pwrite(fd, buf + done, size - done, off + done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        done += (unsigned)n;
    }
    return 0;
}
static void pattern(struct worker *w, unsigned object)
{
    /* Payload template is prepared outside timing; vary the object identity. */
    uint64_t identity = ((uint64_t)w->id << 32) + object + 1;
    unsigned identity_bytes = w->e->logical_size < sizeof(identity) ?
                              w->e->logical_size : sizeof(identity);
    memcpy(w->buf, &identity, identity_bytes);
    memset(w->buf + w->e->logical_size, 0, w->e->stride - w->e->logical_size);
    efs_hash(w->buf, w->e->size, w->buf + w->e->size);
}

static int file_path(struct worker *w, unsigned i, char *path)
{
    if (snprintf(path, 4096, "%s/%u.frag", w->dir, i) >= 4096) {
        errno = ENAMETOOLONG; return -1;
    }
    return 0;
}
static int open_record(struct worker *w, unsigned i, int creating, char *path)
{
    if (w->e->container) return w->fd;
    if (file_path(w, i, path)) return -1;
    return open(path, (w->e->recover ? O_RDONLY : O_RDWR) | (creating ? O_CREAT | O_EXCL : 0) |
                (w->e->direct ? O_DIRECT : 0), 0600);
}
static int verify_record(struct worker *w, unsigned i, int fd, off_t off, int tail_only)
{
    uint8_t got[32];
    if (w->e->recover) {
        struct stat st;
        off_t expected_size=(off_t)w->e->stride*(w->e->container?w->count:1);
        if(fstat(fd,&st) || st.st_size!=expected_size) {errno=EIO;return -1;}
    }
    const uint8_t *expected = w->expected + (size_t)i * 32;
    if (tail_only) {
        /* Buffered digest lookup models conditional-fragment deletion. */
        ssize_t n; do { n = pread(fd, got, 32, off + w->e->size); } while (n < 0 && errno == EINTR);
        if (n != 32 || memcmp(expected, got, 32)) { errno = EIO; return -1; }
        return 0;
    }
    if (io_record(fd, w->buf, w->e->stride, off, 1)) return -1;
    efs_hash(w->buf, w->e->size, got);
    if (memcmp(got, w->buf + w->e->size, 32) || memcmp(got, expected, 32)) {
        errno = EIO; return -1;
    }
    return 0;
}
static int empty_root(const char *root)
{
    DIR *d = opendir(root);
    if (!d) return 0;
    int empty = 1; struct dirent *de;
    while ((de = readdir(d))) if (strcmp(de->d_name,".") && strcmp(de->d_name,"..")) { empty = 0; break; }
    closedir(d); return empty;
}
static int sync_directories(struct worker *w)
{
    if (sync_fd(w->dfd)) return -1;
    w->syncs++;
    if (!w->e->sync_ancestors) return 0;
    char path[4096];
    if (!realpath(w->dir,path)) return -1;
    for (;;) {
        char *slash=strrchr(path,'/');
        if (!slash) { errno=EINVAL; return -1; }
        if (slash==path) path[1]=0; else *slash=0;
        int fd=open(path,O_RDONLY|O_DIRECTORY);
        if (fd<0) return -1;
        int rc=sync_fd(fd), saved=errno;
        if (close(fd) && !rc) return -1;
        if (rc) { errno=saved; return -1; }
        w->syncs++;
        if (!strcmp(path,"/")) return 0;
    }
}
static int flush_batch(struct worker *w, unsigned first, unsigned end)
{
    struct experiment *e = w->e;
    if (!strcmp(e->rw, "write")) {
        if (e->container) {
            if (sync_fd(w->fd)) return -1;
            w->syncs++;
        } else for (unsigned i = first; i < end; i++) {
            char path[4096]; int fd = open_record(w, i, 0, path);
            if (fd < 0) return -1;
            int rc = sync_fd(fd), saved = errno;
            if (close(fd) && !rc) return -1;
            if (rc) { errno = saved; return -1; }
            w->syncs++;
        }
    } else if (e->container && (!strcmp(e->policy, "punch") || !strcmp(e->policy,"tombstone"))) {
        if (sync_fd(w->fd)) return -1;
        w->syncs++;
    }
    return sync_directories(w);
}
/* Reuse the existing geometry, payloads, worker start barrier, latency
 * histogram and retention harness; only publication differs. */
static void run_indexed_worker(struct worker *w)
{
    struct experiment *e=w->e;
    if(e->recover) {
        for(unsigned i=0;i<w->count;i++) {
            uint64_t start=clock_ns(),gen=0;uint32_t n=e->size;
            if(principles_index_get(w->index,i,w->buf,&n,&gen) || n!=e->size || gen!=1) {w->error=EIO;break;}
            uint8_t hash[32];efs_hash(w->buf,e->size,hash);
            if(memcmp(hash,w->expected+(size_t)i*32,32)){w->error=EIO;break;}
            w->ops++;lat_add(&w->lat,(clock_ns()-start)/1000);
        }
        return;
    }
    unsigned group=!strcmp(e->persist,"each")?1:e->batch;
    unsigned bound=(64u<<20)/e->qd/e->size;
    if(group>bound)group=bound;
    if(group>w->count)group=w->count;
    struct principles_change *changes=calloc(group,sizeof(*changes));
    uint8_t *payload=malloc((size_t)group*e->size);
    if(!changes || !payload){free(changes);free(payload);w->error=ENOMEM;return;}
    for(unsigned first=0;first<w->count;first+=group) {
        unsigned n=w->count-first;if(n>group)n=group;
        for(unsigned j=0;j<n;j++) {
            unsigned i=first+j;w->pending_start[i]=clock_ns();pattern(w,i);
            memcpy(payload+(size_t)j*e->size,w->buf,e->size);
            changes[j]=(struct principles_change){i,e->size,0,payload+(size_t)j*e->size};
        }
        if(principles_index_publish(w->index,changes,n) ||
           (e->sync_ancestors && sync_directories(w))){w->error=errno?errno:EIO;break;}
        uint64_t end=clock_ns();
        for(unsigned j=0;j<n;j++)lat_add(&w->lat,(end-w->pending_start[first+j])/1000);
        w->ops+=n;
    }
    struct principles_index_stats stats;
    if(principles_index_stats(w->index,&stats))w->error=errno;
    else w->syncs+=stats.syncs-w->index_start_syncs;
    free(changes);free(payload);
}
static void *run_worker(void *arg)
{
    struct worker *w = arg; struct experiment *e = w->e;
    pthread_mutex_lock(&e->mu);
    e->ready++; pthread_cond_signal(&e->ready_cv);
    while (!e->go) pthread_cond_wait(&e->go_cv, &e->mu);
    int abort = e->abort; pthread_mutex_unlock(&e->mu);
    if (abort) return NULL;
    w->start_us = (clock_ns() - e->release) / 1000;
    if(e->indexed){run_indexed_worker(w);return NULL;}
    unsigned group = !strcmp(e->persist,"each") ? 1 : e->batch;
    unsigned limit = e->container && !strcmp(e->rw,"delete") && !strcmp(e->policy,"release") ? 1 : w->count;
    unsigned first = 0;
    int durable = strcmp(e->rw,"read") && strcmp(e->persist,"none");
    for (unsigned i = 0; i < limit; i++) {
        uint64_t start = clock_ns();
        if (durable) w->pending_start[i] = start;
        char path[4096]; int fd = -1, rc = 0;
        off_t off = e->container ? (off_t)i * e->stride : 0;
        if (!strcmp(e->rw,"write")) {
            pattern(w, i);
            fd = open_record(w, i, !strcmp(e->policy,"allocating"), path);
            if (fd < 0) rc = -1;
            else rc = io_record(fd, w->buf, e->stride, off, 0);
        } else if (!strcmp(e->rw,"read")) {
            fd = open_record(w, i, 0, path);
            if (fd < 0) rc = -1;
            else rc = verify_record(w, i, fd, off, 0);
        } else if (e->container && !strcmp(e->policy,"release")) {
            rc = unlink(w->path);
            /* Close before completion: an open unlinked file retains space. */
            if (close(w->fd) && !rc) rc = -1;
            w->fd = -1;
        } else if (e->container) {
            /* Validate each dead record before invalidating its aligned range. */
            rc = verify_record(w, i, w->fd, off, 1);
            if (!rc && !strcmp(e->policy,"punch"))
                rc = fallocate(w->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                                off, e->stride);
            else if (!rc) {
                memset(w->buf,0,e->stride);
                rc = io_record(w->fd,w->buf,e->stride,off,0);
            }
        } else {
            if (file_path(w,i,path)) rc = -1;
            else fd = open(path, O_RDONLY);
            if (fd < 0) rc = -1;
            else {
                rc = verify_record(w,i,fd,0,1);
                if (!rc) rc = unlink(path);
            }
        }
        int saved = errno;
        if (fd >= 0 && !e->container && close(fd) && !rc) { rc = -1; saved = errno; }
        if (rc) { w->error = saved ? saved : EIO; break; }
        int modifying = strcmp(e->rw,"read") != 0;
        if (modifying && strcmp(e->persist,"none") && (i + 1 == limit || i + 1 - first == group)) {
            if (flush_batch(w,first,i+1)) { w->error = errno; break; }
            /* Every record completes at its cohort's successful persistence
             * boundary, not at pwrite/close. Include its wait for later writes. */
            uint64_t completed = clock_ns();
            for (unsigned j = first; j <= i; j++)
                lat_add(&w->lat, (completed - w->pending_start[j]) / 1000);
            first = i + 1;
        }
        w->ops += limit == 1 && e->container && !strcmp(e->rw,"delete") &&
                  !strcmp(e->policy,"release") ? w->count : 1;
        if (!durable) lat_add(&w->lat, (clock_ns() - start) / 1000);
    }
    return NULL;
}
static uint64_t allocated(struct worker *w)
{
    struct stat st; uint64_t bytes = 0;
    if(w->e->indexed) {
        struct principles_index_stats stats;
        if(principles_index_stats(w->index,&stats)){w->error=errno?errno:EIO;return 0;}
        return stats.allocated_bytes;
    }
    if (w->e->container) {
        if (!stat(w->path,&st)) return (uint64_t)st.st_blocks * 512;
        return 0;
    }
    for (unsigned i=0;i<w->count;i++) {
        char path[4096]; if (!file_path(w,i,path) && !stat(path,&st)) bytes += (uint64_t)st.st_blocks * 512;
    }
    return bytes;
}
static int fixture_seal(struct experiment *e, int create)
{
    for (unsigned i=0;i<e->nroots;i++) {
        char path[4096], expected[256], got[257];
        if (snprintf(path,sizeof(path),"%s/.efs-principles-complete",e->roots[i])>=4096) return -1;
        int n=snprintf(expected,sizeof(expected),"%s %u %u %u %u %u %u %u %u\n",
            e->indexed?"EFS_PRINCIPLES_INDEX_V1":"EFS_PRINCIPLES_V1",
            e->container,e->container_alignment,e->size,e->logical_size,e->objects,e->qd,e->nroots,i);
        if (n<0 || (size_t)n>=sizeof(expected)) return -1;
        int fd=open(path,create?O_WRONLY|O_CREAT|O_EXCL:O_RDONLY,0600);
        if (fd<0) return -1;
        int rc=0;
        if (create) {
            e->sealed_mask |= UINT32_C(1)<<i;
            rc=io_record(fd,(uint8_t*)expected,(unsigned)n,0,0);
            if (!rc) rc=sync_fd(fd);
        } else {
            ssize_t count; do {count=read(fd,got,sizeof(got));} while(count<0 && errno==EINTR);
            if (count!=n || memcmp(got,expected,(size_t)n)) {errno=EINVAL;rc=-1;}
        }
        int saved=errno;
        if(close(fd) && !rc) return -1;
        if(rc) {errno=saved;return -1;}
        if(create) {
            fd=open(e->roots[i],O_RDONLY|O_DIRECTORY);
            if(fd<0)return -1;
            rc=sync_fd(fd);saved=errno;
            if(close(fd) && !rc)return -1;
            if(rc){errno=saved;return -1;}
        }
    }
    return 0;
}
static int number(const char *v, unsigned *out, unsigned max)
{
    char *end; errno=0; unsigned long n = strtoul(v,&end,10);
    if (errno || !*v || *v=='-' || *end || !n || n>max) return -1;
    *out=(unsigned)n; return 0;
}
int efs_bench_principles_main(int argc, char **argv)
{
    if (efs_help_requested(argc,argv)) { efs_cli_help(stdout,argv[0],"principles");return 0; }
    struct experiment e = {.rw="read", .policy="populated", .persist="none",
        .qd=1, .objects=4096, .size=65536, .batch=32, .container_alignment=4096,
        .mu=PTHREAD_MUTEX_INITIALIZER, .ready_cv=PTHREAD_COND_INITIALIZER,
        .go_cv=PTHREAD_COND_INITIALIZER};
    int kind=0;
    for (int i=1;i<argc;i++) {
        const char *k=argv[i];
        if (!strcmp(k,"--help") || !strcmp(k,"-h")) {
            efs_cli_help(stdout,argv[0],"principles");return 0;
        }
        if (!strcmp(k,"--indexed")) { e.indexed=1; continue; }
        if (!strcmp(k,"--retain")) { e.retain=1; continue; }
        if (!strcmp(k,"--recover-existing")) { e.recover=1; continue; }
        if (!strcmp(k,"--direct-io")) { e.direct=1; continue; }
        if (!strcmp(k,"--no-direct-io")) { e.direct=0; continue; }
        if (++i==argc) goto invalid;
        const char *v=argv[i];
        if (!strcmp(k,"--bench")) { if (kind++ || strcmp(v,"principles")) goto invalid; }
        else if (!strcmp(k,"--storage")) { if (!*v || e.nroots==32) goto invalid; e.roots[e.nroots++]=v; }
        else if (!strcmp(k,"--layout")) { if (strcmp(v,"files") && strcmp(v,"container")) goto invalid; e.container=!strcmp(v,"container"); }
        else if (!strcmp(k,"--rw")) e.rw=v;
        else if (!strcmp(k,"--policy")) e.policy=v;
        else if (!strcmp(k,"--persist")) e.persist=v;
        else if (!strcmp(k,"--directory-sync")) {
            if (strcmp(v,"worker") && strcmp(v,"ancestors")) goto invalid;
            e.sync_ancestors=!strcmp(v,"ancestors");
        }
        else if (!strcmp(k,"--objects")) { if (number(v,&e.objects,1048576)) goto invalid; }
        else if (!strcmp(k,"--payload-size")) { if (number(v,&e.size,1048576)) goto invalid; }
        else if (!strcmp(k,"--logical-size")) { if (number(v,&e.logical_size,1048576)) goto invalid; }
        else if (!strcmp(k,"--qd")) { if (number(v,&e.qd,256)) goto invalid; }
        else if (!strcmp(k,"--container-alignment")) {
            if (number(v,&e.container_alignment,4096) || (e.container_alignment!=1 && e.container_alignment!=4096)) goto invalid;
        }
        else if (!strcmp(k,"--batch-size")) { if (number(v,&e.batch,1048576)) goto invalid; }
        else goto invalid;
    }
    if (!e.logical_size) e.logical_size = e.size;
    if (e.logical_size > e.size || (e.container_alignment==1 && (!e.container || e.direct || !strcmp(e.policy,"punch")))) goto invalid;
    if (!kind || !e.nroots || e.qd<e.nroots || e.objects<e.qd ||
        (strcmp(e.persist,"none") && strcmp(e.persist,"each") && strcmp(e.persist,"batch"))) goto invalid;
    if (!strcmp(e.rw,"read")) { if (strcmp(e.policy,"populated") || strcmp(e.persist,"none")) goto invalid; }
    else if (!strcmp(e.rw,"write")) { if (strcmp(e.policy,"allocating") && strcmp(e.policy,"overwrite")) goto invalid; }
    else if (!strcmp(e.rw,"delete")) {
        if (e.container ? (strcmp(e.policy,"punch") && strcmp(e.policy,"tombstone") && strcmp(e.policy,"release")) : strcmp(e.policy,"unlink")) goto invalid;
    } else goto invalid;
    if ((e.retain && (strcmp(e.rw,"write") || !strcmp(e.persist,"none") || e.recover)) ||
        (e.recover && strcmp(e.rw,"read"))) goto invalid;
    if(e.indexed && (!e.container || e.direct || e.container_alignment!=1 ||
        e.objects>65536 || (uint64_t)e.size*e.qd>(64u<<20) ||
        (!e.recover && (strcmp(e.rw,"write") || strcmp(e.policy,"allocating") || !strcmp(e.persist,"none")))))goto invalid;
    for (unsigned i=0;i<e.nroots;i++) {
        if (!e.recover && !empty_root(e.roots[i])) { fprintf(stderr,"principles: scratch root must exist and be empty: %s\n",e.roots[i]); return 1; }
        struct stat a; if (stat(e.roots[i],&a)) return 1;
        for (unsigned j=0;j<i;j++) { struct stat b; if (stat(e.roots[j],&b)) return 1;
            if (a.st_dev==b.st_dev && a.st_ino==b.st_ino) goto invalid; }
    }
    if (e.recover && fixture_seal(&e,0)) {
        fprintf(stderr,"principles: missing or mismatched retained fixture seal\n"); return 1;
    }
    e.stride=e.size+32;
    /* Direct I/O and punching require aligned offsets and full-block lengths. */
    if (e.direct || (e.container && e.container_alignment==4096)) e.stride=(e.stride+4095u)&~4095u;
    struct worker *w=calloc(e.qd,sizeof(*w)); pthread_t *threads=calloc(e.qd,sizeof(*threads));
    if (!w || !threads) { free(w); free(threads); return 1; }
    for (unsigned i=0;i<e.qd;i++) { w[i].fd=-1; w[i].dfd=-1; }
    unsigned created=0; int rc=1, failure=0;
    uint64_t before=0, after=0, ops=0, syncs=0, verified=0, idle=0, max_start=0;
    struct lat_vec total={0}; double wall=0;
    int prepare=!e.recover && (strcmp(e.rw,"write") || strcmp(e.policy,"allocating"));
    for (unsigned i=0;i<e.qd;i++) {
        w[i].e=&e; w[i].id=i; w[i].count=e.objects/e.qd+(i<e.objects%e.qd);
        if (snprintf(w[i].dir,sizeof(w[i].dir),"%s/principle-%u",e.roots[i%e.nroots],i)>=4096) goto done;
        if (!e.recover) {
            if (mkdir(w[i].dir,0700)) goto done;
            w[i].owns_dir=1;
        }
        w[i].dfd=open(w[i].dir,O_RDONLY|O_DIRECTORY); if (w[i].dfd<0) goto done;
        if (posix_memalign((void**)&w[i].buf,4096,e.stride)) goto done;
        w[i].expected=malloc((size_t)w[i].count*32); if(!w[i].expected) goto done;
        if (strcmp(e.rw,"read") && strcmp(e.persist,"none")) {
            w[i].pending_start=malloc((size_t)w[i].count*sizeof(uint64_t));
            if (!w[i].pending_start) goto done;
        }
        uint64_t x=i+1;
        for(unsigned k=0;k<e.size;k++) { x^=x<<13;x^=x>>7;x^=x<<17;w[i].buf[k]=(uint8_t)x; }
        for(unsigned j=0;j<w[i].count;j++) { pattern(&w[i],j); memcpy(w[i].expected+(size_t)j*32,w[i].buf+e.size,32); }
        if(e.indexed) {
            w[i].index=principles_index_open(w[i].dir,w[i].count,UINT64_C(1)<<30,!e.recover,e.recover);
            if(!w[i].index)goto done;
            struct principles_index_stats stats;if(principles_index_stats(w[i].index,&stats))goto done;
            w[i].index_start_syncs=stats.syncs;w[i].index_start_written=stats.index_written_bytes;
        } else if (e.container) {
            if (snprintf(w[i].path,sizeof(w[i].path),"%s/container",w[i].dir)>=4096) goto done;
            w[i].fd=open(w[i].path,(e.recover?O_RDONLY:O_RDWR|O_CREAT|O_EXCL)|(e.direct && strcmp(e.rw,"delete") ? O_DIRECT:0),0600);
            if (w[i].fd<0) goto done;
            /* Size the container up front, as the removed container engine did
             * (ftruncate to SEG_BYTES): records then never extend EOF, so XFS
             * speculative preallocation (allocsize=128m on ORCD) cannot
             * leave post-EOF blocks that no record punch reclaims. */
            if (!e.recover && ftruncate(w[i].fd,(off_t)w[i].count*e.stride)) goto done;
            if (!strcmp(e.policy,"overwrite")) { int err=posix_fallocate(w[i].fd,0,(off_t)w[i].count*e.stride); if(err) {errno=err;goto done;} }
        }
        if (prepare) for (unsigned j=0;j<w[i].count;j++) {
            char path[4096]; int fd=open_record(&w[i],j,1,path); if(fd<0) goto done;
            pattern(&w[i],j);
            int err=0;
            if (!e.container && !strcmp(e.policy,"overwrite")) err=posix_fallocate(fd,0,e.stride);
            int failed=err || io_record(fd,w[i].buf,e.stride,e.container?(off_t)j*e.stride:0,0) || (!e.container && sync_fd(fd));
            if (!e.container && close(fd)) failed=1;
            if(failed) goto done;
        }
        if (e.container && prepare && sync_fd(w[i].fd)) goto done;
        if (e.recover) { before+=allocated(&w[i]); continue; }
        if (sync_fd(w[i].dfd)) goto done;
        int rootfd=open(e.roots[i%e.nroots],O_RDONLY|O_DIRECTORY);
        if(rootfd<0) goto done;
        int rootrc=sync_fd(rootfd);
        if(close(rootfd) || rootrc) goto done;
        before+=allocated(&w[i]);
    }
    for (unsigned i=0;i<e.qd;i++) {
        int err=pthread_create(&threads[i],NULL,run_worker,&w[i]);
        if(err) {errno=err;goto stop;} created++;
    }
    pthread_mutex_lock(&e.mu);
    while(e.ready<e.qd) pthread_cond_wait(&e.ready_cv,&e.mu);
    pthread_mutex_unlock(&e.mu);
    if(perf_command("enable\n")) goto stop;
    e.release=clock_ns();
    pthread_mutex_lock(&e.mu); e.go=1; pthread_cond_broadcast(&e.go_cv); pthread_mutex_unlock(&e.mu);
    for(unsigned i=0;i<created;i++) pthread_join(threads[i],NULL);
    created=0; wall=(clock_ns()-e.release)/1e9;
    if(perf_command("disable\n")) goto done;
    for(unsigned i=0;i<e.qd;i++) {
        ops+=w[i].ops; syncs+=w[i].syncs; lat_merge(&total,&w[i].lat);
        if(w[i].error) { failure++; fprintf(stderr,"principles worker %u: %s\n",i,strerror(w[i].error)); }
        if(!w[i].ops) idle++;
        if(w[i].start_us>max_start) max_start=w[i].start_us;
        /* none measures acceptance, but post-timing flush makes allocation observable. */
        if(!strcmp(e.rw,"write") && !strcmp(e.persist,"none")) {
            if(flush_batch(&w[i],0,w[i].count)) failure++;
        }
        after+=allocated(&w[i]);
        if(w[i].error)failure++;
        /* Independent post-run verification, outside the timed/profiled loop. */
        for(unsigned j=0;j<w[i].count;j++) {
            char path[4096];
            if(e.indexed) {
                uint32_t n=e.size;uint64_t gen=0;uint8_t hash[32];
                if(principles_index_get(w[i].index,j,w[i].buf,&n,&gen) || n!=e.size || gen!=1){failure++;continue;}
                efs_hash(w[i].buf,e.size,hash);
                if(memcmp(hash,w[i].expected+(size_t)j*32,32))failure++;else verified++;
            } else if(!strcmp(e.rw,"delete")) {
                if(!e.container) { struct stat st; file_path(&w[i],j,path); if(!stat(path,&st) || errno!=ENOENT) failure++; else verified++; }
                else if(!strcmp(e.policy,"release")) { struct stat st; if(!stat(w[i].path,&st) || errno!=ENOENT) failure++; else verified++; }
                else { if(io_record(w[i].fd,w[i].buf,e.stride,(off_t)j*e.stride,1)) failure++;
                    else { unsigned k; for(k=0;k<e.stride;k++) if(w[i].buf[k]) break;
                        if(k!=e.stride) failure++; else verified++; } }
            } else {
                int fd=open_record(&w[i],j,0,path);
                if(fd<0) {failure++;continue;}
                if(verify_record(&w[i],j,fd,e.container?(off_t)j*e.stride:0,0)) failure++; else verified++;
                if(!e.container && close(fd)) failure++;
            }
        }
    }
    if(ops!=e.objects || verified!=e.objects || idle) failure++;
    if(!failure && e.retain && fixture_seal(&e,1)) failure++;
    if(e.retain || e.recover)
        printf("BENCH_NOTE fixture=%s seal=outside_timing recovery=read_only\n",e.recover?"recovered":"retained");
    if(e.indexed) {
        uint64_t segments=0,index=0,garbage=0,body_writes=0,index_writes=0;
        for(unsigned i=0;i<e.qd;i++){struct principles_index_stats stats;if(principles_index_stats(w[i].index,&stats)){failure++;continue;}segments+=stats.segment_bytes;index+=stats.index_bytes;garbage+=stats.garbage_bytes;body_writes+=stats.body_written_bytes;index_writes+=stats.index_written_bytes-w[i].index_start_written;}
        printf("BENCH_NOTE index_protocol=immutable_segments_snapshot index_bytes=%llu segment_bytes=%llu garbage_bytes=%llu body_written_bytes=%llu index_written_bytes=%llu per_worker_quota_bytes=1073741824 cohort_memory_bound_bytes=67108864\n",(unsigned long long)index,(unsigned long long)segments,(unsigned long long)garbage,(unsigned long long)body_writes,(unsigned long long)index_writes);
    }
    printf("%s kind=principles model=independent rw=%s layout=%s policy=%s persist=%s directory_sync=%s direct=%d payload_bytes=%u logical_payload_bytes=%u container_alignment=%u record_bytes=%u objects=%u qd=%u paths=%u batch=%u ops=%llu wall_s=%.6f ops_s=%.3f GiB_s=%.6f p50_us=%llu p95_us=%llu p99_us=%llu max_us=%llu latency=%s latency_samples=%llu syncs=%llu allocated_before=%llu allocated_after=%llu reclaimed_allocated_bytes=%llu verified_objects=%llu idle_workers=%llu max_start_us=%llu errors=%d timing=finite_one_pass\n",
           failure?"BENCH_FAIL":"BENCH_OK",e.rw,e.indexed?"indexed":(e.container?"container":"files"),e.policy,e.persist,e.sync_ancestors?"ancestors":"worker",e.direct,e.size,e.logical_size,e.container_alignment,e.indexed?e.size+52:e.stride,e.objects,e.qd,e.nroots,e.batch,
           (unsigned long long)ops,wall,ops/wall,(double)ops*e.size/(1ull<<30)/wall,
           (unsigned long long)lat_percentile(&total,50,NULL),(unsigned long long)lat_percentile(&total,95,NULL),(unsigned long long)lat_percentile(&total,99,NULL),(unsigned long long)total.max,
           !strcmp(e.policy,"release")?"container_release":
           (strcmp(e.rw,"read") && strcmp(e.persist,"none")?"operation_to_durable_completion":"operation_acceptance"),
           (unsigned long long)total.n,(unsigned long long)syncs,(unsigned long long)before,(unsigned long long)after,
           (unsigned long long)(before>after?before-after:0),(unsigned long long)verified,(unsigned long long)idle,(unsigned long long)max_start,failure);
    rc=failure?1:0; goto done;
stop:
    pthread_mutex_lock(&e.mu); e.abort=1; e.go=1; pthread_cond_broadcast(&e.go_cv); pthread_mutex_unlock(&e.mu);
    for(unsigned i=0;i<created;i++) pthread_join(threads[i],NULL);
done:
    if(rc && e.sealed_mask) for(unsigned i=0;i<e.nroots;i++) {
        if(e.sealed_mask & (UINT32_C(1)<<i)) {
            char path[4096];
            if(snprintf(path,sizeof(path),"%s/.efs-principles-complete",e.roots[i])<(int)sizeof(path)) unlink(path);
        }
    }
    if(rc && !failure) fprintf(stderr,"principles: setup/control failed: %s\n",strerror(errno));
    for(unsigned i=0;i<e.qd;i++) {
        if(w[i].index)principles_index_close(w[i].index);
        if(w[i].fd>=0) close(w[i].fd);
        if(w[i].dfd>=0) close(w[i].dfd);
        /* Only paths created under validated empty scratch roots are removed. */
        if(e.indexed && w[i].owns_dir && !e.retain && rc==0) {
            DIR *d=opendir(w[i].dir);if(d){struct dirent *entry;while((entry=readdir(d))) {
                if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
                char path[4096];if(snprintf(path,sizeof(path),"%s/%s",w[i].dir,entry->d_name)<(int)sizeof(path))unlink(path);
            }closedir(d);rmdir(w[i].dir);}
        } else if(!e.indexed && w[i].owns_dir && !(e.retain && rc==0)) { if(e.container) { if(w[i].path[0]) unlink(w[i].path); }
            else for(unsigned j=0;j<w[i].count;j++) {char path[4096];if(!file_path(&w[i],j,path)) unlink(path);}
            if(w[i].dir[0]) rmdir(w[i].dir); }
        free(w[i].buf); free(w[i].expected); free(w[i].pending_start);
    }
    free(w);free(threads);return rc;
invalid:
    fprintf(stderr,"principles: invalid arguments (use --bench principles --help)\n");return 2;
}
