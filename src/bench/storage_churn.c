/* Bounded mixed read/replace/retire experiments over files or XFS containers.
 * Each blocking worker owns its model and storage. GC pauses that worker;
 * other workers continue. This is not an asynchronous production collector.
 */
#include "storage_churn.h"
#include "latency.h"
#include "efs/checksum.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

struct config {
    const char *root, *layout, *persist;
    uint64_t capacity, container, hint;
    unsigned payload, stride, occupancy, qd, steps, warmup, batch, reads;
    int direct, preallocate, filestreams;
};
struct object { uint64_t generation; unsigned location; uint8_t digest[32]; };
struct segment { int fd, dirty; unsigned live; char path[4096]; };
struct sample {
    struct lat_vec read, write, gc;
    uint64_t reads, writes, retired, copied, copy_bytes, reclaimed, syncs;
    uint64_t cleanings, max_dead, max_allocated;
};
struct worker {
    struct config *c;
    unsigned id, slots, per_segment, nsegments, spare, live, cursor;
    struct object *objects;
    /* slot: 0 free, 1 live, 2 retired but still physically allocated. */
    uint8_t *state, *buf, *verify;
    struct segment *segments;
    int dfd, owns_dir, error;
    char dir[4096];
    uint64_t rng, dead, before, after, start_ns, end_ns;
    struct sample stats;
};
struct gate { pthread_mutex_t mu; pthread_cond_t cv; unsigned ready; int go; };
static struct gate gate = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0};

static uint64_t now_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static uint64_t random_next(struct worker *w)
{
    uint64_t x=w->rng; x^=x<<13; x^=x>>7; x^=x<<17; return w->rng=x;
}
static int sync_file(int fd)
{
    int r; do { r=fsync(fd); } while(r && errno==EINTR); return r;
}
static int record_io(int fd, void *buf, unsigned n, uint64_t off, int read_op)
{
    unsigned done=0;
    while(done<n) {
        ssize_t r=read_op ? pread(fd,(char*)buf+done,n-done,(off_t)off+done)
                         : pwrite(fd,(char*)buf+done,n-done,(off_t)off+done);
        if(r<0 && errno==EINTR) continue;
        if(r<=0) { if(!r) errno=EIO; return -1; }
        done+=(unsigned)r;
    }
    return 0;
}
static int file_name(struct worker *w, unsigned loc, char *path)
{
    if(snprintf(path,4096,"%s/%u.frag",w->dir,loc)>=4096) {errno=ENAMETOOLONG;return -1;}
    return 0;
}
static int container_mode(struct worker *w) {return strcmp(w->c->layout,"files")!=0;}
static int open_location(struct worker *w, unsigned loc, int create)
{
    if(container_mode(w)) return w->segments[loc/w->per_segment].fd;
    char path[4096]; if(file_name(w,loc,path)) return -1;
    return open(path,O_RDWR|(create ? O_CREAT|O_EXCL:0)|(w->c->direct?O_DIRECT:0),0600);
}
static uint64_t offset(struct worker *w,unsigned loc)
{
    return container_mode(w)?(uint64_t)(loc%w->per_segment)*w->c->stride:0;
}
static int close_location(struct worker *w,int fd,int rc)
{
    int saved=errno;
    if(!container_mode(w) && close(fd) && !rc) return -1;
    if(rc) errno=saved;
    return rc;
}
static void make_record(struct worker *w,unsigned object,uint64_t generation)
{
    uint64_t identity=((uint64_t)w->id<<32)|object;
    uint64_t x=identity^generation^UINT64_C(0x9e3779b97f4a7c15);
    for(unsigned i=0;i<w->c->payload;i++) {x^=x<<13;x^=x>>7;x^=x<<17;w->buf[i]=(uint8_t)x;}
    memcpy(w->buf,&identity,8); memcpy(w->buf+8,&generation,8);
    memset(w->buf+w->c->payload,0,w->c->stride-w->c->payload);
    efs_hash(w->buf,w->c->payload,w->buf+w->c->payload);
}
static int verify_location(struct worker *w,unsigned object,unsigned loc,uint64_t generation,const uint8_t *expected)
{
    int fd=open_location(w,loc,0); if(fd<0)return -1;
    int rc=record_io(fd,w->verify,w->c->stride,offset(w,loc),1);
    if(close_location(w,fd,rc))return -1;
    uint8_t digest[32]; uint64_t id,gen;
    memcpy(&id,w->verify,8); memcpy(&gen,w->verify+8,8);
    efs_hash(w->verify,w->c->payload,digest);
    if(id!=(((uint64_t)w->id<<32)|object) || gen!=generation ||
       memcmp(expected,digest,32) || memcmp(digest,w->verify+w->c->payload,32)) {errno=EIO;return -1;}
    return 0;
}
static int flush_worker(struct worker *w,struct sample *s)
{
    if(container_mode(w)) {
        for(unsigned i=0;i<w->nsegments;i++) if(w->segments[i].dirty) {
            if(sync_file(w->segments[i].fd))return -1;
            w->segments[i].dirty=0; s->syncs++;
        }
    } else {
        /* File bodies are individually fsynced before closing. batching only
         * amortizes the parent directory sync; it never skips body durability. */
    }
    if(sync_file(w->dfd))return -1;
    s->syncs++; return 0;
}
static int create_segment(struct worker *w,unsigned i)
{
    struct segment *g=&w->segments[i];
    if(snprintf(g->path,sizeof(g->path),"%s/container-%u",w->dir,i)>=4096)return -1;
    g->fd=open(g->path,O_RDWR|O_CREAT|O_EXCL|(w->c->direct?O_DIRECT:0),0600);
    if(g->fd<0)return -1;
    if(w->c->hint) {
        struct fsxattr a;
        if(ioctl(g->fd,FS_IOC_FSGETXATTR,&a))return -1;
        a.fsx_xflags|=FS_XFLAG_EXTSIZE; a.fsx_extsize=(uint32_t)w->c->hint;
        if(ioctl(g->fd,FS_IOC_FSSETXATTR,&a))return -1;
    }
    if(ftruncate(g->fd,(off_t)w->c->container))return -1;
    if(w->c->preallocate) {
        int err=posix_fallocate(g->fd,0,(off_t)w->c->container);
        if(err){errno=err;return -1;}
    }
    g->dirty=1; g->live=0; return 0;
}
static int retire_location(struct worker *w,unsigned loc,struct sample *s)
{
    if(w->state[loc]!=1){errno=EIO;return -1;}
    if(!container_mode(w)) {
        char path[4096]; struct stat st;
        if(file_name(w,loc,path)||stat(path,&st)||unlink(path))return -1;
        w->state[loc]=0; s->reclaimed+=(uint64_t)st.st_blocks*512;
    } else if(!strcmp(w->c->layout,"punch")) {
        struct segment *g=&w->segments[loc/w->per_segment];
        struct stat before,after;
        if(fstat(g->fd,&before)||fallocate(g->fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)offset(w,loc),w->c->stride)||fstat(g->fd,&after))return -1;
        g->dirty=1;g->live--;w->state[loc]=0;
        if(before.st_blocks>after.st_blocks)s->reclaimed+=(uint64_t)(before.st_blocks-after.st_blocks)*512;
    } else {
        w->segments[loc/w->per_segment].live--;w->state[loc]=2;w->dead++;
        if(w->dead>s->max_dead)s->max_dead=w->dead;
    }
    s->retired++; return 0;
}
static uint64_t allocated(struct worker *w)
{
    struct stat st;uint64_t total=0;
    if(container_mode(w)) {
        for(unsigned i=0;i<w->nsegments;i++) {
            if(fstat(w->segments[i].fd,&st))return UINT64_MAX;
            total+=(uint64_t)st.st_blocks*512;
        }
    } else {
        DIR *d=opendir(w->dir);if(!d)return UINT64_MAX;struct dirent *de;
        while((de=readdir(d))) {
            if(de->d_name[0]=='.')continue;
            if(fstatat(w->dfd,de->d_name,&st,0)){closedir(d);return UINT64_MAX;}
            total+=(uint64_t)st.st_blocks*512;
        }
        closedir(d);
    }
    return total;
}
static int clean_segment(struct worker *w,struct sample *s)
{
    uint64_t start=now_ns(); unsigned victim=UINT32_MAX;
    for(unsigned i=0;i<w->nsegments;i++) if(i!=w->spare &&
       (victim==UINT32_MAX||w->segments[i].live<w->segments[victim].live))victim=i;
    if(victim==UINT32_MAX || w->segments[victim].live>=w->per_segment){errno=ENOSPC;return -1;}
    unsigned dest=w->spare, copied=0;
    for(unsigned i=0;i<w->live;i++) {
        struct object *o=&w->objects[i];
        if(o->location/w->per_segment!=victim)continue;
        if(verify_location(w,i,o->location,o->generation,o->digest))return -1;
        if(record_io(w->segments[dest].fd,w->verify,w->c->stride,(uint64_t)copied*w->c->stride,0))return -1;
        s->copy_bytes+=w->c->stride; s->copied++;copied++;
    }
    /* Bodies precede relocation in each/batch modes. No durable index is
     * modeled; this experiment does not claim restart recovery. */
    if(strcmp(w->c->persist,"none")) {
        if(sync_file(w->segments[dest].fd))return -1;
        s->syncs++;
    }
    copied=0;
    for(unsigned i=0;i<w->live;i++) {
        struct object *o=&w->objects[i];
        if(o->location/w->per_segment==victim) {
            o->location=dest*w->per_segment+copied;
            w->state[o->location]=1;copied++;
        }
    }
    w->segments[dest].live=copied;w->segments[dest].dirty=1;
    struct stat st;if(fstat(w->segments[victim].fd,&st))return -1;
    s->reclaimed+=(uint64_t)st.st_blocks*512;
    if(unlink(w->segments[victim].path))return -1;
    if(close(w->segments[victim].fd))return -1;
    w->segments[victim].fd=-1;
    for(unsigned i=victim*w->per_segment;i<(victim+1)*w->per_segment;i++) {
        if(w->state[i]==2)w->dead--;
        w->state[i]=0;
    }
    if(create_segment(w,victim))return -1;
    w->spare=victim;
    if(strcmp(w->c->persist,"none") && flush_worker(w,s))return -1;
    s->cleanings++; lat_add(&s->gc,(now_ns()-start)/1000);return 0;
}
static int free_location(struct worker *w,struct sample *s,unsigned *out)
{
    for(unsigned pass=0;pass<2;pass++) {
        for(unsigned j=0;j<w->slots;j++) {
            unsigned i=(w->cursor+j)%w->slots;
            if(container_mode(w)&&i/w->per_segment==w->spare)continue;
            if(!w->state[i]){*out=i;w->cursor=(i+1)%w->slots;return 0;}
        }
        if(strcmp(w->c->layout,"compact")||clean_segment(w,s))return -1;
    }
    errno=ENOSPC;return -1;
}
static int write_object(struct worker *w,unsigned object,int setup,struct sample *s)
{
    struct object *o=&w->objects[object];unsigned loc;
    if(free_location(w,s,&loc))return -1;
    uint64_t gen=o->generation+1;
    make_record(w,object,gen);
    int fd=open_location(w,loc,1);if(fd<0)return -1;
    int rc=record_io(fd,w->buf,w->c->stride,offset(w,loc),0);
    if(!rc && !container_mode(w) && (setup||strcmp(w->c->persist,"none"))) {
        rc=sync_file(fd);s->syncs++;
    }
    if(close_location(w,fd,rc))return -1;
    if(container_mode(w)) {w->segments[loc/w->per_segment].dirty=1;w->segments[loc/w->per_segment].live++;}
    if(!setup && strcmp(w->c->persist,"none") && container_mode(w) && !strcmp(w->c->persist,"each")) {
        if(sync_file(w->segments[loc/w->per_segment].fd))return -1;
        s->syncs++;
    }
    unsigned old=o->location;
    w->state[loc]=1;o->location=loc;o->generation=gen;
    memcpy(o->digest,w->buf+w->c->payload,32);
    if(!setup && retire_location(w,old,s))return -1;
    s->writes++;
    if(!setup && strcmp(w->c->persist,"none") &&
       (!strcmp(w->c->persist,"each")||s->writes%w->c->batch==0))return flush_worker(w,s);
    return 0;
}
static int cycles(struct worker *w,unsigned count,struct sample *s)
{
    for(unsigned i=0;i<count;i++) {
        unsigned object=(unsigned)(random_next(w)%w->live);
        int reading=random_next(w)%100<w->c->reads;
        uint64_t start=now_ns();
        if(reading) {
            struct object *o=&w->objects[object];
            if(verify_location(w,object,o->location,o->generation,o->digest))return -1;
            s->reads++;lat_add(&s->read,(now_ns()-start)/1000);
        } else {
            if(write_object(w,object,0,s))return -1;
            lat_add(&s->write,(now_ns()-start)/1000);
        }
    }
    /* Final batch barrier is part of elapsed time, not an uncounted flush. */
    if(strcmp(w->c->persist,"none") && flush_worker(w,s))return -1;
    return 0;
}
static void *run(void *arg)
{
    struct worker *w=arg;
    pthread_mutex_lock(&gate.mu);gate.ready++;pthread_cond_broadcast(&gate.cv);
    while(!gate.go)pthread_cond_wait(&gate.cv,&gate.mu);
    pthread_mutex_unlock(&gate.mu);
    w->start_ns=now_ns();
    if(cycles(w,w->c->steps,&w->stats))w->error=errno?errno:EIO;
    w->end_ns=now_ns();return NULL;
}
static int empty_dir(const char *root)
{
    struct stat st;if(lstat(root,&st)||!S_ISDIR(st.st_mode))return 0;
    DIR *d=opendir(root);if(!d)return 0;struct dirent *de;int empty=1;
    while((de=readdir(d)))if(strcmp(de->d_name,".")&&strcmp(de->d_name,"..")){empty=0;break;}
    closedir(d);return empty;
}
static int integer(const char *v,uint64_t *out)
{
    char *end;errno=0;unsigned long long n=strtoull(v,&end,10);
    if(errno||!*v||*v=='-'||*end)return -1;
    *out=n;return 0;
}
static int prepare(struct worker *w)
{
    struct config *c=w->c;
    w->per_segment=(unsigned)(c->container/c->stride);
    unsigned data_segments=(unsigned)(c->capacity/c->qd/c->container);
    if(container_mode(w))w->nsegments=data_segments+1;
    w->slots=container_mode(w)?w->nsegments*w->per_segment:(unsigned)(c->capacity/c->qd/c->stride);
    unsigned data_slots=container_mode(w)?data_segments*w->per_segment:w->slots;
    w->live=(unsigned)((uint64_t)data_slots*c->occupancy/100);
    if(w->live<2 || w->live>=data_slots || w->slots>1048576){errno=EINVAL;return -1;}
    if(snprintf(w->dir,sizeof(w->dir),"%s/churn-%u",c->root,w->id)>=4096)return -1;
    if(mkdir(w->dir,0700))return -1;
    w->owns_dir=1;w->dfd=open(w->dir,O_RDONLY|O_DIRECTORY);if(w->dfd<0)return -1;
    if(c->filestreams) {
        struct fsxattr a;if(ioctl(w->dfd,FS_IOC_FSGETXATTR,&a))return -1;
        a.fsx_xflags|=FS_XFLAG_FILESTREAM;
        if(ioctl(w->dfd,FS_IOC_FSSETXATTR,&a))return -1;
    }
    w->objects=calloc(w->live,sizeof(*w->objects));w->state=calloc(w->slots,1);
    w->segments=calloc(w->nsegments,sizeof(*w->segments));
    if(w->segments)for(unsigned i=0;i<w->nsegments;i++)w->segments[i].fd=-1;
    if(!w->objects||!w->state||(w->nsegments&&!w->segments))return -1;
    if(posix_memalign((void**)&w->buf,4096,c->stride)||posix_memalign((void**)&w->verify,4096,c->stride))return -1;
    for(unsigned i=0;i<w->nsegments;i++)if(create_segment(w,i))return -1;
    w->spare=data_segments;
    struct sample *initial=calloc(1,sizeof(*initial));if(!initial)return -1;
    for(unsigned i=0;i<w->live;i++)if(write_object(w,i,1,initial)){free(initial);return -1;}
    if(flush_worker(w,initial)){free(initial);return -1;}
    if(cycles(w,c->warmup,initial)){free(initial);return -1;}
    /* Warmup is always flushed outside the measured clock, including none. */
    if(container_mode(w)) {if(flush_worker(w,initial)){free(initial);return -1;}}
    else for(unsigned i=0;i<w->live;i++) {
        int fd=open_location(w,w->objects[i].location,0);if(fd<0){free(initial);return -1;}
        int rc=sync_file(fd);if(close_location(w,fd,rc)){free(initial);return -1;}
    }
    free(initial);w->before=allocated(w);return w->before==UINT64_MAX?-1:0;
}
static void cleanup(struct worker *w)
{
    if(w->segments)for(unsigned i=0;i<w->nsegments;i++)if(w->segments[i].fd>=0)close(w->segments[i].fd);
    if(w->owns_dir) {
        DIR *d=opendir(w->dir);struct dirent *de;
        if(d){while((de=readdir(d)))if(de->d_name[0]!='.')unlinkat(w->dfd,de->d_name,0);closedir(d);}
        if(w->dfd>=0)close(w->dfd);
        rmdir(w->dir);
    }
    free(w->objects);free(w->state);free(w->segments);free(w->buf);free(w->verify);
}
int efs_bench_churn_main(int argc,char **argv)
{
    struct config c={.layout="files",.persist="batch",.capacity=UINT64_C(3)<<30,
        .container=UINT64_C(1)<<30,.payload=65536,.occupancy=80,.qd=1,
        .steps=10000,.warmup=1000,.batch=32,.reads=70};
    int seen=0;
    for(int i=1;i<argc;i++) {
        const char *key=argv[i];uint64_t n;
        if(!strcmp(key,"--help")) {
            puts("Independent mixed storage churn (private empty scratch root only).\n"
                 "--bench churn --storage EMPTY_ROOT --layout files|punch|compact\n"
                 "--capacity-bytes N --container-bytes N (default 1 GiB) --payload-size N\n"
                 "--occupancy 50|80|95 --qd N --steps N --warmup N --read-percent N\n"
                 "--persist none|each|batch --batch-size N --direct-io|--no-direct-io\n"
                 "--preallocate (containers only) --extent-hint N --filestreams\n"
                 "All sizes are integer bytes; one reserved cleaning container per worker.\n"
                 "Occupancy is experiment capacity, not host filesystem fullness.\n"
                 "Worker-local synchronous GC; in-memory index; no crash recovery claim.");return 0;
        }
        if(!strcmp(key,"--direct-io")){c.direct=1;continue;}
        if(!strcmp(key,"--no-direct-io")){c.direct=0;continue;}
        if(!strcmp(key,"--preallocate")){c.preallocate=1;continue;}
        if(!strcmp(key,"--filestreams")){c.filestreams=1;continue;}
        if(++i==argc)goto invalid;
        const char *v=argv[i];
        if(!strcmp(key,"--bench")){if(seen++||strcmp(v,"churn"))goto invalid;continue;}
        if(!strcmp(key,"--storage")){if(c.root)goto invalid;c.root=v;continue;}
        if(!strcmp(key,"--layout")){c.layout=v;continue;}
        if(!strcmp(key,"--persist")){c.persist=v;continue;}
        if(integer(v,&n))goto invalid;
        if(!strcmp(key,"--capacity-bytes"))c.capacity=n;
        else if(!strcmp(key,"--container-bytes"))c.container=n;
        else if(!strcmp(key,"--extent-hint"))c.hint=n;
        else if(n>UINT32_MAX)goto invalid;
        else if(!strcmp(key,"--payload-size"))c.payload=(unsigned)n;
        else if(!strcmp(key,"--occupancy"))c.occupancy=(unsigned)n;
        else if(!strcmp(key,"--qd"))c.qd=(unsigned)n;
        else if(!strcmp(key,"--steps"))c.steps=(unsigned)n;
        else if(!strcmp(key,"--warmup"))c.warmup=(unsigned)n;
        else if(!strcmp(key,"--read-percent"))c.reads=(unsigned)n;
        else if(!strcmp(key,"--batch-size"))c.batch=(unsigned)n;
        else goto invalid;
    }
    if(!seen||!c.root||!c.qd||c.qd>64||!c.steps||c.steps>10000000||c.warmup>1000000||
       !c.batch||c.batch>100000||c.reads<1||c.reads>99||c.payload<4096||c.payload>1048576||c.payload%4096||
       (c.occupancy!=50&&c.occupancy!=80&&c.occupancy!=95)||c.container<c.payload*4ull||c.container>(UINT64_C(4)<<30)||
       c.container%4096||!c.capacity||c.capacity>(UINT64_C(64)<<30)||
       (strcmp(c.layout,"files")&&strcmp(c.layout,"punch")&&strcmp(c.layout,"compact"))||
       (strcmp(c.persist,"none")&&strcmp(c.persist,"each")&&strcmp(c.persist,"batch"))||
       (!strcmp(c.layout,"files")&&(c.preallocate||c.hint))||c.hint>UINT32_MAX||c.hint%4096)goto invalid;
    c.stride=c.payload+4096;
    if(strcmp(c.layout,"files") && c.capacity/c.qd/c.container<2)goto invalid;
    if(!empty_dir(c.root)){fprintf(stderr,"churn: root must be an existing empty directory: %s\n",c.root);return 1;}
    struct statvfs space;if(statvfs(c.root,&space))return 1;
    uint64_t required=c.capacity+(uint64_t)c.qd*c.container;
    if((uint64_t)space.f_bavail*space.f_frsize<required*2){fprintf(stderr,"churn: need at least twice bounded fixture capacity free\n");return 1;}
    struct worker *w=calloc(c.qd,sizeof(*w));pthread_t *threads=calloc(c.qd,sizeof(*threads));
    struct sample *total=calloc(1,sizeof(*total));if(!w||!threads||!total){free(w);free(threads);free(total);return 1;}
    for(unsigned i=0;i<c.qd;i++){w[i].dfd=-1;w[i].c=&c;w[i].id=i;w[i].rng=UINT64_C(0x12345678)+i;}
    unsigned made=0;int rc=1,errors=0;uint64_t start=0,end=0,verified=0,live=0,before=0,after=0,dead=0,mem=0;
    for(unsigned i=0;i<c.qd;i++)if(prepare(&w[i])){fprintf(stderr,"churn setup worker %u: %s\n",i,strerror(errno));goto done;}
    gate.ready=0;gate.go=0;
    for(unsigned i=0;i<c.qd;i++) {
        int err=pthread_create(&threads[i],NULL,run,&w[i]);
        if(err){errno=err;errors++;goto release;}made++;
    }
    pthread_mutex_lock(&gate.mu);while(gate.ready<c.qd)pthread_cond_wait(&gate.cv,&gate.mu);pthread_mutex_unlock(&gate.mu);
release:
    start=now_ns();pthread_mutex_lock(&gate.mu);gate.go=1;pthread_cond_broadcast(&gate.cv);pthread_mutex_unlock(&gate.mu);
    for(unsigned i=0;i<made;i++)pthread_join(threads[i],NULL);
    end=now_ns();if(made!=c.qd)goto done;
    for(unsigned i=0;i<c.qd;i++) {
        struct worker *v=&w[i]; struct sample *s=&v->stats;
        if(v->error){errors++;fprintf(stderr,"churn worker %u: %s\n",i,strerror(v->error));}
        if(s->reads+s->writes!=c.steps||!s->reads||!s->writes)errors++;
        lat_merge(&total->read,&s->read);lat_merge(&total->write,&s->write);lat_merge(&total->gc,&s->gc);
#define ADD(field) total->field+=s->field
        ADD(reads);ADD(writes);ADD(retired);ADD(copied);ADD(copy_bytes);ADD(reclaimed);ADD(syncs);ADD(cleanings);
#undef ADD
        if(s->max_dead>total->max_dead)total->max_dead=s->max_dead;
        /* none's flush is outside elapsed time; measure allocation only afterwards. */
        struct sample *flush=calloc(1,sizeof(*flush));if(!flush){errors++;break;}
        if(flush_worker(v,flush))errors++;
        if(!container_mode(v))for(unsigned j=0;j<v->live;j++) {
            int fd=open_location(v,v->objects[j].location,0);if(fd<0){errors++;break;}
            int r=sync_file(fd);if(close_location(v,fd,r)){errors++;break;}
        }
        free(flush);
        for(unsigned j=0;j<v->live;j++) {
            struct object *o=&v->objects[j];
            if(v->state[o->location]!=1||verify_location(v,j,o->location,o->generation,o->digest)){errors++;break;}
            verified++;
        }
        unsigned states=0;for(unsigned j=0;j<v->slots;j++)if(v->state[j]==1)states++;
        if(states!=v->live)errors++;
        v->after=allocated(v);if(v->after==UINT64_MAX)errors++;
        before+=v->before;after+=v->after;dead+=v->dead;live+=v->live;
        mem+=sizeof(*v)+(uint64_t)v->slots+(uint64_t)v->live*sizeof(struct object)+
             (uint64_t)v->nsegments*sizeof(struct segment)+2ull*c.stride;
    }
    struct rusage usage;getrusage(RUSAGE_SELF,&usage);double wall=(end-start)/1e9;
    printf("%s kind=churn model=independent layout=%s persist=%s direct=%d payload_bytes=%u record_bytes=%u container_bytes=%llu capacity_bytes=%llu occupancy_pct=%u qd=%u steps=%u warmup=%u batch=%u read_pct=%u preallocate=%d extent_hint=%llu filestreams=%d wall_s=%.6f reads=%llu writes=%llu retired=%llu ops_s=%.3f payload_GiB_s=%.6f read_p50_us=%llu read_p99_us=%llu read_max_us=%llu write_p50_us=%llu write_p99_us=%llu write_max_us=%llu gc_p99_us=%llu cleanings=%llu copied_records=%llu copy_bytes=%llu released_allocated_bytes=%llu allocated_before=%llu allocated_after=%llu dead_records=%llu max_dead_per_worker=%llu live_objects=%llu verified_objects=%llu syncs=%llu model_bytes=%llu rss_kib=%ld errors=%d gc=worker_local index=in_memory\n",
      errors?"BENCH_FAIL":"BENCH_OK",c.layout,c.persist,c.direct,c.payload,c.stride,(unsigned long long)c.container,(unsigned long long)c.capacity,c.occupancy,c.qd,c.steps,c.warmup,c.batch,c.reads,c.preallocate,(unsigned long long)c.hint,c.filestreams,wall,
      (unsigned long long)total->reads,(unsigned long long)total->writes,(unsigned long long)total->retired,(total->reads+total->writes)/wall,(double)(total->reads+total->writes)*c.payload/(1ull<<30)/wall,
      (unsigned long long)lat_percentile(&total->read,50,NULL),(unsigned long long)lat_percentile(&total->read,99,NULL),(unsigned long long)total->read.max,
      (unsigned long long)lat_percentile(&total->write,50,NULL),(unsigned long long)lat_percentile(&total->write,99,NULL),(unsigned long long)total->write.max,
      (unsigned long long)lat_percentile(&total->gc,99,NULL),(unsigned long long)total->cleanings,(unsigned long long)total->copied,(unsigned long long)total->copy_bytes,(unsigned long long)total->reclaimed,
      (unsigned long long)before,(unsigned long long)after,(unsigned long long)dead,(unsigned long long)total->max_dead,(unsigned long long)live,(unsigned long long)verified,(unsigned long long)total->syncs,(unsigned long long)mem,usage.ru_maxrss,errors);
    rc=errors?1:0;
done:
    for(unsigned i=0;i<c.qd;i++)cleanup(&w[i]);
    free(w);free(threads);free(total);return rc;
invalid:
    fprintf(stderr,"churn: invalid arguments (use --bench churn --help)\n");return 2;
}
