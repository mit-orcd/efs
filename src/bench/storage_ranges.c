/* Concurrent positional I/O to a shared, prepopulated container. No EFS code. */
#include "storage_ranges.h"
#include "latency.h"
#include "efs/checksum.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

struct range_slot { pthread_rwlock_t lock; uint64_t generation; uint8_t digest[32]; };
struct range_config {
    const char *root,*access,*persist;
    uint64_t bytes,sequence;
    unsigned payload,stride,slots,qd,steps,seconds,reads,batch;
    int fd,direct,split,punch;
    struct range_slot *model;
    pthread_mutex_t gate_lock;
    pthread_cond_t gate_cv;
    unsigned ready;
    int go;
};
struct range_worker {
    struct range_config *c;
    unsigned id;
    uint8_t *buf;
    uint64_t rng,reads,writes,torn,lock_ns,start,end;
    uint64_t read_io_max,write_io_max,sync_max,syncs;
    struct lat_vec cycle,wait;
    int error;
};
static uint64_t clock_ns(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
static uint64_t rng_next(struct range_worker *w) {
    uint64_t x=w->rng;x^=x<<13;x^=x>>7;x^=x<<17;return w->rng=x;
}
static int io_record(int fd,uint8_t *buf,unsigned n,uint64_t off,int read_op) {
    unsigned done=0;
    while(done<n) {
        ssize_t r=read_op?pread(fd,buf+done,n-done,off+done):pwrite(fd,buf+done,n-done,off+done);
        if(r<0&&errno==EINTR)continue;
        if(r<=0){if(!r)errno=EIO;return -1;}done+=(unsigned)r;
    }
    return 0;
}
static int sync_body(int fd) {int r;do{r=fsync(fd);}while(r&&errno==EINTR);return r;}
static void fill_record(struct range_config *c,uint8_t *buf,unsigned slot,uint64_t gen) {
    uint64_t x=gen^slot^UINT64_C(0x9e3779b97f4a7c15);
    for(unsigned i=0;i<c->payload;i++){x^=x<<13;x^=x>>7;x^=x<<17;buf[i]=(uint8_t)x;}
    uint64_t id=slot;memcpy(buf,&id,8);memcpy(buf+8,&gen,8);
    memset(buf+c->payload,0,4096);efs_hash(buf,c->payload,buf+c->payload);
}
static int check_record(struct range_config *c,uint8_t *buf,unsigned slot,int expected) {
    uint64_t id,gen;uint8_t hash[32];memcpy(&id,buf,8);memcpy(&gen,buf+8,8);
    efs_hash(buf,c->payload,hash);
    if(id!=slot||memcmp(hash,buf+c->payload,32))return -1;
    if(expected&&(gen!=c->model[slot].generation||memcmp(hash,c->model[slot].digest,32)))return -1;
    return 0;
}
static int write_record(struct range_config *c,uint8_t *buf,unsigned slot) {
    uint64_t off=(uint64_t)slot*c->stride;
    if(c->punch&&fallocate(c->fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,off,c->stride))return -1;
    if(!c->split)return io_record(c->fd,buf,c->stride,off,0);
    unsigned first=(c->payload/2/4096)*4096;
    if(io_record(c->fd,buf,first,off,0))return -1;
    /* Deliberately expose a multi-write update window; not an atomicity test
     * of one pwrite. The same split under a range lock must remain correct. */
    usleep(50);
    return io_record(c->fd,buf+first,c->stride-first,off+first,0);
}
static int worker_sync(struct range_worker *w) {
    uint64_t start=clock_ns();int rc=sync_body(w->c->fd);
    uint64_t us=(clock_ns()-start)/1000;
    if(us>w->sync_max)w->sync_max=us;
    w->syncs++;return rc;
}
static void *range_run(void *arg) {
    struct range_worker *w=arg;struct range_config *c=w->c;
    int hot_read=!strcmp(c->access,"hot-read");
    int locked=!strcmp(c->access,"hot-locked");
    int diagnostic=!strcmp(c->access,"hot-unlocked");
    pthread_mutex_lock(&c->gate_lock);c->ready++;pthread_cond_broadcast(&c->gate_cv);
    while(!c->go)pthread_cond_wait(&c->gate_cv,&c->gate_lock);
    pthread_mutex_unlock(&c->gate_lock);
    w->start=clock_ns();uint64_t until=w->start+(uint64_t)c->seconds*1000000000;
    for(unsigned i=0;i<c->steps;i++) {
        if(c->seconds&&clock_ns()>=until)break;
        unsigned slot=0;
        if(!strcmp(c->access,"disjoint")) {
            unsigned first=(uint64_t)c->slots*w->id/c->qd;
            unsigned last=(uint64_t)c->slots*(w->id+1)/c->qd;
            slot=first+rng_next(w)%(last-first);
        }
        int reading=hot_read||rng_next(w)%100<c->reads;
        uint64_t begin=clock_ns(),acquired=begin;
        if(locked) {
            int err=reading?pthread_rwlock_rdlock(&c->model[slot].lock):pthread_rwlock_wrlock(&c->model[slot].lock);
            if(err){w->error=err;break;}acquired=clock_ns();
            lat_add(&w->wait,(acquired-begin)/1000);w->lock_ns+=acquired-begin;
        }
        int rc=0;
        if(reading) {
            uint64_t io_start=clock_ns();
            rc=io_record(c->fd,w->buf,c->stride,(uint64_t)slot*c->stride,1);
            uint64_t us=(clock_ns()-io_start)/1000;if(us>w->read_io_max)w->read_io_max=us;
            if(!rc&&check_record(c,w->buf,slot,!diagnostic)) {
                if(diagnostic)w->torn++;else{errno=EIO;rc=-1;}
            }
            if(!rc)w->reads++;
        } else {
            uint64_t gen=diagnostic?__atomic_add_fetch(&c->sequence,1,__ATOMIC_RELAXED):c->model[slot].generation+1;
            fill_record(c,w->buf,slot,gen);
            uint64_t io_start=clock_ns();rc=write_record(c,w->buf,slot);
            uint64_t us=(clock_ns()-io_start)/1000;if(us>w->write_io_max)w->write_io_max=us;
            if(!rc&&!diagnostic) {
                c->model[slot].generation=gen;
                memcpy(c->model[slot].digest,w->buf+c->payload,32);
            }
            if(!rc) {
                w->writes++;
                if(!strcmp(c->persist,"each")||(!strcmp(c->persist,"batch")&&w->writes%c->batch==0))rc=worker_sync(w);
            }
        }
        int saved=errno;if(locked)pthread_rwlock_unlock(&c->model[slot].lock);errno=saved;
        if(rc){w->error=errno?errno:EIO;break;}
        lat_add(&w->cycle,(clock_ns()-begin)/1000);
    }
    if(strcmp(c->persist,"none")&&worker_sync(w))w->error=errno;
    w->end=clock_ns();return NULL;
}
static int number(const char *v,uint64_t *out) {
    char *end;errno=0;unsigned long long n=strtoull(v,&end,10);
    if(errno||!*v||*v=='-'||*end)return -1;
    *out=n;return 0;
}
static int empty_root(const char *root) {
    struct stat st;if(lstat(root,&st)||!S_ISDIR(st.st_mode))return 0;
    DIR *d=opendir(root);if(!d)return 0;struct dirent *de;int empty=1;
    while((de=readdir(d)))if(strcmp(de->d_name,".")&&strcmp(de->d_name,"..")){empty=0;break;}
    closedir(d);return empty;
}
int efs_bench_ranges_main(int argc,char **argv) {
    struct range_config c={.access="disjoint",.persist="batch",.bytes=UINT64_C(1)<<30,
        .payload=65536,.qd=4,.steps=10000,.reads=50,.batch=32,.fd=-1,
        .gate_lock=PTHREAD_MUTEX_INITIALIZER,.gate_cv=PTHREAD_COND_INITIALIZER};
    int seen=0;
    for(int i=1;i<argc;i++) {
        const char *key=argv[i];uint64_t n;
        if(!strcmp(key,"--help")) {
            puts("Shared-container range contention (independent benchmark).\n"
                 "--bench ranges --storage EMPTY_ROOT --access disjoint|hot-read|hot-locked|hot-unlocked\n"
                 "--container-bytes N --payload-size N --qd N --steps N --seconds N\n"
                 "--persist none|each|batch --batch-size N --read-percent N\n"
                 "--direct-io|--no-direct-io --split-write --punch-before-write\n"
                 "hot-unlocked is diagnostic: mixed reads are observations, not a correct storage protocol.\n"
                 "split-write deliberately uses two writes with a delay; it does not test one-write atomicity.");return 0;
        }
        if(!strcmp(key,"--direct-io")){c.direct=1;continue;}
        if(!strcmp(key,"--no-direct-io")){c.direct=0;continue;}
        if(!strcmp(key,"--split-write")){c.split=1;continue;}
        if(!strcmp(key,"--punch-before-write")){c.punch=1;continue;}
        if(++i==argc)goto invalid;
        const char *v=argv[i];
        if(!strcmp(key,"--bench")){if(seen++||strcmp(v,"ranges"))goto invalid;continue;}
        if(!strcmp(key,"--storage")){if(c.root)goto invalid;c.root=v;continue;}
        if(!strcmp(key,"--access")){c.access=v;continue;}
        if(!strcmp(key,"--persist")){c.persist=v;continue;}
        if(number(v,&n))goto invalid;
        if(!strcmp(key,"--container-bytes"))c.bytes=n;
        else if(n>UINT32_MAX)goto invalid;
        else if(!strcmp(key,"--payload-size"))c.payload=n;
        else if(!strcmp(key,"--qd"))c.qd=n;
        else if(!strcmp(key,"--steps"))c.steps=n;
        else if(!strcmp(key,"--seconds"))c.seconds=n;
        else if(!strcmp(key,"--read-percent"))c.reads=n;
        else if(!strcmp(key,"--batch-size"))c.batch=n;
        else goto invalid;
    }
    if(!seen||!c.root||c.payload<8192||c.payload>1048576||c.payload%4096||!c.qd||c.qd>64||
       !c.steps||c.steps>10000000||c.seconds>3600||c.reads<1||c.reads>99||!c.batch||c.batch>100000||
       c.bytes<65536||c.bytes>(UINT64_C(4)<<30)||c.bytes%4096||
       (strcmp(c.access,"disjoint")&&strcmp(c.access,"hot-read")&&strcmp(c.access,"hot-locked")&&strcmp(c.access,"hot-unlocked"))||
       (strcmp(c.persist,"none")&&strcmp(c.persist,"each")&&strcmp(c.persist,"batch"))||
       ((c.split||c.punch)&&strcmp(c.access,"hot-locked")&&strcmp(c.access,"hot-unlocked")))goto invalid;
    c.stride=c.payload+4096;c.slots=c.bytes/c.stride;
    if(c.slots<c.qd||!empty_root(c.root))goto invalid;
    struct statvfs fs;if(statvfs(c.root,&fs)||(uint64_t)fs.f_bavail*fs.f_frsize<c.bytes*2)return 1;
    char path[4096];if(snprintf(path,sizeof(path),"%s/shared-container",c.root)>=4096)return 1;
    c.model=calloc(c.slots,sizeof(*c.model));
    struct range_worker *w=calloc(c.qd,sizeof(*w));pthread_t *threads=calloc(c.qd,sizeof(*threads));
    if(!c.model||!w||!threads){free(c.model);free(w);free(threads);return 1;}
    int rc=1,errors=0;unsigned locks=0,made=0;uint8_t *setup=NULL;
    for(unsigned i=0;i<c.slots;i++){if(pthread_rwlock_init(&c.model[i].lock,NULL))goto done;locks++;}
    if(posix_memalign((void**)&setup,4096,c.stride))goto done;
    c.fd=open(path,O_RDWR|O_CREAT|O_EXCL|(c.direct?O_DIRECT:0),0600);if(c.fd<0)goto done;
    if(ftruncate(c.fd,c.bytes))goto done;
    for(unsigned i=0;i<c.slots;i++) {
        fill_record(&c,setup,i,0);memcpy(c.model[i].digest,setup+c.payload,32);
        if(io_record(c.fd,setup,c.stride,(uint64_t)i*c.stride,0))goto done;
    }
    if(sync_body(c.fd))goto done;
    for(unsigned i=0;i<c.qd;i++) {
        w[i].c=&c;w[i].id=i;w[i].rng=UINT64_C(0x12345678)+i;
        if(posix_memalign((void**)&w[i].buf,4096,c.stride))goto release;
        int err=pthread_create(&threads[i],NULL,range_run,&w[i]);if(err)goto release;made++;
    }
    pthread_mutex_lock(&c.gate_lock);while(c.ready<c.qd)pthread_cond_wait(&c.gate_cv,&c.gate_lock);pthread_mutex_unlock(&c.gate_lock);
release:;
    uint64_t start=clock_ns();pthread_mutex_lock(&c.gate_lock);c.go=1;pthread_cond_broadcast(&c.gate_cv);pthread_mutex_unlock(&c.gate_lock);
    for(unsigned i=0;i<made;i++)pthread_join(threads[i],NULL);
    uint64_t end=clock_ns();if(made!=c.qd)goto done;
    struct lat_vec *cycle=calloc(1,sizeof(*cycle)),*wait=calloc(1,sizeof(*wait));
    if(!cycle||!wait){free(cycle);free(wait);goto done;}
    uint64_t reads=0,writes=0,torn=0,verified=0,min_ops=UINT64_MAX,max_ops=0;
    uint64_t read_max=0,write_max=0,sync_max=0,syncs=0;
    int diagnostic=!strcmp(c.access,"hot-unlocked");
    for(unsigned i=0;i<c.qd;i++) {
        uint64_t ops=w[i].reads+w[i].writes;reads+=w[i].reads;writes+=w[i].writes;torn+=w[i].torn;
        if(ops<min_ops)min_ops=ops;
        if(ops>max_ops)max_ops=ops;
        if(w[i].error||(!c.seconds&&ops!=c.steps)||(c.seconds&&w[i].end-w[i].start<(uint64_t)c.seconds*1000000000)||!ops)errors++;
        lat_merge(cycle,&w[i].cycle);lat_merge(wait,&w[i].wait);
        if(w[i].read_io_max>read_max)read_max=w[i].read_io_max;
        if(w[i].write_io_max>write_max)write_max=w[i].write_io_max;
        if(w[i].sync_max>sync_max)sync_max=w[i].sync_max;
        syncs+=w[i].syncs;
    }
    if(sync_body(c.fd))errors++;
    for(unsigned i=0;i<c.slots;i++) {
        if(io_record(c.fd,setup,c.stride,(uint64_t)i*c.stride,1)){errors++;break;}
        if(check_record(&c,setup,i,!diagnostic)){if(diagnostic)torn++;else errors++;}
        else verified++;
    }
    if(!reads || (strcmp(c.access,"hot-read")&&!writes))errors++;
    struct stat st={0};if(fstat(c.fd,&st))errors++;
    printf("%s kind=ranges access=%s diagnostic=%d direct=%d persist=%s qd=%u payload_bytes=%u container_bytes=%llu seconds=%u steps=%u split=%d punch=%d wall_s=%.6f reads=%llu writes=%llu torn_observations=%llu verified_records=%llu records=%u min_worker_ops=%llu max_worker_ops=%llu ops_s=%.3f cycle_p99_us=%llu cycle_max_us=%llu lock_p99_us=%llu lock_max_us=%llu read_io_max_us=%llu write_io_max_us=%llu sync_max_us=%llu syncs=%llu allocated_bytes=%llu errors=%d index=in_memory\n",
      errors?"BENCH_FAIL":"BENCH_OK",c.access,diagnostic,c.direct,c.persist,c.qd,c.payload,(unsigned long long)c.bytes,c.seconds,c.steps,c.split,c.punch,(end-start)/1e9,
      (unsigned long long)reads,(unsigned long long)writes,(unsigned long long)torn,(unsigned long long)verified,c.slots,(unsigned long long)min_ops,(unsigned long long)max_ops,(reads+writes)/((end-start)/1e9),
      (unsigned long long)lat_percentile(cycle,99,NULL),(unsigned long long)cycle->max,(unsigned long long)lat_percentile(wait,99,NULL),(unsigned long long)wait->max,(unsigned long long)read_max,(unsigned long long)write_max,(unsigned long long)sync_max,(unsigned long long)syncs,(unsigned long long)st.st_blocks*512,errors);
    free(cycle);free(wait);rc=errors?1:0;
done:
    if(rc)fprintf(stderr,"ranges: failure: %s\n",strerror(errno));
    if(c.fd>=0){close(c.fd);unlink(path);}free(setup);
    for(unsigned i=0;i<c.qd;i++)free(w[i].buf);
    for(unsigned i=0;i<locks;i++)pthread_rwlock_destroy(&c.model[i].lock);
    free(w);free(threads);free(c.model);return rc;
invalid:
    fprintf(stderr,"ranges: invalid arguments or nonempty scratch root (use --bench ranges --help)\n");return 2;
}
