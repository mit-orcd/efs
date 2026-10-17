# Waiter wake-up outside `cv_mu` — 2026-10-16

**Verdict: PASS. Deployed to orcd-mini as `4aff22c62` (smoke POSIX 216/216).**
- Private RDMA A/B: condvar re-acquire 21.4% → 2.2% of leader lock waits;
  throughput and latency unchanged within noise.
- Under the user's workload, condvar re-acquire is 6.8% of the leader's lock
  waits, down from 45.1% on `f4a373b13`. The leader's lock waits are now
  ~72% `h->mu`; proposals alone (`host_propose_impl`) are 29%.

Follows [orcd-mini profile on `f4a373b13`](orcd-mini-profile-f4a373b13-20261016.md),
which found 45% of the leader's lock waits were woken waiters re-acquiring
`h->cv_mu` inside `pthread_cond_timedwait` (`host_waiter_sleep`).

## Change

`host_waiters_wake` (`src/server/raft_host.c`) still scans the waiter slots
and decides under `cv_mu` (sets `notified`, captures read floors, snapshots
`cv_overflow`), but records the chosen slot numbers and calls
`pthread_cond_signal` / the overflow `applied_cv` broadcast after unlocking.
glibc has no wait morphing, so a waiter signalled under the lock woke only to
block on `cv_mu` until the scan ended.

Lost-wakeup argument, unchanged in substance: a waiter re-reads the view and
registers under `cv_mu`, then parks atomically. Any waiter the scan picks was
parked before the scan took `cv_mu`, so the late signal reaches it unless it
has already left by timeout, in which case its loop re-reads the view. If a
slot is freed and reused before the late signal, the new occupant gets one
spurious wakeup; all three sleep sites (`host_read_index_admitted`,
`host_wait_applied`, the publication read wait) re-check in a loop. Slot
condvars are destroyed only at host teardown. Overflow waiters counted under
the lock were parked before unlock, so the post-unlock broadcast reaches them.

`tests/test_read_waiters.py` now asserts that every signal and broadcast is
issued with `cv_mu` free and that each signal targets its own slot's condvar.
This check fails (SIGABRT) against the previous `raft_host.c`.

Not done: sharding `cv_mu` per group or shrinking per-waiter scan work.
After this change the condvar re-acquire is ~2% of leader lock waits, so
neither is justified by this data; the top waits are now proposals on `h->mu`.

## Builds

- Baseline: orcd-mini release `/data1/efs/releases/f4a373b13dc0` (no source
  change between `f4a373b13` and `ed0133266`), efsd
  `22decf7e6b2d09fcfa7b0c7e607e6a9666b3a01aa817cfb0808a9f025d7b2bd7`,
  efs-fuse `339b4696…2117`, efs-mgmt `7128bb17…0c51`.
- Candidate: `ed0133266` plus this commit's source (identical `raft_host.c`),
  built on node9901 `/data1/efs/work/cvwake`, `EFS_GIT_ID=ed0133266cvw`,
  2026-10-16T23:07:41Z, same flags (`-O3 -g -fno-omit-frame-pointer
  -march=native`), private libfuse. efsd
  `a401d13fdb5c9088fb1dc0b1ba4aaa31be3404c1327a5c5c26dfd241ba415ce2`,
  efs-fuse `c3b47eec…f0c9`, efs-mgmt `a77a600a…d6d7`.

## Environment

node9901 (AMD EPYC 9474F, 192 CPUs), three efsd on 10.1.99.1:17631–17633,
`EFS_TRANSPORT=rdma`, `mlx5_0` (InfiniBand, fw 20.41.1000), 3-way metadata
Raft, `--direct-io`, `--quota 30G`, fresh scratch stores under
`/data1/efs/prof/ab/<label>` per run; one local efs-fuse mount. orcd-mini
(17432–17434), w100 and the mounted clients were not touched.

## Method

`scripts/orcd_mini_readindex_ab.sh REL LABEL 40`, alternating baseline and
candidate three times (base1, cand1, … cand3). Each run: idle probe (200 ×
create/stat/negative lookup/unlink), then 40 s of 16 writer threads creating
256 B files plus one reader and one crawler, with the same probe in the
foreground; `perf -e cs -g` on all three efsd for 10 s; then (new in this
commit) a 3 s `perf -e cs --call-graph dwarf,16384` on the group-0 leader
(n1 in every run; see the leader-detection note below), split into condvar re-acquire
(`__pthread_mutex_cond_lock`) versus plain `pthread_mutex_lock` waits.

## Results

| Metric | Baseline (3 runs) | Candidate (3 runs) |
| --- | --- | --- |
| Leader lock waits that are condvar re-acquire | 21.2 / 21.4 / 21.5% | 2.3 / 2.4 / 1.8% |
| Leader lock-wait switches in 3 s | 16,785 / 17,930 / 16,390 | 15,259 / 13,254 / 13,274 (−18%) |
| All-efsd switches waiting on a mutex | 12.2 / 12.5 / 12.7% | 11.0 / 10.8 / 10.8% |
| 16-writer creates/s | 2,288 / 2,221 / 2,136 (mean 2,215) | 2,250 / 2,266 / 2,206 (mean 2,241) |
| Loaded stat p50 / p99 ms | 1.00–1.12 / 2.89–4.89 | 1.05–1.15 / 2.97–3.92 |
| Loaded create p50 / p99 ms | 2.90–3.29 / 8.63–16.10 | 3.11–3.59 / 9.89–10.38 |
| Idle stat p50 ms | 0.21–0.24 | 0.21–0.27 |
| `read-sleep` > 20 ms log lines | 25 / 25 / 72 | 38 / 84 / 65 |

Baseline leader condvar re-acquire sites: 8.4–8.9% `host_read_index`,
6.1–6.3% `host_wait_applied` (create verdicts), 3.7–5.7% LOOKUP. In the
candidate none reaches the top 12; the leader's waits are `h->mu` in
`host_propose_impl` (≈30%), `host_wait_verdict` (≈17%),
`host_leader_cover_pick` (≈16%), `host_report_release` and `host_pump`.

The create rate (+1.2%) and latencies are within run-to-run noise: this
workload is bounded by proposal serialization on `h->mu` and client RPC
latency, not by the wake-up. The `read-sleep` counts are noisy in both arms
(25–84); no trend is claimed.

## Tests

`make test` on node9901 (candidate tree), run line by line with
`/data1/efs/prof/cvw-test.sh`; lines that hit the LeakSanitizer fatal error
(LSan cannot run under the node's ptrace restrictions) were rerun once with
`ASAN_OPTIONS=detect_leaks=0`. On the Mac, `test_read_waiters.py`, all
`test_readindex_*.py` and `test_raft_read_admission.py` PASS.

**PASS: 264/264 recipe lines**, 0 failures. Three ran without leak checking
after the LSan fatal error: `test_readindex_pair.py`, `test_raft_ack_window.py`
and `test_host_outbox.py`. These are the three with known broken LSan on
node9901. The runner kept the updated `test_read_waiters.py`.

## Reproduce

```sh
# node9901, from a build of each tree (efsd, efs-fuse, efs-mgmt + scripts/)
for r in 1 2 3; do
  scripts/orcd_mini_readindex_ab.sh <baseline-release> base$r 40
  scripts/orcd_mini_readindex_ab.sh <candidate-build> cand$r 40
done
```

Prerequisites: free ports 17631–17633, `perf_event_paranoid <= 1`, `mlx5_0`,
the private libfuse. Each run formats, measures and removes its own cluster
(about 3 minutes); kill leftover efsd and unmount `/data1/efs/prof/ab/*/mnt`
if a run is interrupted. Raw output (outside Git): node9901
`/data1/efs/prof/cvw-ab.log` (sha256 `5195205f…3240b`) and perf data under
`/data1/efs/prof/ab/cvw-*/perf/`.

## orcd-mini deployment of `4aff22c62` — 2026-10-16 23:50Z

**Deployed with the user's approval; smoke PASS.** orcd-mini moved from
`f4a373b13dc0` to `4aff22c62f9f` (version `v0.3.2-pre-alpha-595-g4aff22c62`,
built 2026-10-16T23:45:38Z). The only runtime change is this waiter wake-up.

- **Builds** (clean `git archive`, shared build ID `4aff22c62f9f`):
  - node9901 `/data1/efs/releases/4aff22c62f9f`: efsd
    `7bb704f5d975aa3b7ef05f2301c5ba878bfd6d27055cf35c1a27f7eb448b4f05`,
    efs-fuse `2311201f22d815d6682f70b91743a413830e7ef4116567d88659ba0b0faf8948`,
    efs-mgmt `684748966d427c9d8fd73f67bcd94ec005bffee4f34e18e50d2d2c8f95fc12bc`.
  - node9900 `/data1/efs/orcd-mini-client/releases/4aff22c62f9f`: efs-fuse
    `fb6a0a0759587a1234608fbde2fbcd04b8d245bb2447fa6cfeaf32060b5fd63e`,
    efs-mgmt `dbfcb5422baac346d0ed76acd3aa981821bd5c9fe81203a4e122404ffd215790`.
- **Pre-checks:** `fuser -m` showed no users on either mount; the user's
  screen sessions were detached. The daemons were running GC reclamation
  (about 60 commits/s per group); there was no client I/O.
- **Order:** node9900 `client.sh stop`; node9901 `EFS_CLUSTER=orcd-mini
  ./stop.sh` (all three roots logged `stopped clean`); repoint both `src`
  symlinks; `start.sh` (storage preserved); node9900 `client.sh start`.
- **Checks:**
  - No `recovery uncertain` lines.
  - RDMA up on mlx5_0 for all daemons and both clients.
  - `smoke.sh` POSIX **216/216 PASS** (14.1 s).

## Profile under the user's workload (`4aff22c62`)

The workload ran on both clients at once. Each client ran `tiny_files.py`
(16 workers), `tiny_files_md5.py` and `ecrawl --no-write`. Raft commits were
3,900–4,600/s on group 0 and 2,300–3,300/s on group 2. The captures are
`scripts/orcd_mini_lockprof.sh`, run three times about 3 minutes apart.

**Leader-detection note.** `efs-mgmt raft-status` prints `leader=` as a
0-based peer index, so `leader=1` means n2. Rounds 1–2 read it as a node ID
and profiled follower n1. Round 3 picks the node whose own status says
`role=LEADER` (n2 for both groups), and both scripts now do this. In the
private A/B, n1 was the leader: its captures show proposal and verdict
waits, which only the leader takes.

**Leader n2, DWARF context switches, 3 s (round 3):**
- 100,449 switches in total:
  - 39.6% idle connection threads waiting for a request;
  - 17.6% handlers asleep in `host_waiter_sleep` (waiting for commit or apply);
  - **15.0% mutex waits** (15,066);
  - 4.7% `lsm_rlock` (KV LSM rwlock, not counted as a mutex wait);
  - 3.7% direct I/O.
- Condvar re-acquire is **6.8%** of lock waits. On `f4a373b13` it was 45.1%
  of 30,079 lock waits in the same 3 s window.

Lock-wait sites on the leader, as a share of its lock waits:

| Share | Site | Mutex |
| --- | --- | --- |
| 29.2% | `host_propose_impl` (submit 19.4%, `host_propose` 9.8%) | `h->mu` |
| 17.2% | `host_wait_verdict` (submit 13.4%, CREATE 3.8%) | `h->mu` |
| 13.7% | `host_leader_cover_pick` (retained CREATE 8.3%, REPORT 5.4%) | `h->mu` |
| 5.4% | `host_report_release` (REPORT) | `h->mu` |
| 5.3% | condvar re-acquire in `host_waiter_sleep` (LOOKUP and ReadIndex) | `cv_mu` |
| 5.3% | `host_read_index_admitted` entry (LOOKUP, GETATTR lanes) | `h->mu` / `cv_mu` (not split) |
| 4.0% | `efs_raft_disk_sync_depth` under propose | disk `d->mu` |
| 3.5% | `host_pump` | `h->mu` |
| 2.7% | `host_pub_batch_wait` (REPORT) | `h->mu` |

So `h->mu` accounts for about 72% of the leader's lock waits. Proposals alone
are 29%, the largest single site. Close behind are waits that re-take
`h->mu` to read a verdict (17%) and to pick a leader cover for retained
CREATE and REPORT (14%).

All three efsd, frame-pointer context switches (10 s, round 2):
- n2 (leader): 350,705 switches, 15.1% mutex waits.
- n1: 1.9%; n3: 0.5% (mostly `host_kv_work` WAL fsync locks).

The followers' lock waits are not a bottleneck.

**node9900 probe** (200 files, while both clients ran the workload), p50 ms:

| Round | create | stat | open | unlink |
| --- | --- | --- | --- | --- |
| 1 | 8.15 | 3.67 | 6.79 | 6.31 |
| 2 | 11.69 | 7.08 | 9.44 | 8.61 |
| 3 | 11.16 | 7.50 | 8.66 | 8.45 |

These are not comparable with the `8391950f0` probes (stat p50 1.70–1.82 ms),
which ran with one client's workload; this run had two.

Raw captures (outside Git): node9901 `/data1/efs/prof/wl-4aff-r{1,2,3}/`
(`dwarf-n*.data`, `cs-all.data`, `oncpu-n*.data`, `rs*.txt`) and node9900
`/data1/efs/orcd-mini-client/prof/`. Reproduce on node9901 while the workload
runs: `scripts/orcd_mini_lockprof.sh <dir>` (needs `cs_lock.py` and
`cs_dwarf_locks.py` next to it, `perf_event_paranoid <= 1`). Run the
200-file probe on the node9900 mount at the same time.

## Limitations and remaining gates

- The user's workload is not controlled. This run had both clients active
  where earlier runs had one, so absolute rates and latencies are not
  before/after comparisons. Only the leader's lock-wait mix is compared.
- One correct-leader DWARF capture (3 s) on orcd-mini; three runs per arm in
  the private A/B.
- Next bottleneck: `h->mu`, held for proposals, verdict reads and
  leader-cover picks on the leader. The KV LSM rwlock (`lsm_rlock`, 4.7% of
  leader switches) is the largest wait that is not a mutex.
