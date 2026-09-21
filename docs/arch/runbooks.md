# Measurement runbooks — one per open bug-chasing / measurement item

Written Sep 21 2026 for an agent who has not worked on efs before. Each
runbook is one script under `tests/measure/` that runs the deploy-rule
pre-flight, does the measurement against the cluster **as it is**, and
writes `results/measure/<UTC stamp>-<name>/SUMMARY.txt` with the numbers to
hand back. None of them changes the cluster version; two of them remount
clients with the **same binary** plus an env var (`EFS_RPC_PROF=1`) and
remount them plain afterwards.

**Pinned version for this round: servers and clients on
`b2184a5c7faf-dirty`** (byte-identical source to the §7.2 commit,
`git log -1 --grep="commutative reductions"`). Every script's pre-flight
prints the running build; pass `--expect-build b2184a5c7faf-dirty` to
`tests/preflight.sh` yourself if you want it to refuse anything else. If
the cluster is on a different build when you start, **stop and say so** —
do not redeploy to "fix" it (that is a user decision; a redeploy is
`tests/roll_efsd.sh --all`, START-HERE §1b).

## 0. Rules that apply to every runbook

1. **Read first:** `.cursor/rules/efs-fcstor-deploy.mdc` (pre-flight, what a
   dead mount looks like), `.cursor/rules/efs-remote-timeouts.mdc` (a
   timeout is a FAIL, never widen it), `.cursor/rules/efs-fio-honest.mdc`
   (write numbers need the flush in the clock; `NOT_FUSE` = local disk =
   fiction).
2. **Nothing over ~60 s runs from the login node.** Every runbook below is
   launched as
   `~/.cursor/skills/efs-test-ssh/scripts/efs-bg.sh start <name> '<cmd>'`
   which runs it in a detached `screen efs-<name>` on node9901 with the
   log on NFS (`~/efs-runs/<name>.log`). Poll with
   `efs-bg.sh status <name>` / `efs-bg.sh wait <name> <secs>`; or
   `ssh node9901; screen -r efs-<name>` to watch it live (`Ctrl-a d` to
   leave). Never run two runbooks at once: each one's pre-flight requires
   an idle cluster (commit rate ≤ 5 entries/s) and the other would fail it
   — and pollute the numbers.
3. **Start every session with the pre-flight alone** (12–30 s):
   `bash tests/preflight.sh --expect-build b2184a5c7faf-dirty`.
   `PREFLIGHT_OK` or stop. The scripts run it again internally and abort on
   FAIL; the output is saved as `preflight.txt` in the result dir.
4. Auto-review will block cluster-touching commands. Retry the **same**
   command with `request_smart_mode_approval=true` and the verbatim reason.
   Do not split or weaken the command to dodge it.
5. **Never `pkill -f` / `pgrep -f`** (matches your own ssh). `-x` only.
   Never `wipe_cluster.sh`, `raft-mkfs`, `pkill -x efsd`, `roll_efsd.sh`
   for a measurement. Never start a daemon under strace/perf; attach to
   the running one (the scripts do).
6. **Deliverable = `SUMMARY.txt` + the raw files, committed under
   `results/measure/`, plus 3–6 lines in START-HERE §1b progress log** saying
   what the number is and what it rules in or out. Do **not** change the
   protocol, the backoff, the budgets, or build a feature from the finding:
   each runbook says what the decision is and that it is the user's.
7. If a script prints `FAIL`/`WARNING`, read the named raw file before
   re-running. Re-running blindly on a broken mount produces fiction.

## 1. W6 residual 3 — same-directory op rate (`samedir_rate.sh`)

**Question.** 36 procs doing mkdir/create/rmdir/unlink in ONE parent get
~200 ops/s aggregate (178 ms per op per proc; idle mkdir is 7 ms). Is the
time spent in the client's BUSY backoff (a same-parent CREATE and RMDIR
are mutually BUSY on the dseq emptiness witness by design, 50 ms × 2ⁿ), or
on the server (apply pump / Raft commit queue, 3–4 commits per op)?

```
efs-bg.sh start m-samedir 'PERF=1 bash tests/measure/samedir_rate.sh'      # ~6 min
```
Env: `LEVELS="1x1 1x9 4x9"` (procs-per-host × hosts), `ROUNDS=100`.

**What it does.** Per level: remounts the clients with `EFS_RPC_PROF=1`
(client prints cumulative counters to `fuse.log` every 2 s), runs
`tests/stress/same_parent_storm.sh`, sums the counters. With `PERF=1` it
profiles both group leaders' `efsd` for 20 s during the last level. Then
dumps the raft-log command histogram of both leaders and remounts the
clients plain.

**Read.** `table.tsv`: `ms_per_op_proc` vs level is the curve.
`busy_s / (procs × wall_s)` = share of each proc's time in BUSY backoff;
`recv_s` likewise for server+wire wait. `create_avg_us` / `unlink_avg_us`
growing with contention = server side. In `perf-samedir-g*`, on-CPU time
in `efs_kv_*`/`fold_reduce`/`efs_txn_*` = apply cost; `fsync`/`fdatasync`
= WAL; mostly idle = the wall is queueing/wakeups, not CPU.

**Pass/fail.** The storm must PASS at every level (0 errors, parent
`children=0 nlink=2`). A FAIL is a correctness regression — report that
first and stop.

**Hand back.** The table, the two shares, one sentence per candidate
(backoff / server apply / commit queue) saying whether the data supports
it. Do **not** change `inode_rpc.c` backoff or the txn protocol.

## 2. W6 residual 2 — 1 GiB open costs ~20 s (`open_cost.sh`)

**Question.** Opening a 1 GiB file took 20 s of a 22 s IOR easy-read
phase: 128 sequential GETCHUNKS (≤ 64 chunk records each) plus a 64-lane
stat per open. Is it linear in size, what RPC count/latency makes it up,
and is the warm open cheap?

```
efs-bg.sh start m-open 'bash tests/measure/open_cost.sh'                    # ~3 min
```
Env: `SIZES="128 1024 4096"` (MiB), `WRITER=fcstor007`, `READER=fcstor008`,
`SKIP_WRITE=1` to reuse files from a previous run (`$MNT/measure/open-*m.bin`).

**What it does.** Writes each file with `dd conv=fsync` from a non-zero
source (all-zero chunks are not written), cold-remounts the reader with
`EFS_RPC_PROF=1`, times open / first 4 KiB read / close twice, reads the
client's `getchunks=n/us`, `lookup`, `getattr` counters.

**Read.** `cold_open_ms` vs `size_mib`: linear with slope ≈ (chunks/64) ×
`getchunks_avg_us` = the map is fetched whole at open, sequentially. If
`cold_open_ms` is small and `cold_read4k_ms` is large, the cost moved to
first read. `warm_open_ms` should be ≪ cold. Then the **parallel** line
(`PAR=8x4` by default: 32 procs on 8 hosts cold-open the largest file at
once): min/med/max open ms and the per-GETCHUNKS latency under that
concurrency vs the single-opener one.

**Already known from the Sep 21 smoke run on this build
(`results/measure/20260921-1341*-open-cost`):** single opener, idle
cluster — 128 MiB 101 ms / 1 GiB 228 ms / 4 GiB 359 ms cold; 1 GiB =
exactly 128 GETCHUNKS at 1.6 ms each (205 of the 228 ms); warm open 30 ms.
So the idle cost is **0.23 s, not 20 s**. The parallel phase (32 procs on
8 hosts, same 1 GiB file) gave open min/med/max **1.6 / 2.2 / 2.5 s** and
GETCHUNKS **14.6 ms** avg (9.5× the single opener), with `getchunks_n` =
1024 = 8 × 128 — the client fetches a map once per host, procs share it.
So GETCHUNKS is serialized/queued on the server, and IOR easy-read (36
ranks, 36 *different* files → 4608 GETCHUNKS at ≥ 15 ms, sequential per
rank) lands at ≈ 20 s. Next measurement, if the user wants it: `PAR` with
one file per opener (add a `PAR_OWN_FILES=1` mode) and a 20 s `perf record`
of the group leader during the parallel phase to see whether it is the KV
scan, the apply pump or the socket path.

**Hand back.** The table, the slope, and the parallel line. The spec
answer is §8 per-lane range fetch (`docs/arch/performance.md`), not
implemented — the user decides whether to build it. Do not implement it
as part of this runbook.

## 3. W6 residual 1 — IOR-hard write 45 MiB/s (`ior_hard_scaling.sh`)

**Question.** IOR hard = all ranks write 47008 B records interleaved into
one file, so ~3 ranks share every 128 KiB chunk and each fsync replays the
N-1 CAS losers. Is the 45 MiB/s the CAS replay (should get worse with
ranks and show BUSY/STALE on the client), or just the per-client write
wall W4 sees (flat with ranks)?

```
efs-bg.sh start m-iorhard 'bash tests/measure/ior_hard_scaling.sh'          # ~8 min
```
Env: `NPS="1 4 9 36"`, `SEGS=3000` (per-rank records; 36 × 3000 × 47008 B
= 4.7 GiB), `RANK0=fcstor007`. Prereq: `tests/perf/io500/run.sh prepare`
was run once (`~/orcd/scratch/efs-io500/io500/bin/ior` exists — it does).

**What it does.** Per NP: remounts rank0's client with `EFS_RPC_PROF=1`,
deletes the hard file, launches the raw IOR hard geometry via
`run.sh ior-hard-write` (detached `prterun` on rank0), polls the NFS
`last-run.log` for IOR's `write` row / `Finished`, records MiB/s, the
client's `busy_n`, and the servers' `report-split` lines logged during
the run (`nrec`, `pack_ms`, `push_ms`, `finish_ms` per REPORT ≥ 256 recs
or ≥ 100 ms — **the client's RPC-PROF does not count REPORT on this
build**, `rpc_send_recv_dual` is unprofiled; the server line is the
REPORT number).

**Read.** `write_mibs` vs `np`. Falling past 9 with `busy_n` growing or
`report-split ... rc=-` STALE codes multiplying = CAS replay dominates.
Flat ≈ the W4 single-client number = the write wall; then `finish_ms`
(wait for apply) vs `push_ms` (propose) in the report-split summary says
whether it is Raft commit or the PUBLISH apply.
If a run does not finish in 25 min the script says so and moves on — look
for D-state ranks (`pgrep -x ior` on the clients; `pkill -9 -x ior`, then
remount that client). Do not raise the deadline.

**Hand back.** The curve and which of the two shapes it is. The designed
escape is §9 immutable delta objects for sub-chunk writes — a user
decision. Do not tune IOR's transfer size (47008 is the point) and do not
add a chunk lock (W1 forbids).

## 4. W4 — multi-client write wall (`dd_wall.sh`)

**Question.** 8 GiB dd+fsync per client, own file each: 639 → 251 → 202
MiB/s aggregate at 1 / 4 / 9 clients on Sep 18 (3.8 / 0.46 / 0.37 % of
the 16.7 / 44 / 44 GB/s ceilings). Writes share a ceiling and more clients
make it *worse*. Does the current build still do that, and where is the
server waiting?

```
efs-bg.sh start m-ddwall 'PERF=1 bash tests/measure/dd_wall.sh'             # ~8 min
```
Env: `NCLIENTS="1 4 9"`.

**What it does.** Per client count: cold-remounts the clients, ensures the
8 GiB non-zero source `/tmp/src8g`, starts all dd's at once with the flush
in the clock (`conv=fsync`), checks every client printed `FUSE_OK`. With
`PERF=1`, 20 s `perf record -g` of the group-0 leader's `efsd` and of
fcstor007's `efs-fuse` in the middle of the ≥ 4-client run.

**Read.** `agg_mibs` per row (clients × 8 GiB over the slowest client's
dd wall); `per_client_min_max_s` spread = fairness; `sum_rates` ≈ `agg`
unless one client finished long before the others. A row marked `INVALID`
had a `NOT_FUSE` client — the number is discarded, do not quote it. The
`report-split` summary per phase is the server's view of the fsync tail:
`pack_ms` (build PUBLISH batches) / `push_ms` (propose) / `finish_ms`
(wait for apply) — which of the three grows with client count is the
finding. Sep 21 smoke on this build: 1 client **418 MiB/s** (Sep 18: 639). In the perf reports expect the client on-CPU in blake3
(`hash_write_fragments`) and the server near idle — the wall is off-CPU
(fsync/REPORT tail); a server report dominated by one function is news.

**Hand back.** The 3-row table next to the Sep 18 numbers and the two perf
top-25s. Do not compare with the pre-Raft engine's numbers (different
system). Splitting REPORT into several RPCs is a user decision.

## 5. W7 — slow POSIX tests in isolation (`posix_isolated.sh`)

**Question.** Suite-1 tests `concurrent_creates_same_dir`,
`mtime_monotonic_many_writes`, `dir_deep_nesting*`, `names_crazy_dirs`,
`dir_many_files` exceeded the 15 s budget when per-op metadata latency
was 100+ ms. Since the wakeup fix (mkdir med 7 ms) they pass; this runbook
re-times each alone so a regression shows as ms/op, not as a suite count.

```
efs-bg.sh start m-posix7 'bash tests/measure/posix_isolated.sh'             # ~3 min
```
Env: `TESTS="..."`, `HOST=fcstor007`.

**Read.** `secs` per test (includes ~1 s python start) and `ms_per_op`.
Anything near or over 15 s on an idle cluster = the cluster was not idle
(pre-flight commit-rate line) or a wakeup regressed — strace the group
FOLLOWER for acks-every-other-heartbeat (project-state rule) before
blaming fsync.

**Hand back.** The table. Never raise `POSIX_TEST_SEC` or a `@budget`.

## 6. W8 — 9-way POSIX harness wall (`posix9_none_count.sh`)

**Question.** Nine hosts running suite 1 at once hit the 385 s python cap;
tests that never ran show as `[None]` and get read as failures. This is a
harness bug, not a filesystem one, and its fix is in
`tests/posix/posix_suite.py` (write the TSV incrementally; explicit NOTRUN)
— it does not touch the cluster version.

```
efs-bg.sh start m-posix9 'EFS_TRANSPORT=tcp bash tests/run_tests.sh posix --parallel fcstor007.ib fcstor008.ib fcstor009.ib fcstor010.ib fcstor011.ib fcstor012.ib fcstor013.ib fcstor014.ib fcstor015.ib'   # ~7 min
bash tests/measure/posix9_none_count.sh results/posix/<run-id>             # instant, read-only
```

**Read.** Per host: rows / pass / fail / skip / none. `none > 0` = the run
was cut. Only `fail` rows with a real detail are bugs; compare them with
the 1-client signature (200/201 + `mmap_write_read` SKIP).

**Hand back.** The count table. If you fix the harness, the gate is: every
host's TSV has 201 rows and 0 `[None]` — and the fix must not change any
test's body or budget.

## 7. W11 — Raft log never compacts (`raft_snap_state.sh`)

**Question.** A group's KV export exceeds the one-command SNAP cap, so
`snapshot skipped` latches, `raft.log` grows without bound (3.9 GB on
fcstor005 on Sep 21) and a restarted follower replays all of it. Chunked
InstallSnapshot is not specified anywhere — measure, do not design.

```
efs-bg.sh start m-snap 'bash tests/measure/raft_snap_state.sh'              # ~1 min, read-only
```

**Read.** `raft_log_bytes` per server vs the 4 MiB cap, `log_growth_B_per_s`
(idle), `kv_bytes` (≈ what a snapshot would have to carry), the latched
`snapshot skipped` lines and each voter's lag.

**Hand back.** The table and "replaying N GB took M s in the last
`roll_efsd.sh`" from `~/efs-runs/roll*.log`. The decision (chunked SNAP
protocol shape) is the user's; killing 005 to "fix" the lag breaks 2+1
writes.

## 8. W10 — RDMA empty-table first mkdir — **ask first, do not run**

Needs a wipe + `raft-mkfs` (only reproduces on an empty table). The fix is
in tree and unit-gated (`test_conn_fd`); the live gate is
`tests/rdma_first_inode.sh` on a fresh table. Bring this to the user as a
question; do not wipe.

## 9. After a runbook

- `git add results/measure/<dir>` and commit it with the START-HERE §1b
  progress-log lines (newest first) and, if a fact changed, the one line in
  `.cursor/rules/efs-project-state.mdc`.
- Clients you remounted with `EFS_RPC_PROF=1` are remounted plain by the
  scripts; confirm with `tests/preflight.sh` (the `fuse.log` of a
  RPC_PROF mount grows a line every 2 s — that is how you can tell).
- Delete `$MNT/measure/*` files you created if the next runbook does not
  reuse them (`open_cost.sh` reuses with `SKIP_WRITE=1`).
