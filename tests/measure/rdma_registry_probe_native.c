/* Native test of the collector wrapper, not a TCP mock or RDMA gate. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
#define AI_REG_SITES 4
static const char *ai_site_names[] = {
    "harvest_recv_wcs", "conn_create", "efs_rdma_conn_destroy", "efs_rdma_totals"
};
#include "rdma_registry_probe.h"
static unsigned protected_count;
static void *worker(void *arg)
{
    (void)arg;
    for (unsigned i=0; i<1000; i++) {
        ai_registry_lock(i % AI_REG_SITES);
        assert(pthread_mutex_trylock(&g_reg_lock) == EBUSY);
        protected_count++;
        ai_registry_unlock();
    }
    return NULL;
}
int main(void)
{
    pthread_t workers[8];
    for (unsigned i=0;i<8;i++) assert(!pthread_create(&workers[i],NULL,worker,NULL));
    for (unsigned i=0;i<8;i++) assert(!pthread_join(workers[i],NULL));
    assert(protected_count==8000);
    assert(!pthread_mutex_trylock(&g_reg_lock));
    assert(!pthread_mutex_unlock(&g_reg_lock));
    puts("AI native collector: serialized ownership, paired releases, 8000 acquisitions PASS; RDMA NOTRUN");
    return 0;
}
