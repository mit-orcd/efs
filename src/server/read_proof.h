#ifndef EFS_SERVER_READ_PROOF_H
#define EFS_SERVER_READ_PROOF_H

#include "efs/raft.h"
#include "group_view.h"
#include "read_proof_host.h"

/* read_proof: the life of one metadata ReadIndex request (Task V4).
 *
 * Owner: read_proof.c and read_proof_impl.h alone. The per-group counters
 *   ri_begun and ri_request, the slot table, the follower coalescer
 *   g_ri_coal and the rp_* fields are named by no other file
 *   (scripts/check_group_view_privacy.py fails the fast suite if one does).
 *   raft_host.c embeds one struct read_proof as h->rp.
 *
 * A request asks "is my local state at least as new as everything committed
 * when I arrived?" and gets OK (with the proof index), NOT_PRIMARY (with a
 * leader hint) or BUSY. A follower read passes through any other code of the
 * host's remote call (INVAL, NOMEM, the leader's reply code). The hint may be
 * -1, this member or a stale leader. On the leader it is admitted from the
 * published view (commit floor, first round whose probes cannot precede it,
 * term), raises ri_request, and sleeps until the pump's round has a quorum. On
 * a follower it asks the leader (one coalesced remote call per round), then
 * the host's applied wait. It never takes h->mu. 400 ms (HOST_READ_TRIES x
 * HOST_TICK_US) bounds the leader path (S2 to S6) and a wait for another
 * caller's RPC. A follower read waits up to that deadline for a call in
 * flight, then makes the host's remote call (own 400 ms) and the applied wait
 * (own 400 ms): about 800 ms when no call is in flight on arrival, about
 * 1.2 s otherwise or when a sleeper turns follower. All are upper bounds: an
 * inherited net deadline shortens the remote call. A pair may run two in
 * sequence. No caller cancellation.
 *
 * State table. Each pass of the loop reads a fresh copy of the view and tests
 * the rows in order, except as S2 says.
 *  S0 gate    host gate refuses (damaged/incompatible group): NOT_PRIMARY with
 *             the host's hint. No replica (has == 0): NOT_PRIMARY. Every pass.
 *  S1 follower  role != leader. proof_only or leader == self: NOT_PRIMARY.
 *             Else ask the leader (S9). Reached at the top of the first pass
 *             whose copy shows role != leader. The role is tested before S5,
 *             so a frozen floor never wins over a role change the copy shows.
 *             A pass on an older copy finishes on it (S2 excepted).
 *  S2 not admitted  leader, round == 0. Takes want = commit, round = begun+1,
 *             term from the view. The larger of ri_begun and the view's read
 *             round at its limit: BUSY. A group without a slot: the view is
 *             re-read and the loop spins, with no sleep and no deadline, until
 *             S0 or S1 ends it; it cannot happen while both owners attach the
 *             same ids. S2 then re-reads the view. S0 and S1 (gate, replica,
 *             role) are not tested on that copy; S3, the term and all of S5
 *             are. A new term returns NOT_PRIMARY at
 *             once, with the hint of the older copy (it may name this
 *             member), even if the role changed too. A role change in the
 *             same term is acted on at the next pass.
 *  S3 admitted, not requested  raises ri_request to round once (CAS) and
 *             kicks the pump; readers of one round share the raise and kick.
 *  S4 requested  the pump, or the read-ack path host_read_control on a
 *             receive thread (read_proof_start_requested, with h->mu), begins the
 *             round once the previous one has its quorum, if the core still
 *             leads; else ri_request stays raised. A refused begin is
 *             retried; the reader meanwhile is in S5/S6, ends at its deadline.
 *  S5 check   term != admitted term: NOT_PRIMARY. Read context changed (the
 *             core's read_epoch: config change, storage failure, 0 when not
 *             leader): NOT_PRIMARY. A frozen floor with applied >= floor (or
 *             proof_only): OK(floor). With read context 0 only (with a
 *             non-zero context the floor row decides first):
 *             proof_only quorum: OK; a covering round: OK(proven index).
 *             Past the deadline: BUSY. Else S6.
 *  S6 asleep  mode-3 slot, registered under the wait bracket after a re-read.
 *             Woken by a publish and wake (the pump's cycle end or
 *             host_read_control) when host_read_floor_ready holds
 *             (replica gone, role, term or context changed, proof
 *             sufficient), else by the deadline. Host stop: while handlers
 *             drain the pump still runs; host_stop_leading steps down and
 *             publishes, a wake follows in that cycle, the sleeper goes to S1
 *             (leader -1). server_raft_host_stop must run with no request
 *             alive: after the pump join it destroys the group view, frees
 *             the host and wakes nobody. efsd guarantees that by draining the
 *             connection handlers first. No run-time path publishes has == 0.
 *  S8 done    OK, NOT_PRIMARY, BUSY or a passed-through code of the remote
 *             call; nothing held; ri_request stays raised
 *             (the pump may run one round nobody waits for).
 *  S9 asking the leader  one remote call per round, leaf lock released
 *             across it; `leader` is a preference, -1 allowed. Callers of the
 *             next round share it; a caller's own deadline ends only that
 *             caller (BUSY), and not the caller that issues the call. A local
 *             role or term change during the RPC or the applied wait is not
 *             seen; the RPC's result stands. A failed RPC returns its code
 *             with the hint of the view copy taken before it (the old leader);
 *             the remote's hint is dropped. Then the host's applied wait (its
 *             own 400 ms, not this deadline); a BUSY carrying another leader's
 *             hint becomes NOT_PRIMARY plus that hint.
 *
 * Operations (no lock held by the caller unless stated):
 *   read_proof_init(rp, host, gv, &raft_id, &members)   before any thread.
 *   read_proof_attach(rp, gi, group)   slot gi serves the group id.
 *   read_proof_set_telemetry(rp, t)    once, before readers.
 *   read_proof_read(rp, group, &hint)  OK / NOT_PRIMARY(hint) / BUSY.
 *   read_proof_prove(rp, group, &hint, &idx)  the proof endpoint: the quorum
 *                                      index, never forwards.
 *   read_proof_read_pair(rp, a, b, &hint)  two groups, rounds overlapped.
 *   read_proof_start_requested(rp, cores)  h->mu HELD. cores[i] = slot i's
 *                                      core, NULL if not hosted. Begins the
 *                                      requested rounds, publishes those
 *                                      groups, returns the mask of slots.
 *   read_proof_begin(rp, group, r)     h->mu HELD. Raises ri_begun, then
 *                                      efs_raft_read_begin.
 *
 * Division with group_view. The group view publishes views and owns the
 * sleeper slots; this owner decides what a request does with a copy.
 * host_read_wait_ready and host_read_floor_ready (group_view_impl.h) are the
 * wake and completion rule: the pump evaluates them for every sleeper, and
 * the request evaluates the same function before it sleeps. This owner
 * calls host_view_get, host_view_covers, host_view_quorum_index,
 * host_view_wait_apply, group_view_wait_lock/unlock, host_waiter_sleep (mode
 * 3) and host_publish_view; never host_waiters_wake. The follower's applied
 * wait (host_wait_applied, mode 0) belongs to the host.
 *
 * Locks: h->mu, then the group view's two locks (the order is in
 *   group_view.h). The request side never takes h->mu (the host's remote
 *   call takes the server's node-table and peer-pool locks); the leaf lock
 *   g_ri_coal[].mu is released across the RPC; the pair path's
 *   host_read_pair_mu (read_index_pair_remote.h) guards its worker pool only.
 *   start_requested and begin need h->mu.
 *
 * Publication. A change in role, term or read state reaches a request only
 * through a published view. host_publish_hosted_and_wake (raft_host.c, h->mu
 * held) publishes every hosted group and wakes the sleepers: host_pump at the
 * end of each cycle, host_receive_persist, host_inbox_batch_handoff. Publish
 * some groups, then wake: host_pending_reads (those it completed),
 * host_read_control (one group, after start_requested). Publish, no wake:
 * read_proof_start_requested, host_stop_leading, the pump's damaged-row mark,
 * host_propose_impl, host_publication_committed, attach_replica. Step down and
 * publish nothing (seen at the pump's next cycle): host_mark_incomplete, the
 * apply path's host_mark_incompatible, host_retire_check, the operator
 * server_raft_host_step_down, and host_decide_lost_fault. */

#define READ_PROOF_NGROUPS GROUP_VIEW_NGROUPS

/* EFS_READ_PROF accumulators this owner feeds; the host prints them. */
struct read_proof_prof {
    struct read_prof_acc ri_leader, ri_leader_covered, ri_remote,
        ri_remote_wait, ri_sleep_quorum, ri_sleep_apply;
    uint64_t pair_parallel, pair_scalar;
};
extern struct read_proof_prof g_read_proof_prof;

struct read_proof; struct efs_telemetry;
void read_proof_init(struct read_proof *rp, struct efs_raft_host *host,
                     struct group_views *gv, const int *raft_id,
                     const int *members);
void read_proof_attach(struct read_proof *rp, int gi, uint8_t group);
void read_proof_set_telemetry(struct read_proof *rp, struct efs_telemetry *t);
int read_proof_read(struct read_proof *rp, uint8_t group, int *hint);
int read_proof_prove(struct read_proof *rp, uint8_t group, int *hint,
                     uint64_t *index);
int read_proof_read_pair(struct read_proof *rp, uint8_t first, uint8_t second,
                         int *hint);
unsigned read_proof_start_requested(struct read_proof *rp,
                                    struct efs_raft *const *cores);
int read_proof_begin(struct read_proof *rp, uint8_t group, struct efs_raft *r);

#include "read_proof_impl.h"

#endif
