# K32 ior easy write: where the throughput went (Task FI)

Task FI, Nov 10 2026. Input to Stage 3 after decision
[D50](../decisions.md#d50--k32-moves-to-stage-3-user-nov-10) (K32 moved to Stage 3). **Verdict: PARTIAL.** The gap
reproduces on a private three-member RDMA cluster, three commit ranges carry
it, one range that looked like a regression was recovered by a later commit,
and no code was changed. K32 is not closed by this page. Rows: K32 of the
[checklist](../alpha-1.0-2plus1-checklist.md); background in
[the K32 regression note](../2026-10/k32-write-regression-20261023.md), the
[write-cost profile](../2026-10/k32-write-cost-profile-20261028.md) and run O2
of [batch O](orcd-mini-batch-o-20261109.md).

## Finding

On one host, with the same ior command and fresh stores, `ior easy` write
fell from about 1,140–1,270 MiB/s (base `08cc16af4`) to about 505–525 MiB/s
(devel `9403d0666`, the wave 70 tip; devel has since moved on by 84 commits and was not re-measured), a ratio of 2.2 to 2.4. The loss is four steps, each a
different commit range: K14's per-write cross-group transaction (about
1,100 to 610 MiB/s, the largest), K13's tickets (about 610 to 520), the
K29–K33 range (about 520 to 440, from small steps), and K35's stripe table
(about 440 to 305), of which about 70 MiB/s came back by `028b00d31` without a
named cause and `e6443fd7a` took the rest to about 518. The number of
Raft entries per MiB fell from 28.7 to 8.6 across K29/K32 with no gain in
throughput, so the numbers favour P3-G (serial dependent rounds per write)
over P3-J (entry count) as the lever; neither was tested directly.

## Method

* **Host and cluster.** node9900 (ORCD), one host: three `efsd` daemons (seed
  plus two joiners, `--quota 200G`, fresh stores under `/tmp`), one `efs-fuse`
  mount through `scripts/client.sh`, RDMA (`EFS_TRANSPORT=rdma`,
  `EFS_RDMA_DEV=mlx5_0`), direct I/O only (D42; the old builds get
  `--direct-io`), private ports 18400–18499, loopback addresses. All of `EFS_*`
  is unset first. Builds before the K33 member-count change get
  `EFS_MD_RAFT_N=3`; later builds refuse it, so the script reads the source to
  decide. Nothing on `/data1/efs`, ports 17xxx or orcd-mini was touched.
  orcd-mini was not measured.
* **Pinning a commit.** Each arm is a `git archive` tree of one commit on the
  host, built by [fi_build.sh](../../../scripts/evidence/fi/fi_build.sh) with
  `EFS_GIT_ID=<commit>` and default release flags (`-O3 -g
  -fno-omit-frame-pointer`). The build id is in every run's `meta.txt`
  (`build=`). Merge commits are tested as the merged tree.
* **Workload.** `mpirun -np 8 --host localhost:8 ior -v -a POSIX -F -e -w -r -t
  1m -b 2048m`, 16 GiB written and read back per run, one run per fresh
  cluster, exactly Task EW's O2 command. The write figure is ior's `write`
  line; it includes `close()` (the fsync share, column `close_s` in
  `fi_all.tsv`).
* **Scripts.** [fi_run.sh](../../../scripts/evidence/fi/fi_run.sh) runs one
  measurement; [fi_ab.sh](../../../scripts/evidence/fi/fi_ab.sh) runs a list of
  commits one after the other (interleaved A/B: base, devel, base, devel);
  [fi_cluster.sh](../../../scripts/evidence/fi/fi_cluster.sh) starts and stops
  the cluster; [fi_quiet.sh](../../../scripts/evidence/fi/fi_quiet.sh) is the
  quiet-host gate; [fi_sample.py](../../../scripts/evidence/fi/fi_sample.py) is
  a 1 Hz sampler of per-process CPU, host CPU, `md0` writes and load;
  [fi_summarize.py](../../../scripts/evidence/fi/fi_summarize.py) makes
  `fi_all.tsv`. Example: `sh fi_build.sh <commit>` after unpacking the tree
  to `/tmp/efs-taskFI/<commit>`, then `fi_ab.sh OUT 01-base=/tmp/efs-taskFI/base
  02-devel=/tmp/efs-taskFI/devel`. Prerequisites: an ior build, openmpi, the
  private libfuse, a free RDMA device. Cleanup: the scripts delete their
  stores; remove `/tmp/efs-taskFI*` afterwards (done).
* **Leader placement is a variable.** The two metadata groups (0 and 2) elect
  leaders on any daemon. `rs-before-n*.txt` records them. From set ab4 on, runs
  can pin the leaders with `efs-mgmt raft-step-down` (label suffix `-pG0_G2`,
  node ids). Across the builds from `1cd2092a4` to `718372cfc`, split-leader
  runs (23, the `s` runs below) are lower than co-led runs (38) for 16 of the
  17 builds that have both, by a median of 36 % (2–51 %, mostly one run each).
  Base, `f47a32754`, `d2369a49d` and devel did not move. Every number used for attribution below
  is from a run with both leaders on one daemon.
* **Host load per run.** `meta.txt` holds the load average before and after;
  `fi_all.tsv` adds the mean and the host-busy cores over the write window.
  The host was never idle: other sessions and the user's `tiny-files*` screens
  ran throughout. Before set ab12 there was no quiet gate. From ab12 on,
  `fi_quiet.sh` waited for three consecutive checks, 20 s apart, with no
  `efsd`, `efs-fuse`, `ior`, `mpirun`, `perf`, `efs-bench` or test process of
  another task. It does not look at the load average and cannot see a process
  that starts mid-run.
* **Clean and not clean.** *Gated* means the run passed the quiet gate. A
  *clean* run is gated, has both leaders on one daemon and is not `h`. Runs
  of sets ab1–ab8 and ab10 (06:47 host time and earlier) predate the gate and
  are marked `p`: not clean. Runs with the two leaders on different daemons,
  or leaders not recorded, are marked `s`: not clean. Gated runs whose mean
  1-minute load average over the write window was 8 or more are marked `h`:
  not clean (`08cc16af4` 1141, `f47a32754` 939, `d2369a49d` 590 and
  `e34cba345` 483, which also ran at 20 busy cores while another task's
  clusters were on the host; `1cd2092a4` 426 at 7.9 is kept). The 8 is a
  judgement, not a measured limit. **No run
  overlapped the wave 72 gate (07:04–07:15 host time):** ab10 ended at 06:47
  and ab12 run 01 started at 07:15:44 behind a passed quiet gate. A run that
  passes the gate can still share the host with the user's screens; every run
  is therefore a lower bound on what the host could do. Run-to-run spread of
  the same build with co-led leaders is large: `f47a32754` 1265 and 939
  (the 939 under a mean load of 8.1), base 1141 (9.6) and 1273.

## Every measured build

Write MiB/s per run, with the mean 1-minute load average over the write
window in brackets (`b`: only the load before the run is known), in history
order (`git rev-list --topo-order 08cc16af4..devel`). Flags: `p` pre-gate, `s`
leaders split or unknown, `h` mean load 8 or more. Raft entries
per MiB are the committed entries of groups 0 and 2 added together, taken from
`rs-before` and `rs-after`, per MiB written (median over the build's runs). The
Builds with no run are listed below the table.

| Build | What it is | Clean runs: MiB/s (load) | Not clean runs: MiB/s (load) and flags | Raft entries per MiB |
|---|---|---|---|---|
| `08cc16af4` | the base (dead-code cleanup merge) | – | 1158 (0.58b) p, 1162 (13.73) p, 1202 (3.1) p, 1273 (3.4) s, 1141 (9.6) h | 0.2 |
| `24e58fb1d` | K31 mount session after fork | – | 1180 (8.67) p | n/a |
| `f47a32754` | W154 merge, last build before K14 | 1265 (5.08) | 1047 (10.03) p, 1085 (12.49) p s, 939 (8.06) h, 1038 (6.13) p, 1216 (11.11) p, 1098 (10.07) p | 0.2 |
| `d2369a49d` | K14 atomic multi-chunk publication (merge) | 638 (6.99) | 591 (11.95) p, 519 (10.75) p s, 590 (9.05) h, 651 (4.68) p, 589 (9.64) p, 629 (6.0) p, 610 (12.17) p | 19.9 |
| `bd2cb0d5a` | K14 follow-up: PUB_STAGE kind | – | 627 (7.25) p, 687 (6.35) p, 643 (9.77) p | 19.9 |
| `1f3b25997` | K13 ticketed PUT/REPORT (merge) | – | 518 (9.51) p, 527 (10.92) p, 521 (9.87) p | 28.7 |
| `6b621993d` | K29 TXN_BATCH (merge) | – | 491 (8.89) p | 21.8 |
| `a23d45540` | reverted D39 tree (docs commit) | – | 498 (2.88) p | 21.8 |
| `33c68a465` | W158 (merge) | – | 467 (7.8) p | 21.8 |
| `1cd2092a4` | K32 batch K14 publication and K13 tickets (merge) | 426 (7.93) | 278 (4.57) s, 292 (7.44) p s, 457 (3.81) p | 7.8 |
| `2d03d9837` | K33/W157 formation inc. 1 | 415 (6.59) | – | 8.2 |
| `60f91b0d3` | K33 inc. 2 | 433 (4.54) | – | 7.9 |
| `f00cfdd75` | K33 client view | 452 (4.78) | – | 7.9 |
| `e6a21692d` | D31 drain | 431 (3.84) | 265 (2.35) p s | 7.6 |
| `5a09fd938` | B geometry | 443 (4.44) | 274 (3.51) p s | 7.6 |
| `55ad94072` | D42 direct I/O only (docs) | 388 (2.47) | – | 8.0 |
| `e8d25cc22` | K36 verified recovery | 421 (4.1) | 292 (3.85) p s | 7.6 |
| `ae3e7ab75` | READ_VERIFY (merge) | 458 (3.45) | – | 7.9 |
| `fc5c1d64e` | K34 (merge) | 432 (4.05) | – | 8.0 |
| `6069ada62` | A7 D42 removals (merge) | 457 (4.16), 431 (3.4), 421 (4.78) | – | 7.9 |
| `376369aa8` | K35 fsync succeeds degraded, stripe table (merge) | 311 (4.02), 300 (3.34), 306 (4.37) | – | 8.2 |
| `9fa0079c7` | merge-wave5 into K36 | 308 (4.76), 316 (4.23) | 239 (6.15) p s | 8.2 |
| `87034b518` | K36 on merged tree | 304 (5.71) | 299 (5.76) p s | 8.3 |
| `403bc788a` | READ_VERIFY, K34, A7, K35, K36 docs | 307 (4.25) | 228 (2.84) s, 258 (3.81) p s | 8.2 |
| `1fee9bb04` | report step-down | – | 242 (8.84) p s | 7.9 |
| `5a7463440` | F1 gates docs | – | 256 (3.44) p | n/a |
| `b9dfff609` | Task V read-back evidence | 386 (4.77) | 287 (4.13) s, 246 (2.91) p s | 6.8 |
| `d951ca293` | merge-wave28 (Task S) | 364 (4.17) | 244 (4.4) s | 7.7 |
| `028b00d31` | merge-wave30, devel in | 375 (6.63) | 255 (4.06) s | 7.9 |
| `e6443fd7a` | merge-wave30: exact stripe-identity hash (orcd-dd-profile) | 518 (5.75) | 330 (2.2) s | 7.4 |
| `833f25fe8` | merge-wave30 (Task AA) | 538 (4.63) | 290 (4.24) s | 7.2 |
| `e34cba345` | merge-wave31 (Task I) | – | 317 (2.91) s, 483 (14.42) h | 7.2 |
| `d891c106e` | merge-wave31 devel merge | – | 224 (4.55) s | 5.8 |
| `199ad2afd` | bound sweep metadata RPCs | 521 (5.09) | – | 8.7 |
| `4fdf8ddd2` | merge-wave36 (K56) | 502 (4.34) | – | 8.7 |
| `c887132c5` | revert of a PUT-proof drain | 518 (4.1) | – | 8.6 |
| `68303f40f` | K72, K73 (merge) | 516 (4.89) | 282 (4.97) p s | 7.2 |
| `264cb48d9` | lock BUSY split | 506 (4.59) | 515 (4.44) p | 8.6 |
| `3d5bc3fcb` | K48 (merge) | 508 (5.51) | 306 (2.86) p s | 7.4 |
| `909a07d71` | wave 45 docs | – | 299 (2.92) p s | 5.6 |
| `ff6aca40b` | K84 (merge) | 511 (3.72) | 277 (2.63) p s | 7.1 |
| `718372cfc` | wave 54 | 518 (4.45) | 250 (5.11) s, 509 (2.75) p | 8.7 |
| `79bc63a6a` | wave 60 | – | 521 (4.26) p | 8.7 |
| `2b86f47e1` | K101 | – | 584 (5.27) p s | 6.6 |
| `devel` | devel `9403d0666` (wave 70 tip; all devel runs use this build) | 525 (3.98), 515 (4.75), 505 (4.41) | 534 (2.92) p, 561 (4.69) p s, 600 (0.64b) p, 608 (4.15) p, 589 (4.34) p | 8.6 |

No usable run: `cb970ca7c` (docs only, not run), `246bae7cf` (D39 admission,
later reverted: the cluster did not start), `4026e254d` (W153: ior aborted,
rc=134, no throughput), `1cd2092a4` once with `EFS_MD_RAFT_N` exported (refused,
fixed in the script), and one empty run of set ab15 (`07-c-p1_1`, a launcher
error). `d891c106e` ran once with split leaders (224). The last queued run of
set ab16 (`d891c106e`, leaders 1:1) waited in the quiet gate for over 22 minutes
because other sessions kept the host busy; it was stopped by pid on the
coordinator's instruction and its directory is `06-cd891c106e-p1_1.NOTRUN`. A
devel run at leaders 2:2 (set ab10, run 07) was deleted by mistake and not
repeated; set ab12 has one at 2:2 (505).

## Which commit ranges move throughput

Clean runs where there are any, otherwise the `p` co-led runs, stated as
such. Single clean runs vary by about ±15 %.

| Step | Before | After | Change | Entries per MiB | Evidence |
|---|---|---|---|---|---|
| K14 (`d2369a49d`) | `f47a32754`: 1,265 (clean), 1,038–1,216 (p), 939 (h) | 638 (clean), 519–651 (p), 590 (h) | about −45 % (medians 1,100 and 610) | 0.2 to 19.9 | 1 clean + 1 h + 5 p runs before; 1 clean + 1 h + 5 p after |
| K13 (`1f3b25997`) | `bd2cb0d5a`: 627–687 (p) | 518–527 (p) | about −20 % | 19.9 to 28.7 | 3 + 3 p runs |
| K29 to K33 (`6b621993d` to `f00cfdd75`) | 518–527 (p) | 491, 498, 467 (p); 426 (clean, K32); 415, 433, 452 (clean, K33) | about −15 % in small steps, none above the spread | 28.7 to 7.8 | one to three runs each |
| K35 (`376369aa8`) | `6069ada62`: 457, 431, 421 (clean) | 311, 300, 306 (clean) | about −30 % (medians 431 and 306) | 7.9 to 8.2 | 3 + 3 clean runs |
| Partial recovery before the hash fix | 304–316 (clean, `376369aa8` to `403bc788a`) | 386 (`b9dfff609`), 364 (`d951ca293`), 375 (`028b00d31`), one clean run each | about +60–80, no named cause | 8.2 to 7.7 | not attributed |
| Recovery (`e6443fd7a`) | `028b00d31`: 375 (clean) | 518 (clean) | +38 % | 7.9 to 7.4 | 1 + 1 clean runs; 502–538 on every later clean build |
| Remaining devel | 502–538 on the 10 clean builds after `e6443fd7a` (483 once under mean load 14, `h`) | 505, 515, 525 (devel, clean) | none | 8.6 | 10 builds + devel |

Two further facts. The K35 range also raised `close()` time from 16–22 s to
29–33 s (the fsync share of the write line) and the recovery brought it to
5 s. Between `1cd2092a4` and `5a09fd938` no step exceeds the spread. The
remaining devel gap to base (515–525 against 1,141–1,265) is therefore
carried by K14 (about −500 MiB/s), K13 (about −90) and the K29–K33 range
(about −80), partly offset by about +80 from later commits (devel is above the
pre-K35 level of about 440).

## What the recovery is

`e6443fd7a` is `Merge agent/orcd-dd-profile-20261028` into the wave 30
merge. Against its first parent `028b00d31` (the other parent is `e5bba0a43`) the whole change in `src` and
`include` is `src/client/data_commit.c` (+42 −5): the client's stripe table
for K35 was looked up through the inode's bucket chain for every PUT and
again for every COMMIT reply, so both paths were quadratic in the file's
length; the merge adds an exact `(ino, ci, object)` hash and unlinks from the
inode chain in constant time. The code comment says so. What the
runs show agrees: the K35 range and `d951ca293`/`028b00d31` (364, 375 MiB/s)
have long `close()` times (11–33 s) and `e6443fd7a` does not (5 s). It is an
inference from the diff, the comment and the timing: no profile of
`stripe_find` was taken, and the recovery is one clean run before and after.

## What could and could not be attributed

* **Attributed (measured as commit ranges):** the base-to-devel gap
  reproduces (ratio 1.9 to 2.4 across all pairs, 2.2 to 2.4 with clean runs); K14 is the
  largest step; K13 a second; K35 cost about 30 %, of which about 60–80 MiB/s came back before `e6443fd7a`
  by an unnamed change and `e6443fd7a` took it from 375 to 518.
* **Mechanism known from code and timing, not profiled:** K14's step is a
  cross-group transaction per multi-chunk write (PREPARE, DECIDE, RESOLVE,
  RETIRE; `00c14c355`); K35's step is the stripe-table lookup described above.
* **Not attributed:** the K13 step (tickets) and the K29–K33 range have no
  mechanism beyond the entry counts; the small steps there are inside the
  spread. Why devel (515–525) is higher than the pre-K35 range was not
  traced. The cost split between consensus latency, thread handoffs and
  `fdatasync` was not measured: no `perf` profile and no round-latency
  measurement was taken.
* **CPU shape** (clean and `h` runs, write window): base `efsd` 1.8–2.0 cores each and
  `efs-fuse` 2.0–2.3; devel `efsd` leader 4.6–4.8 and the other two 2.4, fuse
  1.2; host busy 10.3 cores against 12.6–13.0; `md0` writes 1,902–2,151 MiB/s
  against 1,095–1,131. The write phase takes 12.9–14.4 s on base and 31–32 s on
  devel. No `efsd` thread went above 0.6 core in the sampled devel runs (all
  threads are named `efsd`, so the threads cannot be told apart). The
  leader daemon works more but no single thread is saturated, which is the
  shape of waiting rather than of work.

## What the numbers say about P3-G and P3-J

[P3-G](stage3-server-protocol-theories-20261109.md) says a consensus round
costs thread handoffs, not work, and that latency in series is the limit.
**Consistent, not proven.** Throughput fell 45 % when K14 added about 20
entries per MiB in series with each write; no thread is busy; CPU rose but
not in one place. Throughput did not rise when K29/K32 cut the entry count
from 28.7 to 8.6 per MiB, which fits rounds that stay in series even when
their entries are batched (a PREPARE is followed by its DECIDE whatever the
batch size). It does not measure a round: wake counts, round latency and
proposals per write were not taken, so the eight-wake figure is untested.

[P3-J](stage3-server-protocol-theories-20261109.md) (single ticket
admissions; combining on the leader) predicts a modest gain from fewer
entries. **The numbers do not support it as a throughput lever, with one
limit.** A 3.3-fold cut in entries (K29/K32: 28.7 to 8.6) came with no
throughput gain (about 520 to about 440). That cut was of K14 publication and
K13 ticket entries, not of `ADMIT`; a build that combines `ADMIT`s was not run,
and P3-J's own refutation test (the untraced matrix with leader-side
combining) was not run.

## Not verified

* orcd-mini: nothing was measured there. Whether its 360–600 MiB/s and the
  base figure of about 1.2 GiB/s have these causes is not shown: this cluster
  is three daemons on one host with loopback RDMA, orcd-mini's members are on
  separate hosts.
* Most steps rest on one to three runs per build with a spread of about ±15 %
  and an unquiet host; only K35 (3 + 3 clean runs) is repeated enough
  to rely on (K14 has one clean pair). The K13 and K29–K33 figures use `p` runs.
* Only the 1 MiB `ior easy` write was run, at 8 ranks; no other workload.
* The recovery commit is identified by bisection to one merge, with one
  clean run on each side; its mechanism is read from the diff, not profiled.
* Leader placement as a cause: runs with split leaders lose a median of 36 % in
  the intermediate builds; why was not investigated. Natural placement mixed both
  states in early sets, so those numbers are `s`.
* No Raft log-sync counts were taken (entries are commit-index deltas), no
  perf profile, no memory or tail-latency data.
* No change to product code, tests or any correctness check is proposed or
  made. The run trees were removed from node9900 at the end.

## Raw output

Kept outside Git on node9900: `~erbmi1/efs-artifacts/taskFI-20261110/`
(`ab1` to `ab16`, `fi_all.tsv`, 1,837 files, 93 MB). `SHA256SUMS` in that
directory, which lists every other file, has the SHA-256
`d1e35dbb29dd344265e68297220aa5faad2425194e7d4997bc1987132346c62a`.
Set ab9 and one run of set ab10 were deleted by mistake, set ab11 never ran.
