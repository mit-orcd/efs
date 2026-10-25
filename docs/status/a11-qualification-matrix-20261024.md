# A11 reduced qualification matrix and the Stage 2 run plan, Oct 24

**Verdict: plan PASS (scoped): the matrix, the Stage 2 item plan and the
runnable topologies are published; every gate the plan names exists and
`qualify.py plan` checks that it does. Qualification itself NOTRUN: no
distinct-host topology can run today (A4 blocked by host networking), and 7 of
the planned gates are NOTRUN stubs.** Branch `claude/devel-a11-matrix-2bfd14`
on `merge-wave5` (`376369aa8`); not merged, not pushed.

## Purpose

Checklist [A11](alpha-1.0-2plus1-checklist.md#a6a12-one-supported-behavior-audit-and-remove-ab-modes):
publish the supported configuration dimensions that remain after A6/A7 and
D42 ([inventory](a6-settings-inventory-20261024.md),
[A7 removals](a7-removals-20261024.md)), count the reachable configurations
before and after, say what is qualified exhaustively and what is sampled, and
turn Stage 2 (A1–A5, A13, B1–B6, C1–C5, D1–D4, E1–E4) into runs the parent
can start with one command per topology.

## The matrix

### Supported dimensions and why

| Dimension | Supported values | Qualified | Why it stays |
| --- | --- | --- | --- |
| Transport (`EFS_TRANSPORT`) | `tcp`, `rdma`; `auto` picks `rdma` when a device is configured, else `tcp` | `tcp` and `rdma`, each at every qualified size, with identical guarantees | Intentional platform support (A8), not an A/B implementation choice. `auto` is a resolution rule: a run qualifies the transport it resolved to, which `identity`/`topology` record |
| Members (one efsd per host) | 3 to 16 (`EFS_MAX_NODES` 16, K28) | 3, 4 and 5 on distinct hosts | Alpha is 2+1 on 3, 4 and more hosts. Placement differs by size: 3 uses all three, 4 rotates the omitted member, 5+ gives every member every role (B3) |
| Fragment I/O | `O_DIRECT` only (D42); a root without it is refused | the only value | A7 removed buffered fragment I/O |
| Disks per node | one root at start; more with `efs-mgmt add-storage` | 1 | Deployment input. `add-storage` has no test (S3, F11): a multi-root node is outside the qualified matrix until a gate exists |
| Clients | any number of Linux FUSE mounts | 1 and 2 mounts, on one or two client hosts; concurrency inside a mount from the suites (4 jobs, 4 append writers, two-client cases) | Deployment shape, not a behavior selector |
| Features | packing built in; `.efs` follows the cluster commands-policy (default enabled); expiry compiled out; containers off | the one profile | A7/D42 removed the feature switches |
| Numeric tunables | 33 still read from the environment | built-in values only | A8 is open for them. A deployment that sets one runs outside the qualified matrix |

Observability (34), deployment inputs other than transport (8: RDMA device
and GID, NUMA, query-cache capacity and similar) and `EFS_FAULTS`-only hooks
(39) select no behavior and are not dimensions.

### Reachable configurations before and after

Counted with `scripts/env_inventory.py` (A6 method) on the A6 base
`a2326a369` and on this base `376369aa8`.

| Count | Before A6/A7 (`a2326a369`) | Now (`376369aa8`) |
| --- | ---: | ---: |
| Environment variables read by the product | 139 | 116 |
| Binary A/B behavior selectors (environment + efsd CLI) | 15 (13 + 2) | 0 |
| Behavior combinations from those selectors | 2^15 = 32,768 | 1 |
| Transport behaviors | 3 values (`auto`, `tcp`, `rdma`) | 2 (`auto` resolves to one of them) |
| Member counts | 14 (3–16) | 14 (3–16) |
| Selector × transport × member count | 32,768 × 3 × 14 = 1,376,256 | 1 × 2 × 14 = 28 |
| Numeric tunables on top | 44 | 33, fixed at built-in values in the qualified matrix |
| Names refused at startup | 0 | 33 |

Before A6/A7 even two values per numeric tunable would multiply the count by
2^44; that space was never countable, let alone testable. Now the behavior
space is 28 cells (transport × member count) at one storage mode, one feature
profile and built-in tunables.

### Exhaustive and sampled

- **Exhaustive within the qualified values:** the A/B dimension (one
  configuration, nothing to combine) and transport × size for sizes 3, 4 and 5:
  six cells, each running the whole gate list on one frozen candidate. Each
  cell is a full run, not a single-flag or pairwise test.
- **Sampled:** sizes 6–16 (22 of the 28 cells). Loopback B1–B5 ran at 6–8
  ([B evidence](b-geometry-capacity-20261024.md)); 9–16 have not run. One
  5-member PASS does not qualify 6–16 (A3); the release must state the
  qualified range or add cells.
- **Not qualified:** non-default numeric tunables, multi-root nodes, more
  than two mounts as a separate dimension, and RDMA for the live harnesses
  (they start private TCP loopback clusters).
- No claim of exhaustive coverage follows from the single-flag tests of the
  removed selectors or from pairwise runs; workload, timing and failure
  interleavings are sampled by the gates' fixtures.

## The Stage 2 plan

Each item below maps to the checks that decide it. `qualify:` gates are
`scripts/qualify.py` gates (`qualify.py list`); `code:` is `make test-all` on
the candidate tree; the plan's cells are 3-host, 4-host and 5-host (distinct
hosts) × TCP/RDMA, and "once" means a topology-independent run on the
candidate (`loopback-3` and the code gate). The machine-readable copy with
the same content is [`scripts/qualify_matrix.json`](../../scripts/qualify_matrix.json);
`python3 scripts/qualify.py plan` prints it and fails on a gate, file or
item that does not exist.

| Item | Checks | Cells | Duration | Prerequisites |
| --- | --- | --- | --- | --- |
| A1 | `identity` | 3/4/5 × TCP, RDMA | under 1 min | frozen candidate; trees built with `EFS_GIT_ID`, `EFS_GIT_DESCRIBE`, `EFS_GIT_BRANCH`; one build on every host |
| A2 | `identity`, `topology` | same | under 1 min | A1 |
| A3 | `topology` | same | under 1 min | K28 (merged); clusters of 3, 4, 5 |
| A4 | `topology` | same | under 1 min | one efsd per distinct host: **blocked today** (below) |
| A5 | `topology` (profile, members, storage roots), every gate's fixture | same | recorded by every run | fresh format when the candidate changed a store, log or fragment format since the cluster was formatted |
| A13 | `a13-node-addition` **(stub)**, code | 3→4→5 × TCP, RDMA | about 30 min (estimate) | driver-owned cluster (membership changes), spare hosts, fresh format |
| B1–B4 | `b-checks` (`scripts/b_checks.py`) | 3/4/5 × TCP, RDMA | about 2–10 min (pack wait up to 300 s) | quiet cluster; mounts on one host; `--store` only where stores are local |
| B2 | also `fragment-integrity` (`tests/live/fragment_integrity.py`) | once (TCP) | up to 1 h | none |
| B5 | `b-checks`, code (`tests/test_b_geometry.py`: full member, growth) | 3/4/5 × TCP, RDMA | within `b-checks` | quiet cluster (cost figures count every writer) |
| B6 | `b6-member-down` (real gate since the B6/D4 merge; was a stub), code (`test_k33_payload.py`, `test_k34_stop_put.py`, `test_k35_fsync_degraded.py`) | 3/4/5 × TCP, RDMA | about 20 min (estimate) | K34/K35 merged, K36 open; per-member stop/start (driver-owned clusters) |
| C1 | `posix-core`, `posix-extras`, `posix-2client` | 3/4/5 × TCP, RDMA | 3–4 min on loopback; about 30 min on real hosts (two-client suite about 13 min on orcd-mini) | two mounts with the same path on two client hosts, or two mounts on one |
| C2 | `posix-core`, `posix-durability` | same | plus 1 min | `--allow remount` |
| C3 | `posix-2client`, `posix-core`, `append-failover` | same; append-failover once | append-failover about 17 min | — |
| C4 | `append-failover`, `k14-torn-read`, `k13-tickets` | once (TCP) | up to 3 h (deadlines) | an `EFS_FAULTS` build for K14 phases beyond 1 2 3 7 |
| C5 | code; `c5-protocol-live` **(stub)** | once; stub per cell | `make test-all` about 30 min | candidate tree on the code-gate host |
| D1 | `d1-packing` **(stub)**; `b-checks` B4 shows packed segments | 3/4/5 × TCP, RDMA | about 20 min (estimate) | `--allow restart` |
| D2 | `gc-reclamation` (`tests/live/gc_reclamation.py`) | once (TCP) | up to 2 h | a clean `client.sh stop` after a long unmount revoke (finding 1) |
| D3 | `posix-extras`; `d3-efs-load` **(stub)** for the load part (`tests/live/efs_commands_load.py` exists, not wired) | 3/4/5 × TCP, RDMA | extras under 1 min; load about 30 min (estimate) | `.efs` catalog indexed (per-case timeout 150 s on a fresh directory) |
| D4 | `d4-progress` (real gate since the B6/D4 merge; was a stub) | same | about 1 h (estimate) | E4 fixture running |
| E1 | `e1-manifest` | same | about 1 min plus an orderly restart | `--allow restart`; stop/start control |
| E2 | `e2-client-stop`, `posix-durability`, `e1-manifest` | same | under 1 min | `--allow remount`; mounts idle |
| E3 | `e3-turnover` | same | about 1 min | `--allow turnover,restart`; member stop/start for catch-up (driver-owned clusters only) |
| E4 | `e4-scale` **(stub)** | same | hours (declared counts) | quota for the declared counts |

### Gates the plan names that did not exist: stubs added

Each reports NOTRUN with its reason (`no harness yet: …`) until a harness
replaces it, so a run can never count the item as PASS:
`a13-node-addition`, `c5-protocol-live`, `d1-packing`,
`d3-efs-load`, `e4-scale`.

New real gate: `topology` (A3, A4, A5) compares the members `efs-mgmt status`
reports with the description, requires all up, counts distinct hosts (NOTRUN
when daemons share a host) and records the profile, client hosts and storage
roots per member.

### Existing gates: do they run?

| Gate | State on `376369aa8` |
| --- | --- |
| `identity`, `posix-*`, `e1-manifest`, `e2-client-stop`, `e3-turnover` | run (nuc runs below) |
| `b-checks` | **was broken:** the default plugin called `b_checks.py --cluster … --out DIR`, which `b_checks.py` does not accept (`--mount`/`--seed` required, exit 2 = FAIL). Fixed: it now runs on the first mount's host through that host's mounts with `--expect-members N` and prints the JSON result into the gate log. PASS on nuc |
| `append-failover` | runs |
| `k14-torn-read`, `k13-tickets`, `fragment-integrity`, `gc-reclamation` | run: moved to the K33 seed/`--join` CLI (finding 3, fixed). On amd k14, k13 and fragment-integrity PASS; gc-reclamation FAIL on finding 1 (amd run below) |

## Runnable topologies

`python3 scripts/qualify.py run --matrix scripts/qualify_matrix.json --topology NAME --out DIR`
runs one topology with its gates, `--allow` steps, fixture and control
commands from the file; `qualify.py plan --topology NAME` prints it. A
topology marked blocked is refused (`--ignore-blocked` once the blocker is
gone); one on the user's live cluster is refused without `--user-approved`,
which stands for the user's go-ahead for that run and its destructive steps.

| Topology | Members / hosts | Transport | State | Covers | Cannot cover |
| --- | --- | --- | --- | --- | --- |
| `loopback-3` | 3 / 1 (driver-owned) | TCP | runnable | the live harnesses (C4, B2, D2) once per candidate; driver regression | A4, RDMA, 4–5 members |
| `nuc-private-3` | 3 / 1 (driver-owned, via `_node`) | TCP | runnable | the multi-host controller (formation, E1 restart, E3 member catch-up) | A4, RDMA |
| `orcd-mini-rdma` | 3 / 1 daemon host + 1 client host (node9901, node9900) | RDMA (`mlx5_0`) | user's live cluster: `--user-approved` | RDMA for C1–C3, D3 extras, E1–E3, B1–B5 at 3 members; two client hosts | A4 (all daemons on node9901), 4–5 members, E3 catch-up (no per-member stop command), B5 cost figures on a busy cluster, RDMA live harnesses |
| `intel-4x2-tcp` | 4 / 2 (intel1, intel2) | TCP | user's live cluster: `--user-approved` | 4-member rotation and C/E gates over a real network; two client hosts | A4, RDMA, 5 members, E3 catch-up |
| `bare-metal-3-tcp` | 3 / 3 (nuc, intel1, intel2; 10.1.0.0/24) | TCP | **blocked** | A3/A4 at 3, E3 catch-up | RDMA |
| `bare-metal-4-tcp` | 4 / 4 (+ xorinox) | TCP | **blocked** | A3/A4 at 4 | RDMA |
| `bare-metal-5-tcp` | 5 / 5 (+ amd; 10.0.0.0/24 Wi-Fi) | TCP | **blocked** | A3 >4 and A4 at 5 | RDMA |

The bare-metal topologies are driver-owned private clusters
(`private_cluster` in the description): the driver starts one efsd per host
from `/home/maik/efs-qual-matrix/src` (`qualify.py _node start`, seed then
`--join`), mounts the clients, runs the gates, and at the end unmounts, stops
its daemons, keeps their logs as raw output and deletes
`/home/maik/efs-qual-matrix/<topology>` on every host. They never use ports
17432–17435 or `/data1/efs`. Run them from the Mac: the hosts cannot ssh to
each other. `nuc-private-3` is the same controller on one host, which is how
it was tested.

The two live topologies' control lines (devops `stop.sh`/`start.sh`, the
orcd-mini client start) are written from the devops cluster files and have
not run; the parent checks them against devops before the first run.

## Hosts today and the gaps that block A4 (for the user)

Measured Oct 24 from the five bare-metal hosts (TCP connects to closed ports,
never to 17432–17435):

| Host | CPU / RAM | Networks | RDMA | Reachable from the other hosts |
| --- | --- | --- | --- | --- |
| nuc | 8 / 30 GB | 10.1.0.204 (wired), 10.0.0.204 (Wi-Fi) | none | ssh only |
| intel1 | 8 / 15 GB | 10.1.0.208, 10.0.0.208 | none | ssh only |
| intel2 | 8 / 15 GB | 10.1.0.210, 10.0.0.210 | none | ssh only |
| xorinox | 16 / 60 GB | 10.1.0.202, 10.0.0.202 | none | ssh only |
| amd | 32 / 119 GB | 10.0.0.206 (Wi-Fi only) | `rxe0` (soft-RoCE) | any port (no host firewall); no 10.1.0.0/24 link |

- **Distinct-host topologies that can form today: none.** firewalld on the
  four Rocky hosts rejects every probed port except ssh (`No route to host`
  for 4791, 7000–9000, 17400–17600, 18000–19000, 20000–60000), so a private
  cluster across them cannot form. The live intel cluster works only because
  its own ports are open between intel1 and intel2.
- **3 and 4 hosts:** open a private TCP range (for example 18600–19199) on
  10.1.0.0/24 on nuc, intel1, intel2 and xorinox (needs root).
- **5 hosts:** the same, plus a network all five share: amd has no wired
  10.1.0.0/24 link, so today five hosts meet only on Wi-Fi.
- **RDMA on distinct hosts (3, 4, 5):** no fabric exists. orcd-mini has two
  InfiniBand hosts; three distinct RDMA hosts need a third ORCD node, or
  soft-RoCE (`rdma_rxe`, root) on the bare-metal hosts' wired network.
- **No ssh between the hosts:** the driver runs on the Mac (it reaches every
  host); `posix_2client.py --remote` runs there too.
- Every host also runs the user's live clusters on 17432–17435; a private
  qualification cluster shares their CPUs and disks, which matters for
  durations, not for verdicts.

## Validation runs (nuc)

These runs check that the planned gates run, not a topology (one host, A4).
Tree: `git archive` of this branch to nuc `~/efs-qual-matrix/src`, built with
`EFS_GIT_ID`/`EFS_GIT_DESCRIBE`/`EFS_GIT_BRANCH`. Runs `r1`/`r2` used the
working tree before commit `ea3360a75`, identity `g376369aa8-a11wip`; the
`ea3360a75` runs used the commit. Host nuc (8 CPUs, 30 GB, Rocky 10,
`/home` XFS), TCP, `O_DIRECT`, 4 G quota per daemon (1 G on loopback), the
user's nuc cluster running alongside on 17432–17435 (not touched).

| Run | Gates | Result |
| --- | --- | --- |
| `nuc-private-3-r1` (wip) | all 17 | setup (formation via `_node`, two mounts) PASS; identity, b-checks (89 s), posix-core, posix-extras (4 pass, 1 SKIP), posix-2client, e3-turnover (with member stop/start and catch-up) PASS; **posix-durability FAIL** (finding 1); e1/e2 NOTRUN because that stop left the mount unmounted; 7 stubs NOTRUN |
| `nuc-private-3-r2` (wip) | identity, posix-durability, e1-manifest, e2-client-stop | all PASS; E1: 240 files, 140,120,143 B verified before and after the driver-owned orderly stop/start |
| `nuc-private-3-r3-ea3360a75` | all 17 | same as r1: setup 6.6 s, six PASS, **posix-durability FAIL** (finding 1), E3 member catch-up 2.1 s; teardown kept the daemon and client logs, deleted the stores |
| `loopback-3-r1` (wip) | the 15 loopback gates | identity, b-checks, posix-core, posix-extras, posix-2client, posix-durability, e1, e2 PASS; topology NOTRUN (one host); **append-failover FAIL** (finding 2); k14, fragment-integrity, gc-reclamation FAIL (`--node-id`); k13 NOTRUN (its ports are fixed to 18700–18799, outside nuc's range); **e3-turnover FAIL**: the targeted step-down was refused "target not caught up …; retry" and the driver did not retry (driver bug, fixed) |
| `loopback-3-r2-ea3360a75` | identity, posix-extras, e3-turnover | all PASS (extras with the 150 s per-case timeout now taken from the matrix) |

Findings for the parent (candidate `376369aa8` code; not changed here):

1. **Clean client stop refused after a long unmount revoke (E2, C2).** In 2
   of 2 full nuc-private-3 runs, `scripts/client.sh stop` (no force) failed
   in posix-durability: `efs-fuse` unmounted, settled its put tickets, then
   needed about 11 s to revoke its session (`touched=765`), while
   `client_processes.py` allows 10 s (`NATURAL_RETIRE_S`) after SIGTERM and
   then reports "client remains alive … no forced kill". It passed when the
   session was small (r2) and on loopback-3. The mount is gone but the stop
   reports failure, so E2's clean-stop gate cannot pass on a busy client.
2. **One REPORT receive timeout under leader step-downs (C3/C4, K15 W52).**
   loopback-3 append-failover: 8,000 of 8,000 records exact in every view, 0
   missing or duplicated, but one REPORT RPC timed out after 30 s
   (`inode-rpc: retry type=67 attempt=11 … recv_ms=30001`), which the
   harness counts as a failure.
3. **Four live harnesses used the removed `--node-id`** (known since the driver
   evidence): `k14_atomic_cluster.sh`, `ticket_put.py`
   (fragment-integrity), `gc_reclamation.py`, `k13_ticket_cluster.py`; the
   K13 harness also hard-coded ports 18700–18799. **Fixed** (amd run below):
   the seed starts as `efsd ADDR:PORT STORE`, the first start of every other
   node as `efsd --join SEED --listen ADDR:PORT STORE`, restarts by endpoint,
   all with `--discovery-port 0`; k14 waits for "listening" only after all
   three started (a joiner logs it once the cluster forms); `ticket_put.py`
   retries `raft-mkfs` NOT_PRIMARY through `raft_form.form`;
   `gc_reclamation.py` rejoins n2–n4 after a fresh format; K13 takes any
   port base. `qualify.py run --matrix` now lets `--port-base` and
   `--private-port-base` override the topology's.

Raw evidence: nuc `~/efs-artifacts/a11-20261024/` (run directories with
their own `SHA256SUMS`; `python3 scripts/qualify.py verify-sums DIR`):

| Run directory | Files | `SHA256SUMS` sha256 |
| --- | ---: | --- |
| `nuc-private-3-r1` | 204 | `622ab186…fbb47` |
| `nuc-private-3-r2` | 117 | `19165ff3…faa3` |
| `nuc-private-3-r3-ea3360a75` | 209 | `2abc9958…36cc` |
| `loopback-3-r1` | 381 | `11ae424f…d4e1` |
| `loopback-3-r2-ea3360a75` | 73 | `9c7f5ea9…b97b3d` |

Stores, mounts and scratch were deleted by the driver; daemon and client logs
are in each run's `gates/_teardown/` (driver-owned) or `loopback/`.

### K33 harness port (amd)

Purpose: finding 3 (C4, B2, D2 live harnesses). Commit `4d1a583cc` (branch
`claude/gifted-pascal-31d5ba`: this branch plus the K33 harness port), `git
archive` to amd `~/efs-k33h/src3`, `make -j24 all` (build identity
`unknown`). amd (32 CPUs, Ubuntu), TCP loopback, `O_DIRECT`, private ports
18600–18699, trees under `$HOME`; the user's amd cluster on 17432–17435 was
not touched. Command:

```bash
python3 scripts/qualify.py run --matrix scripts/qualify_matrix.json --topology loopback-3 --gates k14-torn-read,k13-tickets,fragment-integrity,gc-reclamation --port-base 18640 --private-port-base 18600 --out OUT
```

| Gate | Verdict | Seconds | Notes |
| --- | --- | ---: | --- |
| `k14-torn-read` | PASS | 171 | phases 1 2 3 7, 0 torn reads |
| `k13-tickets` | PASS | 44 | 256 MiB stream, leader death, cold replay |
| `fragment-integrity` | PASS | 10 | one and two roots |
| `gc-reclamation` | **FAIL** | 254 | 56 checks PASS (POSIX 2-client 68/68), then `client.sh stop` refused: the unmount settled its tickets and had not revoked its session 10 s later (finding 1) |

The same gc-reclamation FAIL at the same step reproduced in a standalone
run before the commit. Raw: amd `~/efs-artifacts/k33-harnesses-20261024/r2`
(106 files, `SHA256SUMS` sha256 `eb8d726c…0384`); private trees deleted.

### Code gate

- nuc, `bash scripts/make_test_summary.sh` (test-all) on `8d858a791` (the
  final code; built with that identity): **458 commands, 1 FAIL, 14 NOTRUN**
  (1,889 s). NOTRUN are host limits: 9 RDMA (no device), valgrind (2),
  `~/git/cluster` absent, an `EFS_FAULTS` phase, the runner's self-test.
  On `ea3360a75` the same 1 FAIL, plus `tests/test_qualify_driver.py` counted
  NOTRUN because its PASS line contained the word (fixed in `8d858a791`).
- **The FAIL is the base's, not this branch's (finding 4):**
  `tests/test_get_integrity.py` (D42 READ_VERIFY) no longer compiles on
  `merge-wave5`: its slice of `src/client/read.c` now calls K34's
  `read_reprobe_down`, which the test neither extracts nor stubs (`implicit
  declaration of function 'read_reprobe_down'`, `-Werror`). The untouched
  base `376369aa8` fails the same way on nuc; this branch changes no file
  the test reads. The merge-wave5 gate fails on it until the test stubs the
  function (one line, like its `efs_client_node_is_down` stub).
- Docs gate: `cd docs && python3 gen-architecture-full.py` and
  `python3 docs/check-architecture.py`: 5/5 checks pass.
- Raw: nuc `~/efs-artifacts/a11-20261024/test-all-8d858a791/` (summary,
  timings, the failure log, the base rerun of the test). The whole evidence
  directory has `SHA256SUMS` over 1,004 files, sha256
  `27d741470aac2c75896b7938cc6ff5faff1ddf2571e4a3ba18607b61e4896711`. The
  build tree `~/efs-qual-matrix` was deleted after the runs.

## Code changes

- `scripts/qualify.py`: `topology` gate; seven NOTRUN stub gates (`STUBS`);
  `b-checks` default plugin fixed to `b_checks.py`'s CLI; `ssh` description
  field (a wrapper called as `PROG HOST COMMAND`, e.g.
  `scripts/orcd_mini_ssh.sh`, also passed to `posix_2client.py --ssh`);
  `private_cluster` descriptions with the `PrivateCluster` controller and the
  `_node start|stop` helper; `run --matrix … --topology …` with
  `--user-approved`/`--ignore-blocked`; `plan [--topology] [--check]`;
  `fuser -m` checks ignore its permission messages; the mount's host reaches
  the `client_stop`/`client_start` templates as `{host}`; E3 re-issues a
  targeted step-down the leader refused with "retry" (target not caught up)
  until the turnover timeout; a matrix topology's `fixture` applies to
  loopback runs too.
- `scripts/qualify_matrix.json` (new): dimensions, cells, seven topologies,
  the 25 Stage 2 items.
- `tests/test_qualify_driver.py` (in `make test`, about 3 s): topology gate
  against a stub `efs-mgmt` (PASS path, shared host, down and missing
  member), stub verdicts, the `b-checks` command line, the ssh wrapper, the
  committed matrix validates and five kinds of broken matrix are caught,
  blocked/live refusals, the planned formation and teardown of a 4-host
  private cluster, `_node` start/refuse/stop with a stub efsd.

## Reproduction and cleanup

```bash
python3 scripts/qualify.py plan                      # the matrix, validated
git archive HEAD | ssh nuc 'mkdir -p ~/efs-qual-matrix/src && tar -x -C ~/efs-qual-matrix/src'
ssh nuc 'cd ~/efs-qual-matrix/src && EFS_GIT_ID=ea3360a75 EFS_GIT_DESCRIBE=gea3360a75 \
  EFS_GIT_BRANCH=claude/devel-a11-matrix-2bfd14 make -j8'
ssh nuc 'cd ~/efs-qual-matrix/src && TMPDIR=$HOME/efs-qual-matrix/tmp python3 scripts/qualify.py run \
  --matrix scripts/qualify_matrix.json --topology nuc-private-3 --out ~/efs-artifacts/RUN --work ~/efs-qual-matrix/work'
```

`loopback-3` is the same with `--topology loopback-3` (about 30 minutes,
append-failover alone about 19 minutes). Run long commands in `screen`. The
driver deletes its stores, mounts and scratch; afterwards remove the tree with
`edelete --force --delete ~/efs-qual-matrix`. On another host pass its own
port ranges (`--port-base`, `--private-port-base` for loopback; edit the
addresses of a private topology).

## Limitations and remaining gates

- A11's qualification part stays open: no distinct-host run is possible until
  the host gaps above are closed; every cell is NOTRUN.
- 7 stub gates must become harnesses before B6, D1, D3 (load), D4, E4, A13
  (real host) and C5 (per topology) can PASS.
- D2 (`gc-reclamation`) live evidence is blocked by finding 1: the POSIX
  phase passes, then `client.sh stop` is refused.
- The live harnesses run on TCP loopback only; RDMA for them is not driven.
- The orcd-mini and intel control lines have not run.
- The E3 retry path is not exercised by a recorded run: in
  `loopback-3-r2-ea3360a75` both step-downs succeeded at the first attempt.
- 6–16 members beyond 5 are sampled only at 6–8 on loopback.
