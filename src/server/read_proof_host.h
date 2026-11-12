#ifndef EFS_SERVER_READ_PROOF_HOST_H
#define EFS_SERVER_READ_PROOF_HOST_H

#include <stdint.h>

/* What the read-proof owner (read_proof.h) needs from the host
 * (raft_host.c): six functions and two constants. raft_host.c defines them;
 * tests/test_read_proof.c defines fakes. The owner calls nothing else of the
 * host and reads two host fields through the pointers given to
 * read_proof_init: the member id (final before start) and the member count
 * (atomic). Every function is called with no host lock held, from any request
 * thread, unless it says otherwise. */

struct efs_raft_host;
struct efs_msg_raft_mkfs_reply;

/* The pump tick and the read grant: HOST_READ_TRIES ticks (80 x 5 ms =
 * 400 ms). Each of the leader path, a wait for a call in flight, the host's
 * remote call and the applied wait gets its own grant (see read_proof.h);
 * the heartbeat is 50 ms. */
#define HOST_TICK_US    5000
#define HOST_READ_TRIES 80

/* One accumulator of the EFS_READ_PROF line: count, sum and max in us. */
struct read_prof_acc {
    uint64_t n, sum, max;
};

/* 1 when the group must not serve reads (damaged or incompatible replica),
 * with the leader hint to give the caller in *hint; 0 otherwise (*hint
 * untouched). */
int read_proof_host_gate(struct efs_raft_host *h, uint8_t group, int *hint);
/* One remote call for the group: a retry loop of up to 400 ms over hinted
 * peers; `leader` is a preference, -1 allowed. OK fills *rep. Blocks. */
int read_proof_host_remote(struct efs_raft_host *h, uint8_t group, int leader,
                           struct efs_msg_raft_mkfs_reply *rep);
/* Wake the pump so it runs a cycle now. */
void read_proof_host_kick(struct efs_raft_host *h);
/* The follower read's applied wait: OK once the local replica applied idx,
 * else NOT_PRIMARY or BUSY; sets *hint. It has its own budget. */
int read_proof_host_wait_applied(struct efs_raft_host *h, uint8_t group,
                                 uint64_t idx, int *hint);
/* EFS_READ_PROF is set; and add one sample (us) to an accumulator. */
int read_prof_on(void);
void read_prof_add(struct read_prof_acc *a, uint64_t us);

#endif
