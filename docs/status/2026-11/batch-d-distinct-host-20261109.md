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
- **Controller on the ORCD login node, using the Mac's forwarded ssh agent:** the fcstor hosts cannot ssh to each other and the login node holds no usable key for them, so the driver and the run queue sit in `screen` sessions on the login node (login007). Their ssh to fcstor010-014 authenticates through the agent that the Mac forwards over its one multiplexed ssh connection to orcd-login. The agent socket exists only on the login node, under the per-connection `/tmp/ssh-*` directory sshd creates, for as long as that Mac connection lives; it disappears when the connection closes. No key was created or copied. No key material and no agent socket path is written into any artifact or log (checked with `grep -rlE` over `~/efs-artifacts/taskEY-20261109` and the controller directory). The controller directory `~/work/efs-taskEY-login` is removed at the end. If the Mac's connection is replaced (other sessions share the same multiplexed connection), it can land on another login node and the old node's agent socket disappears. That happened twice: at 23:08 (3-member TCP follow-up run) and 01:46 UTC (4-member TCP cell, e4-scale), the screens on login007 lost the agent. Their ssh failed on every host at once (`kex_exchange_identification: Connection closed by remote host` or `Permission denied (publickey)`), and the driver's own teardown could not run; both clusters were torn down by hand (`manual-teardown/` in each run directory). From 01:50 UTC the wrapper keeps one persistent master connection per host (ControlPersist 12 h, sockets in a mode-700 directory in the controller's local `/tmp`), so an authenticated master outlives the agent. The queue runs on login006 from then on. The control sockets and the controller directory are removed at the end.
- **Harness fixes made during the run** (scripts only; product binaries unchanged): the ssh wrapper drops the two `cargo/env: No such file or directory` lines the account's `~/.bashrc` prints on every ssh; d4-progress runs on fcstor010 (the driver host has no mount); 16G member quota so the E4 fixture fits; `payload()` in `scripts/qualify.py` and `scripts/stage2_helpers.py` generates in 32 MiB pieces (`6ed972e3a`; Python 3.9 `randbytes` overflows at 256 MiB). The tree's patched scripts: `qualify.py` sha256 `cfd8c17097d6cbd523c5e76edb0329da9f99f6bd0f5ba3713d5257113e1ff068`, `stage2_helpers.py` `58f335dbc8ad95e7bf1860cebeeb4736c6fa94c20625327d1a24bd6c47237e52`.

## Candidate status

`9bb50fe79` is no longer the release candidate: K38 recurred on orcd-mini, K102 is new and K32 may reopen (batch O, Task EW). Every PASS here is "on candidate `9bb50fe79`". K rows (the coordinator's rule, Nov 10): a row whose whole stated acceptance these runs meet becomes VERIFIED "on candidate `9bb50fe79`"; a row with acceptance still owed (another batch, or a part these runs did not cover) keeps its status and gains a dated clause naming what batch D proved; a product failure gets a new row in K110-K119. The cells and own runs that the candidate's fixes touch rerun on the fixed candidate.

## Results: TCP

(filled in as the runs finish)

## Results: RDMA

(not started)

## Limitations

(filled in at the end)

## Follow-ups

- **d4-progress cannot collect member RSS on these hosts: NOTRUN (harness) in every cell.** `scripts/d4_progress.py` runs on the mount host and samples each member's RSS with its own `ssh HOST ps ...` (`server_rss()` in the sampler class, called from the per-sample loop); fcstor hosts cannot ssh to each other, so every sample is `None` and the gate's RSS bound fails. The controller reaches every member through the description's `ssh` wrapper, but the daemons report no RSS over `efs-mgmt`, and the gate needs the local mount, so routing the samples through the controller is not a small change (a member-side sampler started by the driver, or an RSS field in `efs-mgmt status`). The gate's other checks (foreground deadline, KV bounds, pack backlog, index age, drain) are reported for the record but do not make the gate PASS.

## Reproduction

(filled in at the end)
