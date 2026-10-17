# `h->mu` contention plan and design review — 2026-10-16

**Verdict: steps 1–3 implemented on branch `raft-host-mu` with a private
A/B PASS ([steps 1–3](raft-host-mu-steps-20261017.md)); orcd-mini
deployment and steps 5–6 NOTRUN.** Originally a plan and review record only. Follows [waiter wake-up outside `cv_mu`](orcd-mini-waiter-wake-20261016.md)
(deployed as `4aff22c62`). Its leader profile under the user's workload put
~72% of lock waits on `h->mu`: proposals 29%, verdict reads 17% and
leader-cover picks 14%. Source references are to `src/server/raft_host.c` at
`619f5297e`; rechecked at `d68d9a58a` (see [Code and baseline
update](#code-and-baseline-update-2026-10-17)).

## How the plan was reached

1. A first external analysis proposed giving Raft state one owner. It listed
   five changes: a published routing snapshot for `host_leader_cover_pick`,
   per-request completion records for `host_wait_verdict`, batch completion
   from per-entry results for `host_pub_batch_wait`, durable-boundary
   hand-off for `host_report_release`, and a bounded submission queue drained
   by the pump in place of direct `host_propose_impl` callers.
2. Our response, checked against the code:
   - The apply-result ring (`arc_idx`/`arc_term`/`arc_rc`/`arc_extra`,
     8,192 slots per group) is already an exact result store keyed by
     (index, term), so it supersedes completion records. Its only problem is
     that it is read under `h->mu`.
   - `host_pub_batch_wait` already reads each entry's own verdict.
   - On the leader, `host_leader_cover_pick` takes `h->mu` only to learn that
     the leader is itself.
   - The published view lacks only the voter sets.
   - We proposed measuring before redesigning anything, since wait counts are
     not time. The `cv_mu` A/B removed ~90% of those waits with no change in
     throughput or latency.
3. A second review, below, accepted the revised plan with qualifications.

## Review qualifications (second review)

1. **Result-ring synchronization must be race-free C.** A sequence counter
   around plain reads and writes of `index`, `term`, `rc` and `extra` is a
   data race even with a retry
   ([P1478R8](https://www.open-std.org/JTC1/SC22/WG21/docs/papers/2022/p1478r8.html)).
   Use atomic payload accesses with a correctly ordered publication protocol,
   or start with a dedicated result-ring lock. Cover:
   - both writers: the empty/no-KV stamp near line 2691 and the normal apply
     stamp near line 2821;
   - the reset in `attach_group` near line 7734, which clears `arc_idx` and
     `arc_rc` but not `arc_term` or `arc_extra`;
   - associated counters and group-lifetime checks.

   Readers need bounded retries and a safe fallback. Current readers:
   `host_apply_rc_locked` (line 4915) and its extra variant, the term check
   near line 4998, and the batch check in `host_pub_batch_wait` near line 18166.
2. **Targeted tests in addition to existing ones:** slot wraparound,
   concurrent replacement, term mismatch, reset, and `rc` always matching its
   own `extra`. Keep each reader's existing BUSY/STALE/NOT_PRIMARY behavior;
   the readers do not share identical semantics.
3. **RPC waiting does not show that lock contention is unimportant.** A
   client's reply wait includes server-side lock delay. Low follower mutex
   counts do not rule out follower storage or replication latency. Measure
   queueing, lock acquisition, local persistence, quorum acknowledgement,
   apply and reply. Phases overlap, so their durations cannot simply be added.
4. **A/B routing and result-ring changes separately.** Compare throughput,
   CPU per completed operation and p50/p95/p99 latency under the same
   workload, not only lock-event counts.

Also from the review:
- `host_pub_batch_wait` checking every entry's verdict is an invariant to
  preserve, not a defect.
- Moving REPORT's durable-boundary marking to the pump changes when
  durability becomes visible. Before treating it as equivalent, audit
  single-voter operation, waiter wakeups, pump scheduling and orderly
  shutdown.
- The submission queue could later clarify ownership and replace the
  `sched_yield` batching in `host_propose_impl`. Current evidence does not
  justify it.

## Code and baseline update (2026-10-17)

**Code.** Rechecked at `d68d9a58a`. Since `598e9f51d`, `raft_host.c` changed
only in query code, so every finding above still holds. The batch reader moved
from near line 18150 to near line 18166; this page now uses the new number.
`8ad79fcd5` moved Raft log rotation off the metadata path (`src/raft/raft_log.c`).
Rotation used to run from `save_snap` with the pump holding `h->mu`
([rotation stall](raft-rotate-stall-20261017.md)). The `4aff22c62` figures
above were taken before that fix.

**Live baseline.** [orcd-mini profile on `9099380f3`](orcd-mini-profile-9099380f3-20261017.md),
3 s DWARF context-switch capture of the leader efsd, 2026-10-17 21:20Z:
- Lock waits are 15.6% of leader switches (13,688 of 87,487), nearly all
  `h->mu`. The cv_mu wake-up re-acquire is no longer in the top 12.
- Load: ~9,800 commits/s, efsd at 12.8 cores. Clients spend ~49% of their
  switches waiting for RPC replies.
- Workload: two `tiny_files` writers (933 and 745 writes/s) plus `edelete`,
  `ecrawl` and an md5 reader.

| Share of leader lock waits | Path |
| --- | --- |
| 27.9% | submit (`host_propose_impl` 15.5%, propose-wait 12.4%) |
| 13.9% | REPORT |
| 12.7% | retained CREATE (`host_leader_cover_pick` 8.9%, `host_wait_verdict` 3.8%) |
| 7.7% | unlink (propose 4.3%, verdict 3.4%) |
| 4.8% | `host_pub_batch_propose` |
| 3.9% | `host_create` |
| 3.2% | LOOKUP |
| 2.3% | GETATTR lanes |
| 2.2% | `host_pump` |

This is not a before/after comparison with `4aff22c62`. The workload
differs, and the ~72% figure was the `h->mu` share of lock waits, not of
leader switches. Proposals are still the largest site. REPORT is now
roughly as large as the leader-cover picks, so its conditional step 5 may
matter more than the earlier profile suggested. Step 4 decides that.

This is a lock-wait mix only, so step 1 is still NOTRUN. perf cannot split
time into queueing, lock acquisition, local persistence, quorum ack, apply
and reply. That still needs step 1's instrumentation, plus throughput,
CPU per operation and p50/p95/p99 under a controlled workload.

## Agreed sequence

Status on 2026-10-17 ([steps 1–3](raft-host-mu-steps-20261017.md)):
- Steps 1–3 are done on branch `raft-host-mu`.
- Step 4: REPORT's `h->mu` acquisition (~190 µs mean) is now the third
  lock site, so step 5 is the next candidate, after its audit.
- Step 6 is not justified yet.

| Step | Work | Gate before the next step |
| --- | --- | --- |
| 1 | Measure per-phase timing (queueing, lock acquisition, local persistence, quorum ack, apply, reply) and blocked time per completed operation | Same-workload baseline with throughput, CPU/op and p50/p95/p99 |
| 2 | Routing snapshot: publish voter sets; `host_leader_cover_pick` reads the published view as a hint, and the receiver still validates authority | Its own A/B against step 1 |
| 3 | Result-ring synchronization (atomic publication or a dedicated ring lock) for all writers, the reset and all readers, with the tests in qualification 2 | Its own A/B; reader semantics unchanged |
| 4 | Reassess with the step 1 metrics | Decide whether REPORT is on the critical path |
| 5 | Conditional: REPORT durable-boundary marking in the pump, after the audit above | Durability visibility audit recorded |
| 6 | Conditional: bounded proposal submission queue, only if `h->mu` still dominates time | New evidence |

Later, and outside this plan: one owner per Raft group. Both groups share one
Raft disk log and one KV LSM (`lsm_rlock` is 4.7% of leader switches), so
more owners alone would not remove those waits.

## Remaining gates

All steps are NOTRUN. The `9099380f3` profile is a lock-wait baseline, not
step 1's per-phase measurement. Each step's evidence page must record its
environment, build and workload per AGENTS.md.
