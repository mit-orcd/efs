# `h->mu` plan steps 1–3: phase timing, routing snapshot, lock-free verdicts — 2026-10-17

**Verdict: PASS for implementation and private A/B. Deployed to orcd-mini
on 2026-10-18 (`5d673efa8fca`); see the
[deploy page](orcd-mini-deploy-5d673efa8-20261018.md).**
- Both targeted `h->mu` waits are gone: verdict reads went from 79 µs to
  0.2 µs mean, and leader-cover picks from 113 µs to 0.1 µs. The leader's
  lock-wait share of its context switches fell from 16.7% to 12.6%.
- Throughput, CPU per create and latency did not change beyond run-to-run
  noise. Creates/s rose 2,369 → 2,423 → 2,460 (+3.8%), but the per-arm
  ranges overlap.
- `make test` PASS; new slot-protocol and reader-semantics tests PASS.

Implements steps 1–3 of the [`h->mu` contention plan](raft-host-mu-plan-20261016.md)
on branch `raft-host-mu`. The live lock-wait baseline is the
[orcd-mini profile on `9099380f3`](orcd-mini-profile-9099380f3-20261017.md).

## Changes

Branch `raft-host-mu`, based on devel `e04a80f63`:

| Commit | Step | Change |
| --- | --- | --- |
| `a0eae0f09` | 1 | `EFS_WRITE_PROF=1`: per-phase log2 histograms on the leader's write path (`src/server/write_prof.h`), one `write-phase-prof` stderr line per 10,000 local-leader proposals. Off by default. |
| `05c44513f` | 2 | The per-group published view also carries the committed voter set; `host_leader_cover_pick` reads the view instead of taking `h->mu`. Routing stays a hint: the receiver still validates authority. |
| `fa26387a0` | 3 | Apply-result ring slots are seqlocks with atomic payload fields (`src/server/apply_ring.h`). All verdict readers copy a slot without `h->mu`, after at most 64 attempts falling back to a copy under `h->mu`. |
| `df9609d1c`, `382249415` | — | `scripts/raft_host_mu_ab.sh` and `scripts/raft_host_mu_ab_summary.py`. |

### Step 1: phases

All times are wall-clock, and the phases overlap, so they do not add up to
a total:
- `hold`: taking the disk-sync hold.
- `mu`: first `h->mu` acquisition in propose.
- `append`: append and submit, with `h->mu` held.
- `spin`: the `sched_yield` batching loop.
- `persist`: the local fsync of the burst.
- `mu_after`: re-acquiring `h->mu` after the fsync.
- `propose`: all of `host_propose_impl`.
- `commit_apply`: from propose returning to the entry being applied (quorum
  ack, pump pickup and apply). It is 0 when the entry had already applied.
- `wake`: from apply until the waiter sees it.
- `verdict`: reading the verdict.
- `apply`: one `host_apply` call.
- `cover`: `host_leader_cover_pick`.
- `report_mu`: acquiring `h->mu` in `host_report_release`.

Quantiles are reported as their bucket's upper bound, so they are within 2×
and never below the true value. Not instrumented: the RPC reply after the
verdict, and follower-side persistence.

### Step 3: result-ring protocol

This follows Boehm's C11 seqlock protocol, which avoids the data race
described in WG21 P1478:
- **Writer:** sets the sequence odd (relaxed), issues a release fence,
  stores the fields relaxed, then sets the sequence even with release.
- **Reader:** loads the sequence (acquire), loads the fields relaxed,
  issues an acquire fence, and re-checks the sequence.

Writers stay serialized by `h->mu`. The protocol covers both `host_apply`
stamps and the reset in `attach_group`; the reset now clears every field,
where it used to clear only `idx` and `rc`. The arc miss counters became
atomic, and `hosted` is stored atomically at attach.

Each reader takes one slot copy and keeps its existing rules:

| Reader | Slot holds another index | Same index, other term | Group not hosted here |
| --- | --- | --- | --- |
| `host_wait_verdict` / `host_wait_refs` | BUSY | NOT_PRIMARY | OK |
| `host_wait_settled` | OK (not checked) | NOT_PRIMARY | OK |
| `host_propose_wait_ex_impl` | BUSY | NOT_PRIMARY | BUSY with `extra_out` (no local extra) |
| `host_pub_batch_wait` (every entry) | STALE | STALE | OK (forwarded batch) |

Because each decision uses a single slot copy, an APPEND_RSV's `rc` and
its reserved offset always come from the same apply.

## Tests

`tests/test_apply_ring.py` (new):
- **Slot protocol:** wraparound, reset, and readers giving up after the
  retry bound while a writer is stuck. One writer races three readers;
  85–110 M copies per run, 0 torn.
- **ThreadSanitizer, macOS (clang):** the slot protocol is clean, and a
  negative control with plain payload fields is reported as a data race.
  TSan does not model standalone fences, so a clean run is supporting
  evidence, not proof. On node9901 the gcc 11 TSan build fails under
  `-Werror` and the check skips.
- **Readers:** the production functions are compiled from `raft_host.c`
  and checked against the semantics table above. They take no `h->mu` on
  the fast path. While a writer holds a slot under `h->mu`, a reader falls
  back to the lock and returns that writer's complete record.

`tests/test_write_prof.py` (new) checks buckets, quantile bounds,
concurrent adds and the line format. `tests/test_host_cover_route.py` now
fails if the cover pick takes `h->mu`. Ten source-extracting test harnesses
gained stubs for the new hooks.

Results:
- **Full suite:** `make test` on node9901 for the step-3 tree,
  `ASAN_OPTIONS=detect_leaks=0` (LSan is broken on node9901): EXIT=0.
- **Per commit:** the tests that compile one of the changed functions (33
  at steps 1–2, 34 at step 3) PASS on each of the three commits, both on
  the Mac (clang) and on node9901 (gcc).
- **History rewrite:** two rewrites fixed a step-1 Makefile dependency on
  `apply_ring.h` and moved harness stubs into the right commits. Source,
  headers and Makefile came out identical. A third rewrite rebased the
  branch onto devel `e04a80f63` (see Builds).

## Private A/B

### Environment

- **Host:** node9901 (AMD EPYC 9474F, 192 CPUs, kernel
  5.14.0-503.38.1.el9_5).
- **Cluster:** three efsd on 10.1.99.1:17651–17653, `EFS_TRANSPORT=rdma`,
  `mlx5_0` (fw 20.41.1000, PORT_ACTIVE), 3-way metadata Raft,
  `--direct-io`, `--quota 30G`.
- **Storage and client:** a fresh store under `/tmp/efs-mu-ab/<label>` for
  every run, and one local efs-fuse mount.
- **Untouched:** orcd-mini (17432–17434), its clients and w100. The user's
  orcd-mini workload shares the host, so CPU contention is not controlled.

### Builds

`make efsd efs-fuse efs-mgmt` with gcc 11.5.0, `-O3`, the private libfuse:

| Arm | Commit | efsd sha256 |
| --- | --- | --- |
| base (step 1 only, instrumented) | `1a281fc38` = `a0eae0f09` | `fcbf7a02d6eff89c…` |
| route (+ step 2) | `ae09ae498` = `05c44513f` | `bf36797991277e27…` |
| ring (+ step 3) | `dfdf7f28a` = `fa26387a0` | `070f055b09d57d91…` |

Each arm was built from the commit on the left, on devel `b7f7d4e41`.
The branch was then rebased onto devel `e04a80f63`, which orcd-mini was
running. Apart from the two harness-only fixes below, the commit on the
right differs from its arm only by `e04a80f63`, a storage-root fencing fix
in `store.c`/`efsd.c` that sits outside the write path measured here.

### Method

`scripts/raft_host_mu_ab.sh <arm> <arm><round> 40`, three rounds of base →
route → ring, with `EFS_WRITE_PROF=1` in every arm. Each run on its fresh
cluster:
1. An idle probe.
2. 40 s of load: 16 writers creating 256 B files, an md5 reader over 2,000
   files and a crawler, with a 200-file create/stat/unlink probe in the
   foreground.
3. efsd CPU (utime+stime) over the load window, divided by completed
   creates.
4. A 3 s `perf -e cs --call-graph dwarf` capture of the group-0 leader.

Both groups were led by n1 in every run.

### Results

Per-arm means of three runs:

| Metric | base | route | ring |
| --- | --- | --- | --- |
| Creates/s (runs) | 2,369 (2,269 / 2,429 / 2,409) | 2,423 (2,358 / 2,461 / 2,450) | 2,460 (2,411 / 2,542 / 2,428) |
| efsd CPU per create, all three / leader (µs) | 2,943 / 1,644 | 2,850 / 1,578 | 2,905 / 1,619 |
| Loaded create p50 / p95 / p99 (ms) | 2.75 / 4.88 / 9.16 | 2.75 / 4.48 / 9.09 | 2.85 / 4.40 / 7.07 |
| Loaded stat p50 / p95 / p99 (ms) | 0.81 / 1.81 / 4.21 | 0.88 / 1.93 / 3.93 | 0.89 / 1.91 / 3.75 |
| Leader lock-wait switches, % of its switches | 16.7% | 14.7% | 12.6% |
| `arc_fallback` (all runs) | 0 | 0 | 0 |

Leader `write-phase-prof`, mean µs, with p50 / p95 / p99 in brackets. Each
value is the mean over runs of a cumulative line:

| Phase | base | route | ring |
| --- | --- | --- | --- |
| `verdict` | 79.0 (63 / 511 / 1023) | 73.1 (52 / 340 / 852) | **0.2** (0 / 1 / 1) |
| `cover` | 112.6 (42 / 682 / 1706) | **0.1** (0 / 1 / 1) | 0.1 (0 / 1 / 1) |
| `mu` (propose) | 75.4 (12 / 511 / 1023) | 78.5 (20 / 511 / 1023) | 87.4 (31 / 511 / 1023) |
| `persist` | 112.5 (0 / 682 / 2047) | 103.5 (0 / 511 / 2047) | 83.7 (0 / 340 / 1706) |
| `mu_after` | 61.3 (0 / 511 / 1023) | 55.4 (0 / 511 / 1023) | 50.4 (0 / 511 / 1023) |
| `propose` | 286.0 (127 / 1364 / 3412) | 273.4 (127 / 1364 / 2730) | 256.1 (127 / 1023 / 2047) |
| `commit_apply` | 300.1 (255 / 1364 / 2730) | 306.8 (255 / 1023 / 2047) | 321.9 (255 / 1023 / 2047) |
| `wake` | 35.8 | 35.3 | 39.9 |
| `apply` | 28.4 | 27.6 | 27.2 |
| `report_mu` | 193.2 (127 / 1023 / 2047) | 189.7 (127 / 1023 / 2047) | 189.1 (127 / 1023 / 2047) |

Leader lock-wait sites (DWARF), as a share of lock waits:

| Site | base | route | ring |
| --- | --- | --- | --- |
| `host_propose_impl` (both callers) | 30.8% | 37.5% | 48.7% |
| `host_wait_verdict` (submit + CREATE) | 16.0% | 18.6% | — |
| `host_leader_cover_pick` (CREATE + REPORT) | 15.2% | — | — |
| `host_pump` | 8.1% | 8.5% | 11.0% |
| `host_report_release` | 6.8% | 8.5% | 10.7% |
| `host_pub_batch_wait` | (below top 8) | 4.1% | — |

### Reading

- **Targeted waits.** Each change removes the wait it targets. The
  remaining `h->mu` waits move to `host_propose_impl`, `host_pump` and
  `host_report_release`, as expected when two of six holders stop taking
  the lock. The propose `mu` wait grows (75 → 87 µs) while its `persist`
  and `mu_after` shrink.
- **End to end.** Throughput, CPU per create and p50 latency are flat
  within noise: the base range of 2,269–2,429 overlaps both other arms.
  The ring arm's lower create p99 and propose p95/p99 are consistent but
  not proven over three runs. This workload is bound by proposal
  serialization and by `commit_apply`, the quorum round trip plus apply
  (~300 µs mean, unchanged).
- **Step 4 reassessment.** REPORT's `h->mu` acquisition averages ~190 µs
  per REPORT (p50 ≤ 127 µs) and is now the third lock site (10.7%). That
  makes the conditional step 5, durable-boundary marking in the pump, the
  next candidate, after the audit the plan requires. The proposal queue
  (step 6) addresses the largest remaining site (49% of lock waits), but
  the time is in `commit_apply` and `persist` rather than lock hold. This
  data does not yet justify it.

## Reproduce

On node9901, from a checkout of branch `raft-host-mu`:

```sh
for a in base:a0eae0f09 route:05c44513f ring:fa26387a0; do
  n=${a%%:*}; c=${a##*:}; mkdir -p /tmp/efs-mu-arm/$n
  git archive $c | tar -xf - -C /tmp/efs-mu-arm/$n
  (cd /tmp/efs-mu-arm/$n && PKG_CONFIG_PATH=/data1/efs/deps/libfuse/lib64/pkgconfig \
     make -j48 EFS_GIT_ID=$c efsd efs-fuse efs-mgmt)
done
for r in 1 2 3; do for a in base route ring; do
  scripts/raft_host_mu_ab.sh /tmp/efs-mu-arm/$a $a$r 40 >> /tmp/efs-mu-ab/results.txt
done; done
python3 scripts/raft_host_mu_ab_summary.py /tmp/efs-mu-ab/results.txt
```

Prerequisites:
- Free ports 17651–17653 (`PORT=` moves them).
- `perf_event_paranoid <= 1`, `mlx5_0`, and the private libfuse at
  `/data1/efs/deps/libfuse` (`FUSE_LIBDIR=` overrides it).
- About 10 minutes in total.

The script removes the stores on exit. Remove `/tmp/efs-mu-ab` and
`/tmp/efs-mu-arm` when done.

Raw output (outside Git) is on node9901 in `/data1/efs/prof/mu-live/`:
- `ab-results.txt` (sha256 prefix `7a2ed74bf7e0a9b9`), the input to the
  tables above;
- `make-test-d377478c3.log`, the full test log.

The per-run daemon logs and DWARF captures under `/tmp/efs-mu-ab` were
deleted after the summary; they cannot be re-read.

## Limitations and remaining gates

- **Sample size.** Three runs per arm, on a host shared with the user's
  live workload; the end-to-end differences are within noise.
- **Cumulative phase lines.** The `write-phase-prof` lines include setup
  traffic (mkfs, the probe, and the 2,000-file read set: under 5% of the
  ~95,000 creates per run).
- **Not compared:** profiling off. Every arm ran with `EFS_WRITE_PROF=1`.
- **Coverage gaps:** follower persistence and the RPC reply are not
  instrumented.
- **Remaining gates:**
  - Deployment to orcd-mini and the user's workload: done
    ([deploy page](orcd-mini-deploy-5d673efa8-20261018.md)).
  - Step 5: the REPORT audit (single-voter, waiter wakeups, pump
    scheduling, orderly shutdown) is NOTRUN.
  - Step 6: not started.
