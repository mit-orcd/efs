# Stage 1 batch D on fcstor010-014 (Task EY)

**Verdict: IN PROGRESS (Nov 9-10).** This page is filled in as the campaign runs; the verdict, the per-run tables and the K-row consequences are final only when this line says so.

- Purpose: [Stage 1 acceptance matrix](../stage1-acceptance-matrix-20261030.md) batch D (D-TCP then D-RDMA): the D46 cell gates on distinct hosts and the own runs H1-H10, H12-H15, for the K rows the matrix lists against batch D in the [checklist](../alpha-1.0-2plus1-checklist.md).
- Candidate: devel `9bb50fe79` (wave 67), built from `git archive 9bb50fe79`.
- Hosts: fcstor010-014 only (fcstor012-014 the member hosts of every cell, fcstor010 and fcstor011 the FUSE clients; the 4- and 5-member cells add members on fcstor011 and fcstor010). The live orcd-test cluster (fcstor003-006, ports 17432-17435, `/tmp/efs-mount` on fcstor007-013), orcd-mini, `/data1` and the `efs` user were not touched.

## Deploy

| Item | Value |
| --- | --- |
| Source | `git archive 9bb50fe79` into `~/work/efs-taskEY-fcstor012/src` (shared NFS home, one tree for all five hosts) |
| Build | fcstor012, `make -j24 EFS_GIT_ID=9bb50fe7932d2b487d0d5eed1294b003cb735497 EFS_GIT_DESCRIBE=v0.3.2-pre-alpha-2887-g9bb50fe79 EFS_GIT_BRANCH=devel EFS_FUSE_PREFIX=<deps>/libfuse test-bins tests/faults/efsd`, gcc 11.5.0, `-O3 -march=native` (Zen 2 on all five hosts), 28 s, rc 0 |
| Identity | `efsd v0.3.2-pre-alpha-2887-g9bb50fe79 (branch devel, built 2026-11-09T22:18:33Z)`, the same on all five hosts (`--version` read on each) |
| `efsd` | `b80bb27532073319a7c672f7a236571d79d15a37a1827831c1d99cfa54116531` |
| `efs-fuse` | `ae7a078d56a1450c18fc8f031dd5e04096cbf3a4b265094afd36b1474dbe6d6a` |
| `efs-mgmt` | `34fe84e9c8f841d76a4e5d190413a3f13480aa37dda22721e5cb11d71d12eae1` |
| `efs-bench` | `0c5f87cf9287e95abee44ecbbd657b7f7add6882735cbe42bf1560bf5c1db253` |
| `tests/faults/efsd` | `254fc8579e6784ac28852e6b04c4818febe36b798ec150bac7c250959df34ba9` |
| Fault-client tree | the same archive in `~/work/efs-taskEY-fcstor012/faults`, `make -j8 EFS_FAULTS=1 efsd efs-fuse efs-mgmt` (lost-reply modes of H1, K14 phases 4-6) |
| libfuse | 3.16.2, binary-patched copy (see the deviations below), sha256 `0c75ffe4265ff3edbe6bc0704897d97d3b30016b1d1cd8f1fb56977dc8c2ed03` |

Hosts: fcstor010-014, AMD EPYC 7402P (24 CPUs), 250 GB, RHEL 9 (5.14.0-687.39.1.el9_8), Python 3.9.25. Network: IPoIB `ibs1f0` 172.16.223.64-68 on `mlx5_2` (InfiniBand, port ACTIVE on all five). Stores and mounts on local `/tmp` (md0, 406 GB free), never `/data1`. Load average below 0.25 on every host before each run.

## Method

- Driver: `scripts/qualify.py run --cluster <description> --out ~/efs-artifacts/taskEY-20261109/<run>` on the ORCD login node (Python 3.12), through [`scripts/evidence/task-ey/task_ey_ssh.sh`](../../../scripts/evidence/task-ey/task_ey_ssh.sh) (the fcstor hosts cannot ssh onward, so the controller has to sit on the login node). The six descriptions are [`scripts/evidence/task-ey/fcstor-{3,4,5}x1-{tcp,rdma}.json`](../../../scripts/evidence/task-ey/fcstor-3x1-tcp.json): driver-owned private clusters (fresh stores every run, seed + `--join`, `--discovery-port 0`), members on `172.16.223.66-68:18432` (plus `.65` and `.64` for 4 and 5 members), mounts `/tmp/efs-taskEY/cluster/mnt` on fcstor010 and fcstor011, live-harness host fcstor012 (`/tmp/efs-taskEY/live`, ports from 19400), 16G quota per member, `posix_timeout_s` 150, RDMA cells `transport: rdma`, `rdma_dev: mlx5_2`.
- Cell gate list: the matrix's 19 cell gates less `soak` (batch S owns the four-hour soak, S1; it is not in the 60/60/90 min cell estimate) and, on RDMA, less `d5-expiry` (a TCP-only loopback harness).
- Own runs: a serial queue on the login node runs each H run on one host at a time, with a preflight and postflight of all five hosts (no 18xxx/19xxx listener, no cluster tree, no live tree, no `efsd`/`efs-fuse` of `erbmi1`); a dirty postflight stops the queue.
- Raw output: `~/efs-artifacts/taskEY-20261109/` on the shared ORCD home (one directory per run; `ops/` holds each run's driver log, times and pre/postflight).

## Deviations from the brief and from D46

- **Hosts and stores (D46):** D46 names fcstor007-011 with stores under `/data1/01`. This campaign runs on fcstor010-014 (the coordinator's host assignment; batches S and F hold the other hosts) with stores on local `/tmp` (md0), because `/data1` is read-only for this task.
- **Shared hosts:** the 4- and 5-member cells put members on the client hosts fcstor011 and fcstor010: five hosts cannot hold five member hosts and two separate client hosts. Only the 3-member cells keep members and clients apart.
- **`rdma_dev` is `mlx5_2`, not `mlx5_0`:** on fcstor010-014 `mlx5_0` is an Ethernet port (10.1.223.x); `mlx5_2` is the InfiniBand HCA under the IPoIB endpoints 172.16.223.64-68 (as Task DW found).
- **libfuse is a binary-patched copy, not Task DW's:** Task DW used `~/orcd/scratch/efs/orcd-test/deps/libfuse`, which no longer exists (the scratch automount holds no `efs` tree on these hosts). The only libfuse 3.16.2 on the shared home is another session's `~/w123-deps-20261108-v2/libfuse`. It was copied, and the copy is used unmodified apart from two edits: the `fuse3.pc` prefix, and the compiled-in `FUSERMOUNT_DIR` path in `libfuse3.so.3.16.2`. That path points at the original tree's own `fusermount3`, which is not setuid; libfuse executes it first, so every mount failed with `Operation not permitted` (shakedown run, kept as `shakedown-tcp-3x1-fusermount`). The string was overwritten with a same-length non-existent path, so libfuse falls back to the system's setuid `/usr/bin/fusermount3` (3.10.2; the mount protocol is the same). Patched library sha256 `0c75ffe4265ff3edbe6bc0704897d97d3b30016b1d1cd8f1fb56977dc8c2ed03`. Every mount in this campaign runs on this patched library.
- **Controller on the ORCD login node, using the Mac's forwarded ssh agent:** the fcstor hosts cannot ssh to each other and the login node holds no usable key for them, so the driver and the run queue sit in `screen` sessions on the login node (login007). Their ssh to fcstor010-014 authenticates through the agent that the Mac forwards over its one multiplexed ssh connection to orcd-login. The agent socket exists only on the login node, under the per-connection `/tmp/ssh-*` directory sshd creates, for as long as that Mac connection lives; it disappears when the connection closes. No key was created or copied. No key material and no agent socket path is written into any artifact or log (checked with `grep -rlE` over `~/efs-artifacts/taskEY-20261109` and the controller directory). The controller directory `~/work/efs-taskEY-login` is removed at the end. If the Mac's connection is replaced (other sessions share the same multiplexed connection), it can land on another login node and the old node's agent socket disappears. That happened twice: at 23:08 (3-member TCP follow-up run) and 01:46 UTC (4-member TCP cell, e4-scale), the screens on login007 lost the agent. Their ssh failed on every host at once (`kex_exchange_identification: Connection closed by remote host` or `Permission denied (publickey)`), and the driver's own teardown could not run; both clusters were torn down by hand (`manual-teardown/` in each run directory). From 01:50 UTC the wrapper keeps one persistent master connection per host (ControlPersist 12 h, sockets in a mode-700 directory in the controller's local `/tmp`), so an authenticated master outlives the agent. The queue ran on login006 from then on. At 01:56 UTC the connection moved again (to login010) and the fcstor010 master on login006 was gone (cause not found; the other four masters were alive). login006 could no longer open a connection without the agent, so the 5-member TCP cell's d4-progress ended with ssh's exit 255 while `d4_progress.py` kept running on fcstor010. The cell was stopped by pid (driver tree on login006, the orphaned d4 process on fcstor010), torn down from login010 and rerun in full as `tcp-5x1b`. The login006 masters were closed (`ssh -O exit`) and its socket directory removed. From 02:06 UTC the queue checks before every run that a live master exists for each of the five hosts, opening a missing one. If that fails it stops with a `NO-MASTER` line instead of starting a run, and the queue is restarted on the login node the Mac's connection then reaches.
- **Persistent per-host ssh connections from the login node (from 01:50 UTC, Nov 10):** one master connection per fcstor host, opened by the wrapper on login006, with its control socket in `/tmp/efs-taskEY-mux-erbmi1/<host>` on login006 (mode 700). They are kept open for up to 12 h idle and by design outlive the forwarded agent that authenticated them. At the end of the campaign each one is closed with `ssh -O exit` per host, and the end of this page records that no control socket and no ssh process of `erbmi1` from this campaign remains on the login node. They are closed at once if the user asks. The Mac's own shared connection to the login node is never created or re-created by this task; if it is gone, the campaign stops and reports.
- **Harness fixes made during the run** (scripts only; product binaries unchanged): the ssh wrapper drops the two `cargo/env: No such file or directory` lines the account's `~/.bashrc` prints on every ssh; d4-progress runs on fcstor010 (the driver host has no mount); 16G member quota so the E4 fixture fits; `payload()` in `scripts/qualify.py` and `scripts/stage2_helpers.py` generates in 32 MiB pieces (`6ed972e3a`; Python 3.9 `randbytes` overflows at 256 MiB). A failed teardown's record carries `destructive_steps`, so the run summary no longer stops with a KeyError (`ec0f87b13`). The C5 wire probe follows a `NOT_PRIMARY` leader hint by member rather than by list position (`6de76273c`, from the 5-member rerun on): the hint is a raft id, which follows join order, and the 5-member cell's members joined out of description order. The tree's patched scripts: `qualify.py` sha256 `8fa89e5b9af29e6d7a0f071dd7c6a4e55ef8d7e58c9521e761e57b4deb9479ea`, `stage2_helpers.py` `743dc61d1ca457d2948bcfd8652ae8756572a1d505f3c5b97bbd6e8211ff9ae1` (before `6de76273c`: `58f335dbc8ad95e7bf1860cebeeb4736c6fa94c20625327d1a24bd6c47237e52`).

## Candidate status

`9bb50fe79` is no longer the release candidate: K38 recurred on orcd-mini, K102 is new and K32 may reopen (batch O, Task EW). Every PASS here is "on candidate `9bb50fe79`". K rows (the coordinator's rule, Nov 10): a row whose whole stated acceptance these runs meet becomes VERIFIED "on candidate `9bb50fe79`"; a row with acceptance still owed (another batch, or a part these runs did not cover) keeps its status and gains a dated clause naming what batch D proved; a product failure gets a new row in K110-K119. The cells and own runs that the candidate's fixes touch rerun on the fixed candidate.

## Results: TCP

Every run below is on candidate `9bb50fe79`. Times are UTC, Nov 9-10. Raw output: `~/efs-artifacts/taskEY-20261109/<run>/`, and the driver log, times and pre/postflight for each run in `ops/`.

### Cells

| Run | Window | Gates | Result |
| --- | --- | --- | --- |
| `tcp-3x1` | 22:24-22:47 | full list | 15 PASS (topology, b-checks, posix-core/extras/2client/durability, d5, e1, e2, b6, e3, a13, c5 33/33, d1, d3). identity FAIL on the account's `.bashrc` noise (fixed in the wrapper, PASS on every later run). d4 NOTRUN (no mount on the driver host, fixed with the plugin host). e4 NOTRUN (4G quota too small, fixed with 16G) |
| `tcp-3x1-id-d4` | 22:48-23:08 | identity, topology, d4 | identity, topology PASS. d4 FAIL at the 4G quota (ENOSPC, foreground ops over 10 s); superseded by the next run. The controller lost its agent at 23:08 and the cluster was torn down by hand |
| `tcp-3x1-d4e4` | 00:30-00:57 | identity, topology, d4, e4 | d4 FAIL on server RSS only: **NOTRUN (harness, server memory)**; backlog peak 151,868, within the bound. e4 FAIL on the harness (`randbytes` overflow, fixed in `6ed972e3a`); rerun queued as `tcp-3x1-e4b` |
| `tcp-4x1` | 00:57-01:46 | full list | 16 PASS (as `tcp-3x1` plus identity; c5 44/44). d4 **FAIL (peak 203,822 against the 200,000 bound, then declining) plus NOTRUN (harness, server memory)**. e4 FAIL when the controller lost its agent at 01:46 (ssh `kex_exchange_identification`); torn down by hand |
| `tcp-4x1-e4` | 01:50-01:53 | identity, topology, e4 | PASS (20,000/20,000 small + 8/8 × 256 MiB exact on a cold reader, p99 0.061 s) |
| `tcp-5x1` | 01:53-01:59 | full list | interrupted: d4 ended with ssh exit 255 when the fcstor010 master on login006 died; stopped by pid, torn down, rerun in full |
| `tcp-5x1b` | 02:06-02:58 | full list | 15 PASS (identity, topology, b-checks, posix ×4, d5, e1, e2, b6, e3, a13, d1, d3, e4 25,000 + 10 × 256 MiB). d4 **FAIL (peak 256,641 against the 200,000 bound, then declining) plus NOTRUN (harness, server memory)**. c5 **FAIL (harness)**: 53/55. On `.67` and `.65`, `unknown-tag-254` on group 2 got `NOT_PRIMARY` with hint 1, and the probe read the hint as a position in the description, so it asked `.67` again six times. The members joined out of description order (`efs-mgmt status`: node 2 is `.64`, the group-2 leader; `.67` is node 4), so hint 1 named `.64`. Asked directly, `.64` refused the tag with `-7`, as the gate wants. Fixed in `6de76273c`; C5-only rerun `tcp-5x1-c5` queued |

### Pack backlog (D4) and the create rate

The coordinator's rule (Nov 10): open K110 only if the 5-member series still rises at the end of the 600 s load. It does not: the backlog peaks at 240 s and falls at every later sample, so no K row. In all three cells the packer packs almost nothing for the first two to three minutes (it waits for `EFS_PACK_MIN_AGE_MS`, default 60 s, and then ramps up). After that it keeps pace with the creates, and the backlog drains within 63-79 s of the load's end. The create rate falls steadily over the load in every cell, from about 1,240-1,410/s to 740-900/s. That fall starts before packing reaches full speed and continues after, and it is not explained by member CPU (below).

| Cell | Load s | Creates/s | Packs/s | Peak backlog | Drain s |
| --- | --- | --- | --- | --- | --- |
| 3 members (`tcp-3x1-d4e4`) | 599 | 935 | 679 | 151,868 | 63 |
| 4 members (`tcp-4x1`) | 600 | 1,030 | 674 | 203,822 | 66 |
| 5 members (`tcp-5x1b`) | 599 | 1,107 | 659 | 256,641 | 79 |

Per minute (creates/s / packs/s over the minute ending at that point; peaks come from every load sample, the per-minute backlog only from the minute marks):

| Minute | 3 members | 4 members | 5 members (backlog) |
| --- | --- | --- | --- |
| 1 | 1,240 / 8 | 1,365 / 4 | 1,414 / 5 (45,093) |
| 2 | 1,151 / 59 | 1,243 / 63 | 1,341 / 62 (124,549) |
| 3 | 1,020 / 666 | 1,179 / 216 | 1,277 / 50 (198,634) |
| 4 | 930 / 953 | 1,041 / 973 | 1,167 / 569 (256,641) |
| 5 | 916 / 928 | 1,009 / 1,022 | 1,080 / 1,023 (249,863) |
| 6 | 888 / 828 | 961 / 962 | 1,019 / 1,035 (236,206) |
| 7 | 872 / 951 | 947 / 940 | 1,016 / 1,023 (232,330) |
| 8 | 827 / 838 | 898 / 856 | 923 / 1,008 (225,559) |
| 9 | 743 / 810 | 830 / 872 | 918 / 909 (222,851) |
| 10 | 754 / 761 | 826 / 831 | 902 / 919 (218,378) |

**Per-member CPU (5-member cell).** A sidecar (`d4_cpu_sidecar.sh` in the controller directory) recorded each member's efsd CPU time, and its five busiest threads, every 15 s. The load window, 02:14:11-02:24:11, is derived from the gate's end time less the verify and load/drain durations, so it is accurate to about one sample. Mean efsd CPU over the load, in CPUs of the host's 24:

| Host | Role | Mean | Per minute |
| --- | --- | --- | --- |
| fcstor010 | member node 2 (group-2 leader), the d4 client | 1.88 | 2.56 2.45 2.48 1.44 0.62 1.54 1.94 1.94 1.95 |
| fcstor011 | member node 3, second client host | 0.61 | 0.84 0.84 0.83 0.49 0.19 0.48 0.62 0.59 0.62 |
| fcstor012 | member node 1 (group-0 leader) | 0.22 | 0.32 0.30 0.30 0.19 0.10 0.16 0.22 0.22 0.21 |
| fcstor013 | member node 4 | 0.51 | 0.73 0.69 0.68 0.35 0.11 0.43 0.51 0.55 0.52 |
| fcstor014 | member node 5 | 0.09 | 0.17 0.16 0.16 0.13 0.06 0.03 0.03 0.03 0.05 |

The busiest single efsd thread used 35% of one CPU (fcstor010), and no other used more than 21%. No member and no member thread was saturated, so member CPU is not the limiter of the create rate. fcstor010 also runs the d4 client, so its figure is not comparable with the other members'. The limiter is not identified here. The client side (one FUSE client issuing the creates) is the next measurement, and it is listed as a follow-up. **The 200,000 bound itself is a decision for the user**: the backlog drains within 80 s once the load stops. The peak grows with the number of members, because more members accept more creates per second while the packer's start-up delay stays the same.

### Own runs

| Run | Window | Commands (reps) | Result |
| --- | --- | --- | --- |
| H1 `tcp-h01` (K15) | 02:58-03:14 | w52-20k (2), stepdown-8k (2), lostreply-2k (2), sigkill-8k (2), combined-2k (2) | w52, stepdown and lostreply 6/6 PASS. **sigkill-8k 0/2, combined-2k 1/2: FAIL on the final fsync only.** Integrity PASS in every run (8,000/8,000 records exact, no duplicate, missing, torn or out-of-order record, three views identical). The writers fsync once, at the end. By then every member had been SIGKILLed and restarted at least once (six kills, 12 s apart, alternating groups), and each writer's final fsync returned `EIO`: `fsync ... fragments=DPL: 1 of 3 durable; need 2 (a member of another incarnation lost a fragment)`. The K15 accounting page ([Oct 24](../2026-10/k15-k29-accounting-rule-20261024.md), "NOTRUN gates") expects `--failover kill` to fail its final fsync, fail-closed by design; Task N never ran the kill modes. Classification awaits the coordinator's ruling; the RDMA kill modes are held back until then |
| H2 `tcp-h02` (K14, K13) | 03:14-03:18 | k14 phases 1-7 with the fault tree, k13 tickets, fragment integrity | 3/3 PASS |
| H3 `tcp-h03` (K48/W130) | 03:18-03:19 | w130 dir fault (3) | 3/3 PASS |
| H4 `tcp-h04` (K33, K35, K41, K43, K54, E2) | 03:19-03:50 | k33 payload (20), e2 peer-stop loop (20 runs), k35, k33 growth (10), k41 (10), k54 member-down unmount (20 runs, alternate), k43 (3) | all PASS except **k41 9/10**. Run 5's step 2 asserts that node 6 refused at least one segment PUT. Node 6 was still marked down (`marked down for 30s`) from step 1's silence, so the packer never chose it and nothing was refused. The race is in the test's timing, not in placement: nothing degraded, every segment had three fragments. Three solo reruns are queued (`tcp-h04-k41`) |
| H5 `tcp-h05` (K36, K37) | 03:50-03:54 | k36 recovery (3), k36 segment recovery (3), k37 segment rebuild (3) | 9/9 PASS |
| H6 `tcp-h06` (GC) | 03:54-03:56 | gc reclamation `--posix`, `--put-barrier-fault` | 2/2 PASS |
| H7 `tcp-h07` (K38 shared chunk, Task DW) | 03:56-03:58 | DW campaign, tcp arm, 4 runs, members fcstor012-014, clients fcstor010/011 | 4/4 PASS (write 3.1-4.3 s, no `EIO` or torn read, K94 0) |
| H8 `tcp-h08` (AO, dir ops) | 03:58-04:00 | ao readdir `--posix`, dir-ops busy (3), dir-ops core, ao readdir live | 6/6 PASS |
| H9 `tcp-h09` | 04:00 | h09 extend-EOF, h09 exclusive create on the 3-member cluster mount | PASS |
| H13 `tcp-h13` (pump, D4 kit) | 04:00-04:10 | pump file (3), pump blocked (3), pump test, d4 kit obs0 (3), obs1 (3) | 13/13 PASS |
| H14 `tcp-h14` (K5) | 04:10-04:20 | k05 session fence gate (2) | **NOTRUN (harness)**. `scripts/evidence/k05/k05_session_fence_gate.sh` starts efsd with `--node-id`, `--addr`, `--port` and `--storage`, which K33 removed, so no daemon started. Every later CHECK line ran against an unmounted directory: posix-core "216/218" there is the local `/tmp` file system. Porting the script is awaiting the coordinator |
| H15 `tcp-h15` (K22) | from 04:20 | k22 leader admission (3), k22 snapshot import (1 GiB) | leader admission 3/3 PASS; snapshot import running |

## Results: RDMA

(not started)

## Limitations

(filled in at the end)

## Follow-ups

- **D4 create-rate limiter:** member CPU is ruled out (5-member cell above). Measure the client side next: efs-fuse CPU and its busiest threads on the d4 host, and create latency per minute from `scripts/d4_progress.py`'s foreground samples.

- **d4-progress cannot collect member RSS on these hosts: NOTRUN (harness) in every cell.** `scripts/d4_progress.py` runs on the mount host and samples each member's RSS with its own `ssh HOST ps ...` (`server_rss()` in the sampler class, called from the per-sample loop); fcstor hosts cannot ssh to each other, so every sample is `None` and the gate's RSS bound fails. The controller reaches every member through the description's `ssh` wrapper, but the daemons report no RSS over `efs-mgmt`, and the gate needs the local mount, so routing the samples through the controller is not a small change (a member-side sampler started by the driver, or an RSS field in `efs-mgmt status`). The gate's other checks (foreground deadline, KV bounds, pack backlog, index age, drain) are reported for the record but do not make the gate PASS.

## Reproduction

(filled in at the end)
