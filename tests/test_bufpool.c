/* Standalone: cc -Iinclude -Isrc/common -pthread tests/test_bufpool.c -o /tmp/test_bufpool */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
static int fail_malloc, fail_calloc;
static void *test_calloc(size_t n, size_t size) { return fail_calloc ? NULL : calloc(n, size); }
static void *test_malloc(size_t n) { return fail_malloc ? NULL : malloc(n); }
int efs_rdma_zc_region_add(void *p, size_t n) { (void)p; (void)n; return 0; }
#define malloc test_malloc
#define calloc test_calloc
#include "../src/client/bufpool.c"
#include "../src/client/writer_state.c"
#include "../src/client/publication_ack_owner.c"
#undef malloc
#undef calloc

static void check(uint64_t expected)
{
    uint64_t live, reserved, backing, limit;
    efs_buf_budget_stats(&live, &reserved, &backing, &limit);
    assert(live == expected);
    assert(live + reserved <= limit);
    assert(backing <= limit);
}
static void *churn(void *arg)
{
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        if (efs_buf_reserve(EFS_CHUNK_SIZE * 2))
            continue;
        void *p = efs_buf_alloc(1);
        assert(p);
        efs_buf_unreserve();
        efs_buf_free(p, 1);
    }
    return NULL;
}
#ifdef __linux__
static pthread_barrier_t speculative_barrier;
static void *speculative_churn(void *unused)
{
    (void)unused;
    void *bodies[128]; unsigned n=0;
    while (n<128 && (bodies[n]=efs_buf_alloc_prefetch(1))) n++;
    pthread_barrier_wait(&speculative_barrier);
    pthread_mutex_lock(&g_bp_mu);
    assert(g_live+g_reserved<=g_hard/2);
    pthread_mutex_unlock(&g_bp_mu);
    pthread_barrier_wait(&speculative_barrier);
    for (unsigned i=0;i<n;i++) efs_buf_free(bodies[i],1);
    return NULL;
}

#endif
static void *abandon_credit(void *unused)
{
    (void)unused;
    assert(!efs_buf_reserve_request(1u << 20, 4096));
    return NULL; /* pthread-key destructor must release both reservations */
}
static void writer_state_lifetime(void)
{
    uint64_t before = g_metadata;
    fail_calloc = 1;
    assert(!efs_writer_state_alloc(EFS_CHUNK_SIZE) && g_metadata == before);
    fail_calloc = 0;
    assert(!efs_buf_reserve_request(0, sizeof(struct efs_writer_state)));
    struct efs_writer_state *state = efs_writer_state_alloc(EFS_CHUNK_SIZE);
    assert(state && g_metadata == before + sizeof(*state));
    efs_buf_unreserve();
    struct efs_msg_inode_writer_view_reply view = {0};
    view.ino = 100; view.generation = 1;
    assert(efs_writer_ranges_admit(&state->ranges, &view, 0, 100) == EFS_OK);
    struct efs_writer_plan plan;
    assert(efs_writer_ranges_plan(&state->ranges, &view, 0, &plan) == EFS_OK);
    assert(efs_writer_state_free(state) == EFS_ERR_BUSY);
    struct efs_writer_plan invalid_geometry=plan;
    invalid_geometry.original.chunk_size*=2;
    invalid_geometry.surviving.chunk_size*=2;
    assert(efs_writer_state_put(state,&invalid_geometry,1000,1)==EFS_ERR_STALE);
    assert(!state->has_publication);
    invalid_geometry=plan;
    invalid_geometry.original.mutation++;
    invalid_geometry.surviving.mutation++;
    assert(efs_writer_state_put(state,&invalid_geometry,1000,1)==EFS_ERR_STALE);
    assert(!state->has_publication);
    assert(efs_writer_state_put(state, &plan, 1000, 1) == EFS_OK);
    assert(efs_writer_state_put(state, &plan, 1001, 2) == EFS_ERR_BUSY);
    assert(efs_writer_state_report(state, 1000, 1, EFS_ERR_IO) == EFS_ERR_IO);
    assert(state->has_publication && state->ranges.bytes.count);
    assert(efs_writer_state_report(state, 1001, 1, EFS_OK) == EFS_ERR_STALE);
    assert(state->has_publication);
    struct efs_writer_state intact=*state;
    state->publication.plan.surviving.ranges[0].off=100;
    assert(efs_writer_state_report(state,1000,1,EFS_OK)==EFS_ERR_INVAL);
    assert(state->has_publication && state->ranges.bytes.count);
    *state=intact;state->ranges.generation++;
    assert(efs_writer_state_report(state,1000,1,EFS_OK)==EFS_ERR_STALE);
    assert(state->has_publication && state->ranges.bytes.count);
    *state=intact;state->ranges.bytes.count=EFS_DIRTY_RANGE_MAX+1;
    assert(efs_writer_state_report(state,1000,1,EFS_OK)==EFS_ERR_INVAL);
    assert(state->has_publication);
    *state=intact;
    assert(efs_writer_ranges_admit(&state->ranges, &view, 0, 1) == EFS_OK);
    assert(efs_writer_state_report(state, 1000, 1, EFS_OK) == EFS_ERR_STALE);
    assert(!state->has_publication && state->ranges.bytes.count);
    assert(efs_writer_state_free(state) == EFS_ERR_BUSY);
    assert(efs_writer_ranges_plan(&state->ranges, &view, 0, &plan) == EFS_OK);
    assert(efs_writer_state_put(state, &plan, 1001, 2) == EFS_OK);
    assert(efs_writer_state_report(state, 1001, 2, EFS_OK) == EFS_OK);
    assert(efs_writer_state_free(state) == EFS_OK && g_metadata == before);
}

static void writer_admission_copy(void)
{
    uint64_t before=g_metadata;
    struct efs_writer_state *state=NULL;
    uint8_t body[EFS_MIN_CHUNK_SIZE], saved[EFS_MIN_CHUNK_SIZE], src[4]={1,2,3,4};
    memset(body, 0x55, sizeof(body));memcpy(saved,body,sizeof(body));
    struct efs_msg_inode_writer_view_reply view={.ino=100,.generation=1};
    view.generation=0;
    assert(efs_writer_state_write(&state,&view,body,sizeof(body),0,src,4)!=EFS_OK);
    assert(!state && !memcmp(body,saved,sizeof(body)) && g_metadata==before);
    view.generation=1;fail_calloc=1;
    assert(efs_writer_state_write(&state,&view,body,sizeof(body),0,src,4)==EFS_ERR_NOMEM);
    assert(!state && !memcmp(body,saved,sizeof(body)) && g_metadata==before);
    fail_calloc=0;
    for (unsigned i=0;i<EFS_DIRTY_RANGE_MAX;++i)
        assert(efs_writer_state_write(&state,&view,body,sizeof(body),i*8,src,1)==EFS_OK);
    struct efs_writer_state old=*state;memcpy(saved,body,sizeof(body));
    assert(efs_writer_state_write(&state,&view,body,sizeof(body),1024,src,1)==EFS_ERR_BUSY);
    assert(!memcmp(state,&old,sizeof(old)) && !memcmp(body,saved,sizeof(body)));
    view.generation=2;
    assert(efs_writer_state_write(&state,&view,body,sizeof(body),0,src,1)==EFS_ERR_STALE);
    assert(!memcmp(state,&old,sizeof(old)) && !memcmp(body,saved,sizeof(body)));
    view.generation=1;
    assert(efs_writer_state_write(&state,&view,body,sizeof(body),0,body+8,1)==EFS_OK);
    state->ranges.bytes.count=0;
    assert(efs_writer_state_free(state)==EFS_OK && g_metadata==before);
}

static void writer_body_snapshot(void)
{
    uint64_t before=g_metadata;
    struct efs_writer_state *state=NULL;
    uint8_t body[EFS_MIN_CHUNK_SIZE], snapshot[EFS_MIN_CHUNK_SIZE], saved[EFS_MIN_CHUNK_SIZE];
    uint8_t incoming[200];memset(body,0x55,sizeof(body));memset(incoming,'A',sizeof(incoming));
    struct efs_msg_inode_writer_view_reply view={.ino=100,.generation=1};
    assert(efs_writer_state_write(&state,&view,body,sizeof(body),0,incoming,200)==EFS_OK);
    view.authority_epoch=1;view.history.count=1;
    view.history.entries[0]=(struct efs_content_fence){1,100};
    struct efs_msg_inode_getchunks_reply *base=calloc(1,sizeof(*base));assert(base);
    base->ino=100;base->generation=1;base->authority_epoch=1;
    struct efs_writer_plan plan, sentinel;
    memset(&sentinel,0xa5,sizeof(sentinel));plan=sentinel;
    memset(snapshot,0xa5,sizeof(snapshot));memcpy(saved,snapshot,sizeof(saved));
    base->generation=2;
    assert(efs_writer_state_snapshot(state,&view,base,body,sizeof(body),&plan,snapshot)==EFS_ERR_STALE);
    assert(!memcmp(snapshot,saved,sizeof(saved)) && !memcmp(&plan,&sentinel,sizeof(plan)));
    base->generation=1;
    assert(efs_writer_state_snapshot(state,&view,base,body,sizeof(body),&plan,body)==EFS_ERR_INVAL);
    assert(efs_writer_state_snapshot(state,&view,base,body,sizeof(body),&plan,snapshot)==EFS_OK);
    assert(plan.base_absent && plan.original.ranges[0].len==200 && plan.surviving.ranges[0].len==100);
    for(unsigned i=0;i<sizeof(snapshot);++i) assert(snapshot[i]==(i<100?'A':0));
    memcpy(saved,snapshot,sizeof(saved));
    assert(efs_writer_state_put(state,&plan,400,1)==EFS_OK);
    struct efs_writer_plan blocked=sentinel;
    assert(efs_writer_state_snapshot(state,&view,base,body,sizeof(body),&blocked,snapshot)==EFS_ERR_BUSY);
    assert(!memcmp(&blocked,&sentinel,sizeof(blocked)) && !memcmp(snapshot,saved,sizeof(saved)));
    memset(incoming,'B',sizeof(incoming));
    assert(efs_writer_state_write(&state,&view,body,sizeof(body),200,incoming,50)==EFS_OK);
    assert(!memcmp(snapshot,saved,sizeof(saved)));
    assert(efs_writer_state_report(state,400,1,EFS_OK)==EFS_ERR_STALE);
    assert(efs_writer_state_owned(state) && !state->has_publication);
    assert(efs_writer_state_snapshot(state,&view,base,body,sizeof(body),&plan,snapshot)==EFS_OK);
    for(unsigned i=0;i<sizeof(snapshot);++i)
        assert(snapshot[i]==(i<100?'A':i>=200 && i<250?'B':0));
    uint8_t peer[EFS_MIN_CHUNK_SIZE];memset(peer,'P',sizeof(peer));
    assert(efs_dirty_ranges_overlay(&plan.surviving,peer,sizeof(peer),snapshot)==EFS_OK);
    assert(peer[0]=='A' && peer[100]=='P' && peer[200]=='B' && peer[250]=='P');
    assert(efs_writer_state_put(state,&plan,401,2)==EFS_OK);
    assert(efs_writer_state_report(state,401,2,EFS_OK)==EFS_OK);
    assert(efs_writer_state_free(state)==EFS_OK && g_metadata==before);free(base);
}

int main(void)
{
    setenv("EFS_DCACHE_HARD_BYTES", "33554432", 1);
    setenv("EFS_DCACHE_DRAIN_BYTES", "33554432", 1);
    fail_malloc = 1;
    assert(!efs_buf_alloc(1));
    check(0);
    fail_malloc = 0;
    void *large = efs_buf_alloc(1u << 20);
    assert(large);
    check(1u << 20);
    efs_buf_free(large, 1u << 20);
    check(0);
    setenv("EFS_TEST_BUDGET", "33554432junk", 1);
    assert(budget_env("EFS_TEST_BUDGET", 17, 1, UINT64_MAX)==17);
    setenv("EFS_TEST_BUDGET", "-1", 1);
    assert(budget_env("EFS_TEST_BUDGET", 17, 1, UINT64_MAX)==17);
    assert(efs_buf_reserve(33ull << 20)==EFS_ERR_BUSY);
    /* Reservation excludes competing allocators, and consumes actual pool
     * capacity even for a one-byte sparse patch/parity buffer. */
    assert(efs_buf_reserve(32ull << 20) == 0);
    assert(!efs_buf_alloc(64u << 20));
    void *p[512];
    for (int i = 0; i < 256; i++) {
        p[i] = efs_buf_alloc(1);
        assert(p[i]);
    }
    efs_buf_unreserve();
    check(32ull << 20);
    assert(!efs_buf_alloc(1));
    assert(efs_buf_reserve(EFS_CHUNK_SIZE) == EFS_ERR_BUSY);
    /* Writer exhaustion cannot consume the dedicated drain reserve. */
    efs_buf_drain_enter();
    efs_buf_drain_enter();
    for (int i = 256; i < 512; i++) {
        p[i] = efs_buf_alloc(EFS_CHUNK_SIZE);
        assert(p[i]);
    }
    assert(!efs_buf_alloc(1));
    efs_buf_drain_leave();
    efs_buf_drain_leave();
    check(64ull << 20);
    /* Ownership transfer doesn't reduce the charge. Retained failed bodies
     * cannot permit admission until their actual allocation is released. */
    assert(efs_buf_reserve(1) == EFS_ERR_BUSY);
    for (int i = 0; i < 512; i++) efs_buf_free(p[i], EFS_CHUNK_SIZE);
    check(0);
    assert(g_bp_n == 512);
    /* An accepted writer's reservation remains usable while another thread
     * consumes the drain reserve. Previously alloc rechecked the hard cap
     * against drain bytes and failed despite fully reserved credit. */
    assert(!efs_buf_reserve(32ull << 20));
    efs_buf_drain_enter();
    for (int i=0;i<256;i++) { p[i]=efs_buf_alloc(1); assert(p[i]); }
    efs_buf_drain_leave();
    assert(g_live==(32ull<<20) && g_reserved==(32ull<<20));
    for (int i=256;i<512;i++) { p[i]=efs_buf_alloc(1); assert(p[i]); }
    assert(!g_reserved && g_live==(64ull<<20));
    assert(!efs_buf_alloc(1)); /* no credit cannot spend drain capacity */
    efs_buf_unreserve();
    for (int i=0;i<512;i++) efs_buf_free(p[i],EFS_CHUNK_SIZE);
    check(0);
    /* W60: queued speculative bodies cannot exhaust demand capacity. */
    for (int i=0; i<128; i++) {
        p[i]=efs_buf_alloc_prefetch(1); assert(p[i]);
    }
    assert(!efs_buf_alloc_prefetch(1));
    for (int i=128; i<256; i++) { p[i]=efs_buf_alloc(1); assert(p[i]); }
    assert(!efs_buf_alloc(1));
    for (int i=0; i<256; i++) efs_buf_free(p[i],1);
    assert(!efs_buf_reserve(16ull<<20));
    assert(!efs_buf_alloc_prefetch(1));
    assert(t_credit_owner->body==(16ull<<20));
    efs_buf_unreserve();
    efs_buf_drain_enter();
    void *spec=efs_buf_alloc_prefetch(1); assert(spec);
    assert(g_live==EFS_CHUNK_SIZE);
    efs_buf_free(spec,1);
    efs_buf_drain_leave();
    pthread_t workers[16];
    for (int i = 0; i < 16; i++) assert(!pthread_create(&workers[i], NULL, churn, NULL));
    for (int i = 0; i < 16; i++) assert(!pthread_join(workers[i], NULL));
    check(0);
    assert(g_reserved == 0 && g_bp_n == 512);
#ifdef __linux__
    assert(!pthread_barrier_init(&speculative_barrier,NULL,16));
    for (int i=0;i<16;i++) assert(!pthread_create(&workers[i],NULL,speculative_churn,NULL));
    for (int i=0;i<16;i++) assert(!pthread_join(workers[i],NULL));
    assert(!pthread_barrier_destroy(&speculative_barrier));
    check(0);
#endif

    pthread_t retired;
    assert(!pthread_create(&retired, NULL, abandon_credit, NULL));
    assert(!pthread_join(retired, NULL));
    assert(!g_reserved && !g_meta_reserved);
    assert(!efs_buf_reserve_request(1u << 20, 8u << 20));
    void *meta = efs_buf_metadata_alloc(1024);
    assert(meta && !efs_buf_metadata_alloc(8u << 20));
    efs_buf_unreserve();
    efs_buf_metadata_free(meta, 1024);
    assert(!g_metadata && !g_meta_reserved);
    void *m[32];
    for (int i = 0; i < 32; i++) { m[i] = efs_buf_metadata_alloc(256u << 10); assert(m[i]); }
    assert(!efs_buf_metadata_alloc(1));
    for (int i = 0; i < 32; i++) efs_buf_metadata_free(m[i], 256u << 10);
    assert(g_metadata == 0);
    uint64_t ack_before=g_metadata;
    fail_calloc=1;assert(!efs_client_publication_ack_alloc());fail_calloc=0;
    assert(g_metadata==ack_before);
    struct efs_publication_ack_queue *ack=efs_client_publication_ack_alloc();
    assert(ack && !ack->count && g_metadata==ack_before+sizeof(*ack));
    ack->count=1;assert(efs_client_publication_ack_free(ack)==EFS_ERR_BUSY);
    assert(g_metadata==ack_before+sizeof(*ack));
    ack->count=0;assert(efs_client_publication_ack_free(ack)==EFS_OK);
    assert(g_metadata==ack_before);
    assert(!efs_buf_reserve_request(0,sizeof(*ack)));
    ack=efs_client_publication_ack_alloc();assert(ack && !g_meta_reserved);
    efs_buf_unreserve();assert(efs_client_publication_ack_free(ack)==EFS_OK);
    struct efs_publication_ack_queue *queues[1024];unsigned nq=0;
    while (nq<1024 && (queues[nq]=efs_client_publication_ack_alloc())) nq++;
    assert(nq && nq<1024 && g_metadata<=8ull<<20);
    assert(!efs_client_publication_ack_alloc());
    for (unsigned i=0;i<nq;i++) assert(efs_client_publication_ack_free(queues[i])==EFS_OK);
    assert(g_metadata==ack_before);
    writer_state_lifetime();
    writer_admission_copy();
    writer_body_snapshot();
#if EFS_FAULTS
    char fault_path[]="/tmp/efs-drain-budget-XXXXXX";
    int fault_fd=mkstemp(fault_path);assert(fault_fd>=0);close(fault_fd);
    assert(!setenv("EFS_FAULT_DRAIN_FILE",fault_path,1));
    efs_buf_drain_enter();assert(!efs_buf_alloc(1));efs_buf_drain_leave();
    assert(fault_drain_count==512 && g_live==(64ull<<20));
    assert(!unlink(fault_path));
    assert(!efs_buf_reserve(EFS_CHUNK_SIZE));
    assert(!fault_drain_count && !g_live);efs_buf_unreserve();
    unsetenv("EFS_FAULT_DRAIN_FILE");
#endif
    puts("test_bufpool: OK (hard bound, reserve, failures, concurrency, metadata)");
    return 0;
}
