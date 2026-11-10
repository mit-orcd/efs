# orcd-mini batch O on the Stage 1 candidate (Task EW)

Task EW, Nov 9–10 2026. Purpose: batch O of the
[Stage 1 acceptance matrix](../stage1-acceptance-matrix-20261030.md) (runs O1 to
O9) on orcd-mini, the user's live RDMA cluster, on the Stage 1 candidate devel
`9bb50fe79` (wave 67). Rows: K38, K40, K41 (O1), K32 (O2), K17 (O3), K1 (O4),
K54 (O5), K8 (O6), K49, K51 (O8), K26 (O9) of the
[checklist](../alpha-1.0-2plus1-checklist.md). The user authorized the deploy
and the restarts on Nov 9, and the force-discard of the stuck node9900 mount
("Yes, discard", relayed by the coordinator).

**Status (Nov 10):** merged into devel in wave 70. At the merge K26 and K51 became VERIFIED on `9bb50fe79` (O9 and O8 here plus F3 and F2 of [batch F](batch-f-candidate-20261109.md)); the K38 fix ([Task FB](k38-recurrence-20261110.md)) merged in the same wave; the K102 fix (Task FC) is on a branch, not merged.

**Verdict: FAIL for O1; K32 reopened (OPEN); new OPEN row K102.**

* **O1 FAIL.** `ior hard` failed 2 of 8 runs with `write()` `EIO` and the K38
  signature (`rc=-14`, `why=prev-compacted`). The two-client shared-chunk case
  failed 2 of 4 runs with `EIO`, then passed 4 of 4 on a fresh mount.
* **New row K102 (OPEN, blocks alpha: yes).** After a failed `ior hard` run,
  the node9900 client kept retrying the deleted file's writes
  (`dcache-flush-fail ino=6980 stage=put rc=-3`). Its clean stop was refused,
  and the refusal printed `retained-releases=0 wb-pending=0`.
* **PASS:** O5 (K54, 6 of 6), O6 (K8, 9 of 9), O8 (TSan 31/31, ASan/UBSan
  59/59) and O9 (Memcheck 41/41).
* **O3 (K17):** every outcome passes (`errors=0` on both clients, cross-check
  rc 0, no stall), but n2 logged 41 converging `pub-stale why=authority`
  refusals. K17's acceptance says "no `pub-stale` refusal", so that one
  criterion is not met.
* **K32 reopened (OPEN, blocks alpha: yes).** The A/B is NOTRUN, but the
  candidate's `ior easy` write is far below the ~1.2 GiB/s gate: 358–398 MiB/s
  from the node9901 client (the original measurement's client, on a quiet
  cluster) and 567–607 MiB/s from node9900.
* **O4 NOTRUN:** it needs the unapplied devops `stop.sh` patch (k01b).

K38 stays OPEN. K8 is VERIFIED on candidate `9bb50fe79` (O6). The other rows
keep their status and gain a dated clause.

## Results

| Run | Rows | Command | Times (UTC) | Result | Acceptance met? |
| --- | --- | --- | --- | --- | --- |
| Deploy | - | below | Nov 9 22:15–22:21 | PASS: 4 up, Protection full, 4/4 `9bb50fe7932d`, both mounts serve `ls` | - |
| O1 `ior hard` | K38, K40, K41 | `orcd_mini_o1_windows.sh hard 4 …/o1`, then again into `…/o1b` | 22:22–22:30 | **FAIL 2 of 8** (`o1b` reps 1 and 2, rc 134) | no |
| O1 two-client shared chunk | K38, K40 | `orcd_mini_o1_windows.sh shared 4 …/o1-shared` | 22:30–22:34 | **FAIL 2 of 4** (reps 3 and 4: 4 node9900 writers `EIO`) | no |
| O1 shared chunk, fresh mount (classification) | K40 | `orcd_mini_o1_windows.sh shared 4 …/o1-shared-fresh` | Nov 10 00:28–00:32 | 4 of 4 exact through both mounts | does not change O1 |
| O2 `ior easy` reference | K32 | `orcd_mini_o1_windows.sh easy 3 …/o2` | 00:32–00:35 | rc 0 ×3; write 607 / 597 / 567 MiB/s, read 7,240 / 7,429 / 7,401 MiB/s (node9900 client) | A/B NOTRUN; gate not met |
| O2 `ior easy`, node9901 client | K32 | `orcd_mini_easy_node9901.sh …/o2-node9901` | 02:16–02:22 | rc 0 ×3; write 393 / 398 / 358 MiB/s, read 7,049 / 6,214 / 6,063 MiB/s | **no**: below ~1.2 GiB/s, K32 reopened |
| O3 C1 | K17 | `orcd_mini_c1.sh 3600 /tmp/ew-o3-wait-a /tmp/ew-o3-wait-b` | 00:38–01:42 | outcomes PASS (A `errors=0 mismatch=0 verified_bad=0`, B verdict PASS, cross-check rc 0); 41 `pub-stale why=authority` on n2 | **no**: one criterion ("no `pub-stale` refusal") not met |
| O4 | K1 | - | - | NOTRUN: needs the k01b devops `stop.sh` patch | no |
| O5 K54 stage | K54 | `orcd_mini_k54_stage.py --transport rdma --runs 6` | 00:35–00:38 | **PASS 6 of 6** clean unmounts, exact read-back | orcd-mini half yes; distinct-host H4 owed |
| O6 W148 modify | K8 | 3 streams × 3 `w125_modify_repro.sh` on private clusters | Nov 9 22:40–23:17 | **PASS 9 of 9** | yes |
| O8 sanitizers | K49, K51 | `orcd_mini_o8_o9.sh` on node9900 | 22:42–23:05 | **PASS**: TSan 31/31 (610 s), ASan/UBSan 59/59 (371 s) | candidate gates yes; per-entry RDMA use not captured |
| O9 Memcheck | K26 | same script, `make leak-check` | 22:59–23:05 | **PASS 41/41** (311 s) | ORCD half yes; F3 owed |

## Deploy

| | node9901 (daemons n1–n4, client `/data1/efs/mnt`) | node9900 (client `/data1/efs/orcd-mini-mnt`) |
| --- | --- | --- |
| Before | 4 efsd and client on `909a07d717dc`, `fuser -m` kernel only, no `tiny_files`/`ior`/`efs-bench` process | client pid 3806523 on `909a07d717dc`, `fuser -m` kernel only |
| Release | `/data1/efs/releases/9bb50fe7932d` | `/data1/efs/orcd-mini-client/releases/9bb50fe7932d` |
| Build | `git archive 9bb50fe79`, `make clean`, `make -j32 efsd efs-fuse efs-mgmt efs-bench`, rc 0, 10 s | same, rc 0, 10 s |
| Identity | `EFS_GIT_ID=9bb50fe7932d`, `v0.3.2-pre-alpha-2887-g9bb50fe79`, branch `devel`, built 2026-11-09T22:18:29Z | same |
| efsd | `e19123974a3134cfcaa1471706bf37566ad34576920f311f7aa04af130ad34b9` | `0e114c1f6d743df45a1ca8acfed6d075fbf052798aef614108d05784a26b38e1` |
| efs-fuse | `fb0c24ec5b79c1a9b1ffa2f06276a0e7b10f019d3d0ad14257d9555d06c4a53e` | `f50aaad3307628120be964f12880997e68e981bb89b9eed58fb691c595ddd65c` |
| efs-mgmt | `ed80e724bcac7d90753080b2eb2f8e4590835710282b87906117e5915362ccae` | `b9121e82b5569da82785574b3e5abc402187db7e9a95c6e36c63e288df8fc099` |
| efs-bench | `2ae9f3c33a1f1a27cc60e09d90ce2537b434c79e7c87605b548641ab9d55c6d8` | `4957b88a57171c22a97567524e4c5b27f90b536c43b0210192a4c330d3a96370` |

The node builds differ in bytes because each compiles with `-march=native`.
The source and the build id are the same. `/data1/efs/devops` on node9901
matched `~/git/devops/nuc-efs` at `8bc872e` in every common file: nothing was
pushed. Quota check: the start log says `explicit 720.00 GiB (safe maximum
1.94 TiB; reserve 20%)`, and `/data1` is 4 % used on both nodes. There is no
store-format change between `909a07d71` and `9bb50fe79` (no version or magic
constant in `include/` or `src/` changed), so the stores were kept. The
cluster held 0 B of data before.

Sequence:

1. `client.sh stop` on node9900 (rc 0), then on node9901 (rc 0).
2. n4 stopped by hand with `server_processes.py stop-file`: orderly, rc 0. The
   devops orcd-mini env names only n1–n3.
3. `EFS_CLUSTER=orcd-mini ./stop.sh`: three orderly rc 0. `pgrep -af
   '^/data1/efs/src/efsd '` was then empty.
4. Both `src` symlinks switched.
5. `EFS_START_TIMEOUT=900 ./start.sh`: 11 s, mounted `/data1/efs/mnt`.
6. n4 started by hand with the command `start.sh` uses (`EFS_TRANSPORT=rdma
   EFS_RDMA_DEV=mlx5_0 efsd --join … --listen 10.1.99.1:17435 /data1/efs/n4
   --quota 720G`) and recorded.
7. `client.sh start` on node9900.

After the deploy:

* Group 0: members n1–n3 at term 3.
* Group 2: members n2–n4 at term 3, commit 1614412 on all three.
* n2 leads both groups.
* n1 still hosts the retired group-2 replica it already had before
  (`role=FOLLOWER term=2 commit=6 joint=1 retired=1`). This is unchanged and
  not attributed to the candidate.
* Every daemon runs `/data1/efs/releases/9bb50fe7932d/efsd` with
  `EFS_TRANSPORT=rdma`.
* Before the deploy, `Abandoned uploads` was `pending=812`.

## Environment

* node9901: four efsd, 10.1.99.1:17432–17435, stores `/data1/efs/n1`–`n4`
  (XFS on xiRAID), quota 720 GiB each, RDMA `mlx5_0` (InfiniBand, LID 765).
* node9900: the RDMA client. Both nodes run RHEL 9 with 192 CPUs and the
  private libfuse `/data1/efs/deps/libfuse`.
* Fragment I/O on the daemons is always `O_DIRECT` (D42; efsd logs
  `direct_io=on`). `ior` runs buffered through FUSE: no `-B`, the same command
  as Oct 22 and Oct 25.
* `ior` 8 local ranks on node9900, OpenMPI 5.0.10rc2,
  `~/orcd/scratch/efs-io500/io500/bin/ior`.

## O1: ior hard and the two-client shared chunk (K38, K40, K41)

Method: [`scripts/evidence/orcd-mini/orcd_mini_o1_windows.sh`](../../../scripts/evidence/orcd-mini/orcd_mini_o1_windows.sh),
a new Mac-side driver. Per rep it records the byte offset of both clients' logs
and of the four efsd logs. It then runs one rep detached in `screen` on the
node:

* **`hard`:** `orcd_mini_batch_o_ior.sh hard 1`, that is `ior -a POSIX -e -w
  -r -t 47008 -b 47008 -s 2000`, one shared file, 717 MiB.
* **`shared`:** `orcd_mini_shared_chunk.py write` with 8 writers and 2,000
  records. node9901 runs ranks 0–3 through `/data1/efs/mnt` while node9900
  runs ranks 4–7 through `/data1/efs/orcd-mini-mnt`, both at once. Then
  `verify --ranks 0-7` runs through both mounts.

After each rep the driver cuts every log window, so that it holds the bytes
written during the rep.

The acceptance (matrix O1 and the K38 row) requires all of:

* every run rc 0;
* no `EIO`;
* no `report-split stale` or `rc=-14` in either client's log;
* verify exact.

### ior hard, 8 runs

| Run | rc | Seconds | Write MiB/s | Read MiB/s | node9900 client lines `rc=-N` | efsd matching lines |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| o1 rep 1 | 0 | 19 | 50.40 | 185.22 | 0 | 0 |
| o1 rep 2 | 0 | 22 | 40.87 | 161.37 | 0 | 0 |
| o1 rep 3 | 0 | 17 | 55.15 | 199.98 | 0 | 0 |
| o1 rep 4 | 0 | 20 | 45.32 | 166.07 | 0 | 0 |
| o1b rep 1 | **134** | 91 | - | - | 55 | 213 |
| o1b rep 2 | **134** | 69 | - | - | 59 | 243 |
| o1b rep 3 | 0 | 18 | 52.32 | 194.47 | 119 (all `ino=6980 rc=-3`) | 0 |
| o1b rep 4 | 0 | 21 | 43.06 | 163.69 | 135 (all `ino=6980 rc=-3`) | 0 |

Reference numbers:

* Oct 22 `d94ca85bf`, node9901 client: write 47.9 MiB/s, read 240 MiB/s.
* Oct 25 `5a7463440`, node9900 client: write 16.4 MiB/s, read 220 MiB/s.
* Oct 25, node9901 reruns: write 19.8–21.3 MiB/s.

The candidate's passing runs write at 41–55 MiB/s, the Oct 22 level.

Failure signature, in `o1b rep 1` (ino 4174) and `o1b rep 2` (ino 6980) alike:

* **ior:** `WARNING: write(15, …, 47008) failed Input/output error`, then
  `MPI_ABORT`.
* **node9900 client:** a run of
  `report-sync-fail ino=4174 stage=report … resolved_stale=0 rc=-14` and
  `background-pressure ino=4174 rc=-14 retained` lines, from 22:25:44 on.
  Rep 2 has the same lines for ino 6980 from 22:27:42, plus one `rc=-1`.
* **Daemons:** `report-split nrec=1648 … stale=0 … rc=-14 fail=wait/-14 ino=4174`
  and `report-resolve id=… ino=4174`. Each failed run has 30–32 `pub-stale`
  refusals with `why=prev-compacted`.
* **node9901 client:** no matching line in any run.

Before the second batch the cluster carried the first four runs' 2.8 GiB, by
then deleted. `Abandoned uploads pending` reached 2,746 after O1. This is the
K38 failure (the REPORT answered STALE and the client turned it into `EIO`) on
the candidate. The trigger seen here is `why=prev-compacted`.

### Two-client shared chunk, 4 + 4 runs

| Run | Write node9901 (ranks 0–3) | Write node9900 (ranks 4–7) | Verify both mounts | Seconds |
| --- | --- | --- | --- | ---: |
| 1 | 0 errors | 0 errors | 0 / 0 errors | 51 |
| 2 | 0 errors | 0 errors | 0 / 0 errors | 52 |
| 3 | 0 errors | **4 errors**: `[Errno 5] Input/output error` on ranks 4–7 | **5,287 / 5,287** records wrong | 33 |
| 4 | 0 errors | **4 errors**: `[Errno 5]` on ranks 4–7 | **5,171 / 5,171** | 46 |
| fresh 1–4 | 0 errors | 0 errors | 0 / 0 errors | 51–55 |

In runs 3 and 4 every node9900 writer failed within about a second of the
start. No line in the node9900 client log names the shared file, and the
daemon windows show only routine lines. Both mounts read identical bytes. The
first ten wrong records listed (all of ranks 6 and 7) are zeros, which fits
records that a failed writer never wrote. The two failed files were deleted
by this version of the driver, so the full per-record breakdown is lost. That
only unwritten records are wrong is therefore inferred from ten lines. The
driver now keeps a failed run's file.

Runs 3 and 4 ran on the node9900 client that was already stuck on the deleted
ino 6980 (K102, below). Runs 1 and 2 passed on the same stuck client, so the
stuck state alone does not explain the failures; the cause is unproven. On a
fresh mount (after the force-discard, Nov 10 00:28) the case passed 4 of 4.
K40's acceptance is not met: the O1 runs include an `EIO` on two clients
sharing chunks.

## K102: client stuck on a deleted file; clean stop refused

What happened, in order:

1. The second failed `ior hard` run (ino 6980) ended at 22:28:49, and the
   driver removed its file.
2. From then on the node9900 `efs-fuse` (pid 3417610) logged, every few
   milliseconds:

   ```
   dcache-flush-fail ino=6980 stage=put rc=-3
   report-sync-fail ino=0 stage=held-unit-flush total_ms=8 … rc=-3
   background-pressure ino=6980 rc=-3 retained
   ```

   `rc=-3` is `EFS_ERR_NOT_FOUND`. There were 445 `background-pressure`
   lines by 22:38.
3. At 22:38 a plain `client.sh stop` waited 60 s, then printed
   `efs-fuse: stop control refused or unavailable (1); mount retained` and
   exited 1. The client log said `stop-refused: unresolved writes or lease
   releases remain; mount retained (retained-releases=0 wb-pending=0)`: the
   counts it prints do not cover the state that blocks it.
4. The user approved a force-discard. At 22:52:33
   `client.sh stop --force-discard /data1/efs/orcd-mini-mnt` returned rc 1
   after 59.7 s with `client stop … failed: data rc=-3 metadata rc=-3 after
   60s`. The log shows `session revoked at unmount: … rc=0 ms=46` and
   `unmount not clean: flush rc=-3 settle rc=0 revoke rc=0`, and efs-fuse
   exited 1. The mount was then gone and the process ended.
5. The remount at 00:27 served at once.

For K54 the relevant point is this: once a failed PUT is held for a deleted
inode, neither a clean stop nor a discard stop ends cleanly, although the
session revoke succeeds. A small write and fsync (4 MiB) still worked on the
stuck client. RSS was 552 MB, with 133 threads.

Classification: product, client write-back. The retained state of a failed
REPORT/PUT outlives the inode's removal, retries forever on NOT_FOUND, and
blocks the clean stop. The stop's refusal message does not count it.

Not diagnosed and not fixed here. Captures:

* the full client log up to the refusal and up to the discard;
* `/proc/<pid>/status`;
* the last 2,000 log lines;
* the four efsd logs 22:26–22:29Z.

They are under `o1-poisoned-client/`, with their own `SHA256SUMS`, sha256
`226529f152eaf06f1f7c18fa790466b047e0599353e7ed7bec1ee8e1ea4034ae`.

## O2: ior easy reference (K32)

The A/B against `08cc16af4` is **NOTRUN**: that baseline predates the W153
store format and cannot run on today's stores (no backward compatibility).

The candidate ran `ior -a POSIX -F -e -w -r -t 1m -b 2048m`, 8 ranks on the
node9900 client, 16 GiB per run, 3 runs, all rc 0:

| Run | Write MiB/s | Read MiB/s |
| --- | ---: | ---: |
| 1 | 606.74 | 7,240 |
| 2 | 596.53 | 7,429 |
| 3 | 566.60 | 7,401 |

Reference points:

* Oct 22 node9901 client: `08cc16af4` 1,329 MiB/s; `d94ca85bf` 710–863 MiB/s.
* Oct 25 `5a7463440`: 301–305 MiB/s.
* Nov 2 `909a07d71`: 616–698 MiB/s.

The coordinator asked for a rerun from the node9901 client, the client of
the Oct 22 and Oct 25 measurements, to remove the client-host caveat. The
script was
[`orcd_mini_easy_node9901.sh`](../../../scripts/evidence/orcd-mini/orcd_mini_easy_node9901.sh),
with OpenMPI 4.1.7rc1 and the same `ior` binary and command. It first waited
until each of n1–n3 used under 0.2 core over two consecutive 60 s samples,
which took 2 min: every daemon was at 0.01 core, and the host's load of
about 2.2 is not EFS. It recorded `efs-mgmt status` before and after (4 up,
0 degraded, `Abandoned uploads pending=639` both times) and then ran 3
times:

| Run | Write MiB/s | Read MiB/s |
| --- | ---: | ---: |
| 1 | 393.39 | 7,049 |
| 2 | 398.07 | 6,214 |
| 3 | 358.42 | 6,063 |

K32's gate is `ior easy` write of about 1.2 GiB/s with the K13/K14 gates on,
and it is not met. This is the third measurement below it, after Oct 25
(301–305 MiB/s) and Nov 2 (616–698 MiB/s). **K32 is reopened: OPEN, blocks
alpha: yes.** The cause is not attributed. The Stage 3 theories page on devel
(`docs/status/2026-11/stage3-server-protocol-theories-20261109.md`, P3-G and
P3-J) is the pointer.

## O3: C1 STALE-replay convergence (K17)

[`orcd_mini_c1.sh`](../../../scripts/evidence/orcd-mini/orcd_mini_c1.sh)
`3600 /tmp/ew-o3-wait-a /tmp/ew-o3-wait-b`, tag `c1-20261110T0036`. It was
armed during O5 and released once O5 had finished, by writing `RC=0` into
both WAIT files at 00:38:43.

* **Client A, node9901:** 4 writers, 4 readers, 2 deleters and 4 modifiers
  for 3,600 s, with `pack-compact` every 60 s.
* **Client B, node9900:** fresh mount, 4 readers and 2 deleters.

The acceptance ([K17 definition](../2026-10/acceptance-definitions-20261031.md#k17))
has four criteria:

* no `publish-stalled` or `flush-meta` line in either client's output;
* `errors=0` on both clients;
* no `pub-stale` refusal;
* `crosscheck.txt` ends `rc=0`.

| Check | Result |
| --- | --- |
| A final | `live=12694 verified_bad=0 missing=6410 mismatch=0 errors=0 deleted=150872 unresolved=0 unresolved_reads=0`, `verdict=PASS`, `RC=0` |
| B final | `classified base=0 attempted=0 intermediate=0 foreign=0 missing=0 unread=0`, `verdict=PASS`, `RC=0`, handed over |
| Cross-check | `A_missing=6410 B_deleted=144593 missing_not_deleted_by_B=0 B_deleted_still_present=0`, `crosscheck rc=0` |
| `publish-stalled` / `flush-meta` / `pub-stale` in the client log windows (node9901, node9900) | 0 / 0 |
| `pub-stale` in the daemon logs, 00:38:46–01:41:55 | n1 0, **n2 41**, n3 0, n4 0 |

What the decision needs:

* **Pattern.** All 41 lines read `raft-host: pub-stale why=authority ino=…
  ci=… rec_base=ffffffffffffffff span=0+0 flags=261 epoch=0 want=N have=1`.
  The `want` values: `want=1` 17, `want=2` 21, `want=3` 2, `want=4` 1. Every
  line has `epoch=0`. Chunk indices: `ci=0` 26, `ci=8` 8, `ci=16` 4, `ci=24`, `ci=2` and `ci=1` one each.
* **Distribution.** 11 lines in 00:38–00:59 and 30 in 01:00–01:42, spread one
  or two a minute over the hour. They came from 40 inodes: 39 refused once
  and one refused twice.
* **Producer.** Client A (node9901) produced all of them: each inode appears
  in A's log window and in none of B's. B (readers and deleters only)
  publishes nothing. About 3.2 s after each refusal (00:39:12.774 to
  00:39:15.947, for example), A logs `efs: report rec ino=… ci=…
  rejected unowned staging observation nodes=… (putid miss, n=…)`. That is
  `report_fill_owned` in `src/client/write.c`: the rebuilt REPORT had no
  putid or dcache object for the chunk, so it refuses to treat the fetched
  row as its own PUT and retries without identity. The line is rate-limited
  to one a second; its counter reached `n=577` over the hour. A's workload
  logs paths, not inodes, so whether a modifier, a writer or the
  `pack-compact` loop raced each publish is not attributed.
* **Convergence.** No inode shows a `publish-stalled`, `report-stuck` or
  `flush-meta` line, a client error or a read mismatch afterwards. Each
  refused publish was retried and committed. The time to the final commit is
  not logged; the refusal-to-retry step took about 3.2 s.
* **Outcome criteria held:** `errors=0` on both clients, cross-check rc 0, no
  `publish-stalled` or `flush-meta`. The original non-converging failure looked different: 676 refusals
on two inodes, each with the captured epoch one behind the inode (`epoch=2
want=3`), and stalled rows.

So O3's outcomes match the Nov 2 rehearsal, which recorded no `pub-stale`
count. The literal criterion "no `pub-stale` refusal" is **not met**.
Classification: not a product failure observed (no stall, no error, no
mismatch). Whether converging `why=authority` refusals on epoch-0 records
breach K17's acceptance is a decision for the coordinator. K17 keeps VERIFY.
The lines are in `o3/efsd-n2-pub-stale.txt`.

## O5: K54 real-host stage

[`orcd_mini_k54_stage.py`](../../../scripts/evidence/orcd-mini/orcd_mini_k54_stage.py)
`--transport rdma --runs 6` ran from the Mac against the cluster running
`EFS_TRANSPORT=rdma`, on the fresh node9900 mount. The mount had 445
`background-pressure` lines in its log since its start, all of them from the
previous process, and none new.

| Run | Down member | Signal | Unmount | Seconds | Revoke `ms` / `fence_ms` / `cleanup_ms` | Back in sync | Files differing |
| --- | --- | --- | --- | ---: | --- | --- | ---: |
| 1 | n2 (leader of both groups) | SIGKILL | clean, rc 0 | 2.8 | 41 / 17 / 5 | yes | 0 |
| 2 | n3 | SIGTERM | clean | 2.2 | 332 / 105 / 102 | yes | 0 |
| 3 | n4 | SIGKILL | clean | 2.4 | 168 / 55 / 106 | yes | 0 |
| 4 | n2 | SIGTERM | clean | 2.6 | 317 / 4 / 305 | yes | 0 |
| 5 | n3 | SIGKILL | clean | 2.4 | 165 / 51 / 105 | yes | 0 |
| 6 | n4 | SIGTERM | clean | 2.2 | 284 / 54 / 104 | yes | 0 |

* All 6 runs printed `session revoked at unmount … rc=0` with no `phase=`.
* After each run the cluster was 4 up, Protection full, 0 degraded
  segments.
* Each member got both signals.
* After O5 all four daemons ran `9bb50fe7932d`.

The K54 orcd-mini stage is met on the candidate. The distinct-host rotation
(matrix H4) is owed by another batch.

## O6: W148 modify under load (K8)

[`scripts/evidence/w125/w125_modify_repro.sh`](../../../scripts/evidence/w125/w125_modify_repro.sh)
ran on node9901 from an overlay release, `/tmp/ew-o6-rel`, now deleted. The
overlay held:

* the candidate's four binaries, as symlinks;
* `scripts/` from `git archive 18438c2ec`.

That commit fixes
[`w125_p0_cluster.sh`](../../../scripts/evidence/w125/w125_p0_cluster.sh): it
still started efsd with the pre-K33 `--node-id --addr --port --storage
--direct-io` command line, which efsd refuses, so O6 could not start a
cluster. It now uses `efsd ADDR:PORT STORE` / `--join … --listen …` with
`--discovery-port 0`.

The run used three parallel streams of three attempts. Each attempt was a
private 3-member RDMA cluster (ports 17741–17763, stores
`/tmp/w125-modify-<port>-<stamp>`, quota 200G) and one client running
`w125_concurrent.py` for 600 s: 4 writers, 4 verifying readers, 2 deleters,
4 modifiers, packer minimum age 3,000 ms.

| Attempt | Seconds | Written | Modified | `mismatch` | `errors` | `unresolved` | `verified_bad` | RDMA up lines (client) | Stop |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| s0-a1 | 730 | 40,278 | 95,611 | 0 | 0 | 0 | 0 | 194 | clean, store deleted |
| s0-a2 | 722 | 37,230 | 91,555 | 0 | 0 | 0 | 0 | 180 | clean |
| s0-a3 | 720 | 37,046 | 89,679 | 0 | 0 | 0 | 0 | 204 | clean |
| s1-a1 | 726 | 39,349 | 95,747 | 0 | 0 | 0 | 0 | 200 | clean |
| s1-a2 | 725 | 38,327 | 92,455 | 0 | 0 | 0 | 0 | 168 | clean |
| s1-a3 | 724 | 37,681 | 92,464 | 0 | 0 | 0 | 0 | 196 | clean |
| s2-a1 | 733 | 40,841 | 99,305 | 0 | 0 | 0 | 0 | 183 | clean |
| s2-a2 | 724 | 38,057 | 95,298 | 0 | 0 | 0 | 0 | 182 | clean |
| s2-a3 | 724 | 37,981 | 91,346 | 0 | 0 | 0 | 0 | 230 | clean |

Every attempt ended `verdict=PASS`, `PASS stop=clean store deleted`. That
meets K8's owed acceptance: the retained fixed-build W148 rerun, at least 9
attempts on node9901 RDMA, bytes compared exactly. The original
reproduction was 3 of 9 on `7c510c60f`.

## O8 and O9: sanitizer and leak gates (K49, K51, K26)

[`orcd_mini_o8_o9.sh`](../../../scripts/evidence/orcd-mini/orcd_mini_o8_o9.sh)
ran on node9900. It now takes the release's build identity from the
environment and the shared-home Valgrind 3.28.0 (`$HOME/.local/valgrind-3.28.0`,
[BU](../2026-10/orcd-memcheck-20261031.md)). It copied the release to
`/tmp/bq-o8-<pid>`, deleted at the end, and ran with:

* `EFS_RDMA_DEV=mlx5_0`, `EFS_TRANSPORT=auto` (connections upgrade to RDMA
  when the device answers);
* `ASAN_OPTIONS=detect_leaks=0` (the ORCD host limit);
* the private libfuse.

The test clusters bind ephemeral ports (`k33_daemons.free_port`), never the
live 17432–17435.

| Gate | Result | Seconds | Notes |
| --- | --- | ---: | --- |
| `make tsan-check` | VERDICT: PASS (31 run, 0 failed) | 610 | no NOTRUN; Nov 2 rehearsal had 2 timing FAILs (19/21), none here |
| `make asan-check` | VERDICT: PASS (59 run, 0 failed) | 371 | `rdma_handler_alignment` (K49) and `heartbeat_shutdown` (K51) PASS |
| `make leak-check` | VERDICT: PASS (41 run, 0 failed) | 311 | memcheck binaries built with `-march=x86-64-v3 -mtune=generic` |

Limitation: the gates delete their logs on PASS, so these captures do not
show which daemon-starting entries actually upgraded to RDMA on `mlx5_0`.
K49's "real HCA with sanitizers" is met for the candidate's gates on an
RDMA-equipped host; per-entry RDMA use is not proven.

## O4

NOTRUN. It needs the unapplied devops `stop.sh` patch (k01b, in the
coordinator's notes folder as `k01b-devops-nuc-efs-stop-r4.patch`), which
this task may not apply. **Request for the user:** apply it in
`~/git/devops` (`git apply -p1`), push it to node9901 with `ecopy-push`, then
O4 can run.

## Procedure faults found and fixed (scripts only)

* `w125_p0_cluster.sh` used the pre-K33/D42 efsd CLI (fixed in `18438c2ec`).
* `orcd_mini_c1.sh` read `w125_concurrent.py` from its own folder; the
  script triage had moved it to `scripts/evidence/w125/`.
* `orcd_mini_o8_o9.sh` hard-coded the `909a07d717dc` build id and the
  retained `devel-linux-28a3` Valgrind.

The last two are fixed in `0f349947c`, which also adds the O1 driver.

The O1 driver's per-rep count line uses the pattern `EIO|report-split
stale|rc=-14|publish-stalled|flush-meta`. It scored the two failing shared
runs 0, because their client lines carry `rc=-3`. The tables above come from
a broader recount (`rc=-N`, `failed=body`, `background-pressure` by inode).

## Reproduction

Prerequisites:

* the user's ssh control socket to `orcd-login`;
* the candidate deployed as above;
* the cluster on `EFS_TRANSPORT=rdma`;
* both mounts up.

From a Mac checkout of this branch:

```sh
S=scripts/evidence/orcd-mini
A=/home/erbmi1/efs-artifacts/<task>
bash $S/orcd_mini_o1_windows.sh hard 4 $A/o1        # O1, repeat into a second OUTDIR
bash $S/orcd_mini_o1_windows.sh shared 4 $A/o1-shared
bash $S/orcd_mini_o1_windows.sh easy 3 $A/o2        # O2, node9900 client
# O2 from the node9901 client, ON node9901 (detached): waits for quiet daemons
bash $S/orcd_mini_easy_node9901.sh $A/o2-node9901
python3 $S/orcd_mini_k54_stage.py --transport rdma --out k54-rdma.tsv --runs 6   # O5
bash $S/orcd_mini_c1.sh 3600 WAIT_A WAIT_B          # O3; write RC=0 into both WAIT files to start
# O6 on node9901: REL = candidate binaries + scripts/ of this branch; per stream PORT 17740/17750/17760
scripts/evidence/w125/w125_modify_repro.sh REL OUT PORT
# O8/O9 on node9900 (detached):
EFS_GIT_ID=9bb50fe7932d EFS_GIT_DESCRIBE=v0.3.2-pre-alpha-2887-g9bb50fe79 \
  bash $S/orcd_mini_o8_o9.sh /data1/efs/orcd-mini-client/releases/9bb50fe7932d OUTDIR
```

Durations as measured:

* O1: about 12 min.
* O2: 3 min on node9900, 6 min on node9901 (2 of them the quiet wait).
* O3: 62 min.
* O5: 3 min.
* O6: 37 min with three parallel streams.
* O8/O9: 23 min.
* Deploy: 6 min.

Cleanup:

* `edelete --force --delete` for trees, `rm -f` for files;
* the private `/tmp/ew-*` and `/tmp/bq-o8-*` trees;
* `/tmp/ew-o3-wait-*`;
* the C1 trees under `/data1/efs/prof/c1-<tag>`.

## Raw evidence

The raw output is outside Git, in `~erbmi1/efs-artifacts/taskEW-20261109/` on
the NFS home that both nodes share: `build-node990{0,1}`, `deploy`, `o1`,
`o1b`, `o1-shared`, `o1-shared-fresh`, `o1-poisoned-client`, `o2`,
`o2-node9901`, `o3`,
`o5`, `o6`, `o8-o9`.

`SHA256SUMS` lists 1,782 files (357 MiB in all); its own sha256 is
`508f45087ab1b39a9a3b90dab4452d6c0235d3140ae460e465a92eff224b7b2b`. The O3 node-side output also stays in node9901
`/data1/efs/prof/c1-20261110T0036` and node9900
`/data1/efs/orcd-mini-client/prof/c1-20261110T0036`.

## Limitations and remaining gates

* **K38:** still owed is an orcd-mini `ior hard` rerun with no `EIO`. The
  failure is reproducible on the candidate (2 of 8). The user's acceptance of
  the Nov 7 distinct-host result is not affected.
* **K40:** O1 is not met. The fresh-mount 4 of 4 is a classification run,
  not acceptance.
* **K41:** this batch stopped no member during packing. O1 exercises only the
  ior hard gate, which failed.
* **K54:** distinct-host H4 is owed.
* **K49, K51, K26:** per-entry RDMA use is not captured. K51's F2 and K26's
  F3 are owed by batch F.
* **K17, question for the user:** is a `pub-stale` refusal that converges
  (the authority check refusing a stale image, then a retry that commits) the
  protocol working, or the defect? The definition as written treats any
  refusal as a failure, so K17 stays VERIFY until the user rules.
* **K102:** OPEN and not diagnosed. Without the discard, the stuck mount would
  have blocked every later node9900 run.
* **O4:** blocked on the devops patch.
* **`9bb50fe79` is no longer the release candidate.** K38, K102 and K32 are
  OPEN on it. So batch O reruns O1 on the fixed
  candidate, and batch F re-checks the VERIFIED rows there.
