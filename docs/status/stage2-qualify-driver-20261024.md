# Stage 2 qualification driver (`scripts/qualify.py`), Oct 24

**Verdict: driver PASS (scoped); loopback dry run FAIL with real findings
(stale harness CLI; EIO during a member outage); code gate on intel1 PASS
with host limits except `tests/test_k33_payload.py`, which fails as often on
the base (proposed K34).** Branch
`claude/devel-qual-driver-f46b04`, not merged. Real-host qualification runs
belong to the parent session: this page qualifies the driver, not a topology.

## Purpose

One driver for the alpha 1.0 Stage 2 gates of the
[checklist](alpha-1.0-2plus1-checklist.md) (A1–A5, C1–C5, D2/D3, E1–E3) and
the F3 topology ledger, replacing the hand-run drivers. Given a cluster
description, an output directory outside Git and the gates (default all), it
records for every gate the exact commands, hosts, start and end times,
deadlines, exit status and raw output, the verdict (PASS, FAIL or NOTRUN with
the reason), the build identity of every daemon and client read live before
the gate, and the fixture counts and sizes. It writes `SHA256SUMS` over the
raw outputs, a draft evidence summary in the AGENTS.md shape and this
topology's F3 ledger column.

## Interface

```bash
python3 scripts/qualify.py list
python3 scripts/qualify.py run --cluster CLUSTER.json --out DIR [--gates a,b] [--skip a,b] \
    [--allow restart,turnover,remount|all] [--deadline GATE=SEC] [--plugin NAME=COMMAND] [--plan]
python3 scripts/qualify.py run --loopback 3 --port-base 18740 --out DIR --work DIR --allow all
python3 scripts/qualify.py verify-sums DIR
```

The description format (seed, nodes with hosts, mounts, transport, direct I/O,
features, quota, private harness host/root/ports, fixture, control commands,
plugins) is in the script's docstring; commands run under bash on the named
host, through ssh when it is not the driver host. Exit status 0 all PASS, 1 any
FAIL, 3 NOTRUN without FAIL, 2 usage.

| Gate | Items | Runs | Destructive |
| --- | --- | --- | --- |
| identity | A1, A2 | `efs-mgmt version` per node, the live efs-fuse per mount (`/proc/PID/exe`, sha256, `--version`), harness tree binaries; also before every gate and at the end | — |
| b-checks | B1–B6 | plugin: `scripts/b_checks.py --cluster --out` when present, else NOTRUN | plugin's `needs` |
| posix-core, posix-extras, posix-2client | C1–C3, D3 | `tests/posix/` suites on the first (two) mounts | — |
| posix-durability | C2, E2 | `posix_persist.py` prepare, clean client stop and remount, verify | remount |
| append-failover | C3, C4 | `tests/live/append_failover.py` (K15/K29 `account()` rule), private cluster | — |
| k14-torn-read, k13-tickets, fragment-integrity, gc-reclamation | C4, B2, D2 | the live harnesses, private clusters on `private` ports | — |
| e1-manifest | E1 | 240 fsynced files (0 B–4 MiB+1, modes, mtimes, sha256) verified, then client stops, orderly cluster stop/start, readiness, remount, full verify, identity re-read | restart (else NOTRUN after the first verify) |
| e2-client-stop | E2 | `fuser -m` idle check, a holder process: `fuser` sees it and `client.sh stop` (no force) must be refused with the mount still serving; then an unsynced 3 MiB write, clean stop, not a mount point, remount, exact bytes, E1 manifest | remount |
| e3-turnover | E3 | client workload (create, write, fsync, read back) while every group leader is stepped down to a named successor; the leader must move to a node all members agree on; then an orderly member stop/start and catch-up (member applied ≥ leaders' commit); every acknowledged file verified; any client error FAILs, split by window | turnover; restart for catch-up (else NOTRUN) |

Safety: the driver never formats, wipes or stops a cluster it did not start;
restart, member restart and client stop need `--allow` and use only the
description's `control` commands (none default except `client.sh stop`
without force and `client.sh SEED MOUNT`); every performed step is listed in
`run.json` and the summary. Ports 17432–17435, 27432, 27433 and stores under
`/data1/efs` are refused; `--out`/`--work` inside a Git tree are refused.
Plugins exit 0/77/other for PASS/NOTRUN/FAIL and may write
`plugin-result.json`, which can lower but never raise the verdict.

## Code changes

- `scripts/qualify.py` (new): driver, loopback controller, mount-host helpers
  (`_manifest`, `_pending`, `_workload`, `_client-identity`).
- `tests/test_qualify_driver.py` (new, in `make test`, 0.4 s): stub plugin gates
  for PASS, FAIL, NOTRUN (exit 77, missing script, destructive step not
  allowed), a deadline overrun, a plugin claiming PASS with exit 1 and one
  lowering its verdict; every `result.json`/command field; exit 1/0/3;
  `SHA256SUMS` with a tampered file and an unlisted file as negative controls;
  summary headings, verdict line and F3 row; refusals (output inside Git,
  non-empty output, unknown gate, reserved ports); `--plan` command lines for
  the real gates; identity normalization and leader agreement.

## Dry run (loopback, driver-owned)

- Source: `1a0974dda` (this branch), tree copied with `git archive` to intel1
  `~/efs-qual/src`, built with `EFS_GIT_ID=1a0974dda EFS_GIT_DESCRIBE=g1a0974dda`
  so binaries carry the identity; every daemon and both clients reported
  `g1a0974dda (branch claude/devel-qual-driver-f46b04)`, client exe sha256
  `ecc5caa28ce1…` for both mounts.
- Environment: intel1 (8 CPUs, 15 GB, Rocky 10, `/home` XFS), three efsd on
  127.0.0.1:18740–18742 (K33 seed + `--join`), direct I/O, 1 G quota each,
  TCP, `EFS_COMMANDS=1 EFS_COMMANDS_SERVER=1`, two `client.sh` mounts; live
  harnesses on ports 18700–18733. One host: regression evidence, not A4.
- Command (in screen `qual-final`, about 22 minutes):

```bash
cd ~/efs-qual/src && TMPDIR=$HOME/efs-qual/tmp python3 scripts/qualify.py run --loopback 3 \
  --port-base 18740 --private-port-base 18700 \
  --out ~/efs-artifacts/qual-driver-20261024/dryrun-1a0974dda --work ~/efs-qual/work-final --allow all
```

| Gate | Verdict | Seconds | Result |
| --- | --- | --- | --- |
| identity | PASS | 0.3 | 3 daemons, 2 clients, 3 tree programs: one identity |
| b-checks | NOTRUN | 0.3 | `scripts/b_checks.py` not present (another session writes it) |
| posix-core | PASS | 50 | 216/216 cases, 4 jobs, 30 s per case |
| posix-extras | PASS | 10 | 4 pass, 1 SKIP (reported as scope in the reason) |
| posix-2client | PASS | 68 | 68/68 |
| posix-durability | PASS | 54 | 27/27 after a clean stop and remount |
| append-failover | PASS | 992 | 8000 records, 4 writers, step-down every 4 s, direct |
| k14-torn-read | FAIL | 0.6 | harness starts efsd with `--node-id` (removed by K33) |
| k13-tickets | FAIL | 0.6 | same (`k13_ticket_cluster.py`) |
| fragment-integrity | FAIL | 0.4 | same (`ticket_put.py`) |
| gc-reclamation | FAIL | 0.5 | same (`gc_reclamation.py`) |
| e1-manifest | PASS | 59 | 240/240 acknowledged, 140,120,143 B; stop 1.2 s, ready 1.1 s (deadlines 300 s); 240 verified after restart |
| e2-client-stop | PASS | 12 | busy stop refused (rc 1, still serving); clean stop 0.8 s; pending bytes and E1 manifest exact after remount |
| e3-turnover | FAIL | 65 | both groups moved to the named successor (0.04 s, terms 2→3), all members agreed, 0 client errors in that window; member 127.0.0.1:18740 (seed and new group-2 leader) stopped orderly: 249 of 993 ops failed with EIO (the longest gap without an acknowledged op 4.1 s), and it caught up 2.2 s after its restart; all 744 acknowledged files exact |

The four harness FAILs are their own stale CLI (each log shows `--node-id was
removed (K33)`); `append_failover.py` was already moved to the K33 CLI. The E3
EIO is real mounted-I/O failure while one of three members is down after an
orderly stop; `tests/test_k33_payload.py` saw the same family (below). Both are
for the parent to triage; neither is changed here.

Driver corrections found by the dry runs (all in this branch): `efs-mgmt
version` prints `version ADDR efsd …` (the address leaked into the identity
key); an untargeted `raft-step-down` was re-won by the same node at a higher
term, which E3 first accepted as turnover (now a named successor and an agreed
leader on another node are required); the E3 workload directory was clobbered
by the successor variable; failed workload writes counted as extra files; the
loopback claimed `.efs` commands on while extras skipped 4 of 5 cases; a PASS
line containing the word NOTRUN made the test runner count it as NOTRUN.

## Code gate

- Base `a2326a369`, intel1 `scripts/make_test_summary.sh` (test-all): 449
  commands, 0 FAIL, 14 NOTRUN host limits (RDMA device, valgrind,
  `~/git/cluster`, `EFS_FAULTS`), 1330 s.
- Branch `a454230fa`: 450 commands, 0 FAIL, 15 NOTRUN (the extra one was this
  test's own PASS line, fixed in `a15209b11`).
- Branch `1a0974dda`: 450 commands, 1 FAIL, 14 NOTRUN. The FAIL is
  `tests/test_k33_payload.py`, a read EIO in its "node k down" phase; this
  branch changes no daemon or client code. Reruns of that test alone on
  intel1: base `a2326a369` 3 of 5 FAIL, branch `1a0974dda` 2 of 4 FAIL (a
  fifth branch run was cut off when its screen closed and is not counted),
  every failure the same read EIO while one member is down. It fails on the
  base just as often, so it is not caused by this branch: **finding (proposed K34)**, a
  pre-existing flaky EIO in the single-member-outage phase; the s-docs session
  saw 4 of 6 runs fail on nuc. With the E3 EIO above it suggests that reads
  and writes through a mount fail, rather than reconstruct or wait, while one
  of three members is stopped.

## Raw evidence

intel1 `~/efs-artifacts/qual-driver-20261024/` with `SHA256SUMS`
(666 files, `SHA256SUMS` sha256
`6bf894b520e661c28ce52a22459bf3502a9144fb016d00ebc6de88c6319ca9d8`): `dryrun-1a0974dda/` (219 raw files, its own `SHA256SUMS`
`1176b039…9c8c`, `summary-draft.md`, `f3-row.md`), the earlier dry runs
`dryrun-a15209b11/`, `smoke1/`, `smoke2/`, the gate summaries
`base-a2326a369-test-all/`, `branch-a454230fa-test-all/`,
`branch-1a0974dda-test-all/` (with `fail-407.log`) and `k33-payload-reruns/`.
Private stores, mounts and scratch were deleted by the driver; the build trees
`~/efs-qual`, `~/efs-qual-base` were deleted after the run.

## Reproduction and cleanup

Copy the branch (`git archive HEAD | ssh intel1 'tar -x -C ~/efs-qual/src'`),
build with the identity variables above, run the command above with an empty
`--out` outside Git; `make test` runs the bookkeeping test. The driver stops
its loopback cluster and deletes `--work` unless `--keep-work`. For a real
cluster write a description with `control` commands for the user's start and
stop tooling and pass `--allow` only for the steps wanted.

## Limitations and remaining gates

- Real-host 3-, 4- and 5-host runs (A3, A4, F3 cells): NOTRUN, parent.
- The live harnesses always run single-host private TCP clusters; RDMA live
  gates are not driven.
- K14 runs phases 1 2 3 7 unless the description names `faults_source` (an
  `EFS_FAULTS=1` build); append failover's lost-reply mode likewise needs one.
- Not covered: E4/D4 scale and progress, D1 packing inventory, `.efs` under
  load beyond posix-extras.
- Follow-ups: move `k13_ticket_cluster.py`, `ticket_put.py`,
  `k14_atomic_cluster.sh` and `gc_reclamation.py` (and the other `--node-id`
  users under `tests/live/` and `scripts/`) to the K33 CLI; triage E3 EIO
  during an orderly member outage together with the `test_k33_payload.py`
  EIO (proposed K34).
