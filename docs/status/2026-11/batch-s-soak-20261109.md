# Stage 1 batch S: four-host soak and K22 follower plateau (Task EZ)

**Verdict (Nov 10, on candidate `9bb50fe79`): S1 PASS. S2: the plateau
PASSED, and two new product failures were found (K120 and K121, both FAIL).**

- **S1:** four hours, four production members on four hosts, TCP, the E4/D4
  workload and the K43 kill schedule.
  - 27 member kills: 23 clean stops (SIGTERM) and 4 SIGKILLs, every SIGKILL
    of the group-0 leader. Also 4 client crash-restarts.
  - All 126,297 acknowledged versions of 512 files, and all 14,034 appends,
    read back exact on both mounts. That held at all 15 periodic verifies,
    the final verify and after the remount.
  - No writer saw `EBUSY`, `EIO` or any other error.
- **S2 plateau:** node 3's compactor was parked for 4 h at production caps.
  The K22 bound held for a 14,397 s plateau, and node 3's RSS stayed flat at
  0.92-0.95 GiB.
- **K120 (S2, FAIL):** after release, node 3 rejoined through snapshot
  import. The import holds each whole key diff in memory (up to 31.3M keys),
  and RSS peaked at 13.41 GiB, 105 times the 128 MiB budget.
- **K121 (S2, FAIL):** while node 3 was stalled but reachable, its
  storage-fence error failed 6,832 clean client unmounts, despite K54's
  majority rule.
- **Not seen:** no data error, and no failed mkdir or create.
- **K rows:** K43, K29 and K22 keep their status, each with a dated clause.
  K120 and K121 are drafted below for the coordinator; they are not in the
  register, because its numbering must stay contiguous.
- **Merge state:** not merged, not pushed.

- **Purpose:** run Stage 1 batch S, rows S1 and S2 of the
  [acceptance matrix](../stage1-acceptance-matrix-20261030.md), for K43, K29
  and K22 of the [checklist](../alpha-1.0-2plus1-checklist.md).
- **Candidate:** `9bb50fe79` is no longer the release candidate: K38 recurred
  on orcd-mini, K102 is new, and K32 may reopen. A pass here therefore reads
  "on candidate `9bb50fe79`".
- **Source:** devel `9bb50fe7932d2b487d0d5eed1294b003cb735497`, with no
  product change. Branch `claude/modest-mclaren-4215ec`:
  - harness `e7e336f1c`, in [`scripts/evidence/ez/`](../../../scripts/evidence/ez/);
  - this page and the ledger entry.

## Deploy

All binaries were built once, from a clean `git archive 9bb50fe79`, with
`make -j24 EFS_FUSE_PREFIX=$HOME/work/efs-taskEZ-deps/libfuse EFS_GIT_ID=9bb50fe7… EFS_GIT_DESCRIBE=9bb50fe7… EFS_GIT_BRANCH=devel [EFS_FAULTS=1] efsd efs-fuse efs-mgmt efs-bench`
(gcc 11.5.0). Every host ran the same files from the shared NFS home
(`~/work/efs-taskEZ-bin`). Each binary's `--version` reads
`9bb50fe7932d2b487d0d5eed1294b003cb735497 (branch devel, built 2026-11-09T22:19:44Z …)`.

| File | sha256 | Used by |
| --- | --- | --- |
| `efsd` | `50ee7f0c74c99707da5710d93bd66edfdfba41fa20938cc1a3a89a1f4a853c17` | S1 all members, S2 nodes 1-2 |
| `efs-fuse` | `2ab954ac705d0cedae191ef62537d821697c1fed4c56e80bb14bfbb6d4b24954` | S1 mounts |
| `efs-mgmt` | `d77dc3a4258c92b7b6b1bc9ccf57dc530689e5a357e7a83639849deea2e4bb86` | S1, S2 status |
| `efs-bench` | `13f4503b52ff85d7ec5757ab744c60795d621da844042f92f23bc2d7145c2872` | S2 load |
| `faults/efsd` (`EFS_FAULTS=1`) | `fef957f23d9e9d650aef71cf7b334a5dad38778a19bad3041400bad5552e2419` | S2 node 3 (compactor stall hook) |
| `libfuse3.so.3.16.2` (patched copy) | `5741b4e08fdcf34472a3a5ec5000a2ee22482ab91c61513734565eb53656f76e` | every S1/S2 mount |

Other fault-build hashes (not run as daemons):

- `faults/efs-fuse`: `3c79d0ce…cb27`
- `faults/efs-mgmt`: `03e5744f…b5c3`
- `faults/efs-bench`: `de96475b…c17`

All hashes and both build logs are in `~erbmi1/efs-artifacts/taskEZ-20261109/deploy/`.

**Hosts:** fcstor003, fcstor004, fcstor005 and fcstor006:

- AMD EPYC 7402P, 24 cores, 250 GiB RAM;
- Rocky 9 kernel 5.14.0-687;
- local `/tmp` on md0, about 405 GiB free;
- TCP over IPoIB, 172.16.223.57-60.

**Host deviations:** the brief named fcstor007-009 and fcstor015.

- fcstor015 refuses the erbmi1 key.
- fcstor007-009 were carrying a W123 fleet campaign.
- The parent session and the user approved fcstor003-006 instead ("use any
  other fcstor …").
- Those are the hosts of the live orcd-test stores (ports 17432-17435). That
  store was not running during either run: no efsd or efs-fuse of any other
  user appeared in any host-load sample. Its directories, the efs user and
  `/data1` were not touched.

## Harness

The fcstor hosts share an NFS home but refuse host-to-host ssh. Each host
therefore runs [`ez_agent.py`](../../../scripts/evidence/ez/ez_agent.py) in
screen:

- It takes JSON-line commands (spawn, signal, run) from
  `~/work/efs-taskEZ-ctl/HOST/cmd.jsonl`.
- It answers in `res.jsonl` and rewrites `state.json` (pid, exit status and
  RSS of each owned process) every 0.5 s.
- It only ever signals processes it spawned.

On the controller side, [`ez_remote.py`](../../../scripts/evidence/ez/ez_remote.py)
puts that surface under `tests/k33_daemons.Cluster`.

[`ez_soak.py`](../../../scripts/evidence/ez/ez_soak.py) is `scripts/soak.py`'s
endurance soak with its members on other hosts.
[`ez_k22.py`](../../../scripts/evidence/ez/ez_k22.py) is
`tests/measure/k22_follower_bound.sh`'s method, rewritten for three hosts. That
script itself uses an old efsd command line, loopback and `pkill -f`.

A separate `ez-load-HOST` screen appends the load average and every efsd and
efs-fuse process of any user to `hostload-HOST.log` every 300 s.

**Rehearsals (harness, not evidence):**

1. Rehearsal 1 was a harness bug.
2. Rehearsal 2 failed to mount: `fusermount3: mount failed: Operation not
   permitted`. The cause is in libfuse (see Deviations).
3. Rehearsal 3 was flagged by `soak.py`'s quiet-host check. It found my own
   S2 rehearsal efsd on the same host, which is correct behaviour. Rule kept:
   S1 and S2 never share a host at the same time.
4. Rehearsal 4 passed: a 1,200 s soak with the schedule scaled to 0.25 and
   9 events, including a SIGKILL of the group-0 leader that was also mount 0's
   contact node, and a client crash-restart.

An S2 rehearsal with scaled caps also passed, on fcstor007-009.

## Deviations

- **libfuse is a binary-patched copy.** The libfuse path DW used is gone on
  these hosts.
  - The private libfuse 3.16.2 came from `~/w123-deps-20261108-v2/libfuse`
    and was copied to `~/work/efs-taskEZ-deps/libfuse`.
  - Its `libfuse3.so.3.16.2` hard-codes the copy's own non-setuid
    `bin/fusermount3` as `FUSERMOUNT_DIR`, so mounts failed with EPERM.
  - In the copy, that string was overwritten with a nonexistent path of the
    same length, `/home/erbmi1/work/efs-taskEZ-deps/xxxxxxxxxxxx/fusermount3`.
    libfuse's `exec_fusermount` then falls back to the system setuid
    `/usr/bin/fusermount3` on `PATH`.
  - `bin/` was moved aside and the pkgconfig prefix rewritten.
  - Every S1 and S2 mount used this copy (sha256 above).
- **Hosts:** fcstor003-006 instead of fcstor007-009 and fcstor015 (see
  Deploy).
- **S1 members:**
  - S1 ran the production `efsd`, not the fault build.
  - It added no members during the run. The four members were formed before
    load: three seed members, then the fourth joined.
  - Each scheduled SIGKILL targets the group-0 leader at that moment. K43's
    incident was a group-0-leader coordinator.
  - Both mounts and the controller ran on fcstor003, which also held member 1.
- **S1 RSS** is reported, not gated (see below).

## S1: four-host soak (K43 schedule)

**Command** (screen `ez-s1` on fcstor003; the wrapper `run-s1.sh` sets
`TMPDIR=/tmp/efs-taskEZ/tmp`):

```
python3 scripts/evidence/ez/ez_soak.py --ctl ~/work/efs-taskEZ-ctl --work /tmp/efs-taskEZ/s1 \
  --out ~/efs-artifacts/taskEZ-20261109/s1 --bin ~/work/efs-taskEZ-bin --store-root /tmp/efs-taskEZ/stores-s1 \
  --source 9bb50fe7932d2b487d0d5eed1294b003cb735497 \
  --member fcstor003=172.16.223.57:18441 --member fcstor004=172.16.223.58:18442 \
  --member fcstor005=172.16.223.59:18443 --member fcstor006=172.16.223.60:18444 --hours 4
```

Defaults that applied:

- seed 12345, a verify every 900 s, recovery bound 300 s;
- I/O deadline 60 s, metadata deadline 10 s, churn deadline 180 s;
- 256 slots per mount, quota 8G, profile `endurance`.

Prerequisites:

- the agents running on all four hosts;
- the binaries and the source tree on the shared home.

**Times:**

- Started 2026-11-10T00:26:50Z.
- Load ran 00:28:24Z to 04:28:25Z (14,400 s).
- Final verify and the verify after the remount ended at 04:30:57Z
  (14,647 s in all).

**Workload:** two mounts on fcstor003, each with 256 slots.

- 1 B-64 MiB overwrites and truncates, each followed by fsync;
- a shared `O_APPEND` file;
- `.efs/stats` queries;
- 126,297 acknowledged fsyncs (about 530 a minute), of which 14,034 were
  append fsyncs.

**Events:** 31 churn events in all.

- 23 clean stops by SIGTERM: every 600 s, rotating through members 2, 1, 3
  and 4.
- 4 SIGKILLs, at 3,480 s, 7,080 s, 10,680 s and 14,280 s:

  | Victim | Roles when killed |
  | --- | --- |
  | node 1 | group-0 leader |
  | node 1 | group-0 leader |
  | node 3 | group-0 and group-2 leader |
  | node 2 | group-0 leader |

- 4 client crash-restarts, 60 s after each SIGKILL. In each, the crashed
  session was revoked and not resumed, in about 0.035 s.

Each kill is recorded with every member's role, read from raft-status just
before the kill.

**Recovery:**

- Churn events took 7.5-39.7 s each.
- After each SIGKILL, the cluster settled in 2.0-4.1 s.
- Two `.efs/stats` queries returned `query-not-indexed` while a member was
  down. The harness tolerates this outside verifies; no query failed.

**Result:** PASS. Every acknowledged current version and append was exact on
both mounts at every verify and after the final remount. No writer error
occurred and every recovery settled.

- **Verifies:** 15 periodic, every 900 s, plus the final verify and the
  verify after the remount. Every one covered 512 files on both mounts, and
  appends reached 14,034.
- **Writers:** `progress.log` reports `errors=0` at all 240 one-minute lines,
  and `result.json` records no writer error. No `EBUSY` or `EIO` was seen.

**RSS (reported, not gated):** the endurance plateau check measured 72
windows, each at least 360 s of uninterrupted load after a 120 s warmup; 190
more windows were too short (NOTRUN). It marks a window FAIL if RSS grows more
than 5 % over its last third.

- 60 windows passed; 12 daemon windows failed, at 5.3-14.7 %.
- The failing windows show swings of 3-6 MiB on daemons of 30-60 MiB, between
  restarts.
- Peaks show no trend across the four hours:

  | Process | Peak RSS over the run |
  | --- | --- |
  | node 1 | 30-46 MiB |
  | node 2 | 43-58 MiB |
  | node 3 | 45-60 MiB |
  | node 4 | 30-39 MiB |
  | clients | 146-150 MiB at the first window, then 217-235 MiB from the second window to the end |

- The bounded-growth reading is my judgment; the 5 % window rule itself
  FAILs 12 windows.
- RSS is not part of K43's, K29's or K22's acceptance, so it is reported
  rather than gated.

**Host load during S1:** sampled every 300 s, starting from 01:01Z. The
sampler was started 34 min into the run, so the first 34 minutes have no
host-load samples.

| Host | load1 (01:01Z / 02:46Z / 04:26Z) | max load1 | efsd %CPU (ps lifetime) | Other users' efsd/efs-fuse |
| --- | --- | ---: | --- | --- |
| fcstor003 (member 1, both mounts, controller) | 1.44 / 1.22 / 1.19 | 1.56 | 4.0 / 2.9 / 3.1 | none |
| fcstor004 | 0.13 / 0.13 / 0.09 | 0.41 | 4.5 / 4.1 / 7.2 | none |
| fcstor005 | 0.04 / 0.09 / 0.02 | 0.49 | 5.5 / 6.4 / 4.1 | none |
| fcstor006 | 0.03 / 0.06 / 0.08 | 0.25 | 1.2 / 1.7 / 1.8 | none |

The live orcd-test cluster did not run on these hosts during S1.

**Acceptance met:**

- **K43:** the row asks for a four-hour soak and multi-host and RDMA
  reruns. The four-hour multi-host TCP soak is met: 4 SIGKILLs of the group-0
  leader with writers in flight, and no `EBUSY` or `EIO`. Still owed: RDMA
  (the RDMA soak is undecided per matrix note 8; the RDMA incident chain is
  H4).
- **K29:** the row asks for multi-host and RDMA append qualification. S1
  shows append progress that never stalled (14,034 append fsyncs, at a steady
  rate through 27 kills), with no append error, over multi-host TCP. RDMA and
  a dedicated append-throughput qualification are still owed.
- **K22:** not served by S1.

**Teardown:**

- Every member and mount ended under the harness: the agents report no live
  process and `/proc/mounts` has no `efs-taskEZ` entry.
- The stores (about 0.9 GiB per host) and the work tree were removed with
  `edelete --force --delete` on all four hosts.

**Raw output:** `~erbmi1/efs-artifacts/taskEZ-20261109/s1/` (174 MiB):

- `result.json`, `timeline.jsonl`, `progress.log`, `plateau.json`,
  `rss-quiet-*.txt`;
- daemon and mount logs under `logs/`;
- `s1.console`;
- `SHA256SUMS`, sha256
  `346c880f0a3d4cb277b10a42e8888eeefd232f573c5bc395e9f7569ec670c752` before
  `s1.console` was appended.

Host-load logs are in `hostload-fcstor00{3,4,5,6}.log` in the parent
directory.

## S2: K22 follower plateau at production caps

**Command** (screen `ez-s2` on fcstor003; the wrapper `run-s2.sh` sets
`TMPDIR=/tmp/efs-taskEZ/tmp`):

```
python3 scripts/evidence/ez/ez_k22.py --ctl ~/work/efs-taskEZ-ctl --work /tmp/efs-taskEZ/s2 \
  --out ~/efs-artifacts/taskEZ-20261109/s2 --bin ~/work/efs-taskEZ-bin --store-root /tmp/efs-taskEZ/stores-s2 \
  --source 9bb50fe7932d2b487d0d5eed1294b003cb735497 \
  --member fcstor003=172.16.223.57:18451 --member fcstor004=172.16.223.58:18452 \
  --member fcstor005=172.16.223.59:18453 --stall 21600 --plateau 14400
```

Defaults that applied:

- quota 64G;
- 8 bench threads, each looping
  `efs-bench EP1 --meta --files 5000 --dirs 16 --workers 16 --phases mkdir,create --keep`
  (one mount and unmount per round);
- warm 300 s, release 300 s, drain deadline 1,800 s.

Node 3 (fcstor005) runs `faults/efsd` with `EFS_FAULT_COMPACT_STALL=1`. The
fault file is created at the start of the stall phase and removed at
release. Before the stall, leadership is moved off node 3.

**Bound check before waiting:** at 52 s, node 3 reported
`max_unapplied=524288`, `kv_mem_budget=134217728` and
`kv_l0_cap=1073741824` in both groups. These are the production figures,
identical to nodes 1 and 2.

**Times (UTC, Nov 10):**

| Event | Time |
| --- | --- |
| Formed | 06:17:55 |
| Compactor parked | 06:22:57 |
| A leader first reports node 3 busy (plateau window starts) | 06:51:25 (t=2,042 s) |
| Fault removed (release) | 10:51:27 |
| Node 3 caught up | 11:18:51 (drain 1,342 s) |

**Plateau (the K22 bound): PASS.** Over the 14,397 s plateau window, node 3
stayed at or within one batch or flush of each limit:

| Measure on node 3 | Range over the plateau | Limit |
| --- | --- | --- |
| Unapplied entries | 499,247-524,287 | 524,288 |
| Memtable bytes | 134,217,960 | budget 134,217,728 (+232 B, one entry batch) |
| L0 bytes | 1,075,364,700 | cap 1,073,741,824 (+1.55 MiB, one flush) |
| RSS | 960,400-996,192 KiB | flat: last-third growth −0.53 % over 2,839 samples |

- `kv_apply_deferred` rose to 13,634,652.
- Node 3 never led.
- The leaders' `admit_busy` stayed 0.
- The plateau held with the bound at production scale. This answers the
  "about 6 h" plateau that xorinox would have needed. At fcstor commit rates,
  the caps were reached in 29 min.

**Catch-up after release: FAIL, K120.** Node 3's RSS rose from about 0.35 GiB
to a peak of 13.41 GiB (14,063,204 KiB). That is 105 times the 128 MiB
budget and 118 times the warm peak of 119,292 KiB. It held at 12.6-12.8 GiB
for about 26 minutes, until it caught up at 11:18:51, then fell to 0.33 GiB.

- During the stall, the leaders compacted their logs past node 3's
  position. They sent it snapshots (`raft-snap: import start group=0
  incl=16239451` at 09:14:05).
- The import's first step, `efs_kv_lsm_flush`, then waited behind the parked
  compactor until release: `import flush ... ms=5844387`, 97 min. That
  overlap with the plateau is expected under the fault.
- After release, three imports each built their whole diff:
  - group 0: n=20,674,445;
  - group 2: n=31,136,855;
  - group 0 again: n=31,286,751.
- Each diff is built as one in-memory `efs_kv_item` array
  (`efs_kv_group_import_prepare`, held in `import_diff` until the pump has
  applied it; `src/server/raft_host.c` `host_snap_import_pass`). Memory
  therefore scales with the number of keys that differ, not with any budget.
- A follower that falls behind a compacted log by tens of millions of keys
  needs RAM in proportion to that lag to rejoin. K22's bound covers the
  stalled follower, not this rejoin path.
- The earlier Task B import, about 2.1 times the warm RSS, ran at a much
  smaller lag.

**Unmount fence with a stalled member: FAIL, K121.** All 17,391 bench rounds
report `fail=0`: every mkdir and create succeeded. However, 6,832 of the
rounds that ended in the stall and release phases failed their clean unmount
after 15 s:

- 4,629 failed with `rc=-6` and 2,203 with `rc=-15`, both as
  `efs: session revoked at unmount: … phase=storage-fence`.
- The leaders logged `storage-fence: member=3 floor=2 check=0 rc=-15` for
  them.
- `host_storage_fence_members` (`src/server/raft_host.c`) counts an
  unreachable member as missed, and K54 then lets a strict majority seal.
- But a member that is reachable and answers with an error code makes the
  function return that code at once. Node 3 was alive, with its apply
  stalled, so it failed every revoke even though nodes 1 and 2 (a majority)
  sealed.
- The 1,859 rounds that passed came from the warm phase and the first
  minutes of the stall.

**Side effects of K121 on the load:**

- The driver counts a round as created files only if it exits 0. The files
  counter therefore froze at 9,295,000 from about 06:38.
- The quorum commit rate:

  | Phase | g0 | g2 |
  | --- | ---: | ---: |
  | Warm | 9,347/s | 9,241/s |
  | Stall | 1,207/s | 1,213/s |
  | Release | 856/s | 869/s |
  | Drain | 18/s | 21/s (bench stopped) |

  The drop tracks the 15 s fence wait in every round of the load generator.
  It is not evidence about quorum health under a stalled follower.

**Unplanned leader change (observation, cause not established):** no member
was killed.

- At 10:08:10.037, node 2 took group-0 leadership in term 2. Node 3 granted
  the prevote (its log ended at 6,220,069, against the candidate's
  19,804,594). Node 1, the sitting leader, refused it.
- Before that, from 06:40:53, when the members took their first snapshots,
  each healthy member was refusing the other's AppendEntries with `prev=0/0
  … why=prev-compacted`:
  - node 2 refused node 1's in group 0, 408,560 times by 10:07:56;
  - node 1 refused node 2's in group 2.
  Matching indexes still advanced (`match=` kept rising in node 1's
  `AE rejected by peer=1 hint=5417359` lines), so entries were accepted.
- The prev-0 refusal is K45's compacted-prefix rule. Why the leader keeps
  sending prev-0 AppendEntries to a follower that is current, and whether
  that let node 2's election timer expire, is not established.
- This is recorded as an open observation on K22, not as a K row.

**Host load during S2** (the same `ez-load` sampler; load1 at
06:21Z / 08:51Z / 11:16Z):

| Host | load1 | max load1 |
| --- | --- | ---: |
| fcstor003 (node 1, controller, bench) | 9.76 / 2.23 / 0.62 | 9.76 |
| fcstor004 (node 2) | 1.82 / 1.89 / 0.96 | 3.88 |
| fcstor005 (node 3) | 1.66 / 0.57 / 0.70 | 1.66 |

No other user's efsd or efs-fuse appeared. Live-cluster activity did not
distort the plateau.

**Acceptance met (K22):** the row owes three things.

- **Production-scale plateau:** met, multi-host TCP, on candidate
  `9bb50fe79`.
- **Snapshot import into a stalled follower at 1 GiB lag (matrix H15):**
  this run went far past that lag, and the import's memory is unbounded
  (K120).
- **Multi-host and RDMA runs:** RDMA was not run.

K22's status is unchanged, with a dated clause.

**Teardown:**

- Every member and bench process ended under the driver; the agents report
  no live process.
- The node 1 and node 2 stores (34 GiB each) and the work tree were removed
  with `edelete --force --delete`.
- **Kept for the open K120 and K121 investigation:** node 3's store,
  `fcstor005:/tmp/efs-taskEZ/stores-s2/s2/n3` (20 GiB, with `KEPT.txt`).
  Delete it with `edelete` once the coordinator settles K120 and K121.
  The method above recreates it.

**Raw output:** `~erbmi1/efs-artifacts/taskEZ-20261109/s2/` (524 MiB):

- `summary.json`, `samples.tsv`, `progress.log`, `driver.log`;
- `load1-8.log`;
- `logs/n{1,2,3}.log`;
- `s2.console`;
- `SHA256SUMS`, sha256
  `4f773b6cc8e041b0c5df2ff9f2447eaa12184944dacc7e9a21996dad1d7c0250`.

## New K rows (drafted for the coordinator)

The register checker requires contiguous numbers. K102-K119 belong to other
branches (EW, EY), so K120 and K121 are not inserted into the register on
this branch. Their text for both tables is below. In the checklist, the evidence link becomes `2026-11/batch-s-soak-20261109.md#s2-k22-follower-plateau-at-production-caps`.

**K120:**

| ID | Item | Kind | Disposition |
| --- | --- | --- | --- |
| K120 | Task EZ batch S2 (fcstor003-005, TCP, candidate `9bb50fe79`, production caps): a follower stalled for 4 h, then released, rejoined through snapshot import. `efs_kv_group_import_prepare` builds the whole key diff as one in-memory `efs_kv_item` array, held in `import_diff` until applied (`host_snap_import_pass`, `src/server/raft_host.c`). Diffs of 20.7M, 31.1M and 31.3M keys took node 3's RSS from 0.35 GiB to a 13.41 GiB peak (105 times the 128 MiB budget), held for about 26 min until catch-up. Rejoin memory scales with the lag, outside K22's bound. | server / resource bound | OPEN, blocks alpha: yes (default; the coordinator decides); import the diff in bounded batches, or stream it, so rejoin memory stays within the KV budget; gate: the S2 method with the RSS bound checked through drain ([evidence](#s2-k22-follower-plateau-at-production-caps)) |

Recheck-table text: `OPEN, blocks alpha: yes (default); snapshot-import diff held whole in memory, 13.41 GiB peak on rejoin after a 4 h stall`, then the same evidence link

**K121:**

| ID | Item | Kind | Disposition |
| --- | --- | --- | --- |
| K121 | Task EZ batch S2: with one of three members alive but its apply stalled, 6,832 clean client unmounts failed their session revoke after 15 s (`rc=-6` 4,629 times, `rc=-15` 2,203 times, `phase=storage-fence`; leaders logged `storage-fence: member=3 floor=2 check=0 rc=-15`), although the other two members sealed. `host_storage_fence_members` tolerates an unreachable member under K54's majority rule, but returns the first error reply from a reachable one. Every mkdir and create succeeded (`fail=0` in all 17,391 rounds). | server, client / clean stop with one member slow | OPEN, blocks alpha: yes (default; the coordinator decides); count an error reply like an unreached member toward the K54 majority, or bound it, so that one slow member cannot fail a clean unmount; gate: the S2 method with clean unmounts during the stall ([evidence](#s2-k22-follower-plateau-at-production-caps)) |

Recheck-table text: `OPEN, blocks alpha: yes (default); a reachable but stalled member's fence error fails every clean unmount, despite K54's majority rule`, then the same evidence link

## Limitations and remaining gates

- **Candidate:** `9bb50fe79` is not the release candidate. Batch S reruns on
  the final candidate only if a fix touches the member-kill or follower-lag
  paths.
- **Transport:** TCP only. No RDMA soak was run.
- **What was killed:** daemons were killed by SIGTERM and SIGKILL, never by
  power loss. Clients restarted on the controller host only.
- **Other rows:** none of K43, K29 or K22 has its whole acceptance met by
  batch S, so none becomes VERIFIED.
- **S2's leader change:** the group-0 leader change at 10:08:10 has no
  established cause.
- **S2 load:** the bench load was throttled by K121's 15 s unmount waits from
  about 06:38, so S2's stall-phase commit rate says nothing about quorum
  throughput.
- **S2 RSS:** node 3's RSS peak during the stall phase (2.04 GiB) came before
  the plateau window. This page does not attribute it.

## Reproduction

1. Build as in Deploy and patch a private libfuse copy's `FUSERMOUNT_DIR` as
   described in Deviations, or use a libfuse built with
   `FUSERMOUNT_DIR=/usr/bin`.
2. On each host, start
   `screen -dmS ez-agent-HOST python3 scripts/evidence/ez/ez_agent.py CTL HOST`,
   with `CTL` on the shared home.
3. On the controller host, run the S1 or S2 command above.
4. Clean up:
   - touch `CTL/HOST/stop` to end each agent and its children;
   - if a mount is stuck, `fusermount3 -uz` it;
   - remove `/tmp/efs-taskEZ` on each host.
