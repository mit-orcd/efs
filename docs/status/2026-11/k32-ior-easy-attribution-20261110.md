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
fell from about 1,140–1,270 MiB/s (base `08cc16af4`) to about 515–525 MiB/s
(devel `1b355826b`), a ratio of 2.2 to 2.4. The loss is four steps, each a
different commit range: K14's per-write cross-group transaction (about
1,100 to 610 MiB/s, the largest), K13's tickets (about 610 to 520), the
K29–K33 range (about 520 to 440, from small steps), and K35's stripe table
(about 440 to 305), which `e6443fd7a` recovered to about 518. The number of
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
* **Clean and not clean.** *Gated* means the run passed the quiet gate. Runs
  of sets ab1–ab8 and ab10 (06:39–06:47 host time and earlier) predate the
  gate and are marked `p`: not clean. Runs with the two leaders on different
  daemons, or leaders not recorded, are marked `s`: not clean. **No run
  overlapped the wave 72 gate (07:04–07:15 host time):** ab10 ended at 06:47
  and ab12 run 01 started at 07:15:44 behind a passed quiet gate. A run that
  passes the gate can still share the host with the user's screens; every run
  is therefore a lower bound on what the host could do, and run-to-run spread
  of the same build is large (`1cd2092a4`: 426, 457, 292, 278; `b9dfff609`:
  386, 287, 246).

## Every measured build

Write MiB/s per run, in history order (`git rev-list --topo-order
08cc16af4..devel`). `p` pre-gate, `s` leaders split or unknown. Raft entries
per MiB are the committed entries of groups 0 and 2 added together, taken from
`rs-before` and `rs-after`, per MiB written (median over the build's runs). The
load column is the 1-minute average before each run, lowest to highest.
Builds with no run are listed below the table.

| Build | What it is | Gated, co-led runs (MiB/s) | Other runs (MiB/s) | Raft entries per MiB | 1-min load before |
|---|---|---|---|---|---|
| `08cc16af4` | the base (dead-code cleanup merge) | 1141 | 1158 p, 1162 p, 1202 p, 1273 s | 0.2 | 0.6–9.6 |
| `24e58fb1d` | K31 mount session after fork | – | 1180 p | n/a | 7.4–7.4 |
| `f47a32754` | W154 merge, last build before K14 | 1265, 939 | 1047 p, 1085 p s, 1038 p, 1216 p, 1098 p | 0.2 | 2.9–8.1 |
| `d2369a49d` | K14 atomic multi-chunk publication (merge) | 638, 590 | 591 p, 519 p s, 651 p, 589 p, 629 p, 610 p | 19.9 | 1.0–9.4 |
| `bd2cb0d5a` | K14 follow-up: PUB_STAGE kind | – | 627 p, 687 p, 643 p | 19.9 | 4.7–7.0 |
| `1f3b25997` | K13 ticketed PUT/REPORT (merge) | – | 518 p, 527 p, 521 p | 28.7 | 7.6–8.4 |
| `6b621993d` | K29 TXN_BATCH (merge) | – | 491 p | 21.8 | 8.0–8.0 |
| `a23d45540` | reverted D39 tree (docs commit) | – | 498 p | 21.8 | 0.1–0.1 |
| `33c68a465` | W158 (merge) | – | 467 p | 21.8 | 5.5–5.5 |
| `1cd2092a4` | K32 batch K14 publication and K13 tickets (merge) | 426 | 278 s, 292 p s, 457 p | 7.8 | 3.0–7.9 |
| `2d03d9837` | K33/W157 formation inc. 1 | 415 | – | 8.2 | 5.1–5.1 |
| `60f91b0d3` | K33 inc. 2 | 433 | – | 7.9 | 5.9–5.9 |
| `f00cfdd75` | K33 client view | 452 | – | 7.9 | 6.8–6.8 |
| `e6a21692d` | D31 drain | 431 | 265 p s | 7.6 | 0.2–5.1 |
| `5a09fd938` | B geometry | 443 | 274 p s | 7.6 | 5.0–6.6 |
| `55ad94072` | D42 direct I/O only (docs) | 388 | – | 8.0 | 4.3–4.3 |
| `e8d25cc22` | K36 verified recovery | 421 | 292 p s | 7.6 | 5.3–6.0 |
| `ae3e7ab75` | READ_VERIFY (merge) | 458 | – | 7.9 | 4.5–4.5 |
| `fc5c1d64e` | K34 (merge) | 432 | – | 8.0 | 3.9–3.9 |
| `6069ada62` | A7 D42 removals (merge) | 457, 431, 421 | – | 7.9 | 0.2–5.5 |
| `376369aa8` | K35 fsync succeeds degraded, stripe table (merge) | 311, 300, 306 | – | 8.2 | 3.2–6.0 |
| `9fa0079c7` | merge-wave5 into K36 | 308, 316 | 239 p s | 8.2 | 3.0–6.1 |
| `87034b518` | K36 on merged tree | 304 | 299 p s | 8.3 | 5.2–7.2 |
| `403bc788a` | READ_VERIFY, K34, A7, K35, K36 docs | 307 | 228 s, 258 p s | 8.2 | 2.7–6.4 |
| `1fee9bb04` | report step-down | – | 242 p s | 7.9 | 4.3–4.3 |
| `5a7463440` | F1 gates docs | – | 256 p | n/a | 0.5–0.5 |
| `b9dfff609` | Task V read-back evidence | 386 | 287 s, 246 p s | 6.8 | 5.4–6.3 |
| `d951ca293` | merge-wave28 (Task S) | 364 | 244 s | 7.7 | 1.9–2.6 |
| `028b00d31` | merge-wave30, devel in | 375 | 255 s | 7.9 | 2.9–7.3 |
| `e6443fd7a` | merge-wave30: exact stripe-identity hash (orcd-dd-profile) | 518 | 330 s | 7.4 | 4.5–6.5 |
| `833f25fe8` | merge-wave30 (Task AA) | 538 | 290 s | 7.2 | 3.3–4.6 |
| `e34cba345` | merge-wave31 (Task I) | 483 | 317 s | 7.2 | 4.8–6.5 |
| `d891c106e` | merge-wave31 devel merge | – | 224 s | 5.8 | 5.5–5.5 |
| `199ad2afd` | bound sweep metadata RPCs | 521 | – | 8.7 | 5.8–5.8 |
| `4fdf8ddd2` | merge-wave36 (K56) | 502 | – | 8.7 | 3.3–3.3 |
| `c887132c5` | revert of a PUT-proof drain | 518 | – | 8.6 | 5.0–5.0 |
| `68303f40f` | K72, K73 (merge) | 516 | 282 p s | 7.2 | 3.1–5.5 |
| `264cb48d9` | lock BUSY split | 506 | 515 p | 8.6 | 3.6–3.6 |
| `3d5bc3fcb` | K48 (merge) | 508 | 306 p s | 7.4 | 6.0–6.2 |
| `909a07d71` | wave 45 docs | – | 299 p s | 5.6 | 4.8–4.8 |
| `ff6aca40b` | K84 (merge) | 511 | 277 p s | 7.1 | 2.9–5.8 |
| `718372cfc` | wave 54 | 518 | 250 s, 509 p | 8.7 | 1.8–5.4 |
| `79bc63a6a` | wave 60 | – | 521 p | 8.7 | 5.1–5.1 |
| `2b86f47e1` | K101 | – | 584 p s | 6.6 | 5.8–5.8 |
| `devel` | devel 1b355826b (this task's tip, wave 72) | 525, 515, 505 | 534 p, 561 p s, 600 p, 608 p, 589 p | 8.6 | 0.6–8.8 |

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

Medians of the gated, co-led runs where there are any, otherwise of the `p`
co-led runs, stated as such. Single gated runs vary by about ±15 %.

| Step | Before | After | Change | Entries per MiB | Evidence |
|---|---|---|---|---|---|
| K14 (`d2369a49d`) | `f47a32754`: 939–1,265 (gated), 1,038–1,216 (p) | 590–638 (gated), 519–651 (p) | about −45 % | 0.2 to 19.9 | 2 gated + 6 p runs after, 2 gated + 5 p before |
| K13 (`1f3b25997`) | `bd2cb0d5a`: 627–687 (p) | 518–527 (p) | about −20 % | 19.9 to 28.7 | 3 + 3 p runs |
| K29 to K33 (`6b621993d` to `f00cfdd75`) | 518–527 (p) | 491, 498, 467 (p); 426 (gated, K32); 415, 433, 452 (gated, K33) | about −15 % in small steps, none above the spread | 28.7 to 7.8 | one to three runs each |
| K35 (`376369aa8`) | `6069ada62`: 457, 431, 421 (gated) | 311, 300, 306 (gated) | −32 % | 7.9 to 8.2 | 3 + 3 gated runs |
| Recovery (`e6443fd7a`) | `028b00d31`: 375 (gated) | 518 (gated) | +38 % | 7.9 to 7.4 | 1 + 1 gated runs; 483–538 on every later gated build |
| Remaining devel | 483–538 on the 11 gated builds after `e6443fd7a` | 505, 515, 525 (devel, gated) | none | 8.6 | 11 builds + devel |

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
`stripe_find` was taken, and the recovery is one gated run before and after.

## What could and could not be attributed

* **Attributed (measured as commit ranges):** the base-to-devel gap
  reproduces (ratio 1.9 to 2.4 across all pairs, 2.2 to 2.4 with gated runs); K14 is the
  largest step; K13 a second; K35 cost 32 % until `e6443fd7a`.
* **Mechanism known from code and timing, not profiled:** K14's step is a
  cross-group transaction per multi-chunk write (PREPARE, DECIDE, RESOLVE,
  RETIRE; `00c14c355`); K35's step is the stripe-table lookup described above.
* **Not attributed:** the K13 step (tickets) and the K29–K33 range have no
  mechanism beyond the entry counts; the small steps there are inside the
  spread. Why devel (515–525) is higher than the pre-K35 range was not
  traced. The cost split between consensus latency, thread handoffs and
  `fdatasync` was not measured: no `perf` profile and no round-latency
  measurement was taken.
* **CPU shape** (gated runs, write window): base `efsd` 1.8–2.0 cores each and
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
  and an unquiet host; only K35 (3 + 3 gated runs) and K14 are repeated enough
  to rely on. The K13 and K29–K33 figures use `p` runs.
* Only the 1 MiB `ior easy` write was run, at 8 ranks; no other workload.
* The recovery commit is identified by bisection to one merge, with one
  gated run on each side; its mechanism is read from the diff, not profiled.
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
