# ReadIndex admission without the Raft host mutex — 2026-10-16

**PASS (scoped: private cluster performance and unit tests).** Readers of
LOOKUP, GETATTR and other fresh metadata reads no longer lock `h->mu` to admit
themselves or to start a ReadIndex round. On a private 3-daemon RDMA cluster
under a tiny-file load:
- ReadIndex-caller mutex waits fell by 90%;
- all mutex waits fell from 16.4% to 12.1% of context switches;
- loaded stat p50 fell from 1.06 to 0.87 ms, negative lookup p50 from 0.68 to
  0.54 ms.

Not deployed to orcd-mini; that needs the user's word.

Linked item: the next bottleneck named in the
[RDMA credit fix](orcd-mini-rdma-credit-20261016.md#orcd-mini-under-the-users-workload-deployed-09f8af09c).
There, LOOKUP's paired ReadIndex was 9.1% of efsd context switches on
orcd-mini, plus 1.2% from GETATTR's `host_read_index` and 1.0% from
`host_propose_wait_idx_impl`.

## Why every reader took `h->mu`

The proof itself was already batched and lock-free: a reader waits on the
published view for a quorum round whose probes followed its arrival. But two
steps took the pump's mutex:

1. **Admission.** The reader read the core's `read_round` under `h->mu` to
   learn which round may cover it. The round must be one whose probes leave
   after the reader arrived.
2. **Begin.** When no round was pending, the reader called
   `efs_raft_read_begin` under `h->mu`. When a round completed, every queued
   reader woke and raced for `h->mu` to begin the next one.

`h->mu` is held by the pump for drain, apply, tick and publish. Under load,
readers therefore parked behind whole pump cycles.

## Change

- **`ri_begun` (per group).** It is raised under `h->mu` to the round that
  `efs_raft_read_begin` is about to start, before any of its probes can leave
  (`host_read_begin_tracked`). A reader admits itself without the lock: it
  takes role, term and the commit floor from the published view, then requires
  round `max(ri_begun, view.read_round) + 1`. A round with that number or
  higher sends its probes after the reader's load, so after its arrival.
  - A failed begin only makes later admissions require a later round.
  - `recv_persisting` reports round 0, but `ri_begun` never decreases.
- **`ri_request` (per group).** A reader raises it to its round and kicks the
  pump. The pump starts the requested round under the `h->mu` it already holds,
  once the previous round has its quorum, so one round stays in flight as
  before. It does so after each read-vote pass and before each publish. The
  network read-ACK path (`host_read_control`, already under `h->mu` by trylock)
  also starts it when an ACK completes the quorum.
- **Waiting.** Readers sleep on their slot with a "delegated" predicate
  (`any=3`). They are woken for their proof, apply, term, role or context
  changes, but not because no round is pending.
- **Unchanged rules.** Proof validity is unchanged: same term, a quorum round
  at or after the admission round, `read_index >= want`, and own apply before
  rows are returned. `host_read_index_pair` admits both groups this way before
  either round is requested.
- **Removed.** The admission tickets that this replaces (`read_admit_count`
  and related fields) are gone.
- **Test harness path.** The locked admission remains only for the `!h->s`
  harness path. The publication-commit check keeps its rare locked begin,
  which also raises `ri_begun`.

### Rejected variant

The reader that raises `ri_request` would also try `pthread_mutex_trylock` and
start the round inline. Measured as `try-a` and `try-b` below, it showed no
gain (stat p50 0.93 / 0.86 ms; mutex waits 11.3 / 12.3%), and `try-a` had
worse tails. It was not kept.

## Results

- **Cluster.** node9901 (AMD EPYC 9474F, 192 CPUs, RHEL 9.7, kernel
  5.14.0-503.38.1), private 3-daemon cluster, ports 17631–17633, RDMA
  `mlx5_0`, `--direct-io`, fresh stores under `/data1/efs/prof/ab/<label>`
  with a 30 GiB quota, and one FUSE client on the same host.
- **Concurrent load.** orcd-mini kept running on the same host.
- **Run order.** Alternating: base1, new-a, base2, new-b, try-a, base3, try-b.
- **Builds.**
  - Baseline: release `09f8af09c2be` (devel `09f8af09c`; the following
    commits up to `c1c1bc570` change only docs and tests).
  - Candidate: this commit's source on `c1c1bc570`, `EFS_GIT_ID=c1c1ri000002`.
    `raft_host.c` sha256 `2b4036d724441525…`; efsd
    `a7ab26879242eaa6dddeca9d14112e18af815741dfc77325bf64a6148a77dcaf`;
    efs-fuse
    `7bf76aeaabed14fe5a9be3ed9f99e39debdc32fab76bae3ee712b706c994873d`.
- **Load phase.** 40 s with 16 writers creating 256 B files, one reader
  re-reading 2,000 files, and one crawler (walk + lstat). The probe runs
  during it: 200 creates, 200 stats, 200 negative lookups.
- **Profiling.** `perf record -e cs -g` on all three efsd for 10 s during the
  probe.

| Loaded measure | base1 / base2 / base3 | new-a / new-b |
| --- | --- | --- |
| Mutex waits, share of context switches | 16.5 / 16.0 / 16.8% | 12.3 / 11.9% |
| ReadIndex-caller mutex waits (lookup, `host_read_index*`, lanes) | 13,263 / 12,398 / 12,767 (3.3 / 3.2 / 3.4%) | 1,176 / 1,295 (0.3 / 0.3%) |
| stat p50 / p90 / p99 ms | 1.07/1.70/2.75, 1.07/1.53/2.28, 1.04/1.60/2.13 | 0.89/1.37/2.07, 0.85/1.20/1.62 |
| negative lookup p50 / p90 ms | 0.69/1.24, 0.66/1.14, 0.68/1.09 | 0.57/0.98, 0.50/0.87 |
| create p50 / p99 ms | 2.72/6.47, 2.77/5.64, 2.83/6.02 | 2.63/10.53, 2.47/6.19 |
| 16-writer creates/s | 2,717 / 2,705 / 2,685 | 2,785 / 2,899 |
| `read-sleep` (>20 ms) log lines | 10 / 10 / 49 | 7 / 7 |

- **Idle probe.** stat p50 0.21 / 0.20 / 0.22 ms (base) against 0.19 / 0.18 ms
  (new).
- **Largest remaining mutex waits:**
  - `host_propose_wait_idx_impl` and `server_raft_host_submit`, about 1.5%;
  - create/report proposals (`host_create`, `host_pub_batch_propose`);
  - unresolved libc-only stacks, about 2.5%.
  These are proposal and apply paths, not ReadIndex.
- **new-a create p99.** The 10.5 ms create p99 (max 29 ms) did not repeat
  in new-b and is in the write path, not the read path.

Raw perf data and run logs stay outside Git on node9901:
- `/data1/efs/prof/ab/<label>/perf/cs.data`;
- `/data1/efs/prof/ri/<label>.txt`. sha256 prefixes: base1 `85e6dd5f5f448093`,
  base2 `3fb76c4fa4abd904`, base3 `d4cd6da62d441ed1`, new-a `38738390e3275f1e`,
  new-b `0f588e57fd296a27`, try-a `88cc6302414f3194`, try-b
  `82ec69f653421975`.

## Tests

- New `tests/test_readindex_tickets.py`. It checks that `ri_begun` is raised
  before the probe send, and that a reader admitted during the send needs the
  next round. It also covers failure, `recv_persisting`, saturation, monotone
  requests and pump-started shared rounds. Its negative stores `ri_begun`
  after the send, and that negative must fail.
- New `tests/test_raft_read_admission.py`. Four readers make 80,000 lock-free
  admissions while a pump thread starts rounds. Every covering round's probes
  are stamped after its reader's arrival. Moving the `ri_begun` store after
  the send fails this check (3 of 3 tries). It also checks the source: no
  `h->mu` on the server-mode reader path.
- Updated:
  - `test_readindex_pair.py`: server-mode pair admission, plus a negative that
    admits from the view alone and is covered by an unpublished begin;
  - `test_read_waiters.py`: delegated wake predicate;
  - `test_readindex_result.py`, `test_read_control_host.py`: the ACK path
    starts requested rounds.
- `make test` on node9901, final tree (`results/` omitted from the upload).
  Each of the 785 recipe commands ran separately, so that one failure does
  not stop the run.
  - 3 failures are LeakSanitizer fatal errors on this host:
    `test_readindex_pair`, `test_raft_ack_window` and `test_host_outbox`.
    All three pass with `ASAN_OPTIONS=detect_leaks=0`; the baseline release
    fails `test_readindex_pair` the same way.
  - `check-architecture` fails because `results/` was omitted.
  - The first run also failed `test_raft_read_admission.py`: gcc
    `-Wmisleading-indentation` in the test harness, fixed in this commit.
    Afterwards it passed 5 of 5 runs on node9901, along with the other
    ReadIndex tests.

## Reproduce

```sh
# node9901, from a build of the tree under test (efsd, efs-fuse, efs-mgmt)
scripts/orcd_mini_readindex_ab.sh <release-dir> <label> 40
```

Prerequisites: free ports 17631–17633, `perf_event_paranoid <= 1`, the RDMA
device `mlx5_0`, and the private libfuse for efs-fuse. Run baseline and
candidate alternately; each run formats, measures and removes its own cluster
(about 3 minutes). Never point it at orcd-mini (17432–17434) or w100.

## Limitations and remaining gates

- Private-cluster load only. orcd-mini under the user's workload is NOTRUN
  until the user approves a deployment.
- The fresh-read pump hand-off adds at most one pump wake when `h->mu` is
  idle. No idle-latency regression was seen (idle stat p50 0.18–0.19 ms
  against 0.20–0.22 ms).
- Proposal-side waits (`host_propose_wait_idx_impl`, create/report batching)
  are untouched.
