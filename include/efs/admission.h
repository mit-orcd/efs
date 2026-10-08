#ifndef EFS_ADMISSION_H
#define EFS_ADMISSION_H
#include <stdint.h>
#include <pthread.h>

/* D30 reservation primitive; not enabled in production. One owner serializes
 * pressure observations and reservations. Nonblocking BUSY means no acceptance.
 * Bytes must be a conservative retained-memory charge, not just wire length.
 * A reservation belongs to the accepted operation until apply/definite abort;
 * cancellation of a waiter alone cannot release a possibly committed entry.
 * Recovery is a trusted internal class, never a client-controlled bypass. */
struct efs_admission_limits {
    uint64_t bytes;
    uint64_t recovery_bytes;
    uint32_t count;
    uint32_t recovery_count;
};
struct efs_admission {
    pthread_mutex_t mu;
    struct efs_admission_limits limits;
    uint64_t bytes;
    uint32_t count;
    int soft;
    int hard;
    int failed;
};
struct efs_admission_ticket {
    struct efs_admission *owner;
    uint64_t bytes;
};
int efs_admission_init(struct efs_admission *a,
                       const struct efs_admission_limits *limits);
/* Requires all tickets returned and no concurrent calls. */
int efs_admission_destroy(struct efs_admission *a);
void efs_admission_pressure(struct efs_admission *a, int soft, int hard, int failed);
/* Ticket must be zero initialized, must not be copied, and released once.
 * No internal waiting queue. Caller may retry only within its own deadline. */
int efs_admission_try(struct efs_admission *a, uint64_t bytes, int recovery,
                      struct efs_admission_ticket *ticket);
int efs_admission_release(struct efs_admission_ticket *ticket);
#endif
