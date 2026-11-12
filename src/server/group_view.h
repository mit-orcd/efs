#ifndef EFS_SERVER_GROUP_VIEW_H
#define EFS_SERVER_GROUP_VIEW_H

#include "efs/raft.h"
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* group_view: the published view of each Raft group hosted by this node, and
 * the sleepers that wait for it to change. Extracted from raft_host.c (Task V3).
 *
 * Owner: group_view.c and group_view_impl.h (the representation and the
 *   inline bodies; a caller never reads it) alone. Everything private
 *   (the locks view_mu and cv_mu, the condition variable applied_cv,
 *   the sleeper table wait_slot, waiter_hint, waiter_active, cv_overflow, and
 *   every v_* field of struct group_view_slot) is named by no other file;
 *   scripts/check_group_view_privacy.py fails the fast suite if one does.
 *   raft_host.c embeds one struct group_views in its host and reaches it
 *   only through the operations below.
 *
 * What it is. A publisher (the pump, and any thread that has just changed
 *   the core under h->mu) copies a group's replica state (commit, applied,
 *   term, role, leader, voters, read round and proof) into the group's slot.
 *   Readers copy it back out with host_view_get without h->mu. A reader
 *   that must wait for a change sleeps in a slot of the sleeper table; the
 *   pump wakes it with host_waiters_wake after publishing.
 *
 * Locks, in the order they may nest: h->mu, then cv_mu, then view_mu.
 *   view_mu  a leaf. Held by the publisher across its atomic stores only
 *            (never across a core getter), and by a reader for one
 *            lock/unlock pair after HOST_VIEW_TRIES odd loads (K107).
 *            No other lock is taken while it is held.
 *   cv_mu    guards the sleeper table. Held by host_waiters_wake while it
 *            scans (and then host_view_get may take view_mu), by
 *            host_waiter_sleep (as the condition mutex, so it is released
 *            while the thread sleeps) and by group_view_waiting.
 *   No operation returns holding view_mu. cv_mu is held across a return only
 *   between group_view_wait_lock and group_view_wait_unlock (the bracket
 *   below). h->mu is never taken by the owner; host_publish_view and
 *   host_waiters_wake are called with it held or not as stated per
 *   operation.
 *
 * Operations (what the caller hands over, what it gets back):
 *   group_view_init(gv)      a zeroed struct, before any thread uses it.
 *                            Makes the locks, condition variables, slots.
 *   group_view_destroy(gv)   after every user has stopped.
 *   group_view_attach(gv, gi, group)
 *                            slot gi now belongs to the group id; it
 *                            publishes "no replica" (has = 0). Before start.
 *   host_publish_view(gv, gi, r)
 *                            h->mu held by the caller; r is the group's
 *                            core, or NULL when there is none. The owner
 *                            reads the core getters and stores a new view
 *                            only if a field changed; an unchanged state
 *                            of a present core leaves the sequence and the
 *                            stamp alone. A NULL core clears has and every
 *                            proof field and bumps the stamp if has was set
 *                            (it moves the sequence each time, and stores
 *                            the stamp first); it does not touch leader.
 *                            It takes view_mu only to store. Returns nothing.
 *   host_view_get(gv, group, &v)
 *                            any lock state, no lock held on return. v is
 *                            a coherent copy of exactly one publish (round,
 *                            term and completion belong together). An
 *                            unknown group gives has = 0 and leader = -1.
 *                            A known group with no replica gives has = 0;
 *                            its leader is then the last one published (0
 *                            if none was), so test has before leader. After
 *                            HOST_VIEW_TRIES (64) odd sequence loads in one
 *                            call the reader waits on view_mu instead of
 *                            spinning, so a preempted publisher is never
 *                            starved (K107). static inline: it runs on every
 *                            metadata request.
 *   host_view_covers, host_view_quorum_index, host_view_wait_apply,
 *   host_read_wait_ready, host_read_floor_ready
 *                            pure functions of a copy and the request's
 *                            want, round and term; the last updates the
 *                            caller's struct host_read_floor. static inline.
 *   group_view_wait_lock(gv), group_view_wait_unlock(gv)
 *                            the wait bracket, below.
 *   host_waiter_sleep(gv, ...)
 *                            call only inside the bracket; see below.
 *   host_waiters_wake(gv)    after publishing, with h->mu held or not (the
 *                            pump holds it); takes cv_mu, and signals the
 *                            chosen sleepers after releasing it.
 *   group_view_waiting(gv)   1 when a sleeper slot is occupied.
 *
 * The wait bracket. The three waits (host_read_index_admitted,
 * host_wait_applied_mode, host_publication_committed) each have their own
 * predicate, so the owner exposes the lock and not a single wait operation:
 *     group_view_wait_lock(gv);
 *     host_view_get(gv, group, &v);        re-read under the lock
 *     if (<caller's predicate says: not yet>)
 *         host_waiter_sleep(gv, group, any, idx, seen, &end, ...);
 *     group_view_wait_unlock(gv);
 *   The re-read under the lock is what rules out a lost wakeup: a publish
 *   that lands before the lock is seen by the re-read, and one that lands
 *   after it finds the sleeper registered, because the wake scans under
 *   the same lock. Between lock and unlock a caller may call host_view_get,
 *   test its own predicate, call host_waiter_sleep, read the clock and log.
 *   It must not take any other lock (the logger's own try-lock aside), make
 *   an RPC, or return without the unlock. A caller holds h->mu either not at all or
 *   before the lock (h->mu, then cv_mu); none holds cv_mu or view_mu outside the bracket.
 *   host_waiter_sleep modes (any): 0 wakes when applied >= idx or the
 *   replica goes away; 1 when the view stamp moves past `seen`; 3 when
 *   host_read_floor_ready holds for the request (a reader whose rounds the
 *   pump starts). There is no mode 2: it served the direct mode, in which
 *   readers began rounds themselves, and was removed with it; the number
 *   is not reused. The floor is caller memory, copied into
 *   the slot at sleep and back at wake. When all HOST_WAITERS slots are
 *   busy the thread sleeps on the shared applied_cv instead and the pump
 *   broadcasts it. Sleeping ends at the wake or at `end`; the caller
 *   re-checks its predicate in a loop.
 *
 * What a reader may assume. A struct host_view from host_view_get is
 *   coherent and owned by the caller; nothing is held. It may be stale the
 *   moment it is returned. stamp moves after every other field of a changed
 *   publish of a present core, so a waiter that saw the new stamp has the
 *   new fields. */

#define GROUP_VIEW_NGROUPS 2
#define HOST_WAITERS 512
#define HOST_VIEW_TRIES    64 /* odd v_seq loads before a reader waits on view_mu */

/* Caller memory for host_waiter_sleep mode 3. */
struct host_read_floor {uint64_t index,context;};

/* The copy a reader gets. */
/* Coherent snapshot: round, term and completion must come from one publish.
 * host_view_get returns fields of exactly one host_publish_view. After
 * HOST_VIEW_TRIES odd v_seq loads in a row (a publisher is inside its
 * window) the reader waits on view_mu, which the publisher holds only for
 * that window, so a preempted publisher is never starved by readers (K107:
 * under TSan 20 to 66 spinning readers kept the publisher's second v_seq
 * increment waiting on the sanitizer's lock for that word) and the wait ends
 * when the publisher finishes. The uncontended read takes no lock. */
struct host_view {
    int has, role, leader, read_pending, read_done, proven_done;
    uint32_t voters;
    uint64_t commit, applied, read_index, read_round, term;
    uint64_t proven_index, proven_round, read_context;
    uint64_t stamp; /* wake generation from the same coherent publish */
};

struct group_views; /* complete type in group_view_impl.h, included below */

void group_view_init(struct group_views *gv);
void group_view_destroy(struct group_views *gv);
void group_view_attach(struct group_views *gv, int gi, uint8_t group);
int group_view_waiting(struct group_views *gv);
void host_publish_view(struct group_views *gv, int gi, struct efs_raft *r);
/* Only between group_view_wait_lock and group_view_wait_unlock. */
void host_waiter_sleep(struct group_views *gv, uint8_t group, int any,
                       uint64_t idx, uint64_t seen,
                       const struct timespec *end, uint64_t round, uint64_t term,
                       int proof_only, struct host_read_floor *floor);
void host_waiters_wake(struct group_views *gv);

/* static inline, defined in group_view_impl.h. */
static inline void host_view_get(struct group_views *gv, uint8_t group,
                                 struct host_view *v);
static inline int host_view_covers(const struct host_view *v, uint64_t want,
                                   uint64_t round, uint64_t term);
static inline int host_view_quorum_index(const struct host_view *v, uint64_t want,
                                         uint64_t round, uint64_t term,
                                         uint64_t *idx);
static inline int host_view_wait_apply(const struct host_view *v, uint64_t want,
                                       uint64_t round, uint64_t term);
static inline int host_read_wait_ready(const struct host_view *v, uint64_t want,
                                       uint64_t round, uint64_t term,
                                       int proof_only);
static inline int host_read_floor_ready(const struct host_view *v,
                                        struct host_read_floor *floor,
                                        uint64_t want, uint64_t round,
                                        uint64_t term, int proof_only);
static inline void group_view_wait_lock(struct group_views *gv);
static inline void group_view_wait_unlock(struct group_views *gv);

#include "group_view_impl.h"

#endif
