# orcd-mini: K54 real-host stage and batch O rehearsal (Task BQ)

Purpose: run the K54 real-host stage ([K54](k54-member-down-unmount-20261030.md),
D46 item 6) and rehearse batch O of the
[Stage 1 acceptance matrix](../stage1-acceptance-matrix-20261030.md) on orcd-mini,
with measured minutes against the matrix estimates. This is a **rehearsal on
devel `909a07d71`, not the acceptance verdict**: the acceptance run is owed on
the frozen candidate.

**Verdict: K54 real-host stage PASS (24/24 clean unmounts, TCP and RDMA, exact
read-back). Batch O: O3, O5, O6, O9 PASS; O1 FAIL (`ior hard` EIO, 2 of 8);
O2 FAIL (write about 4.5x slower than the baseline); O7 FAIL on one check;
O8 PASS after a rerun of two timing-sensitive TSan entries; O4 NOTRUN.**
Failures are kept as evidence and the run went on.

## Source and build identity

* Source: devel `909a07d71`, shipped once with `git archive` (13 s) to
  `/data1/efs/releases/909a07d717dc` (node9901) and
  `/data1/efs/orcd-mini-client/releases/909a07d717dc` (node9900); both `src`
  symlinks point there. Built on each node (about 1.5 min).
* Build id `909a07d717dc`, `v0.3.2-pre-alpha-2434-g909a07d71`, branch label
  `devel`, built 2026-10-31T04:58:51Z on node9901. Binary hashes are in
  `build-node9901/binaries.sha256` and `build-node9900/binaries.sha256` (efsd
  `5ed07e7e...` / `289f2f39...`; node builds differ, same source).
* Previous release `00e85a299531` (branch `agent/devel-task-k`, built
  2026-10-27) is kept on both nodes. Baseline for O2/O7: `08cc16af4f77`
  (`08cc16af4`), built from `git archive` on both nodes (`build-base-*`).

## Environment

orcd-mini: node9901 (four efsd n1-n4, 10.1.99.1:17432-17435, stores
`/data1/efs/n1..n4`, quota 720G each, client mount `/data1/efs/mnt`) and
node9900 (client mount `/data1/efs/orcd-mini-mnt`). RHEL, XFS, RDMA `mlx5_0`,
192 CPUs, buffered I/O unless stated, private libfuse. All times UTC,
2026-11-02 unless stated. The devops orcd-mini env names only three members, so
n4 was started and stopped by hand (the `scripts/task_k_mini_fourth.sh`
pattern).

## Inventory

| | Before (2026-10-31 04:56) | After (2026-11-02 ~17:45) |
|---|---|---|
| node9901 | 4 efsd on `00e85a299531`, `/data1/efs/mnt` mounted, cluster empty (1.26 MiB), `fuser -m` clean, 4 idle tiny-files/ereport screens, `/data1` 2.9T free | 4 efsd `v0.3.2-pre-alpha-2434-g909a07d71`, `EFS_TRANSPORT=rdma`, 4 up, 0 down, Protection full, degraded 0, used 0 B; mount up, `fuser -m` clean; no screen of mine; `/data1` 2.9T free |
| node9900 | client mount on `00e85a299531`, `fuser -m` clean, 4 idle tiny-files screens, `/data1` 2.9T free | client mount on `909a07d717dc`, `fuser -m` clean, no screen of mine |

`fuser -m` was re-checked on both mounts immediately before the stop on
2026-11-02 and found only the kernel mount. orcd-mini was stopped, `n1`-`n4`
reformatted and restarted on 909a07d717dc as approved by the user. Scratch
removed: `/tmp/bq-*` on both nodes, the overlay `/tmp/bq-o6-rel`, the stale
failed-start store `/tmp/w125-modify-17740-20261102T155120Z`, `~/bq-stage`.
The unrelated thousands of `/tmp/efs-control-*.stop` files on node9901 are not
mine and were left.

## Results

| Row | Transport | Result | Measured | Estimate |
|---|---|---|---|---|
| Deploy (stop, ship, build, format, start, verify) | RDMA | PASS; build ids match, 4 up, full protection | about 5 min active (script 35 s) | 45 min |
| O1 `ior hard`, 8 reps | RDMA | **FAIL 2 of 8** (see below) | | |
| O1 two-client shared chunk | RDMA | PASS, 4/4 exact on both mounts (58-71 s each) | about 17 min for all O1 | 45 min |
| O2 easy write A/B vs `08cc16af4f77` | RDMA | **FAIL**: devel 616-698 MiB/s vs baseline 2755-3322 MiB/s | about 10 min | 40 min |
| O3 C1 (`orcd_mini_c1.sh 3600`) | RDMA | PASS: A rc=0 `verified_bad=0 mismatch=0 errors=0`, B verdict PASS, cross-check `missing_not_deleted_by_B=0` | 66 min | 30 min |
| O4 `stop.sh` blocked-handler | - | **NOTRUN** | 0 | 30 min |
| O5 K54, 12 runs | TCP | PASS 12/12 | about 7 min both stages | 20 min |
| O5 K54, 12 runs | RDMA | PASS 12/12 (two batches of 6) | (included above) | |
| O6 W148 modify, 10 attempts | RDMA | PASS 10/10, no mismatch | about 80 min wall (3 parallel streams) | 30 min |
| O7 `w125_p3_compat.sh` | RDMA | **FAIL on one check** (case 3 message), cases 1-2 PASS | 2 min | 30 min |
| O8 TSan / ASan+UBSan | - | TSan 19/21, 2 timing failures, PASS alone; ASan 50/50 | 10 + 9 min (+4 rerun) | 60 min |
| O9 Memcheck leak gate | - | PASS 41/41 | 5 min | 20 min |

### K54 detail (O5)

Per run: write and fsync four files on node9900, stop one member on node9901
(rotating n2, n3, n4; SIGKILL on odd runs, SIGTERM on even), `client.sh stop`,
restart the member, wait for 4 up / Protection full / degraded 0, remount,
compare byte for byte. Transport confirmed from the daemon environment
(`EFS_TRANSPORT=tcp`, then `rdma`) and the daemon logs.

* 24/24 unmounts exit 0 with `session revoked at unmount ... rc=0`; no
  `phase=` on any line; 0 files differ on read-back.
* Unmount wall time 0.8-1.3 s. Revoke `ms` up to 363, `fence_ms` up to 102,
  `cleanup_ms` about 3-306. (Base failure was 15.3 s at `phase=storage-fence`.)
* Every member got both signals over the runs, as in the loopback stage.

### O1 failures

`ior -a POSIX -e -w -r -t 47008 -b 47008 -s 2000` shared file, 8 local ranks on
node9900. Batch A: rep 1 `write()` EIO at offset 167348480 (rc=134, 116 s),
reps 2-4 PASS (62-73 s). Batch B: rep 2 `close()` error (rc=255, 93 s), the
others PASS. Client log in both: `report-sync-fail rc=-14`, `put-ticket: lost
exchange ... -> NET`, `report-resolve ... -> EIO`, `flush-meta I/O error`.
This is the K38-like shared-chunk EIO reproduced on devel at 2 of 8; the
two-client shared-chunk case was exact 4/4. Not diagnosed here.

### O2 caveat

Devel ran four members, the baseline three (old CLI, `--direct-io`,
`EFS_MD_RAFT_N=3`), so the 4.5x gap is an upper bound on a like-for-like
comparison. It matches the open K32 write-cost finding.

### O7 detail

Reviewed against the script at `909a07d71`: it expects old clients to be
refused (consistent with the no-backward-compatibility rule; the matrix's K4
note is stale). Case 1 PASS (1000 files). Case 2 PASS (old client did not
mount; daemon logged `refused message 12; this daemon serves only after a
protocol 5 declaration`). Case 3: the new client did not mount, but its message
was `Could not discover cluster from any bootstrap node`, not a protocol
refusal, so the "named the protocol refusal" check FAILS.

### O8/O9 detail

TSan, 21 entries, 587 s wall: 19 PASS and 2 FAIL on test assertions with no
TSan report (`d4_progress` read_errors=1 at 129 s; `pump_logging` foreground
failures 2 at 54 s). Both PASS alone (131 s and 76 s). They are timing-sensitive
under the full 8-job load, not rerun-cleared on the full load. ASan/UBSan 50/50
(`ASAN_OPTIONS=detect_leaks=0`, same host limit as
[the Oct 28 run](devel-linux-orcd-mini-20261028.md)). Memcheck 41/41 used the
retained Valgrind headers and runtime from `devel-linux-28a3`.

### NOTRUN

* **O4**: needs the unapplied k01b `stop.sh` patch in `~/git/devops`, which this
  task may not edit. The devops orcd-mini env also names only three members, so
  `stop.sh`/`start.sh` do not manage n4.

## Procedure faults found

* `scripts/w125_p0_cluster.sh` (used by `w125_modify_repro.sh` and
  `w125_p3_compat.sh` for the new build) still uses the pre-K33 efsd CLI
  (`--node-id --addr --port --storage --direct-io`); on devel the daemons answer
  `Could not discover cluster`. O6 and O7 case 3 ran from an overlay copy
  (`/tmp/bq-o6-rel`: symlinked binaries, copied scripts, patched cluster
  script), deleted afterwards. The repo scripts are NOT fixed here. The failed
  first start is kept in `o6-start-failure-stale-cluster-script`.
* One shell mistake of mine (`pgrep -f` matching my own ssh shell) killed the
  O6 loop and one attempt; the partial attempt is kept as
  `aborted-attempt-2-loop-killed` and the remaining attempts were rerun as three
  parallel streams (ports 17740/17750/17760).

## Reproduction

Prerequisites: the user's open ssh control socket to `orcd-login`, the new
release on both nodes, a formatted four-member cluster with the transport under
test. Scripts (all in `scripts/`, run from a Mac checkout of `909a07d71`):

```sh
# K54: orcd_mini_ssh.sh is the ssh helper (host + command); cluster restarted
# with EFS_TRANSPORT=tcp or rdma before each stage
python3 scripts/orcd_mini_k54_stage.py --transport rdma --out k54-rdma.tsv --runs 6
python3 scripts/orcd_mini_k54_stage.py --transport tcp  --out k54-tcp.tsv  --runs 12
# O1 on node9900: ior hard|easy REPS OUTDIR [TAG]; two-client shared chunk
sh scripts/orcd_mini_batch_o_ior.sh hard 4 OUTDIR
python3 scripts/orcd_mini_shared_chunk.py write FILE --writers 8 --ranks 0-3
# O3
sh scripts/orcd_mini_c1.sh 3600
# O8/O9 on node9900 (copies the release to /tmp)
sh scripts/orcd_mini_o8_o9.sh RELEASE_DIR OUTDIR
```

O6 and O7 use `scripts/w125_modify_repro.sh` and `scripts/w125_p3_compat.sh`
with the overlay described above. Cleanup: `edelete --force --delete` for trees,
`rm -f` for files. The `orcd_mini_ssh.sh` helper named in the K54 driver's
default is not part of this change; pass `--ssh` with any helper that takes
`HOST COMMAND`.

## Raw evidence

Both nodes: `/data1/efs/evidence/bq-orcd-mini-20261031`. node9901 holds build,
deploy, swap, O6, O7 and the C1 side A; node9900 holds the K54/O1 driver
output, O1, O2, O8/O9 and C1 side B. Prior release logs and status are under
`previous-*`.

| Node | Files | `SHA256SUMS` sha256 |
|---|---|---|
| node9901 | 179 | `0ef1bfc23e1d83467ce55748341f1972620a257cd484a29fbcb1105316420b57` |
| node9900 | 1377 | `c70ecd37e4c78f17abed764f29c6fcb054885d3e7dbbb6e4127424738ee91987` |

C1 raw output also stays in `/data1/efs/prof/c1-20261102T1443` (node9901) and
`/data1/efs/orcd-mini-client/prof/c1-20261102T1443` (node9900).

## What batch O will cost on the real candidate

The matrix estimates 5 h 50 min including 45 min of deploy. Measured here:
O1 17, O2 10, O3 66, O5 7, O6 about 80 (3 parallel streams; 24 min x 9 serial
would add about 3.6 h), O7 2, O8 about 23 with the rerun, O9 5, deploy 5 min:
roughly 3 h 35 min wall with parallel O6, nearly 7 h with serial O6. Budget
about 4 h plus triage, and expect the O1 EIO, O2 regression, and O7 check to
need either fixes or recorded decisions first. O3 is longer than the estimate
(66 vs 30 min) and O6 is far longer than 30 min per attempt; fix the matrix
estimates, and fix the stale cluster script before the real run or O6/O7 case 3
cannot start.

## Limitations

* Rehearsal on devel, not the frozen candidate; the gate-level K54 decision is
  unchanged.
* O2 compares four members against three. O6 ran on three parallel private
  clusters on node9901 beside the live cluster. TSan failures passed on a
  solo rerun only. O1 failures were not diagnosed. No power-loss or
  distinct-host claim is made.
* Remaining gates: O4 (needs the devops patch), O1/O2/O7 follow-ups, and the
  whole matrix on the frozen candidate.
