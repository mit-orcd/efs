# Stage 1 batch D on fcstor010-014 (Task EY)

**Merge state (Nov 10):** merged into devel in wave 74. At the merge the K110 and K42 rows got a clause from this page, and the third peer-stop loop (20 of 20 stops refused with `uncertified-busy` or `append-open-by-other-session`) became row K112, OPEN, blocks alpha: yes; where this page says K42 for that loop, read K112.

*The paragraph above is the current state. Everything below is the task's report as it was written before the merge: where it says "unmerged", "not merged" or names a next action, the paragraph above replaces it.*

**Verdict: FAIL (Nov 9-10, final).** Two product failures, both owned by the coordinator: K42 recurred on RDMA (clean stops refused in `rdma-4x1` and in one of three peer-stop loops, 20 of 20 stops), and the H1 kill modes end in a final-fsync `EIO` on both transports (K110, number assigned at merge). H15 failed on its fixture and is owed. Every other cell gate and own run that ran passed, apart from harness NOTRUNs. Per-row consequences are in the dated batch D clauses in the [checklist](../alpha-1.0-2plus1-checklist.md).

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
- **Controller on the ORCD login node, using the Mac's forwarded ssh agent:** the fcstor hosts cannot ssh to each other and the login node holds no usable key for them, so the driver and the run queue sit in `screen` sessions on the login node (login007). Their ssh to fcstor010-014 authenticates through the agent that the Mac forwards over its one multiplexed ssh connection to orcd-login. The agent socket exists only on the login node, under the per-connection `/tmp/ssh-*` directory sshd creates, for as long as that Mac connection lives; it disappears when the connection closes. No key was created or copied. No key material and no agent socket path is written into any artifact or log (checked with `grep -rlE` over `~/efs-artifacts/taskEY-20261109` and the controller directory). The controller directory `~/work/efs-taskEY-login` is removed at the end. If the Mac's connection is replaced (other sessions share the same multiplexed connection), it can land on another login node and the old node's agent socket disappears. That happened twice: at 23:08 (3-member TCP follow-up run) and 01:46 UTC (4-member TCP cell, e4-scale), the screens on login007 lost the agent. Their ssh failed on every host at once (`kex_exchange_identification: Connection closed by remote host` or `Permission denied (publickey)`), and the driver's own teardown could not run; both clusters were torn down by hand (`manual-teardown/` in each run directory). From 01:50 UTC the wrapper keeps one persistent master connection per host (ControlPersist 12 h, sockets in a mode-700 directory in the controller's local `/tmp`), so an authenticated master outlives the agent. The queue ran on login006 from then on. At 01:56 UTC the connection moved again (to login010) and the fcstor010 master on login006 was gone (cause not found; the other four masters were alive). login006 could no longer open a connection without the agent, so the 5-member TCP cell's d4-progress ended with ssh's exit 255 while `d4_progress.py` kept running on fcstor010. The cell was stopped by pid (driver tree on login006, the orphaned d4 process on fcstor010), torn down from login010 and rerun in full as `tcp-5x1b`. The login006 masters were closed (`ssh -O exit`) and its socket directory removed. From 02:06 UTC the queue checks before every run that a live master exists for each of the five hosts, opening a missing one. If that fails it stops with a `NO-MASTER` line instead of starting a run, and the queue is restarted on the login node the Mac's connection then reaches. By 12:26 UTC on Nov 10 the Mac's connection reached login007 again; the queue kept running on login010 on its existing masters, and it was followed through its files in the shared home directory, with no new connection to the fcstor hosts from login007.
- **Persistent per-host ssh connections from the login node (from 01:50 UTC, Nov 10):** one master connection per fcstor host, opened by the wrapper on login006, with its control socket in `/tmp/efs-taskEY-mux-erbmi1/<host>` on login006 (mode 700). They are kept open for up to 12 h idle and by design outlive the forwarded agent that authenticated them. At the end (17:32 UTC, Nov 10) the queue's last line, `final_cleanup.sh`, removed `/tmp/efs-taskEY` on all five hosts and the candidate tree, then closed each master with `ssh -O exit` ("Exit request sent" for all five). The socket directory is gone: 0 control sockets. The same script, right after the exits, still listed four `ssh` processes of `erbmi1` on login010 (pids 2306498, 2309118, 2312785, 2311522; bare `ssh`, no arguments shown). The masters may still have been exiting, or these may be other ssh clients of the account; this was not checked afterwards, because login010 was reachable only through those masters. The controller directory was then removed through the shared home. They are closed at once if the user asks. The Mac's own shared connection to the login node is never created or re-created by this task; if it is gone, the campaign stops and reports.
- **A `pkill -f` on the Mac (Nov 10):** to stop a broken background watcher of this session (a local polling loop, whose zsh word splitting never matched), `pkill -f "end rdma-h14"` was run on the Mac, against the campaign's "never `pkill -f`" rule. It touched no host and no cluster process; the watcher was this session's own background shell.
- **Branch rebased onto devel (Nov 10):** to write the checklist clauses against the current register, this branch was rebased onto devel, local only. Commit hashes on this page are the rebased ones. The pre-rebase branch is kept as `backup/kind-darwin-pre-rebase`; devel's K42 row cites its `239b201b1` and `dda2ca748` (now `49a54fbb7` and `75f21fe10`).
- **Harness fixes made during the run** (scripts only; product binaries unchanged): the ssh wrapper drops the two `cargo/env: No such file or directory` lines the account's `~/.bashrc` prints on every ssh; d4-progress runs on fcstor010 (the driver host has no mount); 16G member quota so the E4 fixture fits; `payload()` in `scripts/qualify.py` and `scripts/stage2_helpers.py` generates in 32 MiB pieces (`30d322189`; Python 3.9 `randbytes` overflows at 256 MiB). A failed teardown's record carries `destructive_steps`, so the run summary no longer stops with a KeyError (`cdfc7963c`). The C5 wire probe follows a `NOT_PRIMARY` leader hint by member rather than by list position (`02741aec6`, from the 5-member rerun on): the hint is a raft id, which follows join order, and the 5-member cell's members joined out of description order. The tree's patched scripts: `qualify.py` sha256 `8fa89e5b9af29e6d7a0f071dd7c6a4e55ef8d7e58c9521e761e57b4deb9479ea`, `stage2_helpers.py` `743dc61d1ca457d2948bcfd8652ae8756572a1d505f3c5b97bbd6e8211ff9ae1` (before `02741aec6`: `58f335dbc8ad95e7bf1860cebeeb4736c6fa94c20625327d1a24bd6c47237e52`).

## Candidate status

`9bb50fe79` is no longer the release candidate: K38 recurred on orcd-mini, K102 is new and K32 may reopen (batch O, Task EW). The user has since moved K32 to Stage 3 (D50 on devel), so ior easy write is no longer a Stage 1 gate. Every PASS here is "on candidate `9bb50fe79`". K rows (the coordinator's rule, Nov 10): a row whose whole stated acceptance these runs meet becomes VERIFIED "on candidate `9bb50fe79`"; a row with acceptance still owed (another batch, or a part these runs did not cover) keeps its status and gains a dated clause naming what batch D proved; a product failure gets a new row in K110-K119. The cells and own runs that the candidate's fixes touch rerun on the fixed candidate.

## Results: TCP

Every run below is on candidate `9bb50fe79`. Times are UTC, Nov 9-10. Raw output: `~/efs-artifacts/taskEY-20261109/<run>/`, and the driver log, times and pre/postflight for each run in `ops/`.

### Cells

| Run | Window | Gates | Result |
| --- | --- | --- | --- |
| `tcp-3x1` | 22:24-22:47 | full list | 15 PASS (topology, b-checks, posix-core/extras/2client/durability, d5, e1, e2, b6, e3, a13, c5 33/33, d1, d3). identity FAIL on the account's `.bashrc` noise (fixed in the wrapper, PASS on every later run). d4 NOTRUN (no mount on the driver host, fixed with the plugin host). e4 NOTRUN (4G quota too small, fixed with 16G) |
| `tcp-3x1-id-d4` | 22:48-23:08 | identity, topology, d4 | identity, topology PASS. d4 FAIL at the 4G quota (ENOSPC, foreground ops over 10 s); superseded by the next run. The controller lost its agent at 23:08 and the cluster was torn down by hand |
| `tcp-3x1-d4e4` | 00:30-00:57 | identity, topology, d4, e4 | d4 FAIL on server RSS only: **NOTRUN (harness, server memory)**; backlog peak 151,868, within the bound. e4 FAIL on the harness (`randbytes` overflow, fixed in `30d322189`); rerun `tcp-3x1-e4b` PASS |
| `tcp-4x1` | 00:57-01:46 | full list | 16 PASS (as `tcp-3x1` plus identity; c5 44/44). d4 **FAIL (peak 203,822 against the 200,000 bound, then declining) plus NOTRUN (harness, server memory)**. e4 FAIL when the controller lost its agent at 01:46 (ssh `kex_exchange_identification`); torn down by hand |
| `tcp-4x1-e4` | 01:50-01:53 | identity, topology, e4 | PASS (20,000/20,000 small + 8/8 × 256 MiB exact on a cold reader, p99 0.061 s) |
| `tcp-5x1` | 01:53-01:59 | full list | interrupted: d4 ended with ssh exit 255 when the fcstor010 master on login006 died; stopped by pid, torn down, rerun in full |
| `tcp-5x1b` | 02:06-02:58 | full list | 15 PASS (identity, topology, b-checks, posix ×4, d5, e1, e2, b6, e3, a13, d1, d3, e4 25,000 + 10 × 256 MiB). d4 **FAIL (peak 256,641 against the 200,000 bound, then declining) plus NOTRUN (harness, server memory)**. c5 **FAIL (harness)**: 53/55. On `.67` and `.65`, `unknown-tag-254` on group 2 got `NOT_PRIMARY` with hint 1, and the probe read the hint as a position in the description, so it asked `.67` again six times. The members joined out of description order (`efs-mgmt status`: node 2 is `.64`, the group-2 leader; `.67` is node 4), so hint 1 named `.64`. Asked directly, `.64` refused the tag with `-7`, as the gate wants. Fixed in `02741aec6`; C5-only rerun `tcp-5x1-c5` queued |
| `tcp-3x1-e4b` | 12:25-12:27 | identity, topology, e4 | PASS (15,000/15,000 small + 6/6 × 256 MiB exact on a cold reader under 281 queries, p99 0.061 s; catalog exact; 7,506 deleted and reclaimed, the rest exact) |
| `tcp-5x1-c5` | 12:27-12:28 | identity, topology, c5 | PASS (wire 55/55 on 5 members with the `wire_node()` hint fix `02741aec6`; removed settings refused on 5 hosts; fence in 1 attempt, mutations `EIO`, peer unaffected; clean unmount revoked its session in 0.567 s) |

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
| H1 `tcp-h01` (K15) | 02:58-03:14 | w52-20k (2), stepdown-8k (2), lostreply-2k (2), sigkill-8k (2), combined-2k (2) | w52, stepdown and lostreply 6/6 PASS. **sigkill-8k 0/2, combined-2k 1/2: FAIL on the final fsync only.** Integrity PASS in every run (8,000/8,000 records exact, no duplicate, missing, torn or out-of-order record, three views identical). The writers fsync once, at the end. By then every member had been SIGKILLed and restarted at least once (six kills, 12 s apart, alternating groups), and each writer's final fsync returned `EIO`: `fsync ... fragments=DPL: 1 of 3 durable; need 2 (a member of another incarnation lost a fragment)`. **K110 (number assigned at merge), OPEN, blocks alpha: yes** (the coordinator's ruling, Nov 10; see "K110 capture" below). This is a FAIL against K15's and K29's H1. The Oct 24 note on [the K15 accounting page](../2026-10/k15-k29-accounting-rule-20261024.md) does not cover it: it predates K35 and K36, and the cause it names no longer exists. The RDMA kill modes run once, at the end of the queue, for the transport comparison only |
| H2 `tcp-h02` (K14, K13) | 03:14-03:18 | k14 phases 1-7 with the fault tree, k13 tickets, fragment integrity | 3/3 PASS |
| H3 `tcp-h03` (K48/W130) | 03:18-03:19 | w130 dir fault (3) | 3/3 PASS |
| H4 `tcp-h04` (K33, K35, K41, K43, K54, E2) | 03:19-03:50 | k33 payload (20), e2 peer-stop loop (20 runs), k35, k33 growth (10), k41 (10), k54 member-down unmount (20 runs, alternate), k43 (3) | all PASS except **k41 9/10: a test defect, not a placement fault.** Run 5's step 2 asserts that node 6 refused at least one segment PUT, and it refused none: nothing degraded, and every segment had three fragments. `EFS_FAULT_PACK_PLACE_DOWN=6` keeps node 6 on the placement ring through its down mark (`src/server/packer.c`, the K37 fault block after the down-mark pass), so step 2 depends on placement chance. The packer picks each segment's members by a time-based id. With six ring members, a segment misses node 6 with probability 1/2, and a 60-file batch packs about five segments, so a few percent of runs never place on node 6 and `refused` stays 0. The harness-fix session found this (coordinator, Nov 10); it also failed once in another session's gate on fstor007. The test fix is on branch `claude/blissful-chaum-051bdb` (`fd50deb31`: step 2 packs up to four further batches until node 6 has refused once), proof pending there. Three solo reruns (`tcp-h04-k41`, 12:28-12:30) PASS 3/3; in each, node 6 refused segment PUTs in step 2 (3, 3 and 1 admissions cancelled and placed again on spares) |
| H5 `tcp-h05` (K36, K37) | 03:50-03:54 | k36 recovery (3), k36 segment recovery (3), k37 segment rebuild (3) | 9/9 PASS |
| H6 `tcp-h06` (GC) | 03:54-03:56 | gc reclamation `--posix`, `--put-barrier-fault` | 2/2 PASS |
| H7 `tcp-h07` (K38 shared chunk, Task DW) | 03:56-03:58 | DW campaign, tcp arm, 4 runs, members fcstor012-014, clients fcstor010/011 | 4/4 PASS (write 3.1-4.3 s, no `EIO` or torn read, K94 0) |
| H8 `tcp-h08` (AO, dir ops) | 03:58-04:00 | ao readdir `--posix`, dir-ops busy (3), dir-ops core, ao readdir live | 6/6 PASS |
| H9 `tcp-h09` | 04:00 | h09 extend-EOF, h09 exclusive create on the 3-member cluster mount | PASS |
| H13 `tcp-h13` (pump, D4 kit) | 04:00-04:10 | pump file (3), pump blocked (3), pump test, d4 kit obs0 (3), obs1 (3) | 13/13 PASS |
| H14 `tcp-h14` (K5) | 04:10-04:20 | k05 session fence gate (2) | **NOTRUN (harness)**. `scripts/evidence/k05/k05_session_fence_gate.sh` starts efsd with `--node-id`, `--addr`, `--port` and `--storage`, which K33 removed, so no daemon started. Every later CHECK line ran against an unmounted directory: posix-core "216/218" there is the local `/tmp` file system. **Every H14 result this script produced after K33 is noise.** It was ported in `9aaef59f4`, on the coordinator's ruling: the seed starts on its endpoint, members `--join` it, the script waits for `formed: 3 members` instead of running `raft-mkfs`, and it drops the removed `--no-direct-io`. `EFS_TRANSPORT` and `K05_ADDR` select an RDMA run. There is no path back to the removed flags. The rerun is `tcp-h14b`, then `rdma-h14` |
| H14b `tcp-h14b` (K5) | 12:30-12:33 | k05 session fence gate (2), ported script `9aaef59f4` | **2/2 PASS**, 25/25 checks each: the cluster formed (`formed: 3 members`); POSIX core 218/218 on the mounted cluster; every fenced mutation (chmod, utime, truncate, setxattr, flock, write+fsync, open with a new lease) `EIO`; metadata reads and the open fd's read still served; fence latched in the client log; peer B mutates normally; session A at epoch 2 |
| H15 `tcp-h15` (K22) | 04:20-12:25 | k22 leader admission (3), k22 snapshot import (1 GiB, `scripts/evidence/k22/k22_snapshot_import.sh`) | leader admission 3/3 PASS. **Snapshot import: H15 FAIL at the 28,800 s deadline (not widened). The fixture's store quota filled at 5,618 s because the two 4 MiB writers never delete their files, so the log stopped growing fast enough for a fourth snapshot and the snapshot import was never reached. This says nothing about K22's import path.** No K row; K22 unchanged. With node 3 SIGSTOPped, each group's leader snapshotted past node 3's frozen match three times (group 0 at about 1,540, 4,560 and 24,290 s; group 2 at 1,560, 4,450 and 22,850 s). The log advanced about 5.5M entries per 3,000 s until about 5,600 s, then about 5M in the next 18,000 s. The writers logged 804 errors: 26 `ENOSPC` on close (from 5,618 s), 745 `EAGAIN` on write (from 6,564 s) and 32 `EIO` on close, plus one entry the log interleaved with the driver's trailer. **Ten of the `EIO`s come before the first `ENOSPC` and are not tied to the quota** (see below). The driver's `== rc=1` trailer was interleaved into the error output (`b== rc=1`), so `results.tsv` says `ssh-fail`; the exit status was 1. fcstor012 was clean in the next preflight. **Owed:** the full H15 rerun waits for the K22 snapshot-import memory fix: Task EZ's S2 soak imported a snapshot after a 4 h stall and found the importer holds the whole key diff in memory (13.4 GiB against 128 MiB; a K row at EZ's merge). The import code changes with that fix, so H15 reruns after it, on the fixed fixture (below) |

### H15: close `EIO` before the quota filled (open product question)

The test starts its clock at about 04:20:51 UTC. Node 3 was SIGSTOPped from about 04:21:20 to the end. Ten big-writer or payload closes returned `EIO` before the first `ENOSPC` (5,618 s, 05:54:29). Each matches, to the second, one `dcache-flush-fail` line in the client log (`mnt-a.log`) with efs rc -13, `EFS_ERR_BUSY`. In each, `efs_dcache_flush_ino()` (`src/client/write.c`) found either the PUT pass failing (`stage=put`) or another flusher's PUT window still open after 8 s (`stage=put-window`, `put_win_wait(ino, 8000)`). The first quota backpressure line comes at 05:54:07, after all ten.

| t (s) | Client time (UTC) | File | Client line |
| --- | --- | --- | --- |
| 59 | 04:21:50 | payload/622 | `dcache-flush-fail ino=3581628 stage=put rc=-13` |
| 86 | 04:22:17 | big1/1 | `dcache-flush-fail ino=520815 stage=put rc=-13` |
| 101 | 04:22:32 | big0/1 | `report-sync-fail ino=520928 stage=report ... rc=-13` |
| 123 | 04:22:54 | big1/2 | `dcache-flush-fail ino=524911 stage=put-window rc=-13` |
| 138 | 04:23:09 | big0/2 | `dcache-flush-fail ino=525024 stage=put-window rc=-13` |
| 160 | 04:23:30 | big1/3 | `dcache-flush-fail ino=529007 stage=put-window rc=-13` |
| 612 | 04:31:02 | big0/25 | `dcache-flush-fail ino=4748000 stage=put-window rc=-13` |
| 717 | 04:32:48 | big1/30 | `dcache-flush-fail ino=4768367 stage=put-window rc=-13` |
| 2,316 | 04:59:26 | big1/100 | `dcache-flush-fail ino=10732143 stage=put-window rc=-13` |
| 5,352 | 05:50:02 | big0/236 | `dcache-flush-fail ino=20062944 stage=put-window rc=-13` |

Over the same time the client logged degraded acknowledgements for node 3's fragments (`published degraded for rebuild on return`), so most writes with one member stopped succeeded. The B6 member-unavailability contract allows a close to fail with an explicit error. Whether a busy PUT window after 8 s, with two of three members up, should reach the application as `EIO` was put to the coordinator. It is not diagnosed here. The other 22 `EIO`s, all `ENOSPC` and all `EAGAIN` come after the first `ENOSPC`.

**Coordinator's ruling and reading (Nov 10), not a finding of this run:** no new K row. The ten are recorded in devel under K106 (waits a stopped member still causes) as "likely the same waits, not diagnosed". The coordinator's reading: with node 3 stopped, a PUT placed on it pays two 5 s protocol-declaration waits (K106 part b, 10 s). That outlasts `put_win_wait(ino, 8000)`, so the second flusher gets BUSY and the close returns `EIO`. The candidate `9bb50fe79` predates wave 72, but wave 72 did not change that path. The K106 fix task (proposed as Task FL) is to be checked against this symptom.

**Check against the existing client log (no new run).** The log names a PUT's members only when it completes degraded (`put_fragments ... nodes=a,b,c: degraded acknowledgement, fragment k for node 3 not acknowledged`). It does not log when a PUT was sent, so each PUT's own duration is not in the log. What it does show, with times relative to the failing close:

- **The seven `stage=put-window` cases:** every degraded PUT logged for the inode lacks only node 3's fragment. They arrive in bursts from about -18 s or -10 s up to the failure. The inode's last degraded PUT completes 0.0 to 1.9 s after the 8 s wait gave up (+1.9, +2.0, +1.0, +0.6, +0.0, +0.6, +0.9 s). So in each case a PUT carrying a fragment for node 3 held the window past 8 s and finished just after. Its start time, and so whether it took about 10 s, is not logged.
- **`stage=put`, ino 3581628 (t=59):** no degraded PUT is logged near the failure. The client logged node 3 protocol-declaration failures at -13.4, -8.4 and -3.4 s (5 s apart) and +1.7 s. The failing request's member is not named.
- **`stage=put`, ino 520815 (t=86):** degraded PUTs lacking node 3's fragment came at -20.0, -14.9, -10.0, -4.9 and -0.0 s (5 s apart), and the PUT pass failed with the last burst. The member is not named.
- **`stage=report`, ino 520928 (t=101):** `report-sync-fail ... total_ms=0 rc=-13`, 0.4 s after the inode's last degraded PUT (node 3 not acknowledged). The member the report went to is not logged.
- **Node 3 throughout:** the log has five protocol-declaration failure lines in all, every one for node 3 and every one between 04:21:37 and 04:21:52, so the 5 s declaration wait is logged only around the first case. None of the ten has a line in the 25 s before it that names member 1 or 2 failing. Each involves node 3 as far as the log shows, but for the `put` and `report` cases the log does not name the member the failing request went to.

### rdma-4x1: K42 recurrence, from the logs of that run

The fcstor010 client's session is `e9715a81...` (mounted 14:12:37). Its unmount began at 14:40:10.07, and the revoke failed at 14:40:30.45: `touched=1225 cleaned=824 cleanup_ms=20337 phase=cleanup shard=1460 reason=append-reap-owed`.

- **Who led shard 1460:** group 2's leader for the whole cell was member .67 (`packer: group=2 term=1` runs there; members .65 and .68 log `rejecting AE from=2` for group 2, and no election or term change appears in the window). Shard 1460's only server lines are on .67: `opid-receipt: replay shard=1460 id=e9715a81/1/...`, four at 14:22:57 and the last at `14:38:44.084 ... id=e9715a81/1/3868571 rc=0 ino=4105652`. No line on any member names shard 1460, an append reap or a BUSY reason during the 20 s revoke; the server does not log the refusal it certified.
- **The group 2 leader's GC in the window (.67):** a pass every 1.6-2.5 s, `ms=191` to `ms=253`, with reaps pending in every pass and a few completed each time: for example `14:40:09.223 gc-pass ms=215 ... reap=36 ... reap_sweep=16 reap_fence=2 reap_done=17`, `14:40:17.743 gc-pass ms=253 ... reap=65 ... reap_sweep=16 reap_fence=25 reap_done=23`, `14:40:30.245 gc-pass ms=191 ... reap=34 ... reap_sweep=15 reap_fence=1 reap_done=18`. `reap_scan=0` throughout. Group 0's GC (`gc-health` on .65, .66 and .68) shows `stage=0 ... pending=0,0 reap=0,0`, with `reap_errors=2` on .66.
- **Also in the window:** `raft-host: txn-finish coord=1|15 abort decided rc=0 (coordinator lost its group)`, about every 10 ms on the two group-2 followers (.65: 3,228 lines; .67: 3,386 lines) from 14:39:38. That is during posix-2client (14:39:17-14:40:03), and the lines continue through the revoke. There are none in `rdma-3x1`'s logs. Group 2's followers also reject the leader's AppendEntries with `why=prev-compacted`, with the reject counter at 43,213-43,425 (.65) and 21,380-21,392 (.68) across the window, so the rejects were running long before it. Both are recorded, not diagnosed.
- **Whose append (question 2):** every shard 1460 line comes from this client's session (`e9715a81`). The last is a receipt replay at 14:38:44.084, 86 s before the unmount, as the d4 gate's load on this mount ended (d4 ran 14:13:50-14:38:42). No shard 1460 line comes from posix-durability's own writes (14:40:05-14:40:10). So the reap owed was most likely for this client's own earlier operation on shard 1460. The server does not log appends per shard, so this is inferred, not shown.
- **Was GC behind (question 3):** no, not for packing. The d4 pack backlog peaked at 205,743 at about 14:23:05 (155 s into the sampler), the load ended at about 14:30:29, and the backlog reached 0 at about 14:31:38 and stayed 0 to the last sample (14:32:16, sample clock anchored on the samples file's write time). The revoke asked for the reap at 14:40:10, 8.5 min after the backlog was gone. The reap queue on group 2's leader (above) is a separate queue and was not empty.

**Rate (coordinator's ruling):** `rdma-4x1b` (the queued rerun of the affected gates, posix-durability included) counts as run 1. Two more posix-durability clean stops and three runs of `tests/live/e2_peer_stop_loop.py` follow on the same RDMA cell, each keeping the client log. Results (16:37-17:11):

| Run | Window | Clean stops | Result |
| --- | --- | --- | --- |
| `rdma-4x1b` (run 1) | 16:37-16:54 | posix-durability and every later unmount | **PASS**, 11/11 gates (identity, topology, posix-durability, e1, e2, b6, c5 44/44, d1, d3, e4). Every session revoke at unmount rc=0, cleanup 0-205 ms |
| `rdma-4x1-pd2` | 16:54-16:55 | posix-durability | **PASS**; revokes rc=0, cleanup up to 57 ms |
| `rdma-4x1-pd3` | 16:55 | posix-durability | **PASS**; revokes rc=0, cleanup up to 357 ms. 221 `txn-finish ... (coordinator lost its group)` lines in its logs |
| `rdma-k42-e2` loop 1 | 16:55-16:58 | 20 peer stops | **20/20 PASS**; no `coordinator lost its group` line |
| `rdma-k42-e2` loop 2 | 16:58-17:01 | 20 peer stops | **20/20 PASS**; 579 `coordinator lost its group` lines (from 16:58:30.5) |
| `rdma-k42-e2` loop 3 | 17:01-17:11 | 20 peer stops | **0/20 FAIL.** Every stop refused for the whole cleanup budget and failed closed (`rc=-13`, `cleanup_ms` 20,063-20,102, `cleaned=1`, `touched=2`), all on `shard=3751`: **16 × `reason=uncertified-busy`, 4 × `reason=append-open-by-other-session`**. 2,585 `coordinator lost its group` lines from 17:01:31.48, 2 s after the cluster formed (`coord=3751` 1,329, `coord=1` 1,256); group 0 and group 2 each kept one term-1 leader until shutdown |

**K42 rate on RDMA:** clean-stop cells 3 of 3 PASS; peer-stop loops 2 of 3 PASS (40 of 60 stops). With `rdma-4x1`, clean stops failed in 2 of the 9 RDMA runs that ran them (the three full cells, the three rate cells and the three loops: `rdma-4x1` and loop 3 failed); both runs show `txn-finish ... abort decided (coordinator lost its group)` lines, and so does one passing run (`rdma-4x1-pd3`, 221 lines), so these lines do not by themselves cause a refusal. Not diagnosed here; K42 is the coordinator's row (reopened in devel). The 20/20 e2 loop inside `rdma-h04` is not counted: it did not keep its logs. Kept logs: `~/efs-artifacts/taskEY-20261109/rdma-k42-e2/keep-e2-peer-stop-loop-keep-{1,2,3}/` (loop 3: `efs-e2-peer-uzxx9lq7/c/`, members `n1-n5.log`, clients `peer001-020.log`, `primary.log`).

### K22 fixture fix (test only)

`tests/test_k22_snapshot_import.py`: each 4 MiB writer now keeps a ring of its last `K22_BIG_RING` files (default 32). Before writing file n, it unlinks file n-32 and drops it from the readback manifest, so the live data stays at 2 × 32 × 4 MiB however long the stall lasts. `K22_QUOTA` (default `1G`, the old value) sets the member quota, so a small quota can check the ring in a short run. The deadline and the quota are unchanged. Check run `tcp-k22-ring`: quota 256M, ring 8, the production snapshot window and a 3,000 s stall. Without the ring, a 256M quota fills at about a quarter of the old 5,618 s, about 1,400 s. Result (12:35-13:26 UTC, `tests/test_k22_snapshot_import_ring.py`, a copy of the fixed test placed beside the original in the candidate tree, sha256 `29e87cbf...4e44b3`, same as the committed file): **the ring holds.** The run reached the 3,000 s stall deadline (exit 1, expected: with the production window the log needs hours for four windows), with one leader snapshot past node 3 per group (group 0 at 1,804 s, 5.39M entries; group 2 at 1,865 s, 5.35M). No `ENOSPC` and no quota backpressure line in the whole run. The log kept growing at a steady 1,570-2,120 entries/s per 150 s sample from 1,804 s to the deadline (group 0: 5.39M at 1,804 s, 7.55M at 3,029 s), past the about 1,400 s mark where the old fixture fills a 256M quota. That is about the old run's pre-quota rate between its first and second windows (5.5M in about 3,000 s), and it does not collapse. The writers logged 8 errors, all `EIO` on close (7 in the first 500 s, 1 between 2,000 and 2,500 s), the same class as the ten early H15 `EIO`s recorded under K106 above. At about 1,750 entries/s, four windows past node 3 take about 3.5 h, inside the 8 h deadline, but that is only an estimate until the owed H15 rerun.

### K110 capture (H1 kill modes)

**K110 (number assigned at merge).** The coordinator adds the row at merge and gives it the next free number; the checklist on this branch is not edited. Wording for the two tables:

- Register (Item | Kind | Disposition): "Task EY (batch D H1, candidate `9bb50fe79`, Nov 10): in `tests/live/append_failover.py --failover kill` (3 loopback members, 4 writers on one shared file, the current leader SIGKILLed every ~12 s and restarted after 5 s), each writer's single final fsync returned `EIO` in 3 of 4 kill-mode runs (sigkill-8k 2 of 2, combined-2k 1 of 2). Every acknowledged record read back exact on the warm primary, the warm peer and a cold mount. The fsync fell inside the last kill's 5 s down window. Each refused stripe had one fragment proven, one pending on the down member, and the third `L` (`COMMIT` from another incarnation of a restarted member) or `M`: `1 of 3 durable; need 2 (a member of another incarnation lost a fragment)`. Two members were never down at once. A fourth run with every member back (final fsync 6.5-6.9 s after the last restart) failed the same way, and fsyncs 6.9 s and 24.7 s after that restart still returned `EIO` on the same 227 stripes (one `D`, one `M`, one `L` each); the client re-sent none of them (evidence: this page, `2026-11/batch-d-distinct-host-20261109.md` from the checklist)" | "durability proof / fsync availability, alpha blocker" | "OPEN, blocks alpha: yes: found Nov 10 by Task EY, not diagnosed; owed: an fsync after single-member outages (one at a time, each restored) must not fail for stripes whose bytes are intact, or the contract must say otherwise; the H1 kill modes rerun on the fix (TCP and RDMA)".
- Status: "OPEN, blocks alpha: yes (Nov 10, Task EY): H1 kill-mode final fsync `EIO` (3 of 4 runs, and 1 of 1 in the `--post-fsync` probe with every member back), persisting on later fsyncs with no re-PUT; data exact; not diagnosed, no fix (evidence: this page, `2026-11/batch-d-distinct-host-20261109.md` from the checklist)".

**What makes an `L` fragment provable again.** It never becomes durable again on its own:

- **The fold:** `stripe_prove()` in `src/client/data_commit.c` folds each member's COMMIT verdict into a fragment that is still `P`, and turns it `D` only when the COMMIT came from the incarnation that acknowledged it (`p->epoch[i] == jobs[s].epoch`); otherwise it turns it `L`. Because the fold reads only `P` fragments, an `L` fragment stays `L` under every later COMMIT.
- **Back to `P`:** the only way is a new acknowledgement of the same object. `stripe_note_locked()` (called through `efs_client_stripe_note()` from `src/client/write.c`) resets a `P`, `M` or `L` fragment to `P` when the object's fragment is PUT again.
- **Proof without it:** a stripe is also proven with two fragments `D` plus a degraded mark covering the third, so a stripe with one `L` needs both other fragments `D`.
- **What follows:** the member that was down at the final fsync was itself SIGKILLed. Unless the client re-PUTs, its `P` fragment should turn `L` too once it returns, and every later fsync of those stripes should keep failing.
- **Checked, `tcp-h01-k110` (12:33-12:35, one sigkill-8k run with `--post-fsync`, same command as H1 plus that flag):** the unproven stripes never became provable, and the client re-sent nothing.
  - **Kill schedule:** SIGKILL then restart 5 s later: n1 12:33:17.7 (back 12:33:22.7), n2 about 12:33:29.9 (back 34.9), n3 about 12:33:42.2 (back 47.2), n1 about 12:33:54.5 (back 59.5), n2 12:34:06.8 (back 12:34:11.8). Restart times come from each daemon's `node N: store` line. Never two members down at once.
  - **Final fsyncs:** the four writers' final fsyncs (12:34:18.31-18.69) came 6.5-6.9 s after the last killed member (n2) was back, so in this run every member was up. All four returned `EIO` in 0.03-0.05 s. Integrity PASS: 8,000/8,000 records exact on the warm primary, the warm peer and a cold mount.
  - **After-recovery fsyncs:** `after-failover` at 12:34:18.70 (6.9 s after n2's restart) returned `EIO` (errno 5, 0.022 s). `after-warm-reads` at 12:34:36.47 (24.7 s after) returned `EIO` (errno 5, 0.017 s).
  - **Unproven stripes:** every one of the six fsyncs logged the same 227 stripes without proof: 8 shown plus "219 more", chunks 175-179 of the shared file. Each shown stripe has one `D`, one `M` and one `L` fragment (for example chunk 178, nodes 3,1,2, `DML`): node 3 `D`, node 1 `M` (never acknowledged), node 2 `L`. The other 62 stripes of the first fsync passed with two `D` and a degraded mark (`DKD`, `KDD`, `DDK`).
  - **Re-send:** the client log's last `put_fragments` is chunk 225 at 12:34:12.47, before the final fsyncs. No PUT of chunks 175-179, or of any chunk, follows the first `EIO`. The client unmounted cleanly (`put tickets outstanding at unmount: 0`, session revoked).
  - **Against the prediction above:** node 2's fragment, acknowledged before its 12:34:06.8 kill, is `L` as predicted. The third fragment is `M`, not `L`: node 1's fragment was never acknowledged and carries no degraded mark. So the stripe has one durable fragment, and neither a COMMIT nor time makes it provable. `stripe_close_for_absence()` did not retire these stripes in 24.7 s. Not diagnosed further here, on the coordinator's ruling. The K110 fix is Task FH's.

The capture covers the three failed runs. The code was not diagnosed in this task. Each run is 3 loopback members on fcstor012, 4 writers appending 4 KiB records to one shared file through one mount, and kill-mode failover. The test SIGKILLs the current leader of group 0 or group 2 every ~12.3 s and restarts it after 5 s (`--interval 4 --first-event 3 --down 5`). Each writer fsyncs once, after its last record.

| Run | Kills (member@UTC) | Member down at the final fsync | Final fsyncs (all `EIO`) | Refused stripes (chunk, fragment states in node order) |
| --- | --- | --- | --- | --- |
| sigkill-8k-1 | n1@03:08:37.5, n3@49.8, n2@09:02.0, n1@14.3, n3@26.6, n2@38.9 | n2: killed 03:09:38.9, back 03:09:43.8 | 03:09:42.777-43.102, 4 writers | 202-206: `DPL`, `LDP`, `PLD` |
| sigkill-8k-2 | n1@03:10:46.6, n3@58.9, n2@11:11.2, n1@23.4, n3@35.7, n1@48.0 | n1: killed 03:11:48.0, back 03:11:53.0 | 03:11:52.356-52.809, 4 writers | 201-205: `DMP`, `MPD`, `PDM` |
| combined-2k-1 | n1@03:12:57.3, n3@13:09.6 | n3: killed 03:13:09.6, back 03:13:14.6 | 03:13:11.028-11.097, 4 writers (the peer writer's fsync 0) | 22, 24-28: `DPL`, `LDP`, `PLD` |

Fragment states (`frag_letter()` in `src/client/data_commit.c`):

- `D`: durable.
- `P`: acknowledged but not proven. In every refused stripe this is the member that was down at that moment.
- `L`: that member's COMMIT came from another incarnation: the fragment was written to a member that has since been SIGKILLed and restarted.
- `M`: missing.

- **Two members were never down at the same time.** The down windows are 5 s long and 12.3 s apart. In all three runs the final fsync fell inside the last kill's down window, 1-4 s before that member came back.
- **What refused the fsync:** every refused stripe held exactly one proven fragment. A second one was on the member that was down, and the third was `L` (a restarted member's earlier incarnation) or `M`. The client asked for two (`1 of 3 durable; need 2 (a member of another incarnation lost a fragment)`, `need 2` for the `M` stripes), refused 4-6 stripes by name, and logged "319 more stripes without proof" (run 1). Over the same window, stripes with two fragments proven passed as degraded (`2 of 3 durable; marked degraded for rebuild on return (rc=0)`), with the matching member lines `raft-host: degraded mark ... COMMIT did not prove it durable; rebuild on return`.
- **The data:** every acknowledged record read back exact in all three views: the warm primary, the warm peer, and a cold mount after every daemon and client restarted. So the bytes of the refused stripes survived; it was their durability proof that was refused.
- **Re-sending:** after the `EIO` the client logged no further `put_fragments` for a refused chunk. Mount A then unmounted cleanly at 03:10:11 (run 1: `put tickets outstanding at unmount: 0`, session revoked rc 0). The logs do not show whether the client still held the unsynced bodies.
- **A second fsync:** the logs have none after the member returned. One sigkill-8k rerun with an observation-only `--post-fsync` (`a058b4987`: one fsync of the shared file after the last failover and one after the warm reads, recorded in `report.json`, outside the verdict) is queued as `tcp-h01-k110`.

Raw logs: `~/efs-artifacts/taskEY-20261109/tcp-h01/{sigkill-8k-1,sigkill-8k-2,combined-2k-1}.tmplogs/run/logs/` (`fuse-mnt.log`, `efsd-n{1,2,3}.log`, `report.json`).

## Results: RDMA

Transport check: in every RDMA cell, every daemon and client log has `transport rdma: rdma on mlx5_2 (TCP carries setup)`, and no daemon log has a TCP transport line (`rdma-3x1`: 18 daemon and 10 client lines). For the own runs, see the note above their table: some harnesses delete their logs on success, so their transport is set, not shown.

### Cells

| Run | Window | Gates | Result |
| --- | --- | --- | --- |
| `rdma-3x1` | 13:26-14:12 | full list less `d5-expiry` | 16 PASS (identity, topology, b-checks, posix-core/extras/2client/durability, e1, e2, b6 (22 writes marked degraded, W154 cleared in 9.7 s), e3, a13, c5 33/33, d1, d3 (22,376 queries, p99 0.071 s), e4 15,000 + 6 × 256 MiB). d4 FAIL on server RSS only: **NOTRUN (harness, server memory)**; backlog peak 154,835, within the bound (600 s load, 1,006 creates/s, 745 packs/s, drained in 62.7 s) |
| `rdma-4x1` | 14:12-14:44 | full list less `d5-expiry` | **rdma-4x1 FAIL: K42 recurred (clean stop refused for the whole 20 s cleanup budget, `reason=append-reap-owed`, shard 1460, healthy 4-member RDMA cluster; TCP 4-member passed).** The coordinator reopens K42 (VERIFY to OPEN, blocks alpha: yes) in devel; this branch does not edit the row. Diagnosis and rate below. 7 PASS (identity, topology, b-checks, posix-core, posix-extras, posix-2client, e3, a13). **posix-durability FAIL on a refused clean client stop (K42 family):** the fcstor010 client's unmount at 14:40:10 drained (`put tickets outstanding at unmount: 0`), then its session revoke spent the whole 20 s cleanup budget refused and failed closed: `session revoked at unmount: epoch=1 touched=1225 rc=-13 ms=20372 fence_ms=27 cleanup_ms=20337 cleaned=824 ... phase=cleanup shard=1460 reason=append-reap-owed`. `efs-fuse` exited 1 (`unmount did not complete cleanly`) on a healthy 4-member cluster with every member up. `append-reap-owed` is one of the certified refusal reasons the K42 fix added. K42 is VERIFY with its live multi-host gates outstanding, and the TCP 4-member cell passed this gate. Every later gate that used that mount met an absent mount: e1 and e2 NOTRUN (the harness saw the mount directory in use and left it alone), and b6, c5, d1, d3 and e4 FAIL with `ENOENT` under `/tmp/efs-taskEY/cluster/mnt`. That is a cascade, not separate results. d4 **FAIL (backlog peak 205,743 against the 200,000 bound) plus NOTRUN (harness, server memory)**, recorded as measured; the bound is an open decision of the user's (599 s load, 1,116 creates/s, 759 packs/s, drained in 69.5 s). Hosts clean in the postflight. Not diagnosed here. The affected gates rerun as `rdma-4x1b`, before `rdma-h01-kill` |
| `rdma-5x1` | 14:44-15:32 | full list less `d5-expiry` | 16 PASS (identity, topology, b-checks, posix-core/extras/2client/durability, e1, e2, b6 (6 writes marked degraded, W154 cleared in 10.3 s), e3, a13, c5 55/55 (clean unmount revoked its session in 0.518 s), d1, d3 (20,919 queries, p99 0.081 s), e4 25,000 + 10 × 256 MiB). d4 **FAIL (backlog peak 254,954 against the 200,000 bound) plus NOTRUN (harness, server memory)**, recorded as measured; the bound is an open decision of the user's (599 s load, 1,103 creates/s, 649 packs/s, drained in 89.9 s). 13 RDMA transport lines, no TCP daemon log; hosts clean in the postflight |

### Own runs

`run_h.sh` runs each command with `EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2`. The "RDMA lines" column counts the logs a run left in its temporary directory that say `transport rdma`. It is 0 where the harness deletes its logs on success, so for those rows the transport is set, not shown.

| Run | Window | Commands (reps) | Result |
| --- | --- | --- | --- |
| H1 `rdma-h01` (K15) | 15:32-15:42 | w52-20k (2), stepdown-8k (2), lostreply-2k (2) | **6/6 PASS**, 6 RDMA log lines each |
| H2 `rdma-h02` (K14, K13) | 15:42-15:46 | k14 torn read | **PASS** (5 RDMA lines) |
| H4 `rdma-h04` (K33, K35, K41, K54, E2) | 15:46-16:16 | k33 payload (20), e2 peer-stop loop (20 runs), k35, k33 growth (10), k41 (10), k54 member-down unmount (20 runs, alternate) | **all PASS**: 20/20, 20/20, 1/1, 10/10, 10/10, 20/20 (RDMA lines: none kept) |
| H5 `rdma-h05` (K36, K37) | 16:16-16:21 | k36 recovery (3), k36 segment recovery (3), k37 segment rebuild (3) | **9/9 PASS** (RDMA lines: none kept) |
| H6 `rdma-h06` (GC) | 16:21-16:23 | gc reclamation `--posix`, put-barrier fault | **2/2 PASS** (6 RDMA lines each) |
| H7 `rdma-h07` (K38 shared chunk, Task DW) | 16:23-16:28 | DW campaign, rdma arm, 10 runs, members fcstor012-014 | **10/10 PASS**: no `EIO` or torn read, K94 0, pressure 16-24; every member logs RDMA up (95-101 lines) |
| H8 `rdma-h08` (AO) | 16:28-16:29 | ao readdir `--posix` | **PASS** (10 RDMA lines) |
| H9 `rdma-h09` | 16:29 | h09 extend-EOF, h09 exclusive create, 3-member RDMA cluster | **PASS** (5/5 gates) |
| H12 `rdma-h12` (RDMA only) | 16:29-16:30 | h12 posix-all a and b, 3-member RDMA cluster | **PASS** (5/5 gates) |
| H13 `rdma-h13` (pump) | 16:30-16:34 | pump file (3), pump blocked (3) | **6/6 PASS** (8 RDMA lines each) |
| H14 `rdma-h14` (K5) | 16:34-16:37 | k05 session fence gate (2), ported script `9aaef59f4`, `K05_ADDR=172.16.223.66` | **2/2 PASS** (RDMA lines: none kept) |
| H1 kill modes `rdma-h01-kill` (transport comparison) | 17:11-17:16 | sigkill-8k (2), combined-2k (2) | sigkill-8k **0/2: FAIL on the final fsync only** (`final_fsync_zero` false, integrity PASS, final fsync at most 0.084 s), the same signature as TCP's (K110, number assigned at merge). combined-2k **2/2 PASS**. 6 RDMA lines each |

## Limitations

- **Single-host own runs.** `run_h.sh` runs each harness as a loopback private cluster on fcstor012. Only the cells, H7 (members on fcstor012-014), H9 and H12 (3-member cluster descriptions) used distinct member hosts. The other own-run results are single-host evidence on these hosts, not the distinct-host shape the matrix asks for.
- **RDMA transport not shown for H4, H5 and H14.** `EFS_TRANSPORT=rdma` and `EFS_RDMA_DEV=mlx5_2` are set for every command, and the daemons read them from the environment (`src/common/rdma.c`). These harnesses delete their logs on success, though, so no kept log shows the transport.
- **Not run on RDMA:** H3 (W130), H15, `k13-tickets`, `fragment-integrity`, `test_k43_coordinator_kill.py`, the dir-ops scripts (busy, core), `test_ao_readdir_live.py`, `test_pump_logging.py` and the D4 kit (obs 0 and 1). They ran on TCP only. They were left out when the RDMA queue was built, and no reason was recorded, so they are NOTRUN and owed on RDMA. The RDMA H1 kill modes ran last, for the transport comparison only.
- **H10** had no own run. The cells' `a13-node-addition` covers a clean addition (3 to 4 to 5 members with the seed down); no loaded variant exists, and cross-subnet cannot be covered on these hosts (one subnet).
- **H15 owed:** the snapshot import was never reached (fixture); the rerun waits for the snapshot-import memory fix, on the ring fixture.
- **d4 RSS** is NOTRUN (harness) in every cell (see Follow-ups). The 200,000 backlog bound is the user's open decision; the peaks are recorded as measured.
- **e2 peer-stop loop** (`tests/live/e2_peer_stop_loop.py`) runs its cluster on loopback on fcstor012 only.
- **This candidate is no longer the release candidate.** The cells and own runs touched by its fixes (K38 wave 70, K42, K110) rerun on the fixed candidate.

## K-row consequences

Every row the matrix lists for batch D got a dated clause in both checklist tables naming what these runs proved and what is still owed. No status changed on this branch: no row's whole stated acceptance was met by these runs (the own runs are single-host, and the RDMA columns are incomplete), so there are no proposed VERIFIED rows. Rows without a clause: K42 (the coordinator's row, reopened in devel; the rate above goes to the coordinator), K46 and K47 (no gate named for them ran separately), and the K110 row (added at merge).

## Matrix

A batch D note under the matrix's batch table records the hosts, the single-host own runs, what was not run on RDMA, and H15.

## Follow-ups

- **D4 create-rate limiter:** member CPU is ruled out (5-member cell above). Measure the client side next: efs-fuse CPU and its busiest threads on the d4 host, and create latency per minute from `scripts/d4_progress.py`'s foreground samples.

- **d4-progress cannot collect member RSS on these hosts: NOTRUN (harness) in every cell.** `scripts/d4_progress.py` runs on the mount host and samples each member's RSS with its own `ssh HOST ps ...` (`server_rss()` in the sampler class, called from the per-sample loop); fcstor hosts cannot ssh to each other, so every sample is `None` and the gate's RSS bound fails. The controller reaches every member through the description's `ssh` wrapper, but the daemons report no RSS over `efs-mgmt`, and the gate needs the local mount, so routing the samples through the controller is not a small change (a member-side sampler started by the driver, or an RSS field in `efs-mgmt status`). The gate's other checks (foreground deadline, KV bounds, pack backlog, index age, drain) are reported for the record but do not make the gate PASS.

## Reproduction

Prerequisites: the five fcstor hosts, a login node that can reach them (the controller cannot run on the fcstor hosts), the candidate built as in Deploy, and the patched libfuse. The controller sits in `~/work/efs-taskEY-login` on the login node; its scripts are committed here.

- **Cells:** `scripts/qualify.py run --cluster <description> --gates <list> --allow restart,turnover,remount --out ~/efs-artifacts/taskEY-20261109/<run>` (as in Method), run through `run_cell.sh`. Gate lists: `scripts/evidence/task-ey/controller/gates-tcp.txt` and the queue file.
- **Queue:** `scripts/evidence/task-ey/controller/run_queue.sh` in a `screen` session on the login node, reading `scripts/evidence/task-ey/controller/queue.txt` (one line per run, in run order). Each line runs either `run_cell.sh` with a description and gates, or an `sh:` script. It does a preflight and postflight on all five hosts, and stops on a dirty host or a missing master connection.
- **Own runs:** `scripts/evidence/task-ey/controller/run_h.sh NAME HOST TRANSPORT CMDFILE`. Each command file line is `label<TAB>reps<TAB>command`; the run writes `results.tsv` and one log per rep. H7 uses `scripts/evidence/task-ey/controller/run_dw.sh` (Task DW's `scripts/evidence/dw/k38_dw_campaign.py`).
- **Teardown and cleanup:** `scripts/evidence/task-ey/controller/teardown.sh` (by pid, never `pkill`) and, at the end, `scripts/evidence/task-ey/controller/final_cleanup.sh`.
- **Analysis:** `scripts/evidence/task-ey/d4_backlog_table.py GATE_DIR...` (D4 backlog table; `--self-test`).

Committed files:

- `scripts/evidence/task-ey/d4_backlog_table.py`
- `scripts/evidence/task-ey/dw_ssh.sh`
- `scripts/evidence/task-ey/fcstor-3x1-rdma.json`
- `scripts/evidence/task-ey/fcstor-3x1-tcp.json`
- `scripts/evidence/task-ey/fcstor-4x1-rdma.json`
- `scripts/evidence/task-ey/fcstor-4x1-tcp.json`
- `scripts/evidence/task-ey/fcstor-5x1-rdma.json`
- `scripts/evidence/task-ey/fcstor-5x1-tcp.json`
- `scripts/evidence/task-ey/task_ey_ssh.sh`
- `scripts/evidence/task-ey/controller/d4_cpu_sidecar.sh`
- `scripts/evidence/task-ey/controller/final_cleanup.sh`
- `scripts/evidence/task-ey/controller/gates-tcp.txt`
- `scripts/evidence/task-ey/controller/h/rdma-h01-kill.cmds`
- `scripts/evidence/task-ey/controller/h/rdma-h01.cmds`
- `scripts/evidence/task-ey/controller/h/rdma-h02.cmds`
- `scripts/evidence/task-ey/controller/h/rdma-h04.cmds`
- `scripts/evidence/task-ey/controller/h/rdma-h05.cmds`
- `scripts/evidence/task-ey/controller/h/rdma-h06.cmds`
- `scripts/evidence/task-ey/controller/h/rdma-h08.cmds`
- `scripts/evidence/task-ey/controller/h/rdma-h13.cmds`
- `scripts/evidence/task-ey/controller/h/rdma-h14.cmds`
- `scripts/evidence/task-ey/controller/h/rdma-k42-e2.cmds`
- `scripts/evidence/task-ey/controller/h/tcp-h01-k110.cmds`
- `scripts/evidence/task-ey/controller/h/tcp-h01.cmds`
- `scripts/evidence/task-ey/controller/h/tcp-h02.cmds`
- `scripts/evidence/task-ey/controller/h/tcp-h03.cmds`
- `scripts/evidence/task-ey/controller/h/tcp-h04-k41.cmds`
- `scripts/evidence/task-ey/controller/h/tcp-h04.cmds`
- `scripts/evidence/task-ey/controller/h/tcp-h05.cmds`
- `scripts/evidence/task-ey/controller/h/tcp-h06.cmds`
- `scripts/evidence/task-ey/controller/h/tcp-h08.cmds`
- `scripts/evidence/task-ey/controller/h/tcp-h13.cmds`
- `scripts/evidence/task-ey/controller/h/tcp-h14.cmds`
- `scripts/evidence/task-ey/controller/h/tcp-h14b.cmds`
- `scripts/evidence/task-ey/controller/h/tcp-h15.cmds`
- `scripts/evidence/task-ey/controller/h/tcp-k22-ring.cmds`
- `scripts/evidence/task-ey/controller/queue.txt`
- `scripts/evidence/task-ey/controller/run_cell.sh`
- `scripts/evidence/task-ey/controller/run_dw.sh`
- `scripts/evidence/task-ey/controller/run_h.sh`
- `scripts/evidence/task-ey/controller/run_queue.sh`
- `scripts/evidence/task-ey/controller/task-ey/h09-rdma.json`
- `scripts/evidence/task-ey/controller/task-ey/h09-tcp.json`
- `scripts/evidence/task-ey/controller/task-ey/h12-rdma.json`
- `scripts/evidence/task-ey/controller/teardown.sh`

Raw output: `~/efs-artifacts/taskEY-20261109/<run>/` on the ORCD shared home, each directory with `SHA256SUMS`. `PRUNED.txt` at its top lists the copied Raft WAL files removed on Nov 10, with their sha256. Two are kept for the open K42 investigation: `rdma-k42-e2/keep-e2-peer-stop-loop-keep-3/efs-e2-peer-uzxx9lq7/c/n{4,5}/mdraft/log/raft.log`. The end-of-campaign cleanup record is `ops/final-cleanup.txt`.
