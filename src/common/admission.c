#include "efs/admission.h"
#include "efs/common.h"
#include <string.h>

int efs_admission_init(struct efs_admission *a,
                       const struct efs_admission_limits *limits)
{
    if (!a || !limits || !limits->bytes || !limits->count ||
        !limits->recovery_bytes || limits->recovery_bytes >= limits->bytes ||
        !limits->recovery_count || limits->recovery_count >= limits->count)
        return EFS_ERR_INVAL;
    memset(a, 0, sizeof(*a));
    a->limits = *limits;
    return pthread_mutex_init(&a->mu, NULL) == 0 ? EFS_OK : EFS_ERR_IO;
}

int efs_admission_destroy(struct efs_admission *a)
{
    int busy;
    if (!a) return EFS_ERR_INVAL;
    pthread_mutex_lock(&a->mu);
    busy = a->count != 0;
    pthread_mutex_unlock(&a->mu);
    if (busy) return EFS_ERR_BUSY;
    return pthread_mutex_destroy(&a->mu) == 0 ? EFS_OK : EFS_ERR_IO;
}

void efs_admission_pressure(struct efs_admission *a, int soft, int hard, int failed)
{
    pthread_mutex_lock(&a->mu);
    a->soft = !!soft;
    a->hard = !!hard;
    a->failed = !!failed;
    pthread_mutex_unlock(&a->mu);
}

int efs_admission_try(struct efs_admission *a, uint64_t bytes, int recovery,
                      struct efs_admission_ticket *ticket)
{
    uint64_t cap;
    uint32_t count;
    int rc = EFS_OK;
    if (!a || !ticket || ticket->owner || !bytes || (recovery != 0 && recovery != 1))
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&a->mu);
    cap = a->limits.bytes - (recovery ? 0 : a->limits.recovery_bytes);
    count = a->limits.count - (recovery ? 0 : a->limits.recovery_count);
    if (a->failed)
        rc = EFS_ERR_IO;
    else if (a->hard || (!recovery && a->soft) || a->count >= count ||
             a->bytes > cap || bytes > cap - a->bytes)
        rc = EFS_ERR_BUSY;
    else {
        a->bytes += bytes;
        a->count++;
        ticket->owner = a;
        ticket->bytes = bytes;
    }
    pthread_mutex_unlock(&a->mu);
    return rc;
}

int efs_admission_release(struct efs_admission_ticket *ticket)
{
    struct efs_admission *a;
    if (!ticket || !ticket->owner) return EFS_ERR_INVAL;
    a = ticket->owner;
    pthread_mutex_lock(&a->mu);
    a->bytes -= ticket->bytes;
    a->count--;
    ticket->owner = NULL;
    ticket->bytes = 0;
    pthread_mutex_unlock(&a->mu);
    return EFS_OK;
}
