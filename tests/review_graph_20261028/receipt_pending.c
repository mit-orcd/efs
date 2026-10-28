/* Actual receipt implementation, including its private collision hash. */
#include "../../src/common/request_receipt.c"
#include <assert.h>

static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int release_all, active_first, peak_first;
static struct efs_receipt_table table;
struct attempt { struct efs_receipt_key key; int entered, duplicate, rc, how; };
static int execute(void *ctx, int *proposed, const char **phase, struct efs_receipt_stale *stale)
{
    struct attempt *a = ctx; (void)stale;
    pthread_mutex_lock(&gate_mu);
    if (a->key.seq == 1) {
        active_first++;
        if (active_first > peak_first) peak_first = active_first;
    }
    a->entered = 1; pthread_cond_broadcast(&gate_cv);
    while (!release_all && !a->duplicate) pthread_cond_wait(&gate_cv, &gate_mu);
    if (a->key.seq == 1) active_first--;
    pthread_mutex_unlock(&gate_mu);
    *proposed = 1; *phase = NULL; return EFS_OK;
}
static void *run(void *ctx)
{
    struct attempt *a = ctx;
    a->rc = efs_receipt_run(&table, &a->key, 42, 1, 50, execute, a, &a->how, NULL);
    return NULL;
}
static void entered(struct attempt *a)
{
    pthread_mutex_lock(&gate_mu);
    while (!a->entered) pthread_cond_wait(&gate_cv, &gate_mu);
    pthread_mutex_unlock(&gate_mu);
}
int main(int argc, char **argv)
{
    int fixed = argc > 1 && !strcmp(argv[1], "fixed");
    /* Production capacity, no sleeps or wall-clock deadline assumptions. */
    assert(efs_receipt_table_init(&table, 4096) == EFS_OK);
    struct attempt attempts[8] = {0}; pthread_t threads[8];
    struct efs_receipt_key k = {.epoch = 2, .mint_ns = 99, .seq = 1}; k.uuid[0] = 0xab;
    uint32_t bucket = receipt_hash(&k) & table.mask;
    int n = 0;
    for (; n < 8; k.seq++) {
        assert(k.seq < 1000000);
        if ((receipt_hash(&k) & table.mask) != bucket) continue;
        attempts[n].key = k;
        assert(pthread_create(&threads[n], NULL, run, &attempts[n]) == 0);
        entered(&attempts[n]); n++;
    }
    struct attempt ninth = {.key = k, .duplicate = 1};
    for (; (receipt_hash(&ninth.key) & table.mask) != bucket; ninth.key.seq++);
    run(&ninth);
    struct efs_receipt r;
    int present = efs_receipt_lookup(&table, &attempts[0].key, &r);
    printf("pending first receipt retained=%d, evictions=%llu\n", present,
           (unsigned long long)table.evictions);
    struct attempt duplicate = {.key = attempts[0].key, .duplicate = 1};
    run(&duplicate); /* Must join or return BUSY, never start execute again. */
    printf("duplicate how=%d rc=%d peak concurrent executions of one identity=%d\n",
           duplicate.how, duplicate.rc, peak_first);
    int confirmed = !present && peak_first == 2 && duplicate.how == EFS_RECEIPT_RAN;
    pthread_mutex_lock(&gate_mu); release_all = 1; pthread_cond_broadcast(&gate_cv); pthread_mutex_unlock(&gate_mu);
    for (int i = 0; i < 8; i++) pthread_join(threads[i], NULL);
    efs_receipt_table_free(&table);
    printf("PENDING_EVICTION_CONFIRMED=%d\n", confirmed);
    return fixed ? (present && peak_first == 1 && duplicate.rc == EFS_ERR_BUSY ? 0 : 1) : (confirmed ? 0 : 1);
}
