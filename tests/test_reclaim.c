/* W124 node reclaim journal: acceptance, fences, quota credit and settle,
 * replay after a crash, torn tails, fail-closed recovery, the journal cap and
 * the physical admission guard. Real fragment files on two roots. Each
 * scenario runs in a child process so a crash is a real _exit and replay
 * starts from fresh process state. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define EFS_BENCH_BUILD
#define EFS_SEGMENT_TEST_AUTHORITY
#define EFS_SEGMENT_TEST_PACK_STATE
#define server_gc_sync_roots real_gc_sync_roots
#include "../src/server/store.c"
#undef server_gc_sync_roots
static void (*after_root_sync)(void);
int server_gc_sync_roots(struct efsd_server *s);
int server_gc_sync_roots(struct efsd_server *s)
{int rc=real_gc_sync_roots(s);if(!rc&&after_root_sync)after_root_sync();return rc;}
#include "../src/server/reclaim.c"
#include "../src/server/segment_store.c"
#include "test_tmpdir.h"

/* No Raft host: recovery asks this oracle instead. */
static int host_active; /* read by the recovery thread: atomic */
static efs_ino_t dead_set[64];
static int ndead;
void server_raft_host_expiry_status(struct efs_expiry_status *out){memset(out,0,sizeof(*out));}
int server_raft_host_active(void){return __atomic_load_n(&host_active,__ATOMIC_ACQUIRE);}
int server_raft_host_inode_reaped(efs_ino_t ino,int *dead)
{
    *dead=0;
    for(int i=0;i<ndead;i++) if(dead_set[i]==ino) *dead=1;
    return EFS_OK;
}

/* Segment authority oracle (W125): one object whose phase a test sets;
 * phase 0 means absent. */
static struct efs_pack_object seg_obj;
static uint32_t seg_phase;
int server_raft_host_pack_object(uint32_t group, uint64_t id, uint64_t gen, struct efs_pack_object *out)
{
    if (!seg_phase || group != seg_obj.ledger.publication_group || id != seg_obj.ledger.segment_id ||
        gen != seg_obj.ledger.segment_generation) return EFS_ERR_NOT_FOUND;
    *out = seg_obj;
    out->ledger.phase = seg_phase;
    return EFS_OK;
}

/* K37: the segment's degraded-mark mask beside the object. */
static uint32_t seg_mark;
int server_raft_host_pack_state(uint32_t group, uint64_t id, uint64_t gen, struct efs_pack_object *out,
                                uint32_t *degraded)
{
    int rc = server_raft_host_pack_object(group, id, gen, out);
    if (degraded) *degraded = seg_mark;
    return rc;
}

static char roots[2][256];
static struct efsd_server S;
static struct efs_export EX={.id=1,.chunk_size=EFS_DEFAULT_CHUNK_SIZE};
static uint8_t *body;
static uint8_t sum[EFS_HASH_SIZE];

/* The startup inventory efsd.c runs: the local counter is the full scan,
 * never the advisory usage.bin snapshot. */
static void startup_inventory(struct efsd_server *s)
{
    uint64_t used = server_compute_local_usage(s);
    assert(used != UINT64_MAX);
    pthread_mutex_lock(&s->lock);
    struct efs_node *local = server_local_node(s);
    assert(local);
    local->used = used;
    pthread_mutex_unlock(&s->lock);
}
static void server_setup(void)
{
    memset(&S,0,sizeof(S));
    S.id=1;S.node_count=1;S.storage_path_count=2;S.quota=1ull<<30;
    pthread_mutex_init(&S.lock,NULL);S.nodes[0].id=1;
    strcpy(S.storage_paths[0],roots[0]);strcpy(S.storage_paths[1],roots[1]);strcpy(S.storage_path,roots[0]);
}
static int put(efs_ino_t ino,uint32_t ci,int root)
{
    efs_tls_chunk_gen=1000+ci;efs_tls_write_root=root;efs_tls_path_hint=-1;
    return server_write_fragment_with_sum_sync(&S,&EX,ino,ci,0,body,EFS_FRAGMENT_SIZE,sum);
}
static uint64_t used(void){pthread_mutex_lock(&S.lock);uint64_t u=S.nodes[0].used;pthread_mutex_unlock(&S.lock);return u;}
static uint64_t pending(void){pthread_mutex_lock(&S.lock);uint64_t u=S.reclaim_pending_bytes;pthread_mutex_unlock(&S.lock);return u;}
static void accept_range(efs_ino_t first,uint32_t n,int expect)
{
    struct efs_reclaim_item it[64];int32_t rc[64];
    assert(n<=64);
    for(uint32_t i=0;i<n;i++) it[i]=(struct efs_reclaim_item){.export_id=1,.ino=first+i,.generation=7};
    int accepted=server_reclaim_accept(&S,it,n,rc);
    for(uint32_t i=0;i<n;i++) {
        if(rc[i]!=expect) fprintf(stderr,"reclaim acceptance ino=%llu item=%u rc=%d expected=%d batch_rc=%d used=%llu pending=%llu\n",
            (unsigned long long)first,i,rc[i],expect,accepted,
            (unsigned long long)used(),(unsigned long long)pending());
        assert(rc[i]==expect);
    }
}
static int inode_dir_exists(efs_ino_t ino)
{
    for(int r=0;r<2;r++){
        char path[8192];format_ino_chunk_dir(path,sizeof(path),S.storage_paths[r],1,ino,0);
        *strrchr(path,'/')='\0';
        if(!access(path,F_OK)) return 1;
    }
    return 0;
}
/* Wait until every accepted record is deleted and retired (segment sealed). */
static void wait_drained(void)
{
    for(int i=0;i<400;i++){
        struct efs_reclaim_status st;server_reclaim_status(&S,&st);
        if(!st.journal_records && !st.journal_segments) return;
        usleep(25000);
    }
    struct efs_reclaim_status st;server_reclaim_status(&S,&st);
    fprintf(stderr,"not drained: records=%llu segments=%llu deleted=%llu checkpointed=%llu\n",
            (unsigned long long)st.journal_records,(unsigned long long)st.journal_segments,
            (unsigned long long)st.deleted,(unsigned long long)st.checkpointed);
    assert(0);
}
static void boot(void)
{
    server_setup();
    startup_inventory(&S);
    assert(server_reclaim_init(&S)==EFS_OK);
}
static void run(const char *name,void (*fn)(void),int expect_crash)
{
    pid_t p=fork();assert(p>=0);
    if(!p){fn();if(!expect_crash) server_reclaim_shutdown(&S);_exit(0);}
    int st;assert(waitpid(p,&st,0)==p);
    if(!WIFEXITED(st)||WEXITSTATUS(st)){fprintf(stderr,"%s: FAILED\n",name);exit(1);}
    printf("  %s: ok\n",name);
}
/* Live data the earlier scenarios leave on the shared roots: 6000 (2) and 7001 (1). */
static uint64_t base_after_settle(void){return 3ull*EFS_FRAGMENT_SIZE;}
static void journal_path(char *out,size_t n,const char *leaf){snprintf(out,n,"%s/meta/gc-journal/%s",roots[0],leaf);}
/* The newest segment file (earlier scenarios advance the sequence). */
static void last_seg(char *out,size_t n,uint64_t *seq_out)
{
    char dir[8192];journal_path(dir,sizeof(dir),"");
    DIR *d=opendir(dir);assert(d);struct dirent *de;uint64_t best=0;int any=0;
    while((de=readdir(d))) if(strlen(de->d_name)==20&&!strcmp(de->d_name+16,".seg")){
        uint64_t q=strtoull(de->d_name,NULL,16);if(!any||q>best)best=q;any=1;}
    closedir(d);assert(any);
    snprintf(out,n,"%s%016llx.seg",dir,(unsigned long long)best);
    if(seq_out)*seq_out=best;
}

/* 1. Acceptance credits at once; deletion settles; fences outlive retirement. */
static void t_basic(void)
{
    boot();
    for(efs_ino_t i=0;i<8;i++){assert(!put(5000+i,0,0));assert(!put(5000+i,1,1));}
    assert(!put(6000,0,0)); /* live neighbour */
    assert(used()==17ull*EFS_FRAGMENT_SIZE);
    rj.hold=1;
    accept_range(5000,8,EFS_OK);
    assert(used()==EFS_FRAGMENT_SIZE && pending()==16ull*EFS_FRAGMENT_SIZE); /* credit at acceptance */
    for(efs_ino_t i=0;i<8;i++) assert(put(5000+i,2,0)==EFS_ERR_STALE); /* fenced before checkpoint/delete */
    assert(!put(6000,1,0));
    accept_range(5000,8,EFS_OK); /* duplicate acceptance is idempotent */
    pthread_mutex_lock(&rj.mu);rj.hold=0;pthread_cond_broadcast(&rj.work);pthread_mutex_unlock(&rj.mu);
    wait_drained();
    assert(used()==2ull*EFS_FRAGMENT_SIZE && pending()==0);
    for(efs_ino_t i=0;i<8;i++){assert(!inode_dir_exists(5000+i));assert(put(5000+i,3,0)==EFS_ERR_STALE);}
    assert(server_compute_local_usage(&S)==2ull*EFS_FRAGMENT_SIZE);
    struct efs_reclaim_status st;server_reclaim_status(&S,&st);
    assert(st.accepted==8 && st.deleted==8 && st.retired==8 && st.checkpointed==8);
}

/* 2. Bytes another GC path removes between credit and deletion are not
 * debited twice. */
static void t_settle(void)
{
    boot();
    uint64_t base=used();
    assert(!put(7000,0,0));assert(!put(7000,1,0));assert(!put(7001,0,0));
    uint64_t live=base+EFS_FRAGMENT_SIZE;
    rj.hold=1;
    accept_range(7000,1,EFS_OK);
    assert(used()==live && pending()==2ull*EFS_FRAGMENT_SIZE);
    efs_tls_chunk_gen=1000;
    assert(!server_delete_fragment_if_sum(&S,&EX,7000,0,0,sum)); /* the chunk GC pass */
    pthread_mutex_lock(&rj.mu);rj.hold=0;pthread_cond_broadcast(&rj.work);pthread_mutex_unlock(&rj.mu);
    wait_drained();
    assert(used()==live && pending()==0);
    assert(server_compute_local_usage(&S)==live);
}

/* 3. Crash after acceptance, before any deletion: replay re-fences and
 * re-credits exactly once against the fresh inventory. */
static void t_crash_accept(void)
{
    boot();
    assert(used()==base_after_settle());
    for(efs_ino_t i=0;i<4;i++) assert(!put(8000+i,0,i&1));
    assert(!put(8100,0,0));
    rj.hold=1;
    accept_range(8000,4,EFS_OK);
    _exit(0); /* crash: no shutdown, nothing deleted or checkpointed */
}
static void t_replay(void)
{
    server_setup();startup_inventory(&S);
    uint64_t base=base_after_settle();
    assert(used()==base+5ull*EFS_FRAGMENT_SIZE);
    rj.hold=1;
    assert(server_reclaim_init(&S)==EFS_OK);
    for(efs_ino_t i=0;i<4;i++) assert(put(8000+i,1,0)==EFS_ERR_STALE);
    assert(used()==base+EFS_FRAGMENT_SIZE && pending()==4ull*EFS_FRAGMENT_SIZE);
    pthread_mutex_lock(&rj.mu);rj.hold=0;pthread_cond_broadcast(&rj.work);pthread_mutex_unlock(&rj.mu);
    wait_drained();
    assert(used()==base+EFS_FRAGMENT_SIZE && pending()==0);
    for(efs_ino_t i=0;i<4;i++){assert(!inode_dir_exists(8000+i));assert(put(8000+i,1,0)==EFS_ERR_STALE);}
    assert(!put(8100,1,0));
}

/* 4. A torn tail (unacknowledged partial record) is cut, not fatal. */
static void t_torn_prepare(void)
{
    boot();
    assert(!put(9000,0,0));
    rj.hold=1;
    accept_range(9000,1,EFS_OK);
    _exit(0);
}
static void t_torn(void)
{
    char path[8192];last_seg(path,sizeof(path),NULL);
    int fd=open(path,O_WRONLY|O_APPEND);assert(fd>=0);
    uint8_t junk[37];memset(junk,0x5a,sizeof(junk));assert(write(fd,junk,sizeof(junk))==(ssize_t)sizeof(junk));close(fd);
    boot();
    assert(!__atomic_load_n(&S.reclaim_fail_closed,__ATOMIC_ACQUIRE));
    struct stat st;assert(!stat(path,&st)&&st.st_size==(off_t)(RJ_HDR_BYTES+RJ_REC_BYTES));
    assert(put(9000,1,0)==EFS_ERR_STALE);
    wait_drained();
    assert(!inode_dir_exists(9000));
}

/* Recovery runs in the background (the export walk, then the segment scan,
 * then the fence) and its wall time depends on the host's load: under a full
 * suite run a fixed 5 s bound failed once (W141). Follow its progress instead:
 * give up only after IDLE_MS without a change in the recovery counters, or
 * at the hard cap, and print the status when giving up. */
static void wait_readmitted(void)
{
    enum { IDLE_MS=30000, CAP_MS=300000, POLL_US=10000 };
    struct timespec t0,now;clock_gettime(CLOCK_MONOTONIC,&t0);
    uint64_t last_recovered=~0ull;uint32_t last_recovering=~0u;long long idle_since=0;
    for(;;){
        if(!__atomic_load_n(&S.reclaim_fail_closed,__ATOMIC_ACQUIRE)) return;
        struct efs_reclaim_status st;server_reclaim_status(&S,&st);
        clock_gettime(CLOCK_MONOTONIC,&now);
        long long elapsed=(now.tv_sec-t0.tv_sec)*1000LL+(now.tv_nsec-t0.tv_nsec)/1000000;
        if(st.recovered!=last_recovered||st.recovering!=last_recovering){
            last_recovered=st.recovered;last_recovering=st.recovering;idle_since=elapsed;
        }
        if(elapsed-idle_since>=IDLE_MS||elapsed>=CAP_MS){
            fprintf(stderr,"still fail closed after %lld ms (%lld ms without progress): recovering=%u recovered=%llu records=%llu\n",
                    elapsed,elapsed-idle_since,st.recovering,(unsigned long long)st.recovered,(unsigned long long)st.journal_records);
            assert(0);
        }
        usleep(POLL_US);
    }
}
/* 5. A corrupt journal fails closed: every data PUT is refused until the
 * recovery scan has fenced the inodes metadata proves dead. */
static void t_fail_closed_prepare(void)
{
    boot();
    assert(!put(9100,0,0));assert(!put(9101,0,1));assert(!put(9102,0,0));
    rj.hold=1;
    accept_range(9100,1,EFS_OK);
    _exit(0);
}
static void t_fail_closed(void)
{
    char path[8192];last_seg(path,sizeof(path),NULL);
    /* K30: flip the byte; a constant 0xff left the random epoch byte at
     * offset 20 unchanged 1 time in 256 (header still valid, torn tail). */
    int fd=open(path,O_RDWR);assert(fd>=0);uint8_t x;
    assert(pread(fd,&x,1,RJ_HDR_BYTES+3)==1);x^=0xff;
    assert(pwrite(fd,&x,1,RJ_HDR_BYTES+3)==1); /* corrupt the only record */
    /* The last record failing its hash with nothing valid after it would be a
     * torn tail; corrupt the header instead so verification must fail. */
    assert(pread(fd,&x,1,20)==1);x^=0xff;
    assert(pwrite(fd,&x,1,20)==1);close(fd);
    ndead=2;dead_set[0]=9100;dead_set[1]=9101; /* 9101 was deleted but never journalled here */
    __atomic_store_n(&host_active,0,__ATOMIC_RELEASE);
    boot();
    uint64_t base=used()-3ull*EFS_FRAGMENT_SIZE; /* 9100, 9101, 9102 */
    assert(__atomic_load_n(&S.reclaim_fail_closed,__ATOMIC_ACQUIRE));
    assert(put(9102,1,0)==EFS_ERR_IO); /* even a live inode is refused */
    int32_t rc;struct efs_reclaim_item it={.export_id=1,.ino=9200,.generation=1};
    (void)server_reclaim_accept(&S,&it,1,&rc);assert(rc==EFS_ERR_BUSY); /* leader retries later */
    __atomic_store_n(&host_active,1,__ATOMIC_RELEASE);
    wait_readmitted();
    assert(!__atomic_load_n(&S.reclaim_fail_closed,__ATOMIC_ACQUIRE));
    assert(!put(9102,1,0));
    assert(put(9100,1,0)==EFS_ERR_STALE && put(9101,1,0)==EFS_ERR_STALE);
    wait_drained();
    assert(!inode_dir_exists(9100) && !inode_dir_exists(9101) && inode_dir_exists(9102));
    assert(used()==base+2ull*EFS_FRAGMENT_SIZE && pending()==0);
    char aside[8192];snprintf(aside,sizeof(aside),"%s/meta",roots[0]);
    DIR *d=opendir(aside);assert(d);int found=0;struct dirent *de;
    while((de=readdir(d))) if(!strncmp(de->d_name,"gc-journal.broken.",18)) found=1;
    closedir(d);assert(found); /* the damaged journal is kept for inspection */
}

/* 6. Segments without a state record and a state older than the segments
 * both fail closed; a header-only segment at `next` is a crash leftover. */
static void t_state_rules(void)
{
    char path[8192];
    boot();
    assert(!put(9300,0,0));
    rj.hold=1;
    accept_range(9300,1,EFS_OK);
    server_reclaim_shutdown(&S);
    /* Fabricate the crash window of seg_open_locked: segment `next` exists,
     * holds only its header, and the state was not advanced. */
    uint8_t h[RJ_HDR_BYTES];memset(h,0,sizeof(h));memcpy(h,RJ_HDR_MAGIC,8);
    put32(h+8,RJ_VERSION);put32(h+12,1);put64(h+16,rj.epoch);put64(h+24,rj.next);efs_hash(h,32,h+32);
    seg_path(path,sizeof(path),rj.next);
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0644);assert(fd>=0);assert(write(fd,h,sizeof(h))==(ssize_t)sizeof(h));close(fd);
    _exit(0);
}
static void t_state_rules_replay(void)
{
    char path[8192];uint64_t seq;last_seg(path,sizeof(path),&seq);
    assert(!access(path,F_OK));
    rj.hold=1; /* keep the record (no deletion, so no retirement): the journal stays non-empty */
    boot();
    assert(!__atomic_load_n(&S.reclaim_fail_closed,__ATOMIC_ACQUIRE));
    assert(access(path,F_OK)); /* leftover removed */
    assert(put(9300,1,0)==EFS_ERR_STALE);
    server_reclaim_shutdown(&S);
    char st[8192];snprintf(st,sizeof(st),"%s/" RJ_STATE_FILE,roots[0]);
    assert(!unlink(st));
    _exit(0);
}
static void t_state_missing(void)
{
    __atomic_store_n(&host_active,0,__ATOMIC_RELEASE);
    boot();
    assert(__atomic_load_n(&S.reclaim_fail_closed,__ATOMIC_ACQUIRE)); /* segments without a state record */
    assert(put(9301,0,0)==EFS_ERR_IO);
    server_reclaim_shutdown(&S);
    assert(__atomic_load_n(&S.reclaim_fail_closed,__ATOMIC_ACQUIRE)); /* an interrupted recovery never re-admits writes */
}

/* 6b. Restoring an older, self-consistent copy of meta/ is detected: the
 * state record outside meta/ expects segments the old copy lacks. */
static void t_old_meta_prepare(void)
{
    rj.hold=1;
    boot();
    char cmd[8192];snprintf(cmd,sizeof(cmd),"rm -rf %s/meta-old && cp -a %s/meta %s/meta-old",roots[0],roots[0],roots[0]);
    assert(!system(cmd));
    /* Fill one segment so the next acceptance opens a new one. */
    for(uint32_t base=0;base<RJ_SEG_RECORDS;base+=64) accept_range(400000+base,64,EFS_OK);
    accept_range(500000,1,EFS_OK);
    server_reclaim_shutdown(&S);
    snprintf(cmd,sizeof(cmd),"rm -rf %s/meta && mv %s/meta-old %s/meta",roots[0],roots[0],roots[0]);
    assert(!system(cmd));
    _exit(0);
}
static void t_old_meta(void)
{
    /* K30: earlier failures within the same second left non-empty asides
     * under this second's name; setting the journal aside must not collide. */
    char cmd[8192];time_t now=time(NULL);
    for(int k=0;k<3;k++){
        snprintf(cmd,sizeof(cmd),"mkdir -p %s/meta/gc-journal.broken.%lld && touch %s/meta/gc-journal.broken.%lld/seg",
                 roots[0],(long long)now+k,roots[0],(long long)now+k);
        assert(!system(cmd));
    }
    __atomic_store_n(&host_active,0,__ATOMIC_RELEASE);
    boot();
    assert(__atomic_load_n(&S.reclaim_fail_closed,__ATOMIC_ACQUIRE));
    snprintf(cmd,sizeof(cmd),"test -e %s/meta/gc-journal.broken.%lld/seg",roots[0],(long long)now);
    assert(!system(cmd)); /* the earlier aside is kept */
    assert(put(9302,0,0)==EFS_ERR_IO);
    server_reclaim_shutdown(&S);
}

/* 7. The journal cap refuses (BUSY) instead of dropping records. */
static void t_cap(void)
{
    boot();
    uint32_t n=RJ_CAP+1;
    struct efs_reclaim_item *it=calloc(n,sizeof(*it));int32_t *rc=calloc(n,sizeof(*rc));
    assert(it&&rc);
    for(uint32_t i=0;i<n;i++) it[i]=(struct efs_reclaim_item){.export_id=1,.ino=100000+i,.generation=1};
    assert(server_reclaim_accept(&S,it,n,rc)==EFS_ERR_BUSY);
    for(uint32_t i=0;i<n;i++) assert(rc[i]==EFS_ERR_BUSY);
    struct efs_reclaim_status st;server_reclaim_status(&S,&st);
    assert(!st.journal_records && st.busy==n);
    free(it);free(rc);
}

/* 8. A PUT racing acceptance either lands before the fence (and is counted
 * in the credit, then deleted) or is refused; nothing survives. */
static efs_ino_t race_ino;
static int race_stop; /* atomic (TSan) */
static void *race_writer(void *arg)
{
    (void)arg;
    for(uint32_t ci=10;!__atomic_load_n(&race_stop,__ATOMIC_ACQUIRE);ci++){
        int rc=put(race_ino,ci%4000,ci&1);
        assert(rc==EFS_OK||rc==EFS_ERR_STALE||rc==EFS_ERR_EXIST);
        if(rc==EFS_ERR_STALE) break;
    }
    return NULL;
}
static void t_race(void)
{
    boot();
    for(int round=0;round<20;round++){
        race_ino=20000+round;__atomic_store_n(&race_stop,0,__ATOMIC_RELEASE);
        assert(!put(race_ino,0,0));
        pthread_t t;assert(!pthread_create(&t,NULL,race_writer,NULL));
        usleep(2000);
        accept_range(race_ino,1,EFS_OK);
        __atomic_store_n(&race_stop,1,__ATOMIC_RELEASE);pthread_join(t,NULL);
        assert(put(race_ino,9999,0)==EFS_ERR_STALE);
    }
    wait_drained();
    assert(used()==0 && pending()==0 && server_compute_local_usage(&S)==0);
}

/* 9. The physical guard refuses PUTs when the filesystem sample is spent. */
static pthread_t phys_t[2];
static int phys_ok[2];
static void *phys_writer(void *arg)
{
    int w=(int)(intptr_t)arg;
    for(uint32_t ci=0;ci<8;ci++){
        efs_tls_chunk_gen=1000+ci;efs_tls_write_root=w;efs_tls_path_hint=-1;
        int rc=server_write_fragment_with_sum_sync(&S,&EX,31000+w,ci,0,body,EFS_FRAGMENT_SIZE,sum);
        assert(rc==EFS_OK||rc==EFS_ERR_QUOTA);
        if(rc==EFS_OK) phys_ok[w]++;
    }
    return NULL;
}
static void t_phys(void)
{
    boot();
    /* K30: bg_main resamples every 500 ms; a sample it had started
     * overwrote the forced budget below (1 of 25 on amd). PUT admission
     * needs no reclaim threads, so stop them; run()'s shutdown is a no-op. */
    server_reclaim_shutdown(&S);
    server_phys_sample(&S);
    assert(S.phys_fs[0] && S.phys_fs[0]==S.phys_fs[1]); /* both roots on one /tmp filesystem */
    uint8_t f=S.phys_fs[0]-1;
    assert(S.phys_avail[f]>EFS_FRAGMENT_SIZE);
    pthread_mutex_lock(&S.lock);S.phys_avail[f]=EFS_FRAGMENT_SIZE;S.phys_charged[f]=0;pthread_mutex_unlock(&S.lock);
    last_sample_us=now_us(); /* hold the sample for this check */
    assert(!put(30000,0,0)); /* exactly the remaining budget */
    assert(put(30000,1,1)==EFS_ERR_QUOTA); /* the other root shares the filesystem */
    assert(S.phys_refused>=1);
    char path[8192];fragment_path_at(&S,1,&EX,30000,1,0,path,sizeof(path));
    assert(access(path,F_OK)); /* a refused fragment leaves no file behind */
    server_phys_sample(&S);
    assert(!put(30000,1,1));
    /* Concurrent writers cannot both pass the last budget. */
    pthread_mutex_lock(&S.lock);S.phys_avail[f]=5ull*EFS_FRAGMENT_SIZE;S.phys_charged[f]=0;pthread_mutex_unlock(&S.lock);
    last_sample_us=now_us();
    uint64_t refused=S.phys_refused;
    assert(!pthread_create(&phys_t[0],NULL,phys_writer,(void*)0)&&!pthread_create(&phys_t[1],NULL,phys_writer,(void*)1));
    pthread_join(phys_t[0],NULL);pthread_join(phys_t[1],NULL);
    assert(phys_ok[0]+phys_ok[1]==5 && S.phys_refused-refused==11);
}

/* W125 segments: authority-checked PUT, read back, idempotent retry, then
 * journal-first acceptance (only once not ACTIVE), fence, credit, deletion. */
static uint8_t seg_body[EFS_SEGMENT_PUT_HEADER + 8192];
static void seg_setup(uint64_t id)
{
    memset(&seg_obj, 0, sizeof(seg_obj));
    seg_obj.ledger.publication_group = 0; seg_obj.ledger.segment_id = id;
    seg_obj.ledger.segment_generation = 1; seg_obj.ledger.geometry = 16384;
    seg_obj.ledger.count = 1; seg_obj.ledger.state[0] = EFS_PACK_PENDING; seg_obj.ledger.len[0] = 100;
    seg_obj.nodes[0] = 1; seg_obj.nodes[1] = 2; seg_obj.nodes[2] = 3;
    uint8_t *q = seg_body;
    memset(q, 0, sizeof(seg_body));
    q[0] = 0; q[1] = 0; q[4] = 0; q[5] = 0; q[6] = 0x40; q[7] = 0; /* geometry 16384 */
    for (int i = 0; i < 8; i++) { q[8 + i] = (uint8_t)(id >> (56 - 8 * i)); q[16 + i] = i == 7; }
    memset(q + EFS_SEGMENT_PUT_HEADER, 0x5e, 8192);
    efs_hash(q + EFS_SEGMENT_PUT_HEADER, 8192, q + 24);
    memcpy(seg_obj.checksums[0], q + 24, EFS_HASH_SIZE);
    seg_phase = EFS_PACK_ACTIVE;
}
static int seg_accept(uint64_t id)
{
    struct efs_reclaim_item it = {.export_id = EFS_RECLAIM_SEGMENT_EID(0), .ino = id, .generation = 1,
                                  .kind = EFS_RECLAIM_SEGMENT};
    int32_t rc = 0;
    (void)server_reclaim_accept(&S, &it, 1, &rc);
    return rc;
}
static int seg_file(uint64_t id)
{
    char p[8192]; server_segment_path(&S, 0, id, 1, 0, p, sizeof(p)); return !access(p, F_OK);
}
/* A full quota must not stop packing: a segment PUT may use the reserve
 * above it (it frees the per-file fragments it replaces); beyond the
 * reserve it is refused. */
static void t_segment_quota(void)
{
    boot();
    uint64_t q = S.quota, r = sg_quota_reserve(q);
    assert(r >= (16ull << 20) && r <= (256ull << 20));
    pthread_mutex_lock(&S.lock); S.nodes[0].used = q; pthread_mutex_unlock(&S.lock);
    seg_setup(0x5e60101);
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_OK);
    assert(used() == q + 8192);
    pthread_mutex_lock(&S.lock); S.nodes[0].used = q + r - 4096; pthread_mutex_unlock(&S.lock);
    seg_setup(0x5e60102);
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_ERR_QUOTA);
    assert(used() == q + r - 4096);
    pthread_mutex_lock(&S.lock); S.nodes[0].used = 0; pthread_mutex_unlock(&S.lock);
}
static void t_segment(void)
{
    boot();
    uint64_t base = used();
    seg_setup(0x5e60001);
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_OK);
    assert(used() == base + 8192 && seg_file(0x5e60001));
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_OK); /* retry: no new charge */
    assert(used() == base + 8192);
    uint8_t out[8192], s2[EFS_HASH_SIZE];
    assert(!server_segment_read(&S, 0, 0x5e60001, 1, 0, 8192, out, s2));
    assert(!memcmp(out, seg_body + EFS_SEGMENT_PUT_HEADER, 8192));
    seg_body[EFS_SEGMENT_PUT_HEADER] ^= 1; /* payload no longer matches its checksum */
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_ERR_CHECKSUM);
    seg_body[EFS_SEGMENT_PUT_HEADER] ^= 1;
    seg_body[1] = 1; /* fragment 1 is placed on node 2 */
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_ERR_INVAL);
    seg_body[1] = 0;
    assert(seg_accept(0x5e60001) == EFS_ERR_BUSY); /* still ACTIVE */
    seg_phase = EFS_PACK_RECLAIMING;
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_ERR_STALE); /* no longer ACTIVE */
    rj.hold = 1;
    assert(seg_accept(0x5e60001) == EFS_OK);
    assert(used() == base && pending() == 8192); /* credited at acceptance */
    seg_phase = EFS_PACK_ACTIVE; /* even a (forged) ACTIVE view cannot pass the fence */
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_ERR_STALE);
    seg_phase = EFS_PACK_RECLAIMING;
    pthread_mutex_lock(&rj.mu); rj.hold = 0; pthread_cond_broadcast(&rj.work); pthread_mutex_unlock(&rj.mu);
    wait_drained();
    assert(!seg_file(0x5e60001) && used() == base && pending() == 0);
    assert(server_compute_local_usage(&S) == base);
}
/* K37: the local verdict on a placed fragment, the audit exchange and the
 * rebuild PUT: missing, torn (short) and corrupt (payload) copies are
 * evidence; a rebuild is stored only while the mark names exactly this
 * fragment of a published segment, replaces a bad copy and is idempotent. */
static void seg_audit_req(uint8_t *req, uint32_t flags, uint64_t inc, uint64_t id, uint32_t n)
{
    memset(req, 0, EFS_SEGMENT_AUDIT_HEADER + EFS_SEGMENT_AUDIT_ITEM);
    req[1] = (uint8_t)flags;
    req[7] = (uint8_t)n;
    for (int i = 0; i < 8; i++) req[8 + i] = (uint8_t)(inc >> (56 - 8 * i));
    uint8_t *it = req + EFS_SEGMENT_AUDIT_HEADER;
    for (int i = 0; i < 8; i++) { it[i] = (uint8_t)(id >> (56 - 8 * i)); it[8 + i] = i == 7; }
    it[16] = 0;
    it[22] = 0x40; /* geometry 16384 */
    memcpy(it + 24, seg_obj.checksums[0], EFS_HASH_SIZE);
}
static int32_t seg_audit(uint32_t flags, uint64_t inc, uint64_t id, uint32_t n, uint32_t *v,
                         uint32_t *audited)
{
    uint8_t req[EFS_SEGMENT_AUDIT_HEADER + EFS_SEGMENT_AUDIT_ITEM], *r = NULL;
    uint32_t rl = 0;
    seg_audit_req(req, flags, inc, id, n);
    server_segment_audit(&S, req, EFS_SEGMENT_AUDIT_HEADER + n * EFS_SEGMENT_AUDIT_ITEM, &r, &rl);
    assert(r && rl >= EFS_SEGMENT_AUDIT_REPLY);
    int32_t rc = (int32_t)(((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) | ((uint32_t)r[2] << 8) | r[3]);
    uint32_t cnt = r[7];
    assert(rl == EFS_SEGMENT_AUDIT_REPLY + cnt && (rc != EFS_OK || cnt == n));
    uint64_t me = 0;
    for (int i = 0; i < 8; i++) me = (me << 8) | r[8 + i];
    assert(me == S.incarnation && r[23] == 1); /* settled: no uncertain root */
    if (v) *v = cnt ? r[EFS_SEGMENT_AUDIT_REPLY] : 99;
    if (audited) *audited = r[19];
    free(r);
    return rc;
}
static void t_segment_audit_rebuild(void)
{
    boot();
    S.incarnation = 42;
    const uint64_t id = 0x5e60201;
    uint32_t v = 0, audited = 9;
    char path[8192];
    server_segment_path(&S, 0, id, 1, 0, path, sizeof(path));
    seg_setup(id);
    seg_mark = 0;
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_OK);
    assert(server_segment_check(&S, 0, id, 1, 0, 16384, seg_obj.checksums[0]) == EFS_SEGMENT_AUDIT_OK);
    assert(seg_audit(0, 0, id, 1, &v, &audited) == EFS_OK && v == EFS_SEGMENT_AUDIT_OK && !audited);
    assert(seg_audit(0, 42, id, 1, &v, NULL) == EFS_OK && v == EFS_SEGMENT_AUDIT_OK);
    assert(seg_audit(0, 41, id, 1, NULL, NULL) == EFS_ERR_STALE); /* restarted since */
    /* Missing, torn (short file) and corrupt (payload) copies. */
    assert(!unlink(path));
    assert(seg_audit(0, 42, id, 1, &v, NULL) == EFS_OK && v == EFS_SEGMENT_AUDIT_MISSING);
    seg_obj.ledger.state[0] = EFS_PACK_LIVE; /* switched: published, nothing pending */
    seg_mark = 1;
    seg_body[2] = EFS_SEGMENT_PUT_F_REBUILD;
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_OK);
    assert(seg_audit(0, 42, id, 1, &v, NULL) == EFS_OK && v == EFS_SEGMENT_AUDIT_OK);
    assert(!truncate(path, 4096));
    assert(server_segment_check(&S, 0, id, 1, 0, 16384, seg_obj.checksums[0]) == EFS_SEGMENT_AUDIT_BAD);
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_OK); /* replaces the torn copy */
    assert(server_segment_check(&S, 0, id, 1, 0, 16384, seg_obj.checksums[0]) == EFS_SEGMENT_AUDIT_OK);
    {
        int fd = open(path, O_RDWR);
        uint8_t x = 0;
        assert(fd >= 0 && pread(fd, &x, 1, 100) == 1);
        x ^= 0xff;
        assert(pwrite(fd, &x, 1, 100) == 1 && !fsync(fd));
        close(fd);
    }
    assert(seg_audit(0, 42, id, 1, &v, NULL) == EFS_OK && v == EFS_SEGMENT_AUDIT_BAD);
    /* No authority, no rebuild: unmarked, another fragment, two bits. */
    seg_mark = 0;
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_ERR_STALE);
    seg_mark = 2;
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_ERR_STALE);
    seg_mark = 3;
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_ERR_STALE);
    seg_mark = EFS_PACK_READ_DEGRADED_BAD;
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_ERR_STALE);
    assert(server_segment_check(&S, 0, id, 1, 0, 16384, seg_obj.checksums[0]) == EFS_SEGMENT_AUDIT_BAD);
    seg_body[2] = 0; /* the packer's copy path needs pending records */
    seg_mark = 1;
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_ERR_STALE);
    seg_body[2] = EFS_SEGMENT_PUT_F_REBUILD;
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_OK);
    assert(server_segment_check(&S, 0, id, 1, 0, 16384, seg_obj.checksums[0]) == EFS_SEGMENT_AUDIT_OK);
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_OK); /* retried: already good */
    uint8_t out[8192], s2[EFS_HASH_SIZE];
    assert(!server_segment_read(&S, 0, id, 1, 0, 8192, out, s2));
    assert(!memcmp(out, seg_body + EFS_SEGMENT_PUT_HEADER, 8192));
    /* Header refusals. */
    seg_body[2] = 2;
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_ERR_INVAL);
    seg_body[2] = EFS_SEGMENT_PUT_F_REBUILD; seg_body[3] = 1;
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_ERR_INVAL);
    seg_body[2] = seg_body[3] = 0;
    /* A sweep completes only for this incarnation; COMPLETE carries no items. */
    assert(seg_audit(EFS_SEGMENT_AUDIT_F_COMPLETE, 42, id, 1, NULL, NULL) == EFS_ERR_INVAL);
    assert(seg_audit(EFS_SEGMENT_AUDIT_F_COMPLETE, 7, id, 0, NULL, NULL) == EFS_ERR_STALE);
    assert(seg_audit(0, 0, id, 0, NULL, &audited) == EFS_OK && !audited);
    assert(seg_audit(EFS_SEGMENT_AUDIT_F_COMPLETE, 42, id, 0, NULL, &audited) == EFS_OK && audited == 1);
    assert(seg_audit(2, 42, id, 0, NULL, NULL) == EFS_ERR_INVAL);
    seg_obj.ledger.state[0] = EFS_PACK_PENDING;
    seg_mark = 0;
}
/* Crash after a segment acceptance: the version 2 record replays. */
static void t_segment_crash(void)
{
    boot();
    seg_setup(0x5e60002);
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_OK);
    seg_phase = 0; /* the object is gone (retired) */
    rj.hold = 1;
    assert(seg_accept(0x5e60002) == EFS_OK);
    _exit(0);
}
static void t_segment_replay(void)
{
    seg_setup(0x5e60002); seg_phase = 0;
    boot();
    assert(!__atomic_load_n(&S.reclaim_fail_closed,__ATOMIC_ACQUIRE));
    seg_phase = EFS_PACK_ACTIVE;
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_ERR_STALE); /* fence replayed */
    seg_phase = 0;
    wait_drained();
    assert(!seg_file(0x5e60002));
}
/* A version 1 (W124) segment written before the upgrade still replays. */
static void t_v1_prepare(void)
{
    boot();
    assert(!put(9500,0,0));
    pthread_mutex_lock(&rj.mu);
    uint64_t seq = rj.next;
    char path[4200]; seg_path(path, sizeof(path), seq);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644); assert(fd >= 0);
    uint8_t h[RJ_HDR_BYTES]; memset(h, 0, sizeof(h)); memcpy(h, RJ_HDR_MAGIC, 8);
    put32(h + 8, 1); put32(h + 12, 1); put64(h + 16, rj.epoch); put64(h + 24, seq); efs_hash(h, 32, h + 32);
    uint8_t r[RJ_REC_V1_BYTES]; memset(r, 0, sizeof(r));
    put32(r, RJ_REC_TAG); put32(r + 4, 1); put64(r + 8, 9500); put64(r + 16, 7); put64(r + 24, seq); efs_hash(r, 32, r + 32);
    assert(write(fd, h, sizeof(h)) == (ssize_t)sizeof(h) && write(fd, r, sizeof(r)) == (ssize_t)sizeof(r));
    fsync(fd); close(fd);
    assert(state_write(rj.epoch, rj.oldest, seq + 1) == EFS_OK);
    pthread_mutex_unlock(&rj.mu);
    _exit(0);
}
static void t_v1_replay(void)
{
    boot();
    assert(!__atomic_load_n(&S.reclaim_fail_closed,__ATOMIC_ACQUIRE));
    assert(put(9500,1,0) == EFS_ERR_STALE);
    wait_drained();
    assert(!inode_dir_exists(9500));
}
/* Fail-closed recovery deletes a segment whose object is gone, keeps one
 * still ACTIVE. */
static void t_segment_recover_prepare(void)
{
    boot();
    seg_setup(0x5e60003);
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_OK);
    seg_setup(0x5e60004);
    assert(server_segment_put(&S, seg_body, sizeof(seg_body)) == EFS_OK);
    _exit(0);
}
static void t_segment_recover(void)
{
    char cmd[8192];
    /* A segment file without a state record fails verification closed. */
    snprintf(cmd, sizeof(cmd), "rm -f %s/" RJ_STATE_FILE " && touch %s/meta/gc-journal/0000000000000000.seg",
             roots[0], roots[0]);
    assert(!system(cmd));
    seg_setup(0x5e60004); /* ACTIVE; 0x5e60003 reads absent */
    __atomic_store_n(&host_active,1,__ATOMIC_RELEASE);
    boot();
    for (int i = 0; i < 400 && __atomic_load_n(&S.reclaim_fail_closed,__ATOMIC_ACQUIRE); i++) usleep(25000);
    assert(!__atomic_load_n(&S.reclaim_fail_closed,__ATOMIC_ACQUIRE));
    wait_drained();
    assert(!seg_file(0x5e60003) && seg_file(0x5e60004));
    __atomic_store_n(&host_active,0,__ATOMIC_RELEASE);
}

/* W125 superseded fragments: checksum-guarded deletion of one generation,
 * no inode fence, empty chunk/inode directories removed, a reused slot
 * (different checksum) kept, and replay after a crash. */
static int sup_accept(efs_ino_t ino, uint32_t ci, const uint8_t *s)
{
    struct efs_reclaim_item it = {.export_id = 1, .ino = ino, .generation = 1000 + ci,
                                  .kind = EFS_RECLAIM_SUPERSEDED, .chunk = ci, .fragment = 0};
    memcpy(it.sum, s, EFS_HASH_SIZE);
    int32_t rc = 0;
    (void)server_reclaim_accept(&S, &it, 1, &rc);
    return rc;
}
static int frag_file(efs_ino_t ino, uint32_t ci)
{
    for (int r = 0; r < 2; r++) {
        char path[8192];
        efs_tls_chunk_gen = 1000 + ci;
        fragment_path_at(&S, (uint32_t)r, &EX, ino, ci, 0, path, sizeof(path));
        if (!access(path, F_OK)) return 1;
    }
    return 0;
}
static void t_superseded(void)
{
    boot();
    uint64_t base = used();
    assert(!put(9600, 0, 0) && !put(9601, 0, 1) && !put(9601, 1024, 0) && !put(9602, 0, 0));
    assert(used() == base + 4ull * EFS_FRAGMENT_SIZE);
    uint8_t other[EFS_HASH_SIZE]; memset(other, 0x77, sizeof(other));
    assert(sup_accept(9600, 0, sum) == EFS_OK);
    assert(sup_accept(9601, 0, sum) == EFS_OK);
    assert(sup_accept(9602, 0, other) == EFS_OK); /* reused slot: kept */
    assert(sup_accept(9600, 0, sum) == EFS_OK);   /* duplicate */
    assert(!put(9600, 5, 0)); /* the live inode is not fenced */
    wait_drained();
    assert(!frag_file(9600, 0) && frag_file(9600, 5));
    assert(!frag_file(9601, 0) && frag_file(9601, 1024) && inode_dir_exists(9601));
    assert(frag_file(9602, 0));
    assert(used() == base + 3ull * EFS_FRAGMENT_SIZE);
    /* The only fragment of an inode gone: its directories go too. */
    assert(sup_accept(9600, 5, sum) == EFS_OK);
    wait_drained();
    assert(!inode_dir_exists(9600));
    assert(server_compute_local_usage(&S) == used());
}
static void t_superseded_crash(void)
{
    boot();
    assert(!put(9700, 0, 1));
    rj.hold = 1;
    assert(sup_accept(9700, 0, sum) == EFS_OK);
    _exit(0);
}
static void t_superseded_replay(void)
{
    boot();
    assert(!__atomic_load_n(&S.reclaim_fail_closed,__ATOMIC_ACQUIRE));
    wait_drained();
    assert(!frag_file(9700, 0) && !inode_dir_exists(9700));
    assert(!put(9700, 1, 0)); /* never fenced */
}

static uint64_t late_id;
static void finish_after_sync(void)
{
 after_root_sync=NULL;
 struct rj_rec *r=rec_at(late_id);
 assert(!server_gc_inode_delete(&S,r->eid,r->ino,NULL,0));
 r->deleted=1; /* deletion deliberately completed after the barrier */
}
static void t_retire_cut(void)
{
 boot();server_reclaim_shutdown(&S);rj.stop=0; /* deterministic workers */
 assert(!put(41000,0,0));assert(!put(41001,0,1));
 accept_range(41000,1,EFS_OK);
 close(rj.seg_fd);rj.seg_fd=-1; /* force a second segment */
 accept_range(41001,1,EFS_OK);
 assert(rj.nseg==2&&rj.tail-rj.head==2);
 efs_ino_t inos[2]={41000,41001};assert(!server_gc_death_set_many(&S,1,inos,2));
 struct rj_rec *first=rec_at(rj.head),*last=rec_at(rj.head+1);
 first->checkpointed=last->checkpointed=1;
 assert(!server_gc_inode_delete(&S,1,41000,NULL,0));first->deleted=1;
 late_id=rj.head+1;rj.last_append_us=now_us()-2000000;
 after_root_sync=finish_after_sync;retire_some();
 assert(rj.nseg==1&&rj.head==late_id&&rj.tail-rj.head==1);
 retire_some();assert(!rj.nseg&&rj.head==rj.tail);
}

/* F1: retired records give their ring pages back. Every slot is touched
 * first (as a long load's sweeps do); then retirement of a prefix, and of
 * a window that wraps, must leave resident only the pages that still hold
 * a live record, with the live records intact. */
static size_t ring_resident(void)
{
    size_t pages=RJ_RING_BYTES/rj_page,n=0;unsigned char *v=malloc(pages);assert(v);
    assert(!mincore(rj.recs,RJ_RING_BYTES,v));
    for(size_t i=0;i<pages;i++) n+=v[i]&1;
    free(v);return n;
}
static int live_intact(void)
{
    for(uint64_t i=rj.head;i<rj.tail;i++) if(rec_at(i)->ino!=0x5a00000000ull+(i%RJ_CAP)) return 0;
    return 1;
}
static size_t live_pages(void)
{
    uint64_t sz=sizeof(struct rj_rec),a=rj.head*sz/rj_page,b=(rj.tail*sz+rj_page-1)/rj_page;
    return (size_t)(b-a)+1; /* +1: a window that wraps splits one page run in two */
}
static void t_ring_release(void)
{
    boot();
    rj.hold=1;
    pthread_mutex_lock(&rj.mu);
    for(uint64_t i=0;i<RJ_CAP;i++){memset(rec_at(i),0,sizeof(struct rj_rec));rec_at(i)->ino=0x5a00000000ull+i;}
    assert(ring_resident()==RJ_RING_BYTES/rj_page);
    rj.head=0;rj.tail=3*RJ_SEG_RECORDS;
    rj.head=2*RJ_SEG_RECORDS;ring_release_locked(0);
    /* Slots ahead of the tail were touched above but never retired: only
     * the prefix behind the head is released. */
    size_t ahead=(RJ_RING_BYTES-rj.tail*sizeof(struct rj_rec))/rj_page;
    assert(live_intact());
    assert(ring_resident()<=live_pages()+ahead+1);
    assert(ring_resident()>=live_pages()-1+ahead);
    /* A window across the wrap: [CAP-100, CAP+200). */
    uint64_t old=rj.head;
    rj.tail=RJ_CAP+200;
    for(uint64_t i=3*RJ_SEG_RECORDS;i<rj.tail;i++){memset(rec_at(i),0,sizeof(struct rj_rec));rec_at(i)->ino=0x5a00000000ull+(i%RJ_CAP);}
    rj.head=RJ_CAP-100;ring_release_locked(old);
    assert(live_intact());
    assert(ring_resident()<=live_pages()+1);
    /* Everything retired: nothing but the page under the head is left. */
    old=rj.head;rj.head=rj.tail;ring_release_locked(old);
    assert(ring_resident()<=2);
    pthread_mutex_unlock(&rj.mu);
}

int main(void)
{
    /* Under TMPDIR (make_test_summary.sh sets a disk path): the physical
     * guard keeps max(5%, 1 GiB) + 256 MiB free, more than a small tmpfs /tmp. */
    efs_test_path(roots[0],sizeof(roots[0]),"efs-rj-a-XXXXXX");efs_test_path(roots[1],sizeof(roots[1]),"efs-rj-b-XXXXXX");
    assert(efs_test_mkdtemp(roots[0])&&efs_test_mkdtemp(roots[1]));
    assert(!posix_memalign((void**)&body,4096,EFS_FRAGMENT_SIZE));memset(body,0x61,EFS_FRAGMENT_SIZE);memset(sum,0x62,sizeof(sum));
    printf("reclaim journal:\n");
    run("accept/credit/fence/settle/retire",t_basic,0);
    run("double-debit settle",t_settle,0);
    run("crash after accept",t_crash_accept,1);
    run("replay re-fences, credits once",t_replay,0);
    run("torn tail prepare",t_torn_prepare,1);
    run("torn tail cut",t_torn,0);
    run("fail-closed prepare",t_fail_closed_prepare,1);
    run("fail closed, recover, re-admit",t_fail_closed,0);
    run("header-only segment at next",t_state_rules,1);
    run("leftover removed",t_state_rules_replay,1);
    run("segments without state fail closed",t_state_missing,0);
    run("old meta/ copy prepare",t_old_meta_prepare,1);
    run("old meta/ copy fails closed",t_old_meta,0);
    /* Three valid root paths plus suffixes exceed 256 bytes in the gate's
     * nested TMPDIR. A truncated cleanup left the state record but removed its
     * journal directory, so the next first append failed ENOENT. */
    char cmd[8192];snprintf(cmd,sizeof(cmd),"rm -rf %s/meta/gc-journal* %s/data %s/" RJ_STATE_FILE,roots[0],roots[0],roots[0]);assert(!system(cmd));
    snprintf(cmd,sizeof(cmd),"rm -rf %s/data",roots[1]);assert(!system(cmd));
    char state[8192];snprintf(state,sizeof(state),"%s/" RJ_STATE_FILE,roots[0]);
    assert(access(state,F_OK)<0 && errno==ENOENT); /* cleanup, not reclaim */
    run("journal cap BUSY",t_cap,0);
    run("PUT racing acceptance",t_race,0);
    run("physical guard",t_phys,0);
    run("retirement excludes post-sync deletion",t_retire_cut,0);
    run("retired ring pages released",t_ring_release,0);
    run("segment PUT/accept/fence/delete",t_segment,0);
    run("segment crash after accept",t_segment_crash,1);
    run("segment record replays",t_segment_replay,0);
    run("v1 segment prepare",t_v1_prepare,1);
    run("v1 segment replays",t_v1_replay,0);
    run("segment recovery prepare",t_segment_recover_prepare,1);
    run("segment recovery",t_segment_recover,0);
    run("superseded fragments",t_superseded,0);
    run("superseded crash after accept",t_superseded_crash,1);
    run("superseded record replays",t_superseded_replay,0);
    run("segment PUT at a full quota uses the reserve",t_segment_quota,0);
    run("K37 segment audit and rebuild PUT",t_segment_audit_rebuild,0);
    snprintf(cmd,sizeof(cmd),"rm -rf %s %s",roots[0],roots[1]);(void)system(cmd);
    printf("reclaim journal: PASS\n");
    return 0;
}
