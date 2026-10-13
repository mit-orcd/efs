#ifndef EFS_RDMA_MESSAGE_H
#define EFS_RDMA_MESSAGE_H
#include <stdint.h>
#include <stddef.h>

/* Candidate protocol, not enabled by the verbs adapter yet. All wire integers
 * are big endian. Address/GID negotiation is independent of this version. */
#define EFS_RMSG_VERSION 1u
#define EFS_RMSG_ENVELOPE 32u
#define EFS_RMSG_DATA 1u
#define EFS_RMSG_CREDIT 2u
#define EFS_RMSG_FINAL 1u
#define EFS_RMSG_QUEUE_MAX 4u

#define EFS_RMSG_SETUP_BYTES 72u
#define EFS_RMSG_CAP_SEGMENTED 1u
#define EFS_RMSG_LOGICAL_MAX (128u * 1024u * 1024u)
struct efs_rmsg_setup {
    uint32_t capabilities, physical_cap, logical_cap, receives, outstanding;
    uint32_t address_version, lid, qpn, psn, mtu;
    uint8_t gid[16];
};
/* Setup codecs are separate from existing native structs; integration must
 * replace both old setup paths together. No legacy negotiation shim. */
int efs_rmsg_setup_encode(uint8_t out[EFS_RMSG_SETUP_BYTES],
                          const struct efs_rmsg_setup *s);
int efs_rmsg_setup_decode(struct efs_rmsg_setup *s, const void *wire, size_t len);
int efs_rmsg_negotiate(struct efs_rmsg_setup *limits,
                       const struct efs_rmsg_setup *local,
                       const struct efs_rmsg_setup *peer);

struct efs_rmsg_header {
    uint8_t kind;
    uint16_t flags;
    uint64_t id; /* DATA: message sequence; CREDIT: cumulative reposts */
    uint32_t total, offset, length;
};
int efs_rmsg_encode(uint8_t out[EFS_RMSG_ENVELOPE],
                    const struct efs_rmsg_header *h);
int efs_rmsg_decode(struct efs_rmsg_header *h, const void *frame, size_t len);

/* Initialize before publishing to owners. Reservation operations are atomic,
 * including across different connection owners. limit is immutable. */
struct efs_rmsg_budget { uint64_t limit, used; };
struct efs_rmsg_view { uint8_t *frame; uint32_t len; };
struct efs_rmsg_rx {
    struct efs_rmsg_budget *global;
    uint64_t limit, used, last_id;
    uint32_t physical_cap, logical_cap, active_total, received;
    unsigned capacity, count, head;
    int broken;
    uint8_t *active;
    struct efs_rmsg_view queue[EFS_RMSG_QUEUE_MAX];
    void *(*alloc)(size_t);
    void (*dealloc)(void *);
};
/* One progress owner per rx, outside registry/Raft locks. Views are borrowed
 * until release; destroy invalidates them and releases all reservations. */
int efs_rmsg_rx_init(struct efs_rmsg_rx *r, struct efs_rmsg_budget *global,
                     uint64_t limit, uint32_t physical_cap,
                     uint32_t logical_cap, unsigned capacity);
/* 1: newly completed message, 0: incomplete; -1: poisoned stream, retire QP.
 * Caller reposts the physical buffer after this copies it, then returns credit.
 * CREDIT records must be decoded by the progress owner, not passed here. */
int efs_rmsg_rx_feed(struct efs_rmsg_rx *r, const void *frame, size_t len);
const struct efs_rmsg_view *efs_rmsg_rx_peek(const struct efs_rmsg_rx *r);
void efs_rmsg_rx_release(struct efs_rmsg_rx *r);
void efs_rmsg_rx_destroy(struct efs_rmsg_rx *r);

struct efs_rmsg_credits { uint64_t sent, returned; uint32_t capacity; };
/* One receive is reserved for control. Local SEND completion never adds credit. */
int efs_rmsg_credit_init(struct efs_rmsg_credits *c, uint32_t peer_receives);
int efs_rmsg_credit_take(struct efs_rmsg_credits *c);
int efs_rmsg_credit_return(struct efs_rmsg_credits *c, uint64_t cumulative);
#endif
