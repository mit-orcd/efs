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
 * leader hint) or BUSY. On the leader it is admitted from the published view
 * (commit floor, first round whose probes cannot precede it, term), raises
 * ri_request, and sleeps until the pump's round has a quorum. On a follower
 * it asks the leader (one coalesced RPC per round) and the host's applied
 * wait. It never takes h->mu. HOST_READ_TRIES x HOST_TICK_US (400 ms) bounds
 * every request; there is no caller cancellation.
 *
 * State table. Each pass of the loop reads a fresh copy of the view; the
 * first row that applies wins, so a role change beats everything below it.
 *  S0 gate    host gate refuses (damaged/incompatible group): NOT_PRIMARY with
 *             the host's hint. No replica (has == 0): NOT_PRIMARY. Checked
 *             again on every pass.
 *  S1 follower  role != leader. proof_only or leader == self: NOT_PRIMARY.
 *             Else ask the leader (S9). Reached from any later state when the
 *             role changes, even with a proof already seen: the old proof is
 *             dropped and the request goes to the new leader.
 *  S2 not admitted  leader, round == 0. Takes want = commit, round = begun+1,
 *             term from the view. A group without a slot: NOT_PRIMARY, the
 *             view is re-read and S0's no-replica rule ends it. ri_begun at
 *             its limit: BUSY.
 *  S3 admitted, not requested  raises ri_request to round once (CAS) and
 *             kicks the pump; readers of one round share the raise and kick.
 *  S4 requested  the pump (read_proof_start_requested, with h->mu) begins the
 *             round once the previous one has its quorum, if the core still
 *             leads; a core that does not lead is skipped and ri_request
 *             stays raised. A refused begin is retried; the reader meanwhile
 *             is in S5/S6 and ends at its deadline.
 *  S5 check   term != admitted term: NOT_PRIMARY. Read context changed (the
 *             core's read_epoch: config change, storage failure, 0 when not
 *             leader): NOT_PRIMARY. A frozen floor with applied >= floor (or
 *             proof_only): OK(floor). proof_only quorum: OK. A covering
 *             round: OK(proven index). Past the deadline: BUSY. Else S6.
 *  S6 asleep  mode-3 slot, registered under the wait bracket after a re-read.
 *             Woken by the pump's publish when host_read_floor_ready holds:
 *             replica gone, role not leader, term differs, context changed,
 *             proof sufficient. Else by the deadline. Host stop wakes no
 *             sleeper by itself: relinquishing leadership publishes (host_stop_
 *             leading); a removed core publishes nothing; the deadline bounds it.
 *  S8 done    OK, or NOT_PRIMARY / BUSY; nothing is held, ri_request stays
 *             raised and the pump may run one round nobody waits for.
 *  S9 asking the leader  one RPC per round under a leaf lock; callers of the
 *             next round share it, and each caller's own deadline ends only
 *             that caller (BUSY). A leadership change here is seen only when
 *             the RPC returns. Then the host's applied wait (S10: its own
 *             400 ms grant, not this deadline); a BUSY carrying another
 *             leader's hint becomes NOT_PRIMARY plus that hint.
 *
 * Operations (the caller hands over / gets back; no lock held by the caller):
 *   read_proof_init(rp, host, gv, &raft_id, &members)   before any thread.
 *   read_proof_attach(rp, gi, group)   slot gi serves the group id.
 *   read_proof_set_telemetry(rp, t)    once, before readers.
 *   read_proof_read(rp, group, &hint)  OK / NOT_PRIMARY(hint) / BUSY.
 *   read_proof_prove(rp, group, &hint, &idx)  the proof endpoint: the quorum
 *                                      index, never forwards.
 *   read_proof_read_pair(rp, a, b, &hint)  two groups, rounds overlapped.
 *   read_proof_start_requested(rp, cores)  h->mu HELD. cores[i] is group slot
 *                                      i's core, NULL if not hosted. Begins
 *                                      the requested rounds, publishes those
 *                                      groups, returns the mask of slots.
 *   read_proof_begin(rp, group, r)     h->mu HELD. efs_raft_read_begin after
 *                                      raising ri_begun; for a caller that
 *                                      begins a round itself.
 *
 * Division with group_view. The group view publishes views and owns the
 * sleeper slots; this owner decides what a request does with a copy.
 * host_read_wait_ready and host_read_floor_ready (group_view_impl.h) are its
 * wake rule and completion rule: the pump evaluates them for every sleeper,
 * and the request evaluates the same function before it sleeps. This owner
 * calls host_view_get, host_view_covers, host_view_quorum_index,
 * host_view_wait_apply, group_view_wait_lock/unlock, host_waiter_sleep (mode
 * 3) and host_publish_view; never host_waiters_wake. The follower's applied
 * wait (host_wait_applied, mode 0) belongs to the host.
 *
 * Locks: h->mu, then the group view's two locks (the order is in
 *   group_view.h). The request side
 * takes no host lock; the leaf lock g_ri_coal[].mu is released across the
 * RPC. start_requested and begin need h->mu.
 *
 * Publication. A change in role, term or read state reaches a request only
 * through a published view. The one function that publishes every hosted
 * group and wakes the sleepers is host_publish_hosted_and_wake (raft_host.c,
 * h->mu held): the pump at the end of each cycle and the two batch handoffs
 * call it. Other paths publish without waking (start_requested, stop-leading,
 * incompatibility marks, the commit path) or wake after publishing one group
 * (a read ACK, host_pending_reads); several step-downs publish nothing and
 * wait for the pump's next cycle. */

#define READ_PROOF_NGROUPS GROUP_VIEW_NGROUPS

/* The EFS_READ_PROF accumulators this owner feeds; the host prints them. */
struct read_proof_prof {
    struct read_prof_acc ri_leader, ri_leader_covered, ri_remote,
        ri_remote_wait, ri_sleep_quorum, ri_sleep_apply;
    uint64_t pair_parallel, pair_scalar;
};
extern struct read_proof_prof g_read_proof_prof;

struct read_proof;
struct efs_telemetry;

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
