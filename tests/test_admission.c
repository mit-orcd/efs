#include "efs/admission.h"
#include "efs/common.h"
#include <assert.h>
#include <stdio.h>
#include <sched.h>

static struct efs_admission a;
static void *hammer(void *arg)
{
    unsigned i;
    (void)arg;
    for (i=0; i<20000; i++) {
        struct efs_admission_ticket t = {0};
        int rc = efs_admission_try(&a, 11, i%3 == 0, &t);
        assert(rc == EFS_OK || rc == EFS_ERR_BUSY);
        if (rc == EFS_OK) {
            pthread_mutex_lock(&a.mu);
            assert(a.bytes <= a.limits.bytes && a.count <= a.limits.count);
            pthread_mutex_unlock(&a.mu);
            sched_yield();
            assert(efs_admission_release(&t) == EFS_OK);
        }
    }
    return NULL;
}
int main(void)
{
    struct efs_admission_limits limits = {100, 20, 10, 2};
    struct efs_admission_ticket t[12] = {{0}};
    pthread_t threads[24];
    unsigned i;
    assert(efs_admission_init(&a, &limits) == EFS_OK);
    assert(efs_admission_try(&a, 80, 0, &t[0]) == EFS_OK);
    assert(efs_admission_try(&a, 1, 0, &t[0]) == EFS_ERR_INVAL);
    assert(a.bytes == 80 && a.count == 1);
    assert(efs_admission_try(&a, 1, 0, &t[1]) == EFS_ERR_BUSY);
    assert(!t[1].owner);
    assert(efs_admission_try(&a, 20, 1, &t[1]) == EFS_OK);
    assert(efs_admission_try(&a, UINT64_MAX, 1, &t[2]) == EFS_ERR_BUSY);
    assert(efs_admission_destroy(&a) == EFS_ERR_BUSY);
    assert(efs_admission_release(&t[0]) == EFS_OK);
    assert(efs_admission_release(&t[1]) == EFS_OK);
    assert(efs_admission_release(&t[1]) == EFS_ERR_INVAL);
    for(i=0;i<8;i++) assert(efs_admission_try(&a,1,0,&t[i]) == EFS_OK);
    assert(efs_admission_try(&a,1,0,&t[8]) == EFS_ERR_BUSY);
    for(i=8;i<10;i++) assert(efs_admission_try(&a,1,1,&t[i]) == EFS_OK);
    assert(efs_admission_try(&a,1,1,&t[10]) == EFS_ERR_BUSY);
    for(i=0;i<10;i++) assert(efs_admission_release(&t[i]) == EFS_OK);
    efs_admission_pressure(&a,1,0,0);
    assert(efs_admission_try(&a,1,0,&t[0]) == EFS_ERR_BUSY);
    assert(efs_admission_try(&a,1,1,&t[0]) == EFS_OK);
    efs_admission_pressure(&a,1,1,0);
    assert(efs_admission_try(&a,1,1,&t[1]) == EFS_ERR_BUSY);
    efs_admission_pressure(&a,0,0,1);
    assert(efs_admission_try(&a,1,1,&t[1]) == EFS_ERR_IO);
    /* Failed persistence changes acceptance, never revokes accepted tickets.
     * The owner resolves definite failure before returning the reservation. */
    assert(a.count == 1 && a.bytes == 1 && t[0].owner == &a);
    assert(efs_admission_release(&t[0]) == EFS_OK);
    efs_admission_pressure(&a,0,0,0);
    for(i=0;i<24;i++) assert(!pthread_create(&threads[i],NULL,hammer,NULL));
    for(i=0;i<24;i++) assert(!pthread_join(threads[i],NULL));
    assert(a.count == 0 && a.bytes == 0);
    assert(efs_admission_destroy(&a) == EFS_OK);
    limits.recovery_bytes = limits.bytes;
    assert(efs_admission_init(&a, &limits) == EFS_ERR_INVAL);
    limits.recovery_bytes = 20;
    limits.recovery_count = limits.count;
    assert(efs_admission_init(&a, &limits) == EFS_ERR_INVAL);
    puts("bounded admission: count/bytes, recovery reserve, pressure, failed persistence, 24-thread contention PASS");
    return 0;
}
