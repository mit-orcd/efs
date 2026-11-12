#ifndef EFS_SERVER_READ_PROOF_IMPL_H
#define EFS_SERVER_READ_PROOF_IMPL_H

#ifndef EFS_SERVER_READ_PROOF_H
#error "read_proof_impl.h is private: include read_proof.h"
#endif

/* PRIVATE. The owner's representation and its inline bodies. A caller never
 * needs to read it and must not include it directly; read_proof.h includes
 * it as its last line. Nothing here is a promise: the contract is
 * read_proof.h. */

/* One hosted group's request counters. Both are atomics: the readers load
 * and raise them without any lock. */
struct read_proof_slot {
    /* ri_begun is the highest core read round that may have sent probes; it
     * is raised under h->mu BEFORE efs_raft_read_begin sends anything.
     * ri_request is the highest round a reader waits for; the pump (and the
     * read-ack path) starts it under h->mu, so readers never queue on h->mu. */
    uint64_t ri_begun, ri_request;
    uint8_t rp_group;
};

/* private fields: see the contract above. */
struct read_proof {
    struct efs_raft_host *rp_host;  /* handed back to the six host functions */
    struct group_views *rp_gv;
    struct efs_telemetry *rp_telemetry;
    const int *rp_raft_id;          /* this member's id (host field) */
    const int *rp_members;          /* member count, atomic (host field) */
    struct read_proof_slot rp_slot[READ_PROOF_NGROUPS];
};

/* The slot attached to the group id, or NULL. */
static inline struct read_proof_slot *read_proof_slot_of(struct read_proof *rp,
                                                         uint8_t group)
{
    for (int i = 0; i < READ_PROOF_NGROUPS; i++)
        if (rp->rp_slot[i].rp_group == group)
            return &rp->rp_slot[i];
    return NULL;
}

#endif
