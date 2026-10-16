# Waiter wake-up outside `cv_mu` — private A/B, 2026-10-16

**Verdict: PASS on the private RDMA A/B for the lock it targets (condvar
re-acquire share 21.4% → 2.2% of leader lock waits); throughput and latency
unchanged within noise. Not deployed to orcd-mini (needs the user's word);
the user-workload effect is NOTRUN.**

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
(n1 in every run), split into condvar re-acquire
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

## Limitations and remaining gates

- Synthetic load on a private cluster; under the user's workload the share
  was 45%, so the effect there may be larger. Needs a deploy to orcd-mini
  (user approval) and a repeat of the DWARF leader capture.
- Three runs per arm; DWARF captures are 3 s each.
- Next bottleneck: proposal serialization on `h->mu`.
