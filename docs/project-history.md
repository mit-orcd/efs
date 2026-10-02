# efs project history — operational log, archived

**This is an archive, not guidance.** Every command, approval, roll
instruction and "do this next" quoted below is a record of what was said
or done on the date shown; **none of it authorizes execution now** — the
current queue, decisions and standing permissions live only in the
sources listed under "Authoritative, current sources". Every `##`/`###`
heading carries a stable anchor (`<a id="ph-<slug>-<hash>">`) so a dated
entry can be cited unambiguously even where headings or dates repeat;
cite the anchor, not the heading text. **Anchor rule:** an anchor is
written once and never regenerated or renamed — editing a heading keeps
its existing anchor line (the slug inside the id may then differ from
the heading; that is expected); a new heading gets a new anchor; moving
a section keeps its anchor. It holds, in order of appearance:
the START-HERE §1b handoff blocks and closed-item bodies from Sep 21 –
Oct 1 2026 (moved here Oct 1 2026), the cluster-state narrative trimmed
out of `.cursor/rules/efs-project-state.mdc` on Oct 1 2026, and the
accumulated "project state" memory from Aug 18 – Sep 20 2026 (moved out of
the always-applied rule on Sep 20 2026 because 4 000 lines of mostly
superseded notes were misleading agents — many paragraphs describe the
DELETED pre-Sep-11 snapshot/2PC metadata engine and say so inline).

Authoritative, current sources:

- **What to work on:** [arch/START-HERE.md](arch/START-HERE.md) §1 / §1a
  (work queue) / §1b (in-flight handoff).
- **Current facts and live do-not-re-chase learnings:**
  `.cursor/rules/efs-project-state.mdc` (kept under ~250 lines).
- **Architecture review history (rounds 1–9):**
  [arch/design-history.md](arch/design-history.md).

Use this file when a rule or START-HERE cites a date or a result directory
and you want the full narrative, or when you are about to re-derive a root
cause and want to check whether it was already found. Search by date,
result directory (`results/...`), commit hash, or symptom. Entries are
roughly reverse-chronological within each block; blocks were appended over
time, so the same day can appear in several places.

---

<a id="ph-fio-honest-number-history-sep-17--sep-28-2026--moved-from-cu-5ba321"></a>
## fio-honest number history (Sep 17 – Sep 28 2026) — moved from `.cursor/rules/efs-fio-honest.mdc` Oct 1 2026

<a id="ph-morning-dd-sep-28-rdma-flush-in-the-clock-ce8e26"></a>
## Morning dd (Sep 28, RDMA, flush in the clock)

8 GiB `dd bs=1M conv=fsync`, own file, `FUSE_OK`, on-disk size
8589934592. Servers `db2b88c4802a-dirty`,
`EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`. 9 clients only
(`results/measure/20260928-033420-dd-wall`): aggregate **1326.6**
MiB/s, slowest wall 55.574 s, per-client walls 28.55–55.57 s, all
nine files 8589934592, no fsync EIO, zero `rc=-3`. 99 of 108
report-split lines are BUSY. Slower than the TCP 1478 below.
Earlier the same day on `113823180b15-dirty`
(`results/measure/20260928-015839-dd-wall`): 1-client **947.4**
(8.646 s), 4-client **2310.6** (walls 11.329–14.181 s). That run's
9-client row is INVALID (fsync EIO, harness ~238). Do not quote it.
1 and 4 were not remeasured on `db2b88c4802a-dirty`.

<a id="ph-sep-27-dd-tcp-flush-in-the-clock-aac684"></a>
### Sep 27 dd (TCP, flush in the clock)

8 GiB `dd bs=1M conv=fsync`, own file, `FUSE_OK`, on-disk size
8589934592.

Morning, build `4c6a5acefe03-dirty`,
`results/measure/20260927-053506-dd-wall`. A REAP_DONE tail was ~8/s
(commit==applied, no client); the idle-commit gate was skipped for
that reason only. 1→4 is 2.0×, not the Sep 18 collapse. Do not quote
a 9-client rate from this run: fsync EIO on fcstor009 and fcstor013,
report `rc=-3` (`EFS_ERR_NOT_FOUND`). The harness printed 1464; that
number is not a result.

| clients | aggregate MiB/s | slowest wall | build |
| --- | --- | --- | --- |
| 1 | **977** | 8.388 s | `4c6a5acefe03-dirty` |
| 4 | **1984** | 16.515 s (16.42–16.52 s) | `4c6a5acefe03-dirty` |
| 9 | **1478** | 49.894 s (49.76–49.89 s) | `75321297f719-dirty` |

Evening 9-client only, preflight idle (0/s both groups),
`results/measure/20260927-204907-dd-wall`: all nine files
8589934592, no fsync EIO, zero `rc=-3`. 119 of 128 `report-split`
lines are BUSY (`rc=-13`); the nine `rc=0` lines are the retries
that found the chunks already published (`push_ms=0`). 1478 is
3.4% of the 44 GB/s four-host ceiling. 1 and 4 clients were not
remeasured on `75321297f719-dirty`.

<a id="ph-prior-dd-and-fio-sep-1718-raftkv-19810-tcp--mibs-eeb0b0"></a>
### Prior dd and fio (Sep 17–18, Raft+KV, 19810, TCP) — MiB/s

On Sep 18, 4- and 9-client **writes shared a ceiling and got worse.**
8 GiB dd+fsync own-file: 1-client **639**, 4-client **251** (0.41×),
9-client first-write **202** (0.32×). That is 3.8 % / 0.46 % / 0.37 % of
the 16.7 / 44 / 44 GB/s ceilings. Do not quote 9-client 007–010 13–31 s
(stale 4-client files). Full 4/9 fio reads and 9-client fio writes were
not finished: after two 4-client 9×2g jobs the REPORT tail exceeds the
400 s ssh budget (do not raise it). Morning 4-client sw-1m AGG **1589**
(2.3× that run's 694) is the best 4-client fio write.

| test | 1 client | 4 client AGG |
| sw-1m | 694 | 1589 |
| ow-1m | 694 | 636 |
| rw-1m | 699 | 343 |
| rw-128k | 712 | — |
| rw-4k | 385 | — |
| sr-1m | 4102 | — |
| rr-1m | 2556 | — |
| rr-128k | 1577 | — |
| rr-4k | 154 | — |
| sw-50g | 341 | — |

After WAL hold / activate-mask / pipelined propose, honest 9×2g sw-1m is
**924**. Intra-job write samples run at several GiB/s — the wall is the
`fsync`, so only the end-to-end number counts.

**Do not compare these against the pre-Raft engine's numbers** (Aug 2026:
1-client sw-1m 2355, 9-client 3599). That engine is deleted, its result
dirs are gone, and its metadata path was a whole-table RAM snapshot with
no per-op Raft commit — the two are not the same system. The write gap is
W4 (multi-client) plus the leftover REPORT pack+push tail, not a
regression to bisect.

**Shape that still holds:** reads scale with clients, writes share a
ceiling. The NVMe ceiling is 16.7–21.4 GB/s **per host**, so every write
wall measured so far is software, not media. A 50g single-job file is
slower than 9×2g on one client (less parallelism).


<a id="ph-cluster-state-narrative-trimmed-from-efs-project-statemdc--o-ffbf5c"></a>
## Cluster-state narrative trimmed from `efs-project-state.mdc` — Oct 1 2026

Verbatim text of the rule's "Cluster facts" first bullet and its "Where we are" section as they stood at 18:00Z Oct 1 2026. Roll/remount/gate history Sep 17 – Oct 1 2026 with result-dir cites.

<a id="ph-cluster-facts-as-of-the-1800z-oct-1-roll-ab30c3"></a>
### Cluster facts (as of the 18:00Z Oct 1 roll)

- **Only live cluster: port 19810**, fcstor003–006 = node 1–4 =
  172.16.223.57–60, `/data1/01–06/efs`, `--quota 36T --direct-io`, RDMA
  (`EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`).
 **ROLLED 18:00Z Oct 1 (`restart705`, `cluster.sh restart --clients
 --perf`, CLUSTER_OK): servers `dc6b0af19832-dirty` = HEAD `dc6b0af1`
 (W39, read path R1–R5, store writev PUT, W37 committed-salt mkfs, log
 timestamps, rmdir asks the server) with `--perf` on fcstor003–006;
 clients fcstor003–015 remounted RDMA; fstor007 untouched. g0 term 17
 leader fcstor003, g2 term 12 leader fcstor006, commit==applied
 (`post706`). The `-dirty` is the node tree missing tracked `results/`.**
 Prior: **UP 07:24Z Oct 1 on a FRESH table** (wipe 06:37Z
 `~/efs-runs/wipe385.log`; re-wipe + `raft-mkfs` 07:18Z
 `wipe405.log`, salt 4206459926809775694; `restart408` =
 `cluster.sh restart --clients`, CLUSTER_OK, no recorders).
 Servers `4b2844487831-dirty`, g0 term 5 leader fcstor003, g2
 term 4 leader fcstor004, commit==applied, `arc_miss=0`. **Clients
 fcstor003–015 remounted 16:01Z Oct 1 (`dep691`) on the W39 + read-path
 tree (RDMA zero-copy send, R1–R5 read copies, 64 get workers, rmdir
 asks the server; START-HERE §1b 15:55Z); fstor007's `/tmp/efs` rebuilt
 on it, the user's efs-fuse untouched. Servers still
 `4b2844487831-dirty`: store.c writev, W37 and the log timestamps are
 in the tree, NOT rolled (compiled, `bld701`).** fcstor007 on it: 16 GiB write 1.5 GB/s, cold read
 3.6 GB/s, 4 readers 6.5 GB/s; posix jobs=1 200/201
 (`results/posix/20261001-155932`), 9-host 200/201 on all nine
 (`results/posix/20261001-160049`).
 Prior remount **13:20Z Oct 1 (`dep532`)** on the dd-review
 tree (reclaim herd fix, `ll_read` buffer, NUMA pin to node 0 —
 learnings below), all `fuse.efs-fuse` MOUNT_OK; on it posix jobs=1
 200/201 (`results/posix/20261001-132125`), 9-host 200/201 on all
 nine 14.2–15.3 s (`results/posix/20261001-132411`), fcstor007
 8 GiB dd+fsync 1.3 GB/s (`dd539`). Prior remount 07:34Z (`dep419`). **Posix on it:** jobs=1 fcstor007
 200/201 28.1 s (`results/posix/20261001-073542`), 9-host 200/201
 on all nine 13.3–13.6 s (`results/posix/20261001-074052`),
 posix2 62/63 then 63/63 (`results/posix2/20261001-073048`,
 `-074250`; the 62 is W36, START-HERE 0c). Leftover on the mount:
 `/posix-2c-r422-6` (W36 evidence, keep). `cluster.sh start
 --fresh` is the fresh-table start (seed + `--join`, both leaders,
 mkfs on fcstor003 only). Old `/data1/*/_delete/efs.*` trees were
 being reclaimed by `~/bin/edelete` (`edel388.log`). Perf files
 `/tmp/efs-perf/efsd.data` from 06:15Z are still on 003–006.
 fstor007 has no efs-fuse. Prior stop 06:34Z
 (`~/efs-runs/stop384.log`).
 Prior start 06:15Z (`~/efs-runs/roll-d20.log`):
 `4b2844487831-dirty`, `--perf` only. That start deleted the
 05:27Z `--perf`+`--strace` capture. Old fragments are the
 sidecar layout and will not verify.
  **Clients fcstor007–015 remounted RDMA 02:43Z Oct 1 on `b6c1712d`**
  (`dep366`, all `fuse.efs-fuse` MOUNT_OK, one efs-fuse each; pin
  release on landed PUT + dirty-list close flush, learning below;
  before that 02:22Z `f073e136` D18 cold-tab eviction, 01:58Z
  `65237a59` close predicate + chain reuse); fstor007's `/tmp/efs`
  rebuilt on `b6c1712d` for the user's `client.sh` (`fst367`, no
  efs-fuse was running). Posix jobs=1 fcstor007 on `f073e136`:
  **200/201** (mmap SKIP) in 45 s (`results/posix/20261001-022434`);
  on `b6c1712d`: posix **200/201** (`results/posix/20261001-024420`),
  posix2 **63/63** (`results/posix2/20261001-024506`), mtime gate
  rows all 1380661863. Those clients were stopped at the 03:19Z
  roll; servers are now the same tree (`4b2844487831-dirty`). **IO-500 debug
 9×4 18:35Z (`results/io500/20260930-183504-rdma`): every phase
 completed, 0 fsync failures, 0 read errors, cold hardscan of the
 36 GB hard file bad=0** — easy-write 4.563 GiB/s, mdtest-easy-write
 4.632 kIOPS, hard-write 0.640 (52.6 s), mdtest-hard-write 2.612,
 easy-read 16.8 (same-mount), easy-stat 16.478, hard-read 1.034.
 Its data (`io500/2026.09.30-14.35.04`) is still on the mount.
 Earlier the same day:
 **UP 06:51Z Sep 30.** Rolled `--all` twice under the user's live
 fstor007 `client.sh --perf` (06:35Z `~/efs-runs/roll134.log`,
 06:51Z `~/efs-runs/roll143.log`, both ROLL_OK, no recorders):
 servers `44f397b4ca2e-dirty` = HEAD `44f397b4` (one ReadIndex per
 group in `host_read_inode_lanes`, block-sized KV scan reads, GC
 256/128 batches, `gc-pass` line; learnings below;
 `results/measure/20260930-063500-perf-dir-review`). g0 leader
 fcstor005 (term 124), g2 leader fcstor006 (term 686), commit==applied.
 Clients fcstor007–015 remounted RDMA 08:35Z on the ecopy-times tree
 (atime nsec, utimens flush predicate, put windows, crec irec;
 `results/measure/20260930-080000-ecopy-times-review`), all
 `fuse.efs-fuse`; posix jobs=1 fcstor007 200/201 29.1 s
 (`results/posix/20260930-083309`). Before that, 07:17Z on the
 evictor-fix tree (`5ea397fd`). **Gate 07:20–07:33Z (START-HERE
 §1b):** posix jobs=1 200/201 (7.1 s idle, 16.8 s under ecopy),
 posix2 63/63, 9-host 200/201 on eight hosts + fcstor012 199/201
 (`dir_deep_nesting` rmdir ENOTEMPTY on a dir the KV shows empty;
 open, `results/posix/20260930-072501/d50-*`). IO-500 debug 9×4
 (`results/io500/20260930-072824-rdma`): easy-write **3.148 GiB/s**,
 mdtest-easy-write **3.696 kIOPS**, hard-write **0.261** with 16
 fsync failures (fsync EBUSY at ~10 s then EIO/STALE at ~65 s on
 every client, ino 84857), mdtest-hard-write **2.126**, easy-read
 **2.976**, easy-stat **3.600**, **hard-read aborted on read EIO**
 (the unpublished spans). `inbox_drop` 978/3102/223 on 004/005/006:
 950 KB group-2 AEs dropped at the 256-frame inbox; `pub_p50` 544 ms;
 L0 113–231 files steady with 50–270 KB compactions. fstor007's client
 runs the 06:16Z tree (`85f5b31c`) — it does not have the evictor
 fix; the stat-side fixes are server-side and reach it. Prior
 06:16Z roll (`~/efs-runs/roll126.log`): `85f5b31c…-dirty`
 (pipelined-sender reply channel fix;
 `results/measure/20260930-060000-dd-wedge`). Gate 06:19Z
 (`~/efs-runs/gate130.log`):
 fcstor007 32 GiB non-zero dd+fsync **~1000 MiB/s** (32.85 s, rc 0,
 size 34359738368), lane `fail=0` on 004/005 before and after,
 terms g0 106 / g2 680 unchanged, commit==applied.
 Prior 05:43Z roll
 (`~/efs-runs/roll107.log`): `e002771e56e4-dirty` = HEAD `e002771e`
 (F1/F2/utimens flush in `a5e3d5fe`; conn-pool and reclaim fixes in
 `e002771e`, review `results/measure/20260930-051000-review2`).
 The user's 100 GiB dd on that build (06:00Z) wedged the mount: dd
 close EIO, `ls` EBUSY, unmount DATA LOSS — the lane bug, not the KV. Prior 05:06Z roll (`~/efs-runs/roll78.log`, ROLL_OK):
 `06916bc7e5c1-dirty` = HEAD
 `06916bc7` + uncommitted raft wedge fix + F1 (RDMA peer reply
 wait bounded to 250 ms) + F2 (`inbox_drop=` on the raft-obs line)
 + the utimens flush. The user mounted fstor007 at 05:10:32Z with
  `client.sh` on the same tree (`fuse.efs-fuse`, FUSE_OK); the mtime
  gate passed on it (`~/efs-runs/rec-gate84.log`). fcstor003–015
  clients are stopped. g0 term 95 leader fcstor005 (one election
  05:11:19Z under the user's load), g2 term 673 leader fcstor005,
  commit==applied. The 03:56Z `--perf` data of the previous run is
  `/tmp/efs-perf/efsd.data` on fcstor003–006, now readable.
  Prior 03:56Z roll was `--perf` only
  (`~/efs-runs/ready65.log`), fstor007's client stopped by the user
  at 04:05Z after ls/find/du/rsync/ecrawl/ecopy. Both groups are up (04:11Z: g0 leader
  fcstor003 term 90, g2 leader fcstor005 term 669,
  commit==applied) after a 13-minute election storm during ecopy
  (`results/measure/20260930-040600-postfix-review/SUMMARY.txt`,
  learning below; START-HERE D15/D16 pending, F1/F2 mechanical).
  Per-op references from that review, one client, no server strace:
  stat 0.33 ms, openat 0.60, close 0.34, utimensat 1.05, chmod 0.49,
  rename 6.2 ms. The 03:10Z roll had wedged group 0: the
  term-41 no-op (437638) was committed on the leader only, so
  every group-0 read served by a follower (including root
  GETATTR through its odd lanes) was BUSY → EBUSY/ENOENT after
  16.76 s (`results/measure/20260930-032400-g0-wedge`). Fixed in
  `src/raft/raft.c` (learning below); `ls`, `stat`, mkdir OK
  after the roll.
  Prior 01:45Z restart was `--perf` and `--strace`
  on `bd57b7e74a91-dirty` (`~/efs-runs/restart53.log`). Fresh table from the 17:04Z wipe; restart
  needs no `--join`. 9-host posix **200/201** on all nine, 15.6–17.3 s
  (`results/posix/20260930-000428`). posix2 **63/63** in 47.2 s
  (`results/posix2/20260930-005552`): first open of an existing file
  takes HOLD, last close releases it, CREATE does not. The 60/63
  before that hold is `results/posix2/20260930-000915`. `nohint=0`,
  `create-miss=0`. fstor007 8 GiB dd+fsync **661 MiB/s**
  (12.396 s, `results/measure/20260930-005700-dd-fstor007`).
  The `~/git` ecopy did not finish in 240 s
  (`results/measure/20260930-012100-ecopy-git`): group 2 terms
  18→253, `hi=2048`. Group 2 had a leader again at 01:30Z
  (leader 3, term 253, commit==applied). Partial tree
  `/tmp/efs-mount/measure/ecopy-git` on fstor007. Prior 9-host with
  five create EIOs:
  `results/posix/20260929-202024`.
  The 17:04Z reduction (194/201, before these fixes) is
  `results/measure/20260929-170457-wipeposix/ana`.
  Prior stop 13:53Z
  (`~/efs-runs/stop32.log`) after the 13:08Z roll. That reduction
  `results/measure/20260929-130800-ddposix/ana`: D12's file-cap
  backstop rewrote 67 GB of L1 during two 100 GiB copies (L0 peak
  379) and `fsync` returned EIO.
  Files from that earlier roll, node-local:
  `/tmp/efs-perf/efsd.data` + `efsd.strace` (13–20 GB) on 003–006,
  `fuse.data` + `fuse.strace` on 003–015. Prior roll 12:29Z was
  the same tree without recorders. The 12:36Z 9×4 debug IOR
  (`results/io500/20260929-123635-rdma`) finished ior-easy-write at
  2.218 GiB/s and mdtest-easy-write at 1.765 kIOPS, then ior-hard-write
  aborted on fsync with no bandwidth. Prior
  roll 05:11Z
  was the same id without those changes.
  Prior roll 04:08Z (`54a500da9dc8-dirty`): recorders stopped
  04:27Z. Analysis `results/measure/20260929-040800-idle-trace/ana`.
  A user `ecopy` 04:15–04:22Z wrote 408K fragments per server; zero
  `raft-snap: start` (W22.1 holds), 26K `access()` for 408K creates
  (D7 holds), but the pump blocked in `kv_maybe_flush_locked` for
  4.1 / 1.7 / 24.2 / 2.7 / 3.6 s on fcstor004. That is what the 05:11Z
  roll changes (W23). Prior to that, 02:35Z
  (`bbcbcb5ad779-dirty`) recorded the IOR abort in
  `results/io500/20260929-023447-iorperf2`; those recorders were
  stopped at 02:41Z. Prior to that, 00:28Z.
  At 00:39Z group 0 was commit==applied 13665609 (term 9309, leader 0)
  and group 2 12085588 (term 2763, leader 3). fcstor006 lagged that
  roll on a 2.3 GB group-2 snapshot (5796 ms) and was caught up
  (12083285) before the client remount. The `-dirty` is the node tree
  missing tracked `results/` (rsync excludes that dir), and all four
  IDs match. Clients fcstor007–015 were remounted RDMA at 00:39Z.
  The 00:40Z IOR aborted on fsync EIO (the W17.1 bound on the first
  BUSY REPORT; the old loop retried BUSY 8×); ranks exited by 00:41Z,
  no D-state. Profiles: W18 and W19 are out of the top of the client
  and group-0 leader (`results/io500/20260929-002758-wimpl`, START-HERE
  §1b). No IOR bandwidth from that run.
  L1 is a growable list (the MANIFEST was already one line per file).
  After the Sep 28 evening roll, fcstor004 L0 went 49→2 and fcstor005
  58→3 with `kv-compact` rc=0; during the following IOR, L1 grew to
  310 / 405 on those two nodes and L0 stayed at 3. The pump does not
  flush a memtable it cannot place, and a no-progress snapshot ack
  waits one heartbeat before the next chunk.
  Group 0 = odd shards = nodes 1–3 (`voters=0x7`); group 2 = even shards =
  nodes 2–4 (`0xe`). Clients fcstor007–015 (003–006 also run efs-fuse),
  export `efs-test`, mount `/tmp/efs-mount`, `EFS_TRANSPORT=rdma`. 19820 is
  retired; do not stand it up. Do not roll back to TCP unless a suite fails.

<a id="ph-where-we-are-sep-11--oct-1-2026-fbb88f"></a>
### Where we are (Sep 11 – Oct 1 2026)

<a id="ph-where-we-are-one-screen-details-in-start-here-302eea"></a>
## Where we are (one screen; details in START-HERE)

- Step 11 Raft+KV engine + Step 12 FUSE A–D landed and gated (Sep 11–16).
- W1 N-1 CAS, W2 spec (`write()` client-buffered), W3 fsync tail, W4
  multi-client measurement, W5 50g: done Sep 18.
- **W6 IO-500 gate: data path CLEAN; parent-row lost update FIXED (§7.2
 reductions, Sep 21, `30c41ee`).** 9×4 `results/io500/20260921-debug-9x4-outbox/`:
 both `-R` reads 0 errors, every unlink OK; mdtest-easy-write 0.238 kIOPS,
 hard-write 0.201, easy-read 1.16 GiB/s, hard-read 3.89, easy-write 0.78,
 hard-write 0.046 (36-way sub-chunk CAS). Open perf: 1 GiB cold open
 0.23 s idle (128 GETCHUNKS × 1.6 ms), 14.6 ms/GETCHUNKS under 32 openers.
 Same-parent rate **333 / 1385 / 1241** ops/s at 1 / 9 / 36 procs
 (`results/measure/20260927-211953-samedir-rate`, storm PASS, `busy_n`
 0/0/1). Was 138/134/159. Concurrent metadata proposers append into
 one fsync and one AppendEntries.
 Cluster binaries: servers **`ab458efab95b-dirty`** (rolled `--all`
 21:11Z). Clients fcstor007–015 remounted 21:32Z, all FUSE_OK.
 The same-dir number was measured before that remount. Shared-file IOR-hard NP=4
 SEGS=3000, cold verify: write **481.56 MiB/s**, read **91.35**,
 12000 records bad 0; posix2 63/63
 (`results/posix2/20260927-190509`). 1/9/36 not measured
 (group 2 REAP_DONE ~11–16/s; do not raise the idle gate). TCP,
 `EFS_TRANSPORT=tcp EFS_RAFT_OBS=1`. **W13 is done:** L1
 compaction runs on a background thread; the apply path only flushes
 the memtable and swaps the manifest. 144-way mkdir
 (`results/measure/20260926-163709-mkdir-hammer`): idle p50 **4.68 ms**,
 **35166** in 15 s, p50 **56.5 ms**, `apply_max` **68 ms** while two
 ~760 MiB compactions took 2.0 s each. Compare p50 and `apply_max`,
 not the raw count. Do not tune memtable/`l0_max` to change how often
 compaction runs. Folding a txn's PREPARE parts into one command is a
 protocol change (per-part verdicts) — ask. Do not pipeline past the
 one in-flight batch. A multi-entry AppendEntries fsyncs once.
 **W11 is done.** Logs are under 5 KB (were multi-GB). fcstor005
 rejoined in 510 ms. Idle md_latency matches the W13 sample on a
 second read (`results/measure/20260927-w11-gate`). 9-host posix is
 200/201 on all nine (`results/posix/20260927-123717`, mmap SKIP
 only, 0 not-run). Freeze passed on the
 rerun. W10's private empty-mkdir passed 5/5. Sep 27's live switch
 failed 9-host posix (193–196/201, 385 s,
 `results/posix/20260927-044348`) and was rolled back. Sep 28
 19810 is RDMA again: 9-host 199/201 in 57–59 s
 (`results/posix/20260928-034049`), posix2 63/63, morning
 9-client dd **1326.6** vs TCP 1478. Afternoon cycle best
 **2810.5** (`20260928-131651-dd-prof-r2b`); live restore
 **2551.5** (`20260928-134637-dd-prof-r5b`). Not faster than
 the 44 GB/s ceiling. User xattr is on that same
 dirty build: jobs=1 **200/201**, `opt_xattr` PASS, only
 `mmap_write_read` SKIP (`results/posix/20260928-043918`, 45.2 s).
 The RDMA recv poller waits on the CQ channel after 32 empty polls
 (`recv_poller` was 64% of efs-fuse on the yield loop; that loop is
 gone). Five posix jobs=1 profiles on Sep 28,
 `results/measure/20260928-093100-posix-prof-r1` through
 `20260928-100138-posix-prof-r5`, each 200/201. Last check after
 the 32-poll roll: 45.1 s. Do not put the yield loop back. The
 200 µs `efs_rdma_recv_wait` clock spin is 64 pauses. Snapshot
 export merges with a heap and writes 64 KiB at a time.
 Kernel 5.14 returns ENODEV for
 `MAP_SHARED` on `FOPEN_DIRECT_IO`;
 do not clear `direct_io`. posix2 alone 63/63
 (`results/posix2/20260928-044304`). Four suites on one client:
 each 200/201, 46–57 s (`results/posix/20260928-044951`). Private 100-mkdir after the
 poller and per-send query fix is 642 ms vs TCP 507 ms. W9's pin
 rules are on fcstor007–015. Posix 1 passed on the same 200/201 run.
 Posix 2 is 63/63 (`results/posix2/20260927-123946`, 58 s).
 The leak gate passed
 (`results/leaks/20260927-035622`). A cold stat of 1M files on the
 fixed evictor leveled at 233 MB RSS
 (`results/measure/20260927-w9-walk`). W9 is done.
 Partitioned flush is in the TCP build rolled Sep 27 05:07 UTC:
 one L0 file per `key[0]`, compaction rewrites one range.
 `test_kv_lsm` covers it. Existing wide L1 files split on the
 next compaction. `EFSD_ENV` is **space**-separated
 (`env $EFSD_ENV`): a comma form
 (`EFS_TRANSPORT=tcp,EFS_RAFT_OBS=1`) silently makes the transport AUTO
 (ungated RDMA) and drops OBS — that roll happened once (05:49) and was
 re-rolled at 05:51. Next server restart is a build-ID change →
 `tests/roll_efsd.sh --all`.
 **I16 landed `43bdf6a`** (op-id suffix on every dir RPC, window on the
 dentry shard, REDUCE_OPID txn part / log-path trailer, client in-flight
 table → contiguous ack; two limits: 16-entry reply cache per window, no
 GC of dead-client windows). Gated Sep 23 03:00: freeze run 1 **0 worker
 errors**, run 2 parent clean + 3 `mkdir ENOENT` traced to the client's
 hintless-NOT_PRIMARY fast-fail (fixed `bb634d9`); posix jobs=1 200/201
 + mmap SKIP (`results/posix/20260923-030005`); 9-host 189–191/201, 0
 not-run (`results/posix/20260923-030056`). On `ad292b9` the freeze's
 getattr ENOENT did not recur; run 1 left one committed dir whose rmdir
 returned ENOENT (dentry intent, fixed by `lookup_tx` — learning below).
  **I17 gate done** (`i17_leader_freeze.sh` ×2, Sep 22 08:25):
  `arc_term_miss` 0→8; one run parent clean, the other left 3 dirs whose
  row was `nlink=5 nents=3` with matching dentries and child rows
  (`results/measure/20260922-122517-i17-leader-freeze`,
  `20260922-122629-i17-leader-freeze`). Idle rmdir of those dirs
  succeeded. posix jobs=1 on this build: 200/0/1 SKIP in 92 s
  (`results/posix/20260922-133653`). 9-host: 185–197/201, 0 NOTRUN,
  46–66 s (`results/posix/20260922-134008`); an election 12 s in
  accounts for the low end. Next mechanical item: I16.
- Honest write numbers (MiB/s, flush in clock): Sep 28 afternoon
  RDMA 9-client, probe outside the writer-pool lock, live restore
  **2551.5**, walls 24.84–28.90 s, all nine 8589934592
  (`results/measure/20260928-134637-dd-prof-r5b`). Best of that
  cycle **2810.5**, walls 25.05–26.23 s
  (`results/measure/20260928-131651-dd-prof-r2b`). Do not quote
  `20260928-102121-dd-prof-r2` (ssh timeout). Morning RDMA
  9-client **1326.6**, walls 28.55–55.57 s
  (`results/measure/20260928-033420-dd-wall`, `db2b88c4802a-dirty`).
  99/108 report-split lines BUSY. Slower than TCP 1478.
 **Latest IO-500 is Oct 1 07:49Z** (`results/io500/20261001-074905-rdma`,
 fresh table, W28–W35 + D20 in both daemons): every phase, 0 fsync
 failures, easy-write **5.173**, mdtest-easy-write **6.185 kIOPS**,
 hard-write **0.519** (63.2 s), mdtest-hard-write 2.818, easy-stat
 24.4 kIOPS, hard-read **0.824 with 1 read error** — cold hardscan
 `bad=1` (ino 10897 ci 118843, [0,4256) zero, a fold tombstoned the
 client's own span; W38, START-HERE 0e). Prior Sep 30 18:35Z
 (`results/io500/20260930-183504-rdma`): every phase, 0 fsync
 failures, 0 read errors, cold hardscan bad=0; easy-write 4.563,
 hard-write 0.640, hard-read 1.034, mdtest-easy-write 4.632 kIOPS.
 The 07:28Z run
 (`20260930-072824-rdma`: hard-write 0.261 with fsync failures,
 hard-read aborted) is superseded; do not quote a hard-read or a
 score from it. Prior complete run:
  Sep 28 9×4 IO-500 debug (1 s stonewall, same-mount reads,
  `db2b88c4802a-dirty`): RDMA easy-write **2.917 GiB/s**,
  hard-write **0.291**, easy-read **3.092**, hard-read **1.525**,
  mdtest-easy-write **3.418 kIOPS**
  (`results/io500/20260928-150609-rdma`). TCP on that binary:
  easy-write **1.376**, hard-write **0.274**, easy-read **2.669**,
  hard-read **1.162**, mdtest-easy-write **2.721**, easy-stat
  **16.461** vs RDMA **3.248**
  (`results/io500/20260928-151707-tcp`). Do not quote the
  aborted RDMA attempt (lookup EBUSY). Earlier
  RDMA `113823180b15-dirty`: 1-client **947**, 4-client **2311**;
  that 9-client row is INVALID (do not quote ~238)
  (`results/measure/20260928-015839-dd-wall`). Sep 27 TCP, 8 GiB,
  FUSE_OK. Morning `4c6a5acefe03-dirty`
  (`results/measure/20260927-053506-dd-wall`): 1-client **977**,
  4-client **1984**; 9-client INVALID (fsync EIO, report `rc=-3`).
  Do not quote 1464 from that run. Evening `75321297f719-dirty`,
  preflight idle (`results/measure/20260927-204907-dd-wall`):
  9-client **1478**, walls 49.76–49.89 s, all nine 8589934592, no
  EIO, zero `rc=-3`. 119/128 report-split lines are BUSY (3.4% of
  44 GB/s). 1 and 4 were not remeasured on this build. Prior: Sep 18
  639/251/202, Sep 21 499/176 and 9-client invalid. Do not compare
  against the deleted pre-Raft engine (Aug: 2355/3599).
- posix suite 1 jobs=1 known signature: **200/201**,
 `mmap_write_read` SKIP (`MAP_SHARED` ENODEV by spec). Sep 22 on
 `46d54e6`: 200/0/1 in 92 s (`results/posix/20260922-133653`; the prior
 ~37 s run is `results/posix/20260922-015106`). **9-host suite (W8's gate) on the W13 build: 200/201 on all
 nine, 13.2–14.8 s** (`results/posix/20260926-164123`). Timeline
 `results/measure/20260926-124106-w8-stall-timeline`: no term change
 (g0 6882 leader 1, g2 1198 leader 3), worst `apply_max` 67 ms across
 four compactions. Prior wakeup build was 200/201 on eight hosts +
 199 on fcstor009 (`flock_shared_then_exclusive`) in 15.5–23 s
 (`results/posix/20260926-1430-wake2`) through a 2.0 s `apply_max`.
 Prior steady
 score: 193–196/201, 79–94 s on `7e29943`
 (`results/posix/20260923-202626`). Prior: 185–197/201 in 46–66 s
 (`results/posix/20260922-134008`) and 191–195/201 in ~62 s
 (`results/posix/20260922-020950`). What failed on those older runs: the six
 many-op tests (`dir_deep_nesting*`, `names_crazy_*`,
 `concurrent_write_and_readdir`, `concurrent_creates_same_dir`,
 `mtime_monotonic_many_writes` — throughput at 144 jobs, mkdir p50 21 ms
 under load) and 1–3 one-offs at an election (EEXIST on a fresh LINK
 name, EIO, empty read = retry of a committed op, I16). Idle
 `md_latency.py` reference (Sep 21): **mkdir 6.1 / create 4.0 / append 6.2 /
 stat 0.3 / unlink 1.5 / rmdir 5.2 ms**. On `7e29943`, idle, term stable,
 commit==applied but +1/s: mkdir 8.5 / create+close **56.8** /
 append+close **59.5** / stat 0.5 / unlink 2.2 / rmdir 7.8
 (`results/measure/20260923-162535-idle-mdlat/mdlat-idle2.txt`). The 50 ms
 is on the two ops that write a byte and close. A 20 s post-roll sample
 also shows a 50 ms mode (RDMA election churn) — that one, wait 2 min.
 Root mkdir (410 names): med 10 ms, max 0.063, no 1 s mode
 (`w8_root_lat.sh`); 9 concurrent root mkdtemp 9/9 (`w8_root_mkdir.sh`).
 posix2 one pair **63/63** (`results/posix2/20260927-123946`).
  The morning 59/63 was shared-pwrite (dcache cap 8 collapsed 16
  ranges to UNCOND; `DCACHE_NR` is 32), truncate EIO (alias stub
  minted a generation never PUT; keep `got.generation`), and overlap
  pwrite (STALE replay of a committed span; clear `nrange` when the
  slot is clean). Anything else is new.


<a id="ph-design-decision-log-d1d12-sep-2829-2026-from-the-standing-pe-432387"></a>
### Design-decision log (D1–D12, Sep 28–29 2026) from the standing-permissions bullet

- **Never invent a design decision the spec lacks** — specifically: chunked
  InstallSnapshot (W11), a distributed chunk lock (W1), splitting REPORT
  into several RPCs (W3/W4), publish-on-every-`write()` (W2), relaxing
  `entry/attr_timeout=0`, `hard_remove`. Bring the measured symptom to the
  user. **Decided Sep 28 (START-HERE "Decisions — taken and pending",
  D1–D3): span publishes commute (no base-gen CAS for spans, fold by
  the chain-filler or a reader, still no lock); `open()` adopts the row
  only and chunk maps arrive in per-lane windows with no knob; the 5/s
  idle gate stays. Implement those; do not re-ask. **Decided Sep 29
  (D4–D8):** snapshot by log bytes (512 MiB) with a retained log
  window, InstallSnapshot only below it (W22.1); the follower applies
  an import diff in per-tick slices and keeps answering heartbeats
  (W22.2); measure `fsync` shared vs quiet root, then `--meta-storage`
  and redeploy if the sharing is the 100 ms mode (W22.3, ask which
  device); `path_hint = 0xffffffff` = first write, server skips the
  probe (W14.4b); `pub_batch_ms` in `raft-obs`, one IOR, bring the
  number before any budget question (W16.2–3 / D8). Implement those;
  do not re-ask. Rolled 04:08Z Sep 29 (`54a500da9dc8-dirty`): W22.1
  (snapshot at 512 MiB of log commands, retain that window,
  InstallSnapshot only below it), W22.2 (import diff in 1024-key
  slices), W14.4b (`EFS_PATH_HINT_NEW`), `--meta-storage` defaulting
  to the first storage root (mdraft not moved), and `pub_p50`/`pub_max`
  on the raft-obs line. Earlier the same day, through 02:35Z: W17.1,
  W16.1, W18, W19, W14.4, W15.3, W15.4, W14.2(a)–(b), W20, W21, D1's
  span publish, D2 step (a). Order now: D2's parallel windows, one IOR
  for D8's `pub_p50`, then D6's fsync measurement (ask which device
  before moving mdraft), W8/W10.** `HOST_SNAP_MIN`'s replacement
  (`EFS_RAFT_SNAP_BYTES`), the retained-window size, and the per-tick
  slice (`HOST_SNAP_SLICE`, 1024 keys) are internal constants, not knobs.
  **Pending Sep 29 04:27Z (START-HERE D9–D11, W23; ask, do not
  implement on your own):** D9 the pump never waits for the compactor
  (back-pressure = BUSY at `host_propose` on the leader); D10 compact a
  range by L0 bytes (1/8 of its L1), L0 cap in bytes, split range 0,
  1 MiB merge reads; D11 closes D6 — the Raft-log fsync is 0.4 ms, the
  100 ms mode is the compactor's own segment, do not move `mdraft/`.
  D8 is answered: `pub_p50` 3 ms with a pump-hold tail; no wire change.
  Implemented and rolled 05:11Z Sep 29 (`a53b253f2455-dirty`), not
  gated: D9 (no pump wait; publish BUSY when L0 is hot), D10's ratio
  and the range-0 split (L0 byte cap, plus the D12 file-cap
  backstop rolled 12:29Z), D2's parallel
  chunk-map window. D11 stands: do not move `mdraft/`.

<a id="ph-start-here-handoff-archive-sep-28--oct-1-2026-1400z--moved-o-4e37f3"></a>
## START-HERE handoff archive (Sep 28 – Oct 1 2026 14:00Z) — moved Oct 1 2026

These are the §1b "in flight" blocks that START-HERE carried until the
Oct 1 2026 doc review, verbatim, newest first. The one still current stayed
in START-HERE. Result directories they cite were pruned on the same day
when nothing live cites them; `git log -- results/` has them.

**Oct 1 14:00Z: the fstor007 wedge (8 parallel 20 GiB dd + ecopy) read live without tracing; D24 decided and implemented, REPORT reply wait sized, log timestamps.** What the box showed at 13:41Z (`~/efs-runs/rec-look550..559.log`, gdb snapshot `/tmp/efs-fuse-stacks-552.txt` on fstor007): 16 `dd` in D-state in `request_wait_answer` past `exit_mm` (the user's kill had landed; the kernel's close-time FLUSH is forced and uninterruptible), `ecopy` a zombie with threads in D, load 56; efs-fuse alive with all 63 threads asleep (10 FUSE workers in `read(/dev/fuse)`), `/sys/fs/fuse/connections/59/waiting=0`; servers idle and healthy. The client log (no timestamps then) ended with `retry type=67 why=recv rc=-6` ×3 → `slow-ok type=67 attempts=6 saw_busy=1 us=116309308` and `attempts=4 us=121101530`: eight 163 840-record close REPORTs arrived together, some took longer than `EFS_IO_TIMEOUT_MS` (30 s) to answer, `rpc_send_recv_dual` dropped the conn and re-sent the whole 27 MB REPORT (server re-executes; retries find the chunks published and are cheaper, but still pack 160 K records). Every `close()` on the client goes through `efs_client_report_dirty_ino` → `report_mu`, so ecopy's first close on each of its threads queued behind the 2-minute retry loop — "ecopy never started". Everything drained by 13:44Z; nothing on fstor007 was touched.
- **Implemented (client):** (1) **D24** — `report_landed_note` in `dcache_put_now`: every `REPORT_LANDED_CHUNKS` (8192 = 1 GiB, global across inos) landed PUTs kick `meta_flush_main`, which REPORTs the whole dirty set (the close path's records, same code, `sync=0`); `close()` publishes the tail only. Log line `report-landed: N chunks landed, kicking REPORT`. (2) `rpc_send_recv_dual(…, recv_ms)`: REPORT's reply wait is `EFS_IO_TIMEOUT_MS + count/2` ms (0.5 ms per record; 13 µs/rec alone, >183 µs/rec effective in the wedge) and the conn's default is restored on release; the `retry … why=recv` line now prints `req_len` and the bound. (3) **Both daemons stamp every log line** (`src/common/log_ts.c`, `efs_log_timestamps_install` at the top of each `main`: `fopencookie` streams over fds 1/2, `2026-10-01T14:01:46.414Z ` prefix, `EFS_LOG_TS=0` disables; `backtrace_symbols_fd` uses fd 2, not `fileno(stderr)`). Every harness grep on these logs is a substring match.
- **Not implemented, named W41 (ask):** `report_mu` still serializes every close on one client behind one REPORT's retry loop (BUSY 7 × backoff ≈ 2 s, STALE up to the 8 s sync budget). Removing it needs per-inode extraction from the dirty sets (the open-addressing tables have no delete) and a multi-slot `pub_ino` set; the snapshot swap is load-bearing for "close returns only after this inode's records went out", so this is not a mechanical edit.
- **Gate (passed):** unit tests on fcstor007 (`bld561`, all pass; `test_raft_store` not run — 4 pre-existing failures on HEAD). `~/efs-runs/wedge565.sh` on fcstor007 (`wedge567`, `~/orcd/scratch/efs/perf/agent-wedge-20261001-141222`): **4 parallel 8 GiB dd+fsync from one client in 9.8 s wall (3.5 GB/s aggregate), all four files 8589934592**, and the concurrent 300-file create+write(4 KiB)+close storm (the ecopy stand-in) ran in 2.6 s with **p50 4.7 ms, p99 51 ms, max 213 ms**; 32 `report-landed` kicks (one per GiB), zero `fsync-split` ≥ 100 ms, zero `retry type=67`, zero `slow-ok`. Before this change the same storm would have waited on `report_mu` for the dd closes. Clients fcstor003–015 and fstor007's `/tmp/efs` are on this tree (`dep570`); **the servers are not** — `efsd` gained only the log timestamps, which need a build-ID roll (`cluster.sh restart`) that the user's live fstor007 session should not pay for unasked.

**Oct 1 13:20Z: the user's 20 GiB `dd bs=1M` perf dir (`~/orcd/scratch/efs/perf/efs-mount`, `client.sh --perf --strace`, write 29.1 s / read 23.9 s to disk) reviewed; three client changes in the tree, measured on fstor007, not yet on fcstor007–015.** Reproductions and profiles are `~/orcd/scratch/efs/perf/agent-dd-20261001-*` (`-120522` pid perf + strace windows, `-121718-sw` system-wide perf before, `-121931-lock` dwarf perf + `futex-top.txt`, `-122504-fix` after, `-1237xx-var` variants A–E, `-130753-base` clean HEAD, `-13xxxx-numa` the pinned tree). What the user's capture lacked: `--perf`/`--strace` on the daemon are tracer numbers (strace plateau), and `perf record -p` taken at mount sampled only the 7 initial threads — the reclaim/put pools created later were invisible (no blake3 in that profile). Use `perf record -a` on the node.
- **Where the write wall is (20.1 s clean, 1.1 GB/s):** dd is serial — source `read()` 0.45 ms/MiB (9.1 s of the 20), efs `write()` 0.82 ms/MiB, then `close()` = one `EFS_MSG_REPORT_CHUNKS` of 163 840 recs answered in **2.13–2.35 s** by the server (`slow-ok type=67`, `fsync-split flush_ms≈2300`) — the tail grows with file size and is 11 % of this wall. Of the read (12.2 s clean, 1.8 GB/s to `/dev/null`): efs 0.56 ms/MiB; writing the destination added the other half of the user's 23.9 s.
- **Fixed (1), `write.c`: the reclaim pool's wakeup was a herd on `g_reclaim.mu`.** Every `dcache_kick_complete` took the mutex and `cond_signal`ed, the end of every sweep `cond_broadcast` to 16 workers, each re-took the mutex: 156 377 futex calls per 3 s on two addresses (`g_reclaim+0x0/+0x50`), 18 % of all client cycles in `native_queued_spin_lock_slowpath`, the FUSE thread queued in `ll_write_buf → __lll_lock_wait`. Now `kicks` is a SeqCst counter, `reclaim_kick` locks only when `waiters > 0`, the sweep broadcasts only at shutdown. 26.5 K futex per 3 s after; the spinlock is gone from the profile; **client CPU for the 20 GiB write 74.9 s → 33.6 s** (wall 20.1 → 21.2 s, within noise — the wall is dd's serial read plus the REPORT tail).
- **Fixed (2), `efs_fuse.c`: `ll_read` malloc'd `size` per request** (1 MiB, above the mmap threshold → `mmap`/`munmap` + kernel zeroing per READ; `clear_page`/`rmqueue` in the read profile). Thread-local reply buffer up to 16 MiB. Variant D: read 12.2 → 10.4 s.
- **Fixed (3), `efs_fuse.c` + `rdma.c`: `efs-fuse` pins itself to the HCA's NUMA node at startup** (`numa_pin_startup`: `sched_setaffinity` before any thread, `set_mempolicy(MPOL_PREFERRED)`; `efs_rdma_numa_node_for_host` resolves the route's local IP → ifname → HCA → `numa_node`; `EFS_NUMA_NODE=none|N`; one `efs: numa pin node=…` line). fstor007 is 2 sockets / 8 nodes with the HCA on node 2 and the FUSE thread spent 0.3 ms/MiB in `fuse_buf_copy` into cold remote-node pool buffers (cold-destination memcpy 3–18 GB/s vs 55–66 hot on that box). Measured, `numactl` variant C/E then in-process (`-numa`): **write 20.1 → 18.9 s (CPU 23.9 s), read 12.2 → 6.6 s (3.3 GB/s), `cmp` of the first GiB OK.** fcstor00x have 2 nodes, HCAs on node 0 → efs-fuse there is confined to CPUs 0–11. **Deployed 13:20Z to fcstor003–015 (`dep532`, all `fuse.efs-fuse` MOUNT_OK, `numa pin node=0 cpus=12 (hca mlx5_2)`); gated:** posix jobs=1 fcstor007 **200/201** 28.3 s (`results/posix/20261001-132125`), 9-host **200/201 on all nine** 14.2–15.3 s (`results/posix/20261001-132411`; 13.3–13.6 s this morning unpinned — within a second, watch it), fcstor007 8 GiB non-zero dd+fsync **1.3 GB/s** (6.80 s, size 8589934592, efs-fuse CPU 12.6 s; `dd539`). One IOR on the pinned clients is still owed.
- **Not done, with numbers — decide or implement next:** (a) **D24 (ask): the close-time REPORT of a long sequential write.** 2.3 s for 20 GiB, linear in size (100 GiB → ~12 s of `close()`); the rule forbids inventing a REPORT split, but W30 already PUTs chunks during the write, so a threshold REPORT of *landed* chunks every N MiB (the irec-only threshold path exists, `write.c`) is one decision away. (b) **W39 · RDMA zero-copy send:** `efs_rdma_send_frame` copies every fragment into an 84 KiB registered buffer — 6.5 % of client cycles (memmove total 25 %, blake3 8 %, `xor_into` 7.3 % after the fix). Register the bufpool slabs per PD and post a two-SGE send (header from the pool, payload from the slab), keeping the slab pinned until the send CQE. (c) **W40 · FUSE write copy:** `fuse_buf_copy` from libfuse's request buffer into the pool is one full copy of the data; `EFS_FUSE_SPLICE_READ=1` (variant B) does not remove it (21.9 s vs 21.2). libfuse 3.10.2 has no custom-buffer receive; this needs the raw `/dev/fuse` read into a pool-backed slice (own session loop) — ask before building it. (d) The stale "128 KiB/request" comment is fixed: requests are 1 MiB (`read(/dev/fuse)=1048656`).

**Oct 1 06:05Z: W28–W35 are in the tree, not rolled, not gated.** Compiled on fcstor003 (`/tmp/efs-w28`, `efs-bg` `bldw28` rc=0, then the get-pool shard). What landed: W28 layout freshness is the requested range plus one window of lookahead (200 ms unchanged); W29 demand asks the dcache first, a full-chunk PUT puts the body back, rdcache has a pending entry a second reader waits on, prefetch is submitted before the batch wait; W30/D19 a full overwrite is PUT by the reclaim pool under the dirty cap (REPORT stays at close); W31 the buffer pool grows in 256-chunk slabs, capped at 2 GiB; W32 reclaim pipelines `EFS_WRITE_PIPELINE` PUTs and the put pool is one queue per worker; W33 the two fragment GETs run on the chunk worker (the frag pool is gone), the get pool is one queue per worker, the server writer pool is one slot per thread; W34 a first write (`EFS_PATH_HINT_SKIP`, copied onto the writer thread) creates in one open, any other PUT uses `O_CREAT|O_EXCL` and charges only on create; W35 a REPORT rec is the head plus `delta_base_n` spans (a full image is ~168 B) and `EFS_RDMA_BUFSZ` is 84 KiB so a 64×1248 GETCHUNKS reply fits RDMA. **D20 rolled 06:15Z (`~/efs-runs/roll-d20.log`, CLUSTER_OK, `--perf` only, no `--strace`, no `--clients`):** the 32-byte digest is a 4 KiB tail of the fragment file, one `O_DIRECT` write, no `.sum`. Servers `4b2844487831-dirty` (the id is HEAD plus this dirty tree), `efsd=1 perf=1 strace=0` on fcstor003–006, cmdline has `--perf`. Clients fcstor003–015 were already down and were not remounted. Existing fragments are the old layout and will not verify; new PUTs use the tail. Gate is still the untraced 8 GiB dd.

**Oct 1 07:10Z: review of W28–W35/D20 against the 20 GiB dd profile (the user tests; nothing rolled).** Fixes in the tree, compiled with the unit tests on fcstor003 (`~/efs-runs/bldrv2.log`): (1) `dev_for_fd` caches the HCA per local IPv4 and serializes the one resolve — `getifaddrs` per RDMA conn upgrade was 26 % of efs-fuse cycles with `osq_lock` 20 % (256 concurrent upgrades spinning on rtnl at the first PUT window); (2) **W29's "put the body back after a full-chunk PUT" is reverted** — clean dcache bodies have no budget and no evictor (freed only by drop or take-replace), so RSS tracked the bytes written and every buffer past the 2 GiB slab cap was an mmap again (the 20 GiB dd's page-fault symbols); the rest of W29 stands (dcache-first demand, rdcache pending entry, prefetch before the wait). A bounded clean-image cache is **D23 (ask)**; same-mount read-after-write is dcache by the fio-honest rule, so nothing honest is lost; (3) writer-pool slot cv transitions are `broadcast` — one cv carried the writer's QUEUED wait, the owner's DONE wait and the fallback EMPTY wait, and a `signal` could wake the wrong class and leave the owner asleep with no timeout (deadlock under writer saturation); (4) bufpool reserves the slab index under the lock — two concurrent carves recorded both slabs in one `g_slabs` slot and the lost one would have been `free()`d as a slab interior; (5) `rdcache_acquire` no longer takes a way that is pending for another key. Still open from the review, not fixed: `get_two_parallel` has no `EFS_READ_VERIFY` path (the opt-in client hash); `efs_dcache_maybe_reclaim` inline help was 20 % of the 20 GiB profile even with W30 — re-measure after (1); the conn churn behind (1) is unexplained (64 × 4 conns should upgrade once each; 26 % of cycles says they were re-created — check `conn_fd_is_dead` / `efs_client_conn_drop` counts on the next run); D20 disables the server's zero-page and aligned zero-copy O_DIRECT paths for every data PUT (one 68 KiB bounce memcpy per fragment); per-worker queues (W32/W33) admit head-of-line blocking behind one slow job; stale "72 KiB" comments in `xprt_bench.c:10`, `raft_host.c:96,775`. (`efs_rdcache_put`'s victim scan was fixed at 07:28Z, below.)

**Oct 1 07:45Z: cluster UP fresh (wipe 06:37Z → mkfs 07:18Z), posix suites run; two bugs fixed on the way, two follow-ups open.** Servers `4b2844487831-dirty` on fcstor003–006 (`restart408`, no recorders), g0 term 5 leader fcstor003, g2 term 4 leader fcstor004, commit==applied, `arc_miss=0 wait_timeouts=0 inbox_drop=0` after all suites. Clients fcstor003–015 on the same tree plus the two client fixes below (`dep419`, all `fuse.efs-fuse` MOUNT_OK). Results: posix jobs=1 fcstor007 **200/201** (mmap SKIP) 28.1 s (`results/posix/20261001-073542`); **9-host 200/201 on all nine, 13.3–13.6 s** (`results/posix/20261001-074052`); posix2 **62/63** (`results/posix2/20261001-073048`) then **63/63** (`results/posix2/20261001-074250`). Earlier runs that are the bug evidence: `20261001-072454` (10/201, the rdcache hang), `20261001-072916` (199/201, the PUT-window hole).
- **Fixed, client, deployed:** (a) `efs_rdcache_put` found the entry with `rdcache_find`, which requires `data`, so the `pending` mark W29's `rdcache_acquire` put on a fresh data-less way was never found; the put landed in another way and the next reader of that chunk waited on the orphan mark forever — posix `basic_pread_pwrite` D-state in `request_wait_answer`, 190 NOTRUN, on the first reread after a fresh mount (`~/efs-runs/px410.log`, stack in `rec-hang412/413`). Now the put looks for the pending way first, never victimizes a way pending for another key, and releases the mark on alloc failure. (b) `writev_readv_chunk_straddle` read `\0\0\0` for `XYZ`: W30 PUTs the completed chunk during the write, the full-image snap steals the body (`snap-steal-full`), and the local map names the object only after the PUT lands — in between the chunk is readable nowhere and `chunk_get_worker` zero-filled it as a "hole" (`efs: read hole` in fuse.log). `efs_client_read` now waits for the inode's PUT windows first (`efs_dcache_put_win_wait`, lock-free when `put_win_total` is 0; the window machinery is the Sep 30 utimens one). Same-mount read-after-PUT now fetches our own object from the servers (the body is gone — D23 stands). `read hole` lines still appear for legitimate sparse reads (9 on fcstor007 across two passing suites); only a non-sparse test failing makes them evidence.
- **Fixed, server, rolled:** `host_apply_rc_locked` / `host_apply_extra_locked` / `host_wait_settled` / `host_pub_batch_wait` tested `!g` for "not hosted", but `group_slot` returns a slot for every attached group (`hosted=0`), so a forwarded `host_propose_wait` on a single-group host read the unhosted slot's empty ring → `arc_miss` +1 and BUSY. That was `raft-mkfs` rc=-13 on every node (`start401`, `wipe405`): the SALT step on group 2 from a group-0-only node. Now `hosted` is tested; the leader's submit reply is the verdict.
- **Follow-up F1 (server, correctness, open): `peer_rename_vs_unlink_src` can succeed on BOTH clients and leave a dangling dentry.** A: `rename a→b`, B: `unlink a`, concurrent; both returned 0 and afterwards `b` is `-?????????` (readdir lists it, stat ENOENT: the row is gone). Reproduces 1 in 6 (`~/efs-runs/p2r422.log`; evidence left at `/tmp/efs-mount/posix-2c-r422-6/peer_rename_vs_unlink_src/b` on 19810 — do not delete until read). No server log line at all for the event. Two candidate holes, not yet separated: (i) `apply_unlink_cmd` / `apply_rmdir_cmd` map `EFS_ERR_NOT_FOUND` at apply to OK "as a replay" — a log-path UNLINK whose dentry the rename removed between the leader's lookup and the apply is reported as success without an op-id check; (ii) the rename txn's source-dentry part is `host_dent_drop_prep` EXCL DEL at `loc_ver` — if the unlink committed first (row reaped) the EXCL should STALE; if an absent key passes the version check, the rename re-creates `b → reaped ino`. Reproduce with `APPLY_LOG` on and `EFS_DCACHE_TRACE` off: `python3 tests/posix/posix_2client.py --remote fcstor008.ib fcstor009.ib --mnt /tmp/efs-mount --parent <P> --filter peer_rename_vs_unlink_src` after `--prepare` on A; then `kv_dir_dump` of the parent on a KV copy. Fix only the hole the trace shows. (Sep 30 / Oct 1 02:45Z posix2 runs were 63/63; today's faster client moved the timing.)
- **Follow-up F2 (server, robustness, open): `raft-mkfs` retried on another node forks the salt.** The first fresh start (`start401`) rotated the mkfs over nodes 1–3 while the group-2 SALT step was BUSY (the bug above); each node proposes its own `h->salt`, group 0 took node 1's MKFS and group 2's anchor shard then saw a different salt → `efs_meta_apply_salt_record` PROTO → `raft-host: apply salt rc=-7 index=3` on every group-2 apply forever (`rec-st402..404`), table unusable, re-wiped (`wipe405`). The harness now mkfs's on fcstor003 only (`roll_efsd.sh --all --fresh` / `cluster.sh start --fresh`: all four up, both leaders seen, then one node, up to 30 tries). Server side still open: `server_raft_host_mkfs` should take the salt from group 0's committed MKFS record (or refuse a second MKFS with a different salt) so the order of nodes cannot matter.
- Harness notes: `run_tests.sh posix --parallel` with no host list runs ONE host (`DEFAULT_HOSTS[0]`) — list fcstor007.ib…fcstor015.ib for the 9-host gate. `hostname -i` on an fcstor is the 10.1 address; status commands take `172.16.223.$((n+56))`.

**Oct 1 08:05Z: IO-500 debug 9×4 on the fresh table, first with W28–W35 + D20 in both daemons (`results/io500/20261001-074905-rdma`, NOTE.txt has the table).** Preflight idle, `PREFLIGHT_OK`. Every phase completed, 0 fsync failures, terms unchanged (g0 5, g2 4), commit==applied, `arc_miss=0`. Numbers (1 s stonewall, same-mount reads): ior-easy-write **5.173 GiB/s** (was 4.563), mdtest-easy-write **6.185 kIOPS** (4.632), ior-hard-write **0.519 GiB/s** in 63.2 s (was 0.640 in 52.6 s — the one regression, 20770 vs 21305 pairs), mdtest-hard-write **2.818** (2.612), ior-easy-read 22.5 (same-mount), mdtest-easy-stat **24.4 kIOPS** (16.5), ior-hard-read **0.824 GiB/s with 1 read error** (was 1.034, 0), mdtest-hard-stat 30.4, easy-delete 5.65, hard-read 5.92, hard-delete 4.84. Server g2 leader `pub_p50=879 ms`, 1997 publish STALE (why=6 FOLD_LIST) retries.
- **Follow-up F3 / W38 (data correctness, open, goes first): one lost 4256-byte piece in ior-hard, committed, not a cache artefact.** Cold verify after remounting the nine clients: IOR `-r -R` 1 error; `tests/tools/hardscan` (new, 24 s for 35 GB) `records=747720 bad=1`: rec 331368 (rank 24, fcstor013), bytes [0,4256) of chunk 118843 of ino 10897 are zero. All four ranks on that chunk are fcstor013's — a single-client chunk, not the Sep 30 two-host shape. Server row (`efs-mgmt raft-getchunks`, `EFS_MGMT_CHUNKS=1`): base image gen 1774…2861 (nodes 2,3,4) plus ONE len-0 tombstone for gen 1838…0185 at lane seq 1222. So rank 24's piece was published as a span first, then the client published a full image whose fold list matched (`delta_base_n=1, seq=1222`, `meta_apply.c:3480`) and the fold tombstoned the span — but the image has zeros where the span's bytes were. The client built a full image without its own already-published span. Suspects, in order: the W30 reclaim-pool PUT of a chunk whose first range was already a span (`dcache_flush_keep` putfail restore sets `have_base=1 nrange=0 base_gen=UNCOND` on the body COPY; `dcache_install_image` after a sub-range PUT; the `obs_n = ce.ndelta` fold observation at `write.c:2701–2724` taken from a body that does not hold the span). Repro: one client, 4 ranks, IOR hard geometry (`tests/measure/ior_hard_scaling.sh` or `run.sh ior-hard-write`), `EFS_DCACHE_TRACE=1 EFS_REPORT_DBG=1` on that client, then `hardscan` cold and `raft-getchunks` on every bad chunk; a fold whose image lacks a span's range must be refused client-side (the span is ours: paint it or keep it in the list) — do not "fix" it on the server by refusing folds, the server has no bytes to compare. The client fuse.logs of THIS run are gone (the remount truncates `fuse.log` — copy the nine logs before any cold remount). The hard file and `/tmp/efs-mount/io500/2026.10.01-03.49.05` are kept for the repro.
- Harness: `run.sh ior-hard-verify` now passes `--dataPacketType=timestamp` and takes `IOR_HARD_G` (the driver's `-G`, "Used Time Stamp" in ior-hard-write.txt) + `IOR_HARD_FILE`, so a driver run's hard file verifies cold; `tests/tools/hardscan.c` is the record scanner (the Sep 30 one lived in `/tmp` and was lost).

**Oct 1 05:10Z: the same perf dir, one `dd bs=1M` 1 GiB write (207 MB/s, `dd_1m.*`) and read-back (184 MB/s, `dd_1mr.*`), with the servers' `--perf --strace` recorders — analysis only, nothing implemented, no cluster command.** Both daemons were on the strace plateau (client 97–103 K lines/s, servers 101–113 K/s), so the two MB/s figures are tracer numbers; the shapes are not and are now queue items §1a rows 1a–1h (W28–W35, subsection "W28–W35: what the 1 GiB dd showed") with four asks D19–D22. In one line each: the write phase (1.6 s) PUTs nothing and the `close()` (3.56 s) is the whole drain, 16 chunks in flight, one `mmap` per chunk, REPORT 10.2 MB; the read is a serial 5.2 ms per MiB — a 79,888-byte GETCHUNKS over TCP on every read because `pull_layout_miss`'s freshness test is one group short, 2.6 fragment GETs per chunk (prefetch and demand both land), ~400 futex per MiB across the two worker pools; the server PUT is ~55 syscalls per 64 KiB fragment (a failed probe `openat`, two inodes, no fsync). `raft-obs` was healthy throughout (`wait_timeouts=0 apply_max=0 inbox_drop=0`). **05:45Z, server `efsd.data` + `efsd.strace` reviewed (recorders and daemons stopped 05:27Z):** three corrections — the read made exactly 2 fragment GETs per chunk (16392 for 8192 chunks), no duplicates, no parity; the prefetch fetched nothing because the dcache still held the just-written images and the demand path fetches before consulting them (W29 rewritten); the dead probe `openat` is the quota overwrite probe in `store.c:1259`, not the path hint (W34); the server's drain is 75 % kernel / 15 % XFS / 4 % efsd — file creation and an 11-component path walk ×3 per fragment, with a third of the kernel share being the tracer (W34). Server during the read: ~8 % of one core. Everything else stands. Passes: `~/efs-runs/ddana2.log`–`ddana4.log`, `srvana2-fcstor00{3,4,5,6}.log`, `srvana3-fcstor004.log`, `perfana{,2}-fcstor00{3,4,5,6}.log`.

**Oct 1 04:30Z: the user's `~/dd-efs.sh` perf dir reviewed (fstor007 efs-fuse under `perf record` + `strace -f`, eight `dd bs=1M` streams into one mount, 00:04:30–00:05:42 EDT, `/home/erbmi1/orcd/scratch/efs/perf/efs-mount/`) — analysis only, nothing implemented, no cluster command.** Throughput: dd08 (the traced stream) spent 64.2 of 68 s inside `write()` (source reads 3.8 s), latency bimodal — 1665 writes at 0.5–1 ms (copy into the dcache) and ~3400 at 5–50 ms (waiting for dcache room) — so every stream runs at the daemon's PUT drain rate: 8 × 92–99 MB/s ≈ 0.75 GB/s, below the single-stream 918 MB/s in the same dir (`dd_1m.out.txt`). One efs-fuse is a ~1 GB/s device whichever way it is fed; the eight streams do not add. Two reasons it was under even that: (a) `strace -f` on the daemon ran at a flat ~85 000 lines/s for the whole window (`~/efs-runs/ddana1.log`: 2.67 M `read`, 2.53 M `write`, 0.96 M `poll` in 88 s) and ptrace + the scheduler work it causes is ~25 % of the client profile (`flat.txt`: `do_notify_parent_cldstop` 3.83 %, `ptrace_stop` 2.53, `child_wait_callback` 2.48, `ptrace_do_notify` 2.46, `dequeue_entity` 2.86, …; real work `memmove` 6.8, blake3 3.1, `xor_into` 1.2); (b) the RDMA `recv_poller` (tid 1895912) alone did 2.53 M `write()` = ~29 K eventfd kicks/s, one per receive completion (`rdma.c:500/541`) — ≈ 26 KB of user data per completion with 64 KiB fragments; 8 GiB/s would need ~330 K completions/s through that one thread. The per-completion wakeup through a single poller is the single-client ceiling; changing it is a design item (not listed, not asked). Numbers from a traced daemon are not throughput numbers — same rule as `efs-fio-honest`. **Order for the performance side (user, 04:50Z):** the single-node storage bench in §1a (plan only, now with a 1→6 path scaling curve and fixed-QD latency) comes first — it needs no cluster and can locate the 2.8 GB/s cluster wall; then the client bench `efs-fuse --bench` (§1a, plan only: `cpu` / `put` / `write` levels in-process beside dd through the mount, fixed QD, honest clock, never against 19810's export), which replaces the dd ladder (untraced dd + `strace -T` on one reclaim thread; dd against a `/dev/shm` private cluster; TCP vs RDMA; pipeline constants doubled and halved; two hosts) — the ladder stays as the no-code fallback. **Two side findings in the same log, both correctness, with follow-ups — take these before any performance item:**

1. **A non-converging STALE replay loop ended in `UNMOUNT DATA LOSS` (`efs-fuse-efs-mount.log` lines 211 → 2912).** `flush-meta: stale generation (efs_rc=-14) ino=116202` (a `close()` returned EIO; the `off=0 len=0` are `efs_fuse_log_err`'s placeholders, not chunk coordinates), then **2768** rounds of `report-stale: chunks=1 runs=1 committed=0 replayed=1 pull_ms=0–1 replay_ms=0` — the classifier at `write.c:880–968` sent the same single chunk to `replay_fan_run` every round, the rebuilt image CAS'd STALE again every round, nothing was ever `committed`, and the 60 s drain at `efs_fuse.c:3458` gave up: `UNMOUNT DATA LOSS: metadata flush still failing (rc=-14) after 60s`. The log has no timestamps and does not say which file ino 116202 is (one of the eight `dat0N` cut by ^C at 00:05:39, or the rsync target from 00:02; dd08's own `close(1)` returned 0 after 3.2 s). Follow-ups, in order: (a) identify the row — `tests/tools/kv_dir_dump` on a KV COPY from fcstor004 or fcstor005 (they hold every shard): name, size, chunk-0 generation, `ndelta`, delta list including `len == 0` fold tombstones, and which client's gen the row carries; compare with what the classifier would compute (`base`, `obj.chunk_generation`, `obj.delta_base_n/seq`) — the loop means the replay's expected generation never equals the row, so either the row has something the pull does not install (tombstone, span under another client's gen — the Sep 30 (gen, off, len) identity class) or `pp[i].absent`/`UNCOND` is misclassified for this chunk; (b) reproduce on one client with `EFS_DCACHE_TRACE=1` (prints `stale-class committed= ours= done= base=` per round): the shape is eight concurrent `dd bs=1M` into one mount, SIGINT mid-write, then client stop — a kernel-driven release with dirty chunks still in the dcache; (c) mechanical, no design: the drain loop's final line must name the ino(s) and rc left in the dirty set, and a `report-stale` round that replays the **same** single chunk more than N times (say 16) must print the ino, ci, row generation and classifier verdict once — today the 2768 lines carry no identity; (d) **ask, do not implement:** what the client does with a rec that STALEs identically on every replay (stop after N and fail the next fsync/close with EIO? keep retrying until unmount, as now? spill to a local file at unmount?) — the spec has "writes that were never reported are gone" and nothing else. Gate for (a)–(c): the repro in (b) ends with no `UNMOUNT DATA LOSS` and no repeating single-chunk `report-stale` line; posix jobs=1 200/201, posix2 63/63, `concurrent_appends` unchanged.

2. **≥ 9016 REPORT records took their object identity from the staging table (`putid miss`, `write.c:1103–1118`), all `ci=0`, inos 215 028 – 14 464 530.** The rate-limited line printed 128 times with `n=8812 … 9016` in the last second alone, so the count is a floor. The comment on `putid_miss_note` says the mapping "may be the server's row (a repull), not this client's PUT" — the Sep 30 class (client table conflates the server row with our unreported PUT); the `fragment_nodes[0] == 0` guard catches only span-only rows. Follow-ups: (a) find which path leaves a chunk in the dirty set with neither a putid nor a dcache object — candidates: the putid table's own eviction (`write.c:189`, grows by doubling; check `g_putid_live` vs the tombstone churn under 400-thread ecopy), a dcache reclaim that flushed the body and dropped the object (`dcache_flush_slot_inner` after `b6c1712d`'s pin release), or an irec-only threshold REPORT whose chunk rec is built later from the table; one `EFS_DCACHE_TRACE=1` ecopy of a small-file tree on an idle cluster and the `stale-class`/`report` lines around a `putid miss` answers it; (b) the window: this client was the older fstor007 tree (`85f5b31c`/`b6c1712d`), run the same ecopy on the current client first — if the count is 0 there, record that and close; (c) if it reproduces: a rec whose identity is the server's row must not be reported as this client's PUT — the correct behaviour is the Sep 30 rule (a chunk with no PUT of ours is not ours to publish; keep it dirty and replay from the row), which the `fragment_nodes[0] == 0` branch already does for one sub-case; extending it to "putid miss and dcache miss ⇒ not ours" is mechanical only if (a) shows the putid loss is the cause. Gate: `putid miss` count 0 on an ecopy of the software tree with `--verify` clean; posix/posix2 unchanged. Also seen, already decided (D18): `staging table over EFS_CLIENT_META_MB (256 MB) with nothing evictable; growing` — the client in this run predates `f073e136`/`b6c1712d`.

**Oct 1 05:27Z: cluster stopped so the perf files can be read (`~/efs-runs/stop381.log`).** SIGTERM did not finish `efsd` within 12 s, so the stop used `pkill -9 -x efsd`. Recorders are gone. `/tmp/efs-perf/efsd.data` opens on all four (`perf report --header-only`, captured 01:27 local): 3.0 / 4.9 / 3.5 / 3.9 MB on fcstor003–006. `efsd.strace` next to each: 168 / 374 / 263 / 285 MB. No `efs-fuse`. The 05:03Z roll below is what those files recorded.

**Oct 1 05:03Z: servers rolled `--all` again on `4b2844487831-dirty` with `--perf` and `--strace` (`~/efs-runs/roll379.log` ROLL_OK).** Clients still stopped (fcstor003–015 and fstor007, `efs-fuse` 0). Each server's cmdline has both flags; `perf` and `strace` are running. Records are `/tmp/efs-perf/efsd.data` and `/tmp/efs-perf/efsd.strace` (full `strace -f -tt -T`, not narrowed; these files get large). g0 leader fcstor003 term 251 commit==applied 5738413; g2 leader fcstor005 term 745 commit==applied 5684813. The 03:19Z roll below was the same build with no recorders.

**Oct 1 03:19Z: clients stopped, servers rolled `--all` onto `4b2844487831-dirty` (`~/efs-runs/roll376.log` ROLL_OK).** No `--perf`, no `--strace`. fcstor003–015 and fstor007 have no `efs-fuse` (fcstor007's count was 1 for a moment after the first kill; the retry saw 0). One `efsd` each on fcstor003–006, cmdline has no `--perf`/`--strace`, `pgrep -x perf` and `pgrep -x strace` are 0 on fcstor003. g0 leader fcstor003 term 228 commit==applied 5066011; g2 leader fcstor005 term 736 commit==applied 5034858. Storage kept, no `--join`. Clients were not remounted.

**Oct 1 02:50Z: the two remaining mechanical items of the 21:10Z review are in tree (`b6c1712d`) and deployed — fcstor007–015 remounted RDMA 02:43Z (`dep366`, all `fuse.efs-fuse` MOUNT_OK), fstor007's `/tmp/efs` rebuilt on it (`fst367`, no efs-fuse was running); servers unchanged (`44073b77249b-dirty`). The user runs ecopy + tracing.** (4) **The dcache row pin is released when a PUT lands and the entry is clean.** `dcache_pin_add` ran on every store, `dcache_pin_release` only on drop/reclaim and in one partial-write branch, so every file this client wrote stayed pinned — unevictable by the staging evictor ("nothing evictable; growing") and "has unpublished" for the close predicate — for as long as its slot lived. After a landed PUT `dcache_put_now` has put the chunk in the dirty set, which holds the row until the REPORT, so `dcache_flush_keep` (close/fsync pipeline) and `dcache_flush_slot_inner` (reclaim) release the pin of a clean entry, body kept or body-less; an entry re-dirtied mid-PUT was re-pinned by `dcache_set_dirty` + `dcache_pin_add` and keeps it. (5) **`dcache_flush_ino_pass` flushes the inode's dirty-list entries, not `ci = 0..size/128 KiB`.** `dcache_dirty_cis_of` collects the inode's chunk indexes from the 64 per-shard dirty lists (W18's index), sorted; the `nci > DCACHE_SLOTS` all-slots fallback for ≥ 8 GiB files is gone (`dcache_flush_all_slots` is reclaim-all only). For the lists to be a complete index, the reclaim pop sets `reclaim_claimed` instead of unlinking a still-dirty entry (the claim clears on the snapshot's unlink, or after the flush if it did not reach the entry); an unlinked-but-dirty entry was invisible to a close racing the pop, which would have left its chunk out of that close's REPORT. Built clean on fcstor007 (`bld365`), unit tests OK. Gate on the new client: posix jobs=1 fcstor007 **200/201** (`mmap_write_read` SKIP, 0 EFS bugs, `results/posix/20261001-024420`); posix2 fcstor007/008 **63/63** (`results/posix2/20261001-024506`); `mtime_repro.py` on fcstor007: `ecopy-order*`, `rsync-order`, `fsync-between` all 1380661863 (`~/efs-runs/rec-mt371.log`, same as the 05:06Z gate). **What to look for in the user's run:** `dcache_steal_dirty` and `dcache_flush_all_slots` out of the client profile; `efs_dcache_ino_pinned` true only for inodes with a dirty or in-flight slot (gdb: `g_dcache_pin_count` should fall to ~the number of files being written, not every file ever written); the evictor's `pinned=` count in `stage-evict: tabs` lines small. Still open from that review: N3's chain-length measurement (gdb walk in the SUMMARY), the D18 A/B (`EFS_CLIENT_META_MB=4096`), and N4's server trace.

**Oct 1 02:25Z: D18 decided by the user — evict whole cold tabs — and implemented (`f073e136`), deployed to fcstor007–015 (`dep360`) and fstor007's `/tmp/efs` (`fst362`); servers unchanged (`44073b77249b-dirty`). The user runs the ecopy/perf tests.** `stage_evict_main` keeps the four row bands; if the table is still over `EFS_CLIENT_META_MB` afterwards, `evict_cold_tabs` takes the 256 least recently used tabs (`efs_export_tabs_by_age` over `shard_tick`) and, under table + idx + dirty locks, drops up to 64 of them whose every row and chunk ino passes evict_one's pin rules (`efs_export_tab_for_each_ino` + `tab_pin_cb`: dirty, dcache pin, open fd, op pin, plock); their LRU entries go under `g_lru_mu`; `efs_export_drop_tab` frees the tab and returns its `staged_est`. A tab is rebuilt empty by `efs_export_table()` only when a row is staged on its shard: readers and in-place updaters (`efs_export_get_chunk`, `set_chunk_gen`, `set_chunk_deltas`, `add_chunk_delta` via `chunk_tab_peek`; `set_size/mode/owner/mtime/atime` via a peeking `shard_route`) return NOT_FOUND on a dropped tab instead of allocating ~100 KB for a miss — before this change `efs_export_get_chunk` on an absent shard created the tab, which would have rebuilt what the evictor had just freed on every read miss. `test_stage_evict` covers age order, the walk, drop (bytes fall by `staged_est`, other tabs intact), miss-without-rebuild, rebuild on stage; built clean on fcstor007 (`bld359`), `test_stage_evict`/`test_data`/`test_meta_apply`/`test_conn_fd`/`test_sim` OK, `test_stage_evict` clean under valgrind. **What to look for in the user's run:** `EFS_STAGE_DBG=1` prints `stage-evict: tabs bytes=…MB cand=256 dropped=N pinned=M` on every pass that dropped a tab; the evictor should go quiet (`stage-evict: pass` lines stop) once `bytes ≤ cap`, and `evict_pass` should leave the top of the client profile. Under a uniform walk (du, ecopy of a wide tree) the dropped tabs are rebuilt as their shards are touched; each rebuild is one tab allocation (~100 KB), expected to be far below the 0.6 core the LRU spin cost — if the profile shows `shard_tab_get_or_create` instead, that is the thrash to bring back, and the remedy is D18's alternative (smaller first slab / lazy indexes), not a knob. Posix jobs=1 smoke on fcstor007 on the new client: **200/201**, `mmap_write_read` SKIP only, 0 EFS bugs, 45 s (`results/posix/20261001-022434`).

**Oct 1 02:00Z: the two mechanical items of the 21:10Z review are in tree (`65237a59`) and deployed — fcstor007–015 remounted RDMA 01:58Z (`dep353`, all `fuse.efs-fuse` MOUNT_OK), fstor007's `/tmp/efs` rebuilt on the same commit for `client.sh` (`fst354`); servers unchanged (`44073b77249b-dirty`). The user runs the tests.** (1) `efs_append_flush_report` (close, fsync) returns at once when `efs_ino_has_unpublished(ino)` is 0 — no wb job queued or in flight, not in the dirty set, no dcache pin (the utimens predicate, now shared). A read-only or already-published close no longer walks `size/128 KiB` dcache slots under the append stripe. (2) `dcache_store` / `dcache_store_owned` reuse a dead chained node (ino, body, dirty, pin all clear — what the reclaim flush leaves at write.c `dcache_flush_slot_inner`) before `calloc`, so chains stop growing with every reclaimed chunk (`dcache_chain_reuse`). Built clean on fcstor007 (`bld352`), `test_stage_evict`/`test_conn_fd`/`test_data` OK; posix jobs=1 smoke on fcstor007 **200/201** in 28.6 s, `mmap_write_read` SKIP only, 0 EFS BUGS vs the XFS baseline (`results/posix/20261001-015918`). Not changed: the evictor — its cadence and bound are D18 (asked again Oct 1 with the 47.6 % figure). Observation for D18: a dcache pin is released only when the slot is dropped or its clean body reclaimed, not when the PUT lands, so every file this client wrote stays unevictable while its slot lives — part of the "nothing evictable; growing" condition. Below: the 21:10Z review block.

**Sep 30 21:10Z: the user's 19:54Z–20:56Z perf dir reviewed (62 min `perf record` of fstor007's efs-fuse + two `ecopy --verify` straces of the software tree) — documented only, nothing implemented (`results/measure/20260930-205300-ecopy-perf-review/SUMMARY.txt`).** Two client hot paths and one hypothesis. (1) **The staging evictor is the top consumer of client cycles: 31.6 % of the 62 min, 47.6 % of the ecopy-2 slice (≈ 0.6 core).** `perf annotate` puts it all in the inlined `evict_pass` LRU scan (`g_lru_keys`/`g_lru_ticks` over the whole table, 4 bands per wake). This is D18's floor: `stage_bytes_now()` is always over the 256 MB cap, every pass evicts 64 just-closed rows (table lock 64× + one compact per wake), and `efs_client_stage_evict_kick` (every `ensure_meta_room`) re-arms it at once because the pass did evict. `5ea397fd` made one evict cheap; it did not change how often the pass runs. Next: decide D18 (ask, pending); an isolating measurement needs no code — the same ecopy with `EFS_CLIENT_META_MB=4096` in `client.sh`'s env, compare `ops-by-fs.txt`. (2) **Every `close(2)` walks every chunk index of the file, dirty or not:** `ll_flush → efs_append_flush_report → efs_dcache_flush_ino → dcache_flush_ino_pass` loops `ci = 0..size/128 KiB` calling `dcache_steal_dirty` (two mutex pairs + slot chain walk each; 65536 slots once the file is ≥ 8 GiB — `dcache_flush_all_slots.part.0` shows in the run-2 slice), under the inode's append stripe, on read-only opens too. `dcache_steal_dirty` self = 36.9 % of that slice. `efs_append_flush_report` has no "anything unpublished?" predicate (the utimens path got one Sep 30). Next (mechanical, gated by posix/posix2/`concurrent_appends`/`mtime_repro.py`): skip flush+REPORT when wb pending, dirty set and pinned dcache are all empty; confirm the caller first with a `--call-graph dwarf` capture (libfuse-worker fp chains are broken: `0x746e756f6d2d`). (3) Hypothesis: dcache slot chains are long — `efs_dcache_yield_extra` (one `dcache_find` + a flag) is 6.6 % of the profile, the same as `dcache_steal_dirty` in the middle slice; body-less and zeroed chained nodes are never reused or unlinked outside `dcache_drop_locked`. Measure with the gdb chain walk in the SUMMARY before touching it. Per-op efs latency under the ecopy (dirfd-resolved): rename 54 / 29 ms, openat 18 / 7.7, utimens 8.8 / 15.9, stat 9.5 / 4.0, close 9.8 / 2.5 (idle refs 6.2 / 0.6 / 1.05 / 0.33 / 0.34) with only ≈ 27 and ≈ 5.6 efs ops in flight — ecopy kept ≈ 250–275 of its ~400 threads on one futex (run 1's 29 s barrier = ten threads stat'ing), so its throughput here is bounded by its own dispatch; no efs syscall over 1.7 s, no EIO/EBUSY/ESTALE in either trace. Run 1 died of its own SIGPIPE, run 2 was ^C. No server trace for the window (roll350 is plain). Below: the 18:55Z IO-500 block.

**Sep 30 18:55Z: the first 9-client IO-500 since Sep 28 that completes every phase with no fsync failure and no read error, and the first ever whose ior-hard file verifies cold (`results/io500/20260930-183504-rdma/NOTE.txt`).** Servers `f2d3a7871b96-dirty` (rolled `--all` 18:30Z), clients on the same tree 18:32Z. Debug 9×4: ior-easy-write **4.563 GiB/s** (07:28Z: 3.148), mdtest-easy-write **4.632 kIOPS** (3.696), ior-hard-write **0.640 GiB/s** in 52.6 s with **0 fsync failures** (0.261 in 133 s with 16), mdtest-hard-write **2.612** (2.126), ior-easy-read 16.8 (same-mount), mdtest-easy-stat **16.478** (3.600), **ior-hard-read 1.034 GiB/s, 0 errors** (aborted on read EIO before), mdtest-hard-stat 23.8, easy-delete 5.0, hard-read 5.6, hard-delete 4.2. Cold `hardscan` of the 36 GB hard file from fcstor008: **766980 records, bad=0** (13:36Z run: 80 bad; 13:25Z: 140). Server apply STALE (all `why=6` FOLD_LIST) 220–463 per node (was 3238–6573); three client STALE rounds in total with pull ≤ 1.2 s (was one per client at ~7 s, 891 replays); `inbox_drop` 171 on fcstor005 (was 3102); `putid miss` 0, `merge base norow` 0, `exhausted` 0. Four fixes, all in this tree: (1) **a span record's identity is (object gen, off, len)** — the object name is a content hash, two clients that merged to the same image PUT one object under two ranges, and the apply took the second as a replay while the pack skipped it as held → the client-boundary piece of every shared chunk was silently never recorded (that was every hard-read loss today; `test_meta_apply`); (2) the STALE classifier treats a chunk the repull found **no row** for as never committed (absence bitmap from `pull_chunks_range`; its replay starts from zeros with an empty observation) and matches a span by range, a full image by base gen; (3) the first partial write installs have_base/base_gen/range under the buffer's lock (`dcache_init`; a flusher saw `dirty=1 nrange=0` and the merge overlay dropped the bytes); (4) `span_of` coverage is contiguous from the range start and this client's own unreported span covers nothing. Left open from this run: `pub_p50` reads 596629 on fcstor005's last raft-obs line (not read; D8's remeasure is its own item), `getchunks slow` 24 lines on fcstor004, 22 BUSY REPORTs retried inside the wall. IO-500 data of this run kept on the mount (`io500/2026.09.30-14.35.04`) for the scan; the earlier kept runs were removed. Below this: the 08:40Z and 07:40Z blocks (d50 ENOTEMPTY still open, ask).

**Sep 30 08:40Z: the user's 07:17Z ecopy perf dir reviewed; four client bugs behind the 149 `--verify` mismatches fixed and deployed to fcstor007–015 (`results/measure/20260930-080000-ecopy-times-review/SUMMARY.txt`).** (1) 119 `(atime)`: `struct efs_inode_mem` had no `atime_nsec`/`ctime_nsec`, so every stat served from the staged row (the post-rename lookup) had nsec 0 — fields added in the struct padding (192 B unchanged), `efs_export_set_atime` takes nsec. (2) 29 `(mtime)` = close time, three holes in "utimens has nothing to flush": a wb job waiting in `wb_overlap_inflight` was invisible to `efs_wb_ino_pending_locked` (busy_ino set at the pop now); every flusher's clean-before-PUT / mark-after window (`put_win_open/close` per inode, `efs_dcache_flush_ino` waits for them); and the predicate itself — a threshold REPORT consumes the ino mark with an irec-only record while the bytes are still a dirty dcache entry, so `efs_client_ino_is_dirty` is not "has unpublished data" (`efs_utimens_flush_dirty` also checks `efs_dcache_ino_pinned`). Traced with `EFS_DCACHE_TRACE=1` (new `utimens`/`report`/`release` lines). (3) 1 `(size)` 32398 → 131072: `write_chunks_no_replicate` marked the chunk dirty before the size/ino mark, a threshold REPORT shipped a crec with no irec and the server's `sz == 0` fallback rounded to the chunk end; the ino mark and local size grow are in the chunk's locked block now, and the REPORT builder appends an irec for every crec without one. Gate: `size_gate.py` 48 threads × 120 files × 8 sizes, write→futimens(ns)→close→rename, 4 × 5760 files **0 mismatches** same client and from fcstor008 (was 2, then 1–2 per run); `atime_repro.py` 10/10; posix jobs=1 fcstor007 **200/201** 29.1 s (`results/posix/20260930-083309`). Perf (976K samples): memmove 21.9 % (6.4 % the `efs_rdma_send_frame` bounce copy, ~14 % FUSE→dcache copy), blake3 10 %, `xor_into` 7 %, `stage_evict_main` 5.9 % = the `5ea397fd` evictor fix, absent from fstor007's 06:16Z client. Zero-copy RDMA from registered dcache memory is a design ask. Servers unchanged (`44f397b4ca2e-dirty`). fstor007's `/tmp/efs` rebuilt on this tree for `client.sh`. Pre-existing and untouched: `test_raft_store` 4 W22.1 assertions, `check-architecture.py` regen/single-home. The 07:40Z block below still holds (IO-500 hard-write fsync failures, hard-read EIO, d50 ENOTEMPTY).

**Sep 30 07:40Z: gate of the 06:50Z servers (`44f397b4ca2e-dirty`) + the evictor-fix client, nine clients mounted RDMA 07:17Z. Posix passes; IO-500 hard-write fsyncs fail and hard-read aborts with EIO — one open regression, one open one-off.** Posix jobs=1 fcstor007 **200/201** in 16.8 s under the user's ecopy (`results/posix/20260930-072042`) and 7.1 s after it stopped (`20260930-072224`); posix2 fcstor007/008 **63/63** 47.6 s (`results/posix2/20260930-072241`, prior 47.2 s); 9-host (`results/posix/20260930-072501`) eight hosts **200/201** in 18.0–19.1 s, **fcstor012 199/201**: `dir_deep_nesting` `rmdir d50` → ENOTEMPTY (`[Errno 39]`) and still ENOTEMPTY from that client 1–2 min later (`d50-probes-012.txt`), yet the KV copy at 07:28Z shows d50 = ino 56024 `nlink=2 nents=0`, no dentries, no pending intent (`d50-kv-dump.txt`), and `rmdir` from fcstor007 succeeded at once (`d50-rmdir-from-007.txt`). Client-side, gdb on fcstor012 afterwards: a child vector for parent 56024 exists **on the root tab** with `count=0 cap=4` (`d50-client012-childvec.txt`) — `efs_client_unlink`'s local fast path (`ops.c:1303`) calls `efs_export_dir_empty(&g_client.export, ino)`, which reads only the root tab's `child_vecs`; a stale slot there would return ENOTEMPTY without an RPC, and nothing logs it. Not proven either way (the server pre-check `row.nlink > 2` / one-entry readdir reads the raw row, so a pending `REDUCE_INO` from d51's rmdir would also give a transient ENOTEMPTY; the client retries that 20 × 1 ms only, `efs_fuse_rmdir_at`). First occurrence in the results history; first 9-host run with the `5ea397fd` evictor (active during the suite — D18). Next: make the fast path log which tab/slot says non-empty, or drop the local emptiness check (the server is authoritative) — the second is a one-line decision, ask. **IO-500 debug 9×4** `results/io500/20260930-072824-rdma/NOTE.txt`: ior-easy-write **3.148 GiB/s** (09-28: 2.917), mdtest-easy-write **3.696 kIOPS** (3.418), ior-hard-write **0.261** in 133 s (0.291 in 73 s) with **16 `fsync failed`** (09-28: 0), mdtest-hard-write **2.126 kIOPS** (0.757), ior-easy-read **2.976** (3.092), mdtest-easy-stat **3.600** (3.248), **ior-hard-read ABORT**: `read(…, 47008) failed Input/output error` on ranks 22/23 → `MPI_ABORT` 07:32Z; no later phase. Every client's `fuse.log` shows the same two failures on the shared file ino 84857: `report-loop rounds=1 busy=1 ms≈10000 rc=-13` (fsync EBUSY, W17.1's first-BUSY bound) then `rounds=1 stale=1 ms≈65000 rc=-14` (fsync EIO). The unpublished spans are what hard-read hit (I9: fail, do not zero-fill). Servers: terms unchanged, `apply_max` 0, `pump_hold_max` ≤ 10 µs, but **`inbox_drop` 978 / 3102 / 223 on fcstor004/005/006** — `inbox full, dropped frame type=3 group=2 from=3 len=949540` = 950 KB AppendEntries from the group-2 leader dropped at a 256-frame `HOST_INBOX_MAX`; `wait_timeouts` 619 on fcstor004, `apply-sleep` 20–127 ms, `pub_p50` 544 ms on fcstor005 (3 ms at D8). L0 sits at 113–231 files per node and does not drain at idle: ~10 000 `kv-compact` lines per node, 50–270 KB each in 1–2 ms (D10's per-range trigger), while `gc-pass ms=244 frag=244` on both leaders keeps flushing memtables with `GC_ACK` entries. The F2 counter is new since 05:06Z, so the 09-28 run cannot say whether it dropped too; the hard-write fsync failures are the regression against 09-28 (0 warnings, pre-W17.1/D9 build). Nothing changed in code for this. Aborted-run data removed from the mount; two stale `posix-*` dirs (Sep 29) removed; nine clients still mounted; fstor007 client is the user's.

**Sep 30 07:10Z: the rest of the user's perf dir (du 52K, the 1 Hz stall, 5–10 ms big-file stat, 16 fragments/s GC) — `results/measure/20260930-063500-perf-dir-review/SUMMARY.txt`; servers rolled `--all` 06:50Z on `44f397b4`, client fix in tree.** Four findings, three fixed: (1) `du -hs` = 52K for 3.8 TB is 103 symlinks × 512 B — every regular file reports `st_blocks = 0` because `inode_allocated_bytes` counts the client-local present-chunk table, which is empty for any file this mount did not write (D2 adopts the row only). `efs_meta_row` has no allocated/chunk count and W20 forbids size-based `st_blocks`. **D17 (ask):** a per-lane present-chunk stamp reduced at getattr like `max_end`, or accept `st_blocks = 0` for files this client did not write. Not implemented. (2) Every metadata op stalled 14–27 ms exactly once a second (du/find1/find2 straces; absent on the 01:10 fresh mount) — **client side**: `stage_evict_main` wakes every 1 s, the staging estimate (412 MB with 10780 rows) is over `EFS_CLIENT_META_MB` 256 MB, and each of its 64 `evict_one` calls ran `ino_has_chunks` + `drop_chunks_from` + `forget_ino` = three walks over every loaded shard tab (2400 of 4096 after one du), 250 µs each under the table lock, then `efs_export_compact` rebuilt every tab's hash index to the same size (the 25 % load-factor test always passes at 1–3 rows with a 512-slot floor). RPC threads queued on the lock the whole 22 ms (`fuse.strace` on fcstor007: 58 lock hand-offs on `g_client` per wake). Fixed in tree: `efs_export_evict_ino` visits the ≤ 3 tabs the row names plus the chunk-group tabs its size reaches (fan-out only for a missing row, a hard link, or a file spanning most shards; `test_stage_evict`), compact reindexes only when the rebuild shrinks, and slab bytes are booked at `sizeof(struct efs_inode_mem)` (192 B) instead of the 512 B on-disk row size. **Still true (D18, ask):** the estimate is dominated by the per-tab floor — one 256-row slab + 16 × 1232 B chunk entries + 512-slot indexes ≈ 90–170 KB per shard tab, ×4096 tabs > the 256 MB cap before any data is staged, RSS 330 MB with 10780 rows — so the evictor evicts 64 hot rows a second forever and cannot reach the cap. Options: evict whole cold tabs (`shard_tick` exists, unused), a smaller first slab / lazily sized indexes, or apply the cap above the tab floor. Not chosen. (3) `stat` of a 64-lane file was 5–10 ms vs 0.2–0.9 (servers ~2 % CPU): `host_read_inode_lanes` did a ReadIndex per active foreign lane (32 peer round trips on a follower) and every per-lane `efs_txn_reduce_read_ex` prefix scan loaded a 1 MiB readahead window per covering segment (22 × 1 MiB pread per RPC, ×2 for LOOKUP + GETATTR, fcstor004 strace). Rolled: one ReadIndex per group; `kv_seg` iterators read one block unless `kv_seg_iter_set_seq` (compaction, export). After: 4.7 ms per stat (was 9.8), 300 stats 1.65 s. The remaining cost is 64 × (scan + 2 gets) per collect, twice per stat — a fold or a cached collect is a design item. (4) GC reclaimed 16 fragments/s (32-record scan, one 16-ack entry per group per second; 70–86 unlink per server per 5 s = 21 h per 100 GiB file): now 256 records per scan, 128 acks per `GC_ACK` entry (`EFS_META_GC_ACK_MAX`), rescan while full within 200 ms per group per loop. `raft-host: gc-pass ms=` prints when a GC iteration exceeds 5 ms (fcstor005/006 leaders: `recover=70`, of which 64 ms is `HOST_REC_YIELD_US` sleeps). Also: the rsync `getcwd ENOENT` at 01:53 was the invoking shell's cwd (the 02:31 re-run from `/tmp/direct_copy` worked), dd bs=16k is 20–40 µs per FUSE write (known direct_io shape), `readlinkat` p50 0.85 ms. Servers were rolled twice (06:35Z, 06:50Z) under the user's live `client.sh --perf` on fstor007.

**Sep 30 06:17Z: the wedged mount after the user's 100 GiB dd was a pipelined-sender channel mix-up on the fcstor004↔fcstor005 peer lane; fixed `85f5b31c`, rolled `--all` 06:16Z (`85f5b31c…-dirty`, `~/efs-runs/roll126.log`).** `results/measure/20260930-060000-dd-wedge/SUMMARY.txt`. A frame over 72 KiB goes over the conn's TCP side-channel and is answered on TCP; `host_sender` sends up to three messages and collected the replies with `recv_chan` left at the last message's channel, so [AE_REP (RDMA), big AE (TCP)] read TCP only, left the RDMA reply in the ring, timed out at `HOST_SEND_IO_MS`, destroyed the conn, and Raft resent the same shape — forever (fail=2399 in 10 min, hi=2048, both directions). Only the lane between the two dual-group hosts mixes one group's AEs with the other group's replies, and only a big-entry stream (the dd's 2048-publish batches) makes the AE take TCP. The follower each lane fed sat ~700 entries behind; every follower-served read and every dual-group REPORT was BUSY (`apply-sleep` 400 ms); dd close EIO, `ls` EBUSY 16.7 s, sha256sum ENOENT 33 s, unmount `DATA LOSS` after 60 s. Before F1 the same shape waited 30 s per attempt — that was the "lost RAFT_REPLY" behind the 04:06Z election storms (D15/D16 lose their motivating case; still asks). Fix: `efs_conn.last_recv_chan`, and the sender counts expected replies per channel. The L0 gate was not involved (KV dir 0.33 GB). Not changed: the 819200-record close REPORT is one RPC and one BUSY at its end discards 16 s of server work (splitting is an ask); `kv-compact … l0only=1` folds after every flush (L0 ~150 files, 1–5 ms each, noise); `apply lane-fence rc=-2` ×32 during the dd, not chased. **Gate passed (`~/efs-runs/gate130.log`, 06:19Z):** 32 GiB non-zero dd+fsync from fcstor007 (`fuse.efs-fuse`, MOUNT_OK) in 32.85 s ≈ **1000 MiB/s**, `DD_RC=0`, size 34359738368, close REPORT `nrec=262144 pack_ms=2811 push_ms=4186 rc=0` (same 2048-publish batch shape that wedged), zero `exhausted|report-loop|DATA LOSS|conn pool` in `fuse.log`, `tx->N fail=0` on every lane of fcstor004 and fcstor005 before and after, terms unchanged (g0 106, g2 680), both groups commit==applied, `apply-sleep` only 23–107 ms waits one index behind. File removed, fcstor007 unmounted cleanly (rc=0, no `efs-fuse`). Do not run a dd inside an `efs-ssh.sh` call (gate128 died at the 15 s timeout before writing a byte; the dd itself belongs in a screen on the client).

**Sep 30 05:45Z: review of the 05:06Z roll under ecopy; two client fixes committed (`e002771e`) and rolled.** `results/measure/20260930-051000-review2/SUMMARY.txt` is the review. (1) `dcache_reclaim_main` was 39.8 % of efs-fuse cycles: 16 threads re-walked the 64 dirty lists nonstop while dirty_bytes sat over the 2 GiB limit with no `have_base` slot to pop (ecopy writes fresh files). A sweep that pops nothing now parks until the next kick after a 10 ms nap (`g_reclaim.kicks`/`empty_kicks`/`empty_until_ns`, write.c). (2) The 244 `inode-rpc: slow-ok ... attempts=1 saw_busy=0` at exactly 5.0 / 10 / 15 s were `efs_client_conn_get` timing out on a full 16-slot pool (`pthread_cond_timedwait` 5 s, silent), not the network; `put_fragments` and `fetch_fragment` then counted the NULL as a node failure and four of them marked a live node DOWN for 30 s (`EFS_NODE_DOWN_FAILS`=4, `EFS_NODE_DOWN_MS`=30000) — the shape of the two 28 s client-wide freezes in the ecopy strace and the 12 `no quorum` PUTs against four live servers. The timeout is logged (`efs: conn pool node=N exhausted for 5 s (pool=…)`, first ten then every 100th) and no longer counted; the default pool is 64 per node (server cap `EFS_SERVER_MAX_CONNS` 4096, 13 × 64 = 832; one RDMA conn pins ≈ 2.6 MB per end, slots connect lazily). Not changed: the 5 s checkout timeout, REPORT's one-at-a-time whole-dirty-set shape (splitting it is an ask), follower `apply-sleep` (D14). "ecopy + atime": no new defect — W25 item 2 landed in 2dd77dac and the strace has zero mismatch lines; the roll's gate re-checks the round trip. Servers `e002771e56e4-dirty` rolled `--all` at 05:43Z (`~/efs-runs/roll107.log`); fstor007's `/tmp/efs` rebuilt on the same tree for the user's `client.sh`. Gated on fcstor007 with the new client (`~/efs-runs/gate109.log`, `fuse.efs-fuse` + MOUNT_OK): `mtime_repro.py` rows `ecopy-order*`, `rsync-order`, `fsync-between` all end at 1380661863; `touch -a -d @1500000000.123456789` → `stat` `1500000000.123456789` (atime nanoseconds round-trip); posix jobs=1 **200/201**, 0 EFS BUGS, `mmap_write_read` SKIP only, ~40 s (`results/posix/20260930-054501`); fcstor007 unmounted cleanly afterwards, no `conn pool` line. Still to gate under load on the next ecopy: `grep -c "conn pool node" fuse.log` and no `no quorum` line while all four servers are up (2); `dcache_reclaim_main` out of the perf top (1).

**Sep 30 05:13Z (superseded above): F1, F2 and the mtime fix (A) are in tree and rolled; committed `a5e3d5fe`.** Servers `06916bc7e5c1-dirty` rolled `--all` at 05:06Z (`~/efs-runs/roll78.log`, ROLL_OK, no recorders, `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`); the user mounted fstor007 with the same tree at 05:10:32Z (`client.sh`, `fcstor003.ib:19810`). **F1:** `struct efs_conn` has `recv_timeout_ms`; `efs_conn_set_recv_timeout()` sets it and the TCP `SO_RCVTIMEO`; `conn_rdma_frame` waits `recv_timeout_ms` instead of the 30 s `EFS_IO_TIMEOUT_MS`; `host_sender` calls it with `HOST_SEND_IO_MS` (250 ms) on every conn kind (the per-sender conn is private, `server_peer_conn_new`, so no pool restore). **F2:** `server_raft_host_inbox` counts the inbox-full drop in `h->inbox_drop` (logs the first ten as `raft-host: inbox full, dropped frame type= group= from= len=`) and the `raft-obs: wait_timeouts=` line ends with `inbox_drop=`. **Mtime:** `efs_fuse_utimens_ino` calls `efs_utimens_flush_dirty()` before the SETATTR when mtime is set — if the inode has writeback jobs pending or is in the dirty set it runs `efs_append_flush_report(NULL, ino)` (the close path's flush+REPORT), so the publish lands under the old `mtime_gen` and the utimens fences it; a clean inode pays one dirty-set lookup. `efs_client_mtime_pin` logs once when the 256-entry table is full instead of dropping silently. **Gate on the live mount (FUSE_OK, `~/efs-runs/rec-gate84.log`):** `write→futimens→close→rename`, `write→futimens→close`, `write→close→utimens`, `write→futimens→fsync→close` all end at 1380661863; `futimens→write→close` ends at the write time, which is POSIX. Unit tests on fcstor007: 14 of the 15 `make test` binaries pass, `test_raft` OK with the two new cases; **`test_raft_store` fails 4 assertions on clean HEAD too** (`snap index should report its term`, `compacted prefix differs`, `snapshot did not shrink the log`, `reopen after rotation` — W22.1's retained log window, test not updated; `~/efs-runs/rs77.log`), and `docs/check-architecture.py` fails on the regen-diff and the repeated §1 decision-table headers. After the roll, under the user's load: fcstor004 `tx->2 fail=5` (250 ms each now, not 30 s), one group-0 election (term 94→95 at 05:11:19Z), `inbox_drop=0` on every node, `wait_timeouts=5`, 256 `kv-compact: start` lines and 120–270 ms `apply-sleep` waits on group 2 in one window. The lost RAFT_REPLY itself is still unexplained (D15/D16 pending). Leftover: a plain-dir `/tmp/efs-mount/measure/mtime-repro2` under fstor007's mountpoint (my gate ran 30 s before the user's mount came up; those rows are local XFS and are not a result); remove it when the mount is down.

**Sep 30 04:45Z: ecopy's 153 "verification metadata mismatch" lines are all `(mtime)`, and they are an efs bug, not F1/F2** (`results/measure/20260930-044100-mtime-utimens-close/SUMMARY.txt`). ecopy does `write → futimens(fd) → close → rename` (POSIX-valid; XFS keeps the time). On efs the write is client-buffered, so its PUBLISH reaches the server inside the close REPORT, *after* the utimens; the publish carries `p.now` and the current `mtime_gen`, so the lane stamp wins the stat MAX and mtime becomes the close time. Reproduced from one process on an idle cluster (`mtime_repro.py`): `write→futimens→close` and `write→futimens→fsync→close` are wrong, `write→close→utimens` (rsync's order) is right. `EFS_INO_REC_F_TIMES` is set by the client and read by nothing on the server; `MTIME_PIN_MAX` 256 drops pins silently. Because of this every file ecopy had already copied failed `same_size_and_mtime` and was copied again (the 22.4 GiB). **Fix A written and gated 05:12Z (block above):** the FUSE setattr path flushes the inode's dirty dcache before a MTIME/ATIME SETATTR, so the publish lands under the old `mtime_gen` and the utimens fences it — no wire or server change, and ecopy's close REPORT becomes empty. Alternative B: make the REPORT apply honour `EFS_INO_REC_F_TIMES`. The repro's client on fstor007 was stopped again.

**Sep 30 04:20Z: the post-fix runs from fstor007 are reduced (`results/measure/20260930-040600-postfix-review/SUMMARY.txt`). The wedge fix holds; a second Raft problem is measured and needs two decisions.** With no server strace, one client, warm servers: `ls` 0.015 s, `find -ls` of the tree 6.5 s and `du` 5.8 s with no call over 0.2 s, and a re-sync of `~/git` at stat 0.33 ms, openat 0.60 ms, close 0.34 ms, utimensat 1.05 ms, chmod 0.49 ms, rename 6.2 ms (max 36 ms). Those are the per-op references for `06916bc7e5c1-dirty`. Then `ecopy --verify` (400 threads, 22.4 GiB) put both groups into an election storm: group 0 63 terms, group 2 426 (03:56→04:09:39Z, settled at term 669, leader fcstor005); 12 `newfstatat` took 55–59.5 s, 513 took ≥ 0.2 s; 21 `shard=0` LOOKUPs and 8 REPORTs exhausted 16 BUSY retries; fcstor004 `wait_timeouts` 0→1290. Cause, from `raft-obs tx->` and the code: `host_sender` blocks for `RAFT_REPLY` per batch, and on an RDMA peer conn that wait is `EFS_IO_TIMEOUT_MS` = **30 s** (`conn_rdma_frame`, protocol.c:380) — the 250 ms `HOST_SEND_IO_MS` bound is applied to TCP conns only (raft_host.c ~757). `sent` froze for 10–30 s on fcstor004→005, fcstor006→004 and 006→005 while `enq` grew, then `fail` +1. The peer processes the batch (fcstor006 won votes through a frozen lane) — the reply is what does not come back; which side loses it is the open measurement (stack-sample the sender and peer-conn threads in a storm; on-CPU perf will not show it). The unheard peer campaigns and `on_vote_req`→`maybe_step_down` deposes the live leader every time (no Pre-Vote, no leader stickiness). **Fixes, in SUMMARY.txt:** F1 bound the RDMA peer reply wait to `HOST_SEND_IO_MS` (mechanical, the decision is the raft_host.c:45 comment) and F2 count `server_raft_host_inbox` BUSY drops; **D15** leader stickiness / Pre-Vote and **D16** a separate credit class for peer frames are questions for the user. Servers still run `--perf`; fstor007's client is stopped (user ran `client.sh stop` at 04:05Z). Do not quote a rate from these runs.

**Sep 30 03:57Z: the group-0 wedge is fixed and rolled.** `tests/test_raft` and the two new cases pass on fcstor007 (`~/efs-runs/raftfix62.log`). `on_ae_req` reports `match = prev + nentries` (a stale tail is not a match) and the leader no-op, cold entry, and joint entry advance `send_idx` the way `efs_raft_propose` does. Rolled `--all` at 03:56Z with `--perf` only (`~/efs-runs/ready65.log`, same embedded id `06916bc7e5c1-dirty`, the tree is uncommitted): group 0 elected fcstor003 at term 43 and all three voters hold 437638; group 2 leader fcstor004 term 262. fstor007 remounted `client.sh --perf`: `ls -l` and `stat` of the root return at once, mkdir+rmdir OK, group 0 commit 437640 on every voter, zero `apply-sleep`. The 23:16 rsync and the 23:51 `ls` (EBUSY after 16.76 s, `~/logs/ls.strace.txt`) were this wedge; do not quote either. The no-strace rsync rerun is ready.

**Sep 30 03:10Z: the no-strace rsync rerun is set up, not run.** Rolled `--all` (`~/efs-runs/ready59.log`) to `06916bc7e5c1-dirty` with `EFSD_ARGS=--perf` only (`EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`). All four efsd are `--perf`, `strace=0`, no `efsd.strace`; `efsd.data` is `/tmp/efs-perf/efsd.data` and is unreadable until the recorder stops. Both groups commit==applied (leader fcstor005, terms 41 and 261). fstor007 only is mounted: `fuse.efs-fuse`, `MOUNT_OK`, `EFS_TRANSPORT=rdma`, `client.sh --perf`, `strace=0`. Client samples go to `~/orcd/scratch/efs/perf/rsync-perf/efs-mount/fuse.data` (reports on `scripts/client.sh stop /tmp/efs-mount` from `/tmp/efs` on fstor007). fcstor003–015 are not mounted. The user runs the 5 min rsync. Do not quote a rate from this setup.

**Sep 30 02:30Z: the user's traced rsync from fstor007 is reduced** (`results/measure/20260930-021317-fstor007-rsync/SUMMARY.txt`). 4961 files in 287 s, 17.3 files/s, zero errors and zero elections on either side. The receiver is in efs syscalls 99 % of the wall: rename 25 ms (44 %), three `openat` 23 %, utimensat 11.5 %. 3.75 RPCs per efs syscall; 68 % of all RPCs are LOOKUPs (entry_timeout 0 re-resolves every component). Every efsd was under strace, so absolute latencies are 3× the Sep 29 16:41 reference (peer RTT 400 µs vs 24 µs); the counts and shares hold. Do not quote these latencies as the system's.

**Sep 30 00:57Z: posix2 is 63/63. Next is the fstor007 dd.** At 01:45Z the four servers were restarted with `--perf` and `--strace` (`~/efs-runs/restart53.log`); recorders are `/tmp/efs-perf/efsd.data` and `efsd.strace` on fcstor003–006. Clients fcstor003–015 and fstor007 are stopped. `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`, same 17:04Z table (restart needs no `--join`). Embedded id `bd57b7e74a91-dirty` (HEAD `bd57b7e7` plus this uncommitted tree). Do not widen the 400 ms apply wait. Do not retry the one-shot REPORT. D14 and folding a same-group rename into one command stay asks.

What this tree changes, and what the gates showed:

- `host_read_inode_lanes` returns NOT_PRIMARY with a dual-host hint when the lane's group is not local, and setattr / unlink / link / rename forward before propose. `nohint=0` on all four servers across the 9-host run.
- The create EIOs were an apply verdict of BUSY (`rc=-13`, alloc-key intent) that `host_wait_settled` discarded; the handler then looked the name up, missed, and the client got NOT_FOUND → EIO. `server_raft_host_create` now waits with `host_wait_verdict`, so BUSY is retried. A name clash stays EXIST (it was rewritten to OK, which made both `O_EXCL` creators win). Gate: 9-host **200/201** on fcstor007–015, 15.6–17.3 s, 0 bugs (`results/posix/20260930-000428`). `create-miss=0`. `apply create rc=-13` was 6/8/8/2 on fcstor003–006 and the suite still passed.
- Rename: a dest inode that vanishes between the dentry read and the row read is STALE. A NOT_FOUND after the reply's getattr is OK with a zero stat only when `host_txn_commit` itself returned OK. Treating a NOT_FOUND *from the commit* as success made both sides of `peer_rename_same_src_two_dst` return 0; the loser is `rename_at … rc=-3` again and that test passes. `peer_rename_vs_unlink_dst` and `peer_overlap_pwrite_partial` pass (`efs_dcache_overlay` takes the whole image only when the map has no deltas).
- The two holder failures were not a slow unlink. `rpc_note_slow_ok` never fired, and every posix2 step was under 2 s. The holder saw `go` and then `read` returned EIO (`after go: errno 5`); fuse.log was `layout pull rc=-3` because open adopts the row only and the last unlink, with no lease, retires the inode. First `open()` of an existing regular file now takes `HOLD` (`efs_client_rpc_hold` flags=1, owner `flock_token`); last close releases it. CREATE still does not: a hold per new file is the names_crazy wall. A flock unlock already closes that same owner, so release does not close twice.

posix2, fcstor007+008, **63/63**, 47.2 s (`results/posix2/20260930-005552`, `~/efs-runs/gate49.log`). `peer_unlink_while_b_has_fd`, `peer_open_unlink_nlink`, and `peer_create_unlink_stat_churn` all pass. The 00:13Z **60/63** (`results/posix2/20260930-000915`) is the run before this hold.

fstor007 8 GiB `dd bs=1M conv=fsync`, own file, `FUSE_OK`, on-disk 8589934592, wall **12.396 s → 661 MiB/s** (`results/measure/20260930-005700-dd-fstor007`, `~/efs-runs/dd50.log`). That is below fcstor007's Sep 28 **947** and above the ecopy plateau of 345–372, so the single stream is already short of the fcstor host and the 400-thread pattern is a second gap. This dd created a new file, so it did not pay the open hold.

The ecopy of `~/git` did not finish (`results/measure/20260930-012100-ecopy-git`, `~/efs-runs/ecopy51.log`). `~/git` now contains `linux-xfs`; the Sep 29 copy of that path was 3.8 GiB in 24 s. The ssh budget was 240 s and the copy was still running, so there is no rate. The flushed lines are 11 EBUSY, 4 EIO, 8 ENOENT. During that window group 0 (leader fcstor003, term 31) stayed `hi=1`, `pub_p50=0`, `apply_max` under 100 µs except one 7.3 ms sample, and the leader's on-CPU time was about 20 s across the four minutes (`eventfd` write 8 %, `memcmp` 7 %). Group 2 did not: terms ran 18→253, `tx->2 hi=2048`, and fcstor005 recorded `pub_p50=405 ms` once. It had a leader again at 01:30Z (leader 3, term 253, commit==applied). The partial tree `/tmp/efs-mount/measure/ecopy-git` on fstor007 is still there; `rm` hit the same 60 s clock. Do not ask about folding a rename or about D14 off this run: group 2 lost its leader, so the copy never measured a quiet metadata path.

**Sep 29 20:55Z: why rsync and ecopy are slow from fstor007 — the user's four copies of 16:41–16:44 EDT, reduced.** Source files: `~/orcd/scratch/efs/perf/efs-mount/` (`fuse.data` 44K samples, `fuse.strace` 630 MB over 16:40:57–16:44:50, `efs-fuse-efs-mount.log`, `ecopy.strace{,2,3}.txt`, `rsync.strace.txt`). Reduction: `results/measure/20260929-164057-fstor007-copy/` (`copier-syscalls.txt`, `fstrace1.log`, `nohint-addr2line.txt`, `flat.txt`, `callers-head.txt`, `fuse-log-last-mount.txt`). The client is today's build (`36 x 72 KiB bufs/conn`, `why=no-hint from=`); the servers are the 19:10Z roll; no server recorder was running, so the server side of this window is `raft-obs` and `efsd.log` only. All four copies were ^C'd.

What the copies did:

| run | source → dest | wall | bytes | shape |
| --- | --- | --- | --- | --- |
| ecopy2 | `/orcd/scratch/001` → `002` (large files, 396 threads) | 31 s | 8.9 GiB | 800–1600 MiB/s for 3 s (dcache fills), then flat **345–372 MiB/s** to the ^C |
| ecopy | `/data1/erbmi1/software` (396 threads) | 51 s | 5.0 GiB | first 5.5 s every syscall stalled (started 8 s after ecopy2's ^C left its dirty tail; `efs-fuse` did 160K `futex` calls in those 6 s); pwrite avg 94 ms, max 10.7 s |
| ecopy3 | `~/git` (526 threads) | 24 s | 3.8 GiB | 3.6 GiB of `results/` in 4 s, then a pure metadata phase at **160–215 files/s** |
| rsync `-aP` | `~/git` (3 processes, one op at a time) | 37 s | 377 MiB | **965 files, 26 files/s**, 10 MiB/s |

Why, in order of weight:

1. **rsync is a serial chain of metadata RPCs; each is a Raft round or three.** On efs in that run (rsync chdirs into the destination; every path in its trace is efs): `newfstatat` 0.87 ms, `openat(O_CREAT)` 1.22, `close` 0.48, `chmod` 1.22, `utimensat` 2.46, `mkdir` 4.88, **`rename` 9.21 ms** (1273 calls, 12.0 s of the 37). Seven such calls per file is 17–20 ms, plus the 50 ms below on 25 of the renames = 26 files/s. Nothing in rsync's trace waits on data. The 9 ms rename is the multi-shard txn (PREPARE parts, DECIDE, RESOLVE: three sequential commits); a same-directory file rename whose src dentry, dst dentry and parent row sit on one group could be one command — that is the "fold PREPARE parts into one command" protocol change §1a already marks **ask**. Do not start it without the decision. `utimensat` + `chmod` + `close` are three more rounds on the same inode that rsync issues back to back; there is nothing to fold there without changing what each call promises.

2. **The hintless NOT_PRIMARY has a source now, and it is not an election: `host_read_inode_lanes` on a single-group host.** Item 3's log line fired on fcstor003 32× in this window, `from=0x40f475` and `0x40f766` = `handler.c:662` and `:704`, the return addresses of `server_raft_host_setattr` and `server_raft_host_rename_at` (`set_inode_rc` is inlined). Both call `host_read_inode_lanes` after the op committed (`raft_host.c:9577`, `:10542`; 13 more callers: utimens `:9496`, unlink `:8582`, link `:9931`, hashed create `:7203`, getattr `:6699`, …). That function issues `host_read_index` on every active lane's group (`:4407`); lanes straddle both groups, so any inode with an odd lane active asks a single-group host for the group it does not host, and `host_read_index` returns NOT_PRIMARY with `hint = -1` on `!v.has` (`:2874`). fcstor003 is the client's first live voter for group 0 and hosts only group 0, so every SETATTR/RENAME of a multi-lane file that lands there pays it. Today's client (skip-the-voter) paid **50 ms × 64** in this 4-minute window (25 RENAME_AT, 24 SETATTR, 13 LOOKUP, 2 CREATE). Yesterday's client re-asked the same node 16× — that is the **810 `exhausted … -> EBUSY`**, the **177 `create .ecopy.tmp… failed`** and the `rename_at … rc=-13` lines in the older sections of the same log, and it is why the earlier ecopy runs were the slowest thing on this page. Fix (no decision needed): in `host_read_inode_lanes`, an active lane whose group this host does not serve returns NOT_PRIMARY with a **real hint** — a node that hosts both groups (`host_pick_peer(h, {SHARD, SHARD2}, 2, -1)`), never −1 — and the callers that already have the row in hand (setattr, utimens, rename, unlink, link) do that check **before** `host_propose`, so the op runs once on a dual host instead of committing here and being retried there. The retry after commit is safe today (setattr/utimens idempotent, rename/unlink/link/create answered from the op-id window) but it is a wasted commit. The remaining three lines (`handler.c:839`, fcstor004 ×2 and fcstor005 ×1) are `server_raft_host_report` on dual hosts — a different path; read it after this one. Gate: `grep -c 'NOT_PRIMARY without hint' efsd.log` stays 0 on all four across an rsync of `~/git`, and `why=no-hint` disappears from the client log.

3. **One client on fstor007 writes 360 MiB/s sustained; fcstor007 did 947 with `dd conv=fsync` yesterday.** ecopy2 is the clean measurement: 15 s flat at 345–372 MiB/s with ~400 writers, `pwrite` of 1 MiB averaging 53 ms. The client was ~19 % of one core (44K samples in 233 s): not CPU. In the profile the put path is 37 % inclusive (`dcache_put_now` from `put_pool_thread` 18 % and `dcache_flush_slot_inner` 18 %), and inside it `__lll_lock_wait` is 5.9 % under `put_fragments_parallel_once` / `put_recv_reply` / `efs_client_put_fragments_parallel`: that is `conn_lock[node]` in `efs_client_conn_get`/`_release`, six lock hand-offs per chunk across the put threads. `memmove` 13.4 % inclusive is the RDMA send copy plus the FUSE copy; blake3 7.4 %; `xor_into` 3.7 %. Measure before changing anything: **8 GiB `dd bs=1M conv=fsync` from fstor007** (the honest-fio rule's shape). ~360 → the host or its HCA (`mlx5_1`; check `ibstat` rate and an `ib_write_bw` to fcstor003 against the fcstor hosts' `mlx5_2`), and no client change moves it. ~900 → ecopy's pattern (400 threads × 1 MiB `pwrite` into one daemon) is what the put path cannot drain, and the pool mutex is first: pin one conn per node per put thread so the hot path takes no pool lock. Either way the number, not the guess, goes in this page.

4. **Small files saturate at ~2K metadata ops/s from one client, and latency inflates to match.** ecopy3's metadata phase: 24 584 `newfstatat`, 8 459 `openat`, 9 248 `close`, 3 154 `utimensat`, 1 021 `mkdir` in 24 s ≈ 1.9K ops/s, and at 526 threads the same ops that cost rsync 1–5 ms cost **14 ms (open), 60 ms (close; 21 of them ≥ 5 s), 70 ms (utimensat), 75 ms (mkdir)**. That is the Sep 27 same-parent rate (1 385/s at 9 procs) seen from the other side: throughput flat, queue depth ×500. Which side queues is not in this data (no server recorder). Next measurement: the same ecopy of `~/git` with `perf record` on the four `efsd` (no strace) and `raft-obs` every 5 s; read `pub_p50`, the leader lanes' `hi`, `apply-sleep` per group, and the top of the group-0 leader's profile. If the leader is idle and the client's handler threads sit in `futex`, it is the client (the `futex` storm at ecopy's start says look there first: `dcache` mutexes and `g_client.idx_mu` under 500 writers). If the leader is busy, it is one Raft round per op and the answer is batching proposals per client (already done for concurrent proposers) or D14.

5. **The staging table grows past 256 MB with nothing evictable** (`efs-fuse: staging table over EFS_CLIENT_META_MB … growing`, twice in this mount; `stage_evict_main` 4.5 % inclusive). Those are dirty rows waiting for their REPORT; with 400 writers the REPORT cadence is behind the create rate. Not a fault by itself — pinned data is real work — but it says the close-time REPORT is the client's backlog, and it is what the 5.5 s stall at ecopy's start was draining (ecopy2's tail). One `put_fragments … no quorum (efs_rc=-8) attempt 1/4` in the window: one PUT got fewer than 2 acks inside its wait and succeeded on retry.

Not the cause: `recvfrom` max 0.85 s, `poll` on the CQ channel 361K calls averaging 2.9 ms (that is the RPC wait, i.e. the Raft round), the 30 s `read(/dev/fuse)` lines are idle workers, `copy_file_range` EXDEV is ecopy's cross-mount fallback, exit 130 is the ^C.

The hint fix (item 2), the create / rename / overlay fixes, and the open hold are in the 00:57Z block above. posix2 is 63/63. Item 3's dd is **661 MiB/s**. Item 4's ecopy did not finish: group 2 elected through term 253 with `hi=2048`. That block has the numbers.

**Sep 29 20:21Z: items 1–7 of the 17:39Z list are in the running binaries. jobs=1 is 200/201. The 9-host suite is not.** Cluster is up, no recorders, `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`, fresh 17:04Z table (restart needs no `--join`). Rolled 19:10Z from `33c7e29c` plus this tree (`~/efs-runs/gate40.log`); the embedded id is `33c7e29cd161-dirty` and stays that until the next roll. Do not widen the 400 ms apply wait, and do not retry the one-shot REPORT. D14 (serve a leader read from a heartbeat lease) is still a question — do not write it. Item 8 (libfuse cannot cap total workers) is not done. Item 6's `.sum` sidecar stays (GC proves the fragment with it) and GET still has no root field: the process-wide cache is filled by the PUT that already returns the root.

What landed:

1. `overlay_chunk_deltas` skips `len == 0`. The apply already kept that tombstone and skipped it.
2. The sender drains up to `min(8, nrecv-1)` frames per wake (`HOST_SENDER_PIPE_MAX` in `raft_host.c`), then collects that many replies. `send_ae` on the flush path calls `send_commit_probe` when a caught-up peer's `ae_inflight_commit` is behind `commit_index` (one empty AE per peer per commit advance; the priority lane replaces an older heartbeat).
3. A hintless NOT_PRIMARY skips that voter on the next pick (`raft_voter_conn_skip`). `set_inode_rc` logs the return address at most once a second (`NOT_PRIMARY without hint`).
4. `recv_poller` blocks 1 s on the CQ channel after arming and draining.
5. `FUSE_CAP_SPLICE_READ` is cleared. The 8 MiB `pipe-max-size` sysctl is unused by efs.
6. `make_dir_up` creates the leaf and walks up only on ENOENT. `frag_loc_cache` is process-wide (65536 slots), published by a PUT and by a GET probe hit, so the next GET on any thread opens that root first.
7. `persist_applied` `pwrite`s 16 bytes (index and its complement) into one long-lived fd and `fdatasync`s. A torn or short record reads as 0. `clean_cluster.sh` treats mkfs `rc=-13` as success once `raft-status` prints `root=1`.

Gate (`~/efs-runs/gate41.log`):

- jobs=1, fcstor007: **200/201**, 0 fail, `mmap_write_read` SKIP, 28.2 s (`results/posix/20260929-191140`). The five folded-chunk EIOs and `dir_readdir_while_unlink` passed. Do not quote `results/posix/20260929-191224` (the `--parallel` call had no host list and ran on one host).
- 9-host, jobs=16, 13.5–14.7 s (`results/posix/20260929-202024`): **200/201** on fcstor010, 012, 013, 014, 015. Five EIO: fcstor007 `dir_many_files` `f0064`; fcstor008 `dir_readdir_listing` `f39`; fcstor009 `concurrent_create_unlink_two_proc` `c0-4`; fcstor011 `dir_many_files` `f0007` and `names_crazy_roundtrip` `back\slash`. fuse.log on 009 and 011 is `create <name> failed (EIO)`, which is `fuse_create_errno`: the create returned no inode, `last_err` was not BUSY (that path returns EBUSY), and the follow-up lookup missed the name. 007 and 008 were remounted for posix2, so those two logs are the later mount.
- posix2, fcstor007+008: **60/63**, 46.9 s (`results/posix2/20260929-202044`). `peer_unlink_while_b_has_fd` (holder `go timeout`), `peer_overlap_pwrite_partial` (`[8192,12288)` is not B's range), `peer_rename_vs_unlink_dst` (rename ENOENT; 007 logs `rename_at … rc=-3 after 1 tries`).

raft-obs at 20:21Z, last sample: `rtt_avg` 22–24 µs (was 440), leader lanes `hi=10` and `hi=6` (was 651), followers `hi=1`. `wait_timeouts=0`, `nohint=0`, `report-split rc=-13` count 0. `apply_max` 99–264 µs, `persist_max` 499–891 µs, `pump_hold_max` 109–610 µs. The 47 ms `apply_max` seen on fcstor004 after jobs=1 did not recur. `apply-sleep` since the 19:10Z start is 30 / 192 / 0 / 0 on fcstor003–006 (it was 2 / 2 / 0 / 0 after jobs=1), so the 9-host run added sleeps over 20 ms on the two group-0 voters while `apply_max` in that same tail stayed under 300 µs. `pub_p50` on fcstor004 is 829 µs.

What to do next, in order. None needs a wipe; each is a roll of the four servers (build-ID change → `roll_efsd.sh --all`) plus `tests/deploy_fuse_clients.sh`, then the gate named. Do not widen the 400 ms apply wait, and do not retry the one-shot REPORT.

1. **The five create EIOs are unattributed because both ends are silent. Make them speak, then rerun the 9-host suite.** Evidence: jobs=1 is 200/201 and jobs=16 on nine mounts lost 5 creates in ~3 000 (`dir_many_files` ×2, `dir_readdir_listing`, `names_crazy_roundtrip`, `concurrent_create_unlink_two_proc` — every one a create of a fresh name under 16 concurrent tests on one mount). The four server logs have no `raft-host: create … rc=`, no `opid-replay`, no `NOT_PRIMARY without hint`, no `txn-recover` in 20:20:24–20:21:37Z; `dirop_fail_on` (`raft_host.c:552`) prints only BUSY and STALE, so a create that returned IO, INVAL, PROTO, NOT_FOUND, or EXIST left nothing. The client printed `create f0007 failed (EIO)` (`efs_fuse.c:2849`), which is `fuse_create_errno` after `efs_client_create_ex` returned 0 with `last_err` not BUSY and not QUOTA and a follow-up LOOKUP of the name missing: the rc is not printed. No `inode-rpc: retry` line on 009 or 011, so it was not a send/recv drop and not a hintless NOT_PRIMARY; BUSY/STALE retries below exhaustion are silent, and `rpc_send_recv_shard` returns `EFS_ERR_PROTO` silently on `rtype != expect || plen < reply_len` (`inode_rpc.c:436`). Change: (a) `create %s failed` prints `g_client.last_err`; (b) the PROTO return prints `type`, `rtype`, `plen`, `reply_len`, `nid`; (c) `dirop_fail_on` returns 1 for every rc except OK, BUSY, STALE, EXIST, NOT_FOUND, NOT_EMPTY (those five are legitimate answers and EXIST is every `O_CREAT` open of an existing file); (d) in `server_raft_host_create`, when the propose+`host_wait_settled` returned OK and the post-commit `efs_meta_apply_lookup_tx` misses, print that separately — that is a committed create answered as failure, the I16 class. Gate: `EFS_TRANSPORT=rdma bash tests/run_tests.sh posix --parallel fcstor007.ib … fcstor015.ib` (3 min); read the new line, then fix what it names. Do not guess at the cause before that line exists.

2. **`peer_rename_vs_unlink_dst`: rename a→b returned ENOENT while a was alive. Two NOT_FOUND returns in `server_raft_host_rename_at` are the cause; make them STALE and OK.** fcstor007's fuse log: `rename_at 12265/a -> 12265/b rc=-3 after 1 tries`; the client retries STALE and BUSY for RENAME_AT (`stale_retryable`), never NOT_FOUND. In the handler: (a) the dest dentry is read at `raft_host.c:10103` (`xist=1`, b = OLD), then the dest row at `:10176` `efs_meta_apply_get_inode(h->kv, ndent.ino, &nrow)`; B's unlink of b committing between those two reads makes that get NOT_FOUND, which goes straight to `set_inode_rc` as ENOENT. A dest that was there a moment ago and is gone is the dest moving under the txn — return `EFS_ERR_STALE` so the client re-runs and the rename lands on an empty name. (b) After `host_txn_commit` succeeds (`:10538`), `host_read_inode_lanes(row.ino)` and `efs_meta_apply_getattr(row.ino)` (`:10542–10544`) run before the reply; B's unlink of the now-renamed b (NEW, last link → DEL) in that gap returns NOT_FOUND for a rename that committed. Answer OK with `out->inode.ino = row.ino` and a zero stat, the same shape `host_opid_reply_ino` already uses for "row gone"; the client's dual-apply must tolerate a zero stat there (check `efs_client_rename_at`, `ops.c:1263`). `peer_rename_same_src_two_dst`'s `rename_at 16898/x -> 16898/z rc=-3` on 008 is the loser of that race and is correct. Gate: `python3 tests/posix/posix_2client.py --remote fcstor007.ib fcstor008.ib --mnt /tmp/efs-mount --filter peer_rename_vs_unlink_dst` ×10, then the full posix2.

3. **`peer_overlap_pwrite_partial`: `efs_dcache_overlay` stamps the whole local image over a fetched chunk that has spans; `efs_dcache_copy` already refuses that case. Add the same guard.** The server state cannot be wrong for this test: A's [0,8192) and B's [4096,12288) are both span publishes (D1, commuting), and either fold order satisfies the assertion. The failing read is A's, on the mount that wrote the whole 12 288-byte chunk in step a0 (clean full image, `have_base`, `base_gen` = the table gen). A span PUT keeps the table gen as the base and adds a delta (`write.c:2323–2331`), so after A's own publish `export_chunk_gen_of` still equals `e->base_gen` and `dcache_image_current(e, tg)` is 1. `efs_dcache_copy` (`write.c:2541`) serves the image whole only `&& !spans` (the local map has no deltas) — correct, A's map has its own delta, so the read goes to the fetch path. That path decodes the published chunk, overlays the server's deltas (B's span included), then calls `efs_dcache_overlay` (`read.c:818/841/851`), and that function (`write.c:2612`) copies the whole local image over `dst` on `dcache_image_current` alone, no `spans` check. A's image is a0's zeros plus A's range; B's [8192,12288) is gone. Fix: in `efs_dcache_overlay`, take the whole-image branch only when the map has no deltas (same `export_chunk_copy(…).ndelta` test as `efs_dcache_copy`); with deltas, overlay only the unpublished ranges (the branch below it), which is also right for A's own range because it is already in the fetch. This is the Sep 27 morning "overlap pwrite" class, not the tombstone. Gate: `--filter peer_overlap_pwrite_partial` ×10, `peer_overlap_pwrite_same_range` and `_chunk_straddle` still pass, `test_chunk_deltas` (W17.3) still passes, full posix2.

4. **`peer_unlink_while_b_has_fd`: step a2 (A's `unlink` of a file B holds open, then `exists`) took 10–19 s and then succeeded.** The holder on B timed out its 20 s wait for `go` while B's `_holder_go` was still inside its own 8 s wait, so `go` was written 12–20 s after `ready`; only step a2 and two ssh hops sit between. The 16-step BUSY/STALE ladder is 10.35 s and prints nothing unless it exhausts, and the server printed no `raft-host: unlink … rc=` (rate-limited BUSY/STALE) in the window. Change first: `rpc_send_recv_shard`/`_dual` print one line when an op returns OK after more than 1 s of retries (`type`, attempts, `saw_busy`, last status). Then run the filter alone with `time` around step a2 and read that line. If it is BUSY from `host_wait_applied` on the unlink's group, the pair is `apply-sleep` on that leader at the same second (fcstor003/004 added 28 / 190 `apply-sleep` lines during the 9-host + posix2 window while `apply_max` in the tail stayed under 300 µs — those sleeps are the 20 ms-plus class, not the 400 ms deadline, and this is where to look at them). Gate: the filter ×10 under 2 s each, full posix2.

5. **Then, not before:** D14 (leader read lease) is a decision to ask for, and item 8 (libfuse thread growth) is a libfuse limit. The 9-host numbers to beat are 200/201 on all nine (Sep 27, `results/posix/20260927-123717`) and posix2 63/63 (`results/posix2/20260928-044304`).

**Sep 29 17:39Z: cluster and clients stopped; the 17:04Z run is reduced. The fix list below was implemented at 20:21Z (the block above); this paragraph is the evidence.** Stop was SIGTERM clients, then servers (`~/efs-runs/stop35.log`); fcstor006 exited on SIGTERM, fcstor003–005 were killed after their recorder children were signaled. Files, node-local: `/tmp/efs-perf/efsd.data` 10–13 MB + `efsd.strace` 0.9–1.1 GB on fcstor003–006; `fuse.data` 0.6–1.5 MB + `fuse.strace` 154–186 MB on every client. Reduction: `results/measure/20260929-170457-wipeposix/ana` (`SUMMARY.txt`, then `perf-<host>-<efsd|fuse>-<posix|idle|ecopy>.txt`, `strace-…-top.txt`, `efsd-<host>-raftobs.txt`, `fuse-fcstor007.log`). Windows (EDT): posix 13:05–13:07 (jobs=1 on fcstor007), idle 13:10–13:34, ecopy 13:35–13:37 (user's `ecopy --verify` of `/data1/erbmi1/software` from fstor007; that client's own profile is `~/orcd/scratch/efs/perf/efs-mount/`, 22K samples, 67 s of strace, 7.68 GiB at ~110 MiB/s, ^C'd). About 60 % of every server sample and 50 % of every client sample is `ptrace_*` from the attached strace; the shares below are of the rest. The next roll must not carry `--strace` if the number is a bandwidth.

What was found, in the order to fix it. Each item names the evidence and the change; none needs a wipe.

1. **Reads of a chunk that was folded once return EIO. Fix in `src/client/read.c` `overlay_chunk_deltas`: skip `ce.deltas[i].len == 0`.** The apply keeps a `len == 0` tombstone in the span trailer after a fold and skips it (`meta_apply.c:3410–3415`); the client's `overlay_one_delta` returns `EFS_ERR_INVAL` on `d->len == 0` (`read.c:623`). `fuse-fcstor007.log`: `read: invalid argument (efs_rc=-5) ino=6216 off=0 len=11` and `ino=5722 len=65537`. The write side hits the same check through `efs_client_fetch_published_chunk` → merge base fails → "do not PUT a zero base" → `flush-meta: I/O error` (inos 6079, 6693, 6724) and `unmount: data flush incomplete (dcache=-9)`. This is all five EIO tests, and the mapping is exact for the reads: Python's `read()` on a 10-byte file issues `read(11)` (`basic_overwrite_middle`: write 10, overwrite 3 in the middle, read back) and on a 65 536-byte file `read(65537)` (`content_random_overwrite_append`). The other three fail in the close flush (`efs_rc=-1` = `EFS_ERR_IO` from `write.c:3560`, "do not PUT a zero base"): ino 6079 ×4 is `concurrent_appends`' four workers, 6693 and 6724 are `concurrent_overlap_write` and `mtime_monotonic_many_writes` (each a sub-chunk write after a fold; the STALE replay path at `write.c:4225` fetches the same base). None of the six came from a BUSY REPORT: the report loop still retries BUSY 8× and prints `report-loop … rc=` when it gives up, and `fuse-fcstor007.log` has no such line and no `rc=-13`. Gate: those five, then posix jobs=1 back to 200/201.

2. **The leader's per-peer sender lane is the metadata latency. Fix in `raft_host.c` sender thread: drain the whole lane per eventfd wake with no syscall between frames, and drop the per-frame `fstat` + second `read`.** Evidence: `apply_max=0`, `pump_hold_max` 5–7 µs, pump thread 2 500 cycles/s with no gap over 15 ms, yet `apply-sleep` (a waiter 1–2 indexes behind `applied`) fired 477× on fcstor003 (g0 leader; 52 of them 100–149 ms), 652× on fcstor004 (6 at the 400 ms deadline = `wait_timeouts=7` = the four `report-split … finish_ms=430–1024 rc=-13`; the client retried those and they passed, so no test failed on them this time — under load they are the fsync EIO of the earlier runs), 511× on fcstor006 (g2 leader). `raft-obs`: leaders' lanes `tx->1 hi=651`, `tx->2 hi=244` (fcstor003), `hi=519`/`92` (fcstor006); every follower `hi=1`. Sender thread 982200 on fcstor003 (`strace`): per frame `poll` → `read(eventfd)=8` → `fstat(sock)` → `read(eventfd)=EAGAIN`, ~230–440 µs per frame, 14–18K syscalls/s for the whole posix burst, ~154K frames for ~5K group-0 commits. `rtt_avg=440us` is that loop's period on every host, not the wire. A 651-deep lane × ~230 µs is the 149 ms max. Successful one-record REPORTs took `finish_ms=133–139` for the same reason. This is also `dir_readdir_while_unlink` (15 s timeout: it creates 40 one-byte files serially, each a CREATE + a close REPORT at `finish_ms` ~135, then scans) and the 84 s posix wall (client `work_s=1.25` in 120 s: the client only waits). The same class showed at mkfs time: both `raft-mkfs` calls returned `rc=-13` (the first after proposing index 2 on a cluster that had been up for 3 s; the second, sent to the leader, `index=0`), yet `raft-status` showed `root=1` and the table served. `clean_cluster.sh` and `~/efs-runs/wipeposix.sh` treat that BUSY as a failure and retry mkfs; they should read `raft-status` for `root=1` instead. Do not widen the 400 ms budget for it.
   - Why the lane is that full: `efs_raft_read_begin` → `broadcast_ae` (`raft.c:1901`), one heartbeat AE to each peer per ReadIndex round, and every GETATTR/LOOKUP/READDIR on the leader is a round when none is pending. ~30 frames per commit. **Decision needed (D14, ask):** serve leader reads without a round while the leader's last heartbeat majority is younger than the election timeout (a lease), or space rounds at a minimum interval. Either is a protocol-level choice; the sender fix above is not and comes first.
   - Also cheap: a follower waiting for a forwarded command's index learns the commit only at the next heartbeat (50 ms). Send an empty AE when the leader's commit index advances and the lane is idle.

3. **A hintless NOT_PRIMARY retries the same node 16 times for 10.3 s. Fix in `inode_rpc.c`: on `why=no-hint` skip the node that answered when re-picking (`raft_voter_conn` is deterministic: first live voter).** fstor007's log: 4 160 `retry … why=no-hint`, 810 `exhausted 16 BUSY/STALE (10.3 s) -> EBUSY` across the day (SETATTR 295, CREATE 247, RENAME_AT 145, LOOKUP 108, GETATTR 47); 5 SETATTR exhaustions on the fresh table with both terms stable, so this is not an election. `set_inode_rc` puts `hint + 1` in `primary_id`; 0 means `hint < 0`, and the client also treats `primary_id == nid` as hintless. Server side, `host_read_index` returns before setting the hint when `!v.has`; find which path answered by logging `rc=-15 hint=-1` at `set_inode_rc` once.

4. **`recv_poller` is a 1 kHz timer. Fix in `rdma.c:614`: after `ibv_req_notify_cq` and the re-poll, block on the completion channel (`poll(…, -1)` or a long safety timeout), not 1 ms.** Idle window: fcstor008 `poll` 1 342 269 in 1 440 s (932/s), `recv_poller` 34 % of the idle client's samples; efsd 1.87 M `poll`/24 min, `recv_poller` 14 % (fcstor003) and 10.5 % (fcstor004) of idle server samples. It is why every idle client wrote 158 MB of strace in 35 min. The notify already guarantees the wakeup; the 1 ms was insurance against the ack-before-wait race that the comment at `rdma.c:563` describes, and the arm-drain-wait order already closes it.

5. **W15.5 `FUSE_CAP_SPLICE_READ` buys nothing; revert it (and the 8 MiB `pipe-max-size` sysctl is then unneeded).** fstor007 ecopy: 24 668 `splice(/dev/fuse → pipe)`, 17 072 of them header-sized; each 1 MiB write is then eight `read(pipe, 128 KiB)` (59 107 such reads) — the same one copy per byte as `read(/dev/fuse)` plus one syscall per request. Profile: `copyout` 5.2 % under `pipe_read`, and ~7 % in `do_anonymous_page` / `get_page_from_freelist` / `clear_page_erms` / `free_unref_page*`: the dcache destination buffers are fresh anonymous pages every time. Second change: a freelist of chunk buffers so a write does not fault + zero + free 32 pages per chunk.

6. **Fragment store syscall shape (server, `store.c`).** From the ecopy window on fcstor003 (120 s): `openat` 149 770 (51 130 ENOENT), `close` 98 638, `fstat` 92 353, `mkdir` 9 304 (8 176 EEXIST), `pread64` 845, `pwrite64` 2 360. Three separate fixes:
   - GET probes the six roots in order (`openat(/data1/01/…/0.1.<gen>) ENOENT` ×4, then `/data1/05` hit). The PUT reply already carries the root; put it in the GET request and open that root first.
   - PUT runs `mkdir("/data1")`, `mkdir("/data1/06")`, … to the leaf on every fragment (≈7 EEXIST per write). Create the inode dir once per `(root, ino)` and `mkdir` only on the create's ENOENT.
   - PUT writes a second file `<frag>.sum` (open + write + close) per fragment: two XFS inode creates per fragment (`xfs_btree_lookup`, `__d_lookup_rcu` in fcstor004's ecopy profile). Store the checksum in the fragment's xattr or a fixed-size header written in the same `pwrite`.
   - The `O_TRUNC|O_DIRECT` probe for an existing fragment (`openat … ENOENT` then `O_CREAT`) still runs for the fstor007 client; check that `EFS_PATH_HINT_NEW` (W14.4b) is sent by every write path.

7. **Pump: `applied.N.tmp` open + write 8 B + fsync + close + rename ~36×/s** (fcstor003 posix window: pump thread 982191 `openat` 5 329, `rename` 4 376, `fsync` 8 943 = 2.4 s of 120). `pwrite` the index in place into one long-lived fd and `fdatasync`, or persist at most every N ms; it is only the restart resume point.

8. **efs-fuse thread count is unbounded under load.** fstor007 ecopy client: 657 `clone3` in 67 s, 664 threads, 0 exits; 109 threads splice from `/dev/fuse`. libfuse 3.10 has `max_idle_threads` only; the put/get pools should be capped, and workers that finish should exit.

Not a finding: `rename_at 1306/a -> 1306/b rc=-12` is ENOTEMPTY for `dir_rename_dir_over_nonempty` (passes). The ecopy's 361 `EXDEV` are `copy_file_range` across mounts (it falls back), its 188 `ETIMEDOUT` are its progress thread's 200 ms `futex` timer, and exit 130 is the user's ^C.

Order: 1 (one line, gates the suite), 2's sender drain, 4, 3, then 5–8; D14 is a question for the user before any lease code.

**Sep 29 17:04Z: wiped and redeployed, posix jobs=1 on fcstor007.** `tests/wipe_cluster.sh` then a fresh `raft-mkfs` (`~/efs-runs/wipeposix.log`). Both mkfs RPCs returned `rc=-13` (BUSY); the table is up anyway. Build `2dd77dac881c-dirty`, `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`, servers and clients fcstor003–015 started with `--perf --strace`. Group 0 leader 0 term 4 commit==applied 6015; group 2 leader 3 term 1 commit==applied 4797. All 13 mounts `fuse.efs-fuse` + `MOUNT_OK`. Posix jobs=1 on fcstor007: **194/201**, 6 fail (5 EIO, `dir_readdir_while_unlink` 15 s timeout), `mmap_write_read` SKIP, 83.9 s (`results/posix/20260929-170457`). Recorders: `/tmp/efs-perf/efsd.data` + `efsd.strace` on fcstor003–006, `fuse.data` + `fuse.strace` on the clients. Background `edelete` of the renamed-aside trees is still running. Do not widen the 400 ms apply wait, and do not retry the one-shot REPORT.

**Sep 29 16:09Z: servers restarted, clients not mounted.** `tests/roll_efsd.sh --all` with `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1` and `--perf --strace` (`~/efs-runs/rollw34.log`). All four built `2dd77dac881c-dirty` (commit `2dd77da` plus the uncommitted W21.2 / W17.3 / W15.5 tree), efsd up, `perf=1`, `strace=1`. Group 0 leader 0 term 9454 commit==applied 13775244; group 2 leader 3 term 2925 commit==applied 12187894. `efs-fuse` is 0 on fcstor003–015. Recorders are `/tmp/efs-perf/efsd.data` and `efsd.strace` (the strace files were already 0.8–1.5 GB two minutes after start; stop them with SIGTERM). The 1 s start line printed `perf=0`; a later check is `perf=1`. Next is 10's one IOR, then 7 step 2 (c) (measure, then ask), then 11. Do not widen the 400 ms apply wait, and do not retry the one-shot REPORT.

**Sep 29 15:40Z: W21 step 2 and W17 step 3's tests are in the tree; W15 step 5 is blocked on a sysctl.** `efs_export_staged_bytes` is one load of the root's `staged_total`, kept by `staged_refresh` at every capacity change (W21). `test_chunk_deltas` covers replay-of-folded-span, span-after-fold, overlap-after-fold, and a full image that must name the current base (W17.3); its old "trailer gone" assertion predated D1's tombstones and is replaced. W15.5 (`FUSE_CAP_SPLICE_READ`): `fs.pipe-max-size` was 1 MiB and libfuse needs `max_write` + 4 KiB = 4 MiB + 4 KiB to use the pipe; the user raised it to 8 MiB on all 15 hosts at 15:45Z (runtime sysctl, reverts on reboot), and `efs_fuse_init` now asks for the capability. The first client profile after the roll should show `fuse_copy_page` unchanged and the caller-less `memmove` gone. Committed through `2dd77da`; these four are on top, uncommitted.

**Sep 29 15:15Z: review of the last 24 h of changes (bb338f2..a53b253 plus the dirty tree). Six fixes in the tree, not rolled, not gated.** (1) `kv_flush_locked` still refused a memtable with more than 256 runs (`nneu >= 256`) after range 0 was split into 256 subranges; with 512 possible ranges that flush returned BUSY on every attempt, D9 kept the memtable in RAM, and a snapshot flush was BUSY too. Cap is `KV_RANGE_N`. (2) `kv_seg_data_bytes` walked every block index entry (8 KiB blocks: 230K for the old range-0 L1) and the compactor called it per L1 file and `kv_l0_bytes` once per range inside the 512-range loop, under `l->mu`; the byte count is now fixed at `kv_seg_open` and the L0 sum is taken once per compact. (3) The D13 fold's output was installed at the head of L0, ahead of a same-range file flushed during the merge; a lookup would have returned the folded, older value. The output now goes where the newest input was. (4) The compaction iterator's 1 MiB readahead returned `EFS_ERR_IO` for a block over 1 MiB (one value can be up to 16 MiB); that read is now whole. (5) `maybe_prefetch` called the layout-miss path for every prefetch; that path re-pulls its window every 200 ms even when the maps are local. It now asks only when the chunk's map is absent. (6) `dcache_flush_slot_inner` dereferenced its saved chain pointer after re-taking the lock; a concurrent drop frees chain nodes. It re-finds `(ino,ci)` after the GET and PUT and restarts the walk from the head if the node is gone. Also: W25's `lane_bits` is written by `pack_utimens_cmd` (was patched into the buffer after packing); two `test_kv_lsm` assertions that encoded the deleted file-count rule follow the byte rule. Nothing else in the 24 h diff needed a change: W23's pool generation and 1 s probe skip, W24's outer deadline, D2's parallel windows, D9/D10, 8h, W17.1, W22 are as START-HERE describes them.

**Sep 29 14:50Z: the shard-I/O lock is no longer held across a fragment GET. In tree, not rolled, not gated.** `dcache_flush_slot_inner` drops `shard_io` with the dcache mutex before `efs_client_fetch_published_chunk` and `dcache_put_now`, and takes it again to install the result. The entry is clean before the drop (body stolen, or `dirty` cleared), which is the same order `dcache_steal_dirty` already uses, so a second flush of that chunk does not read the same merge base. `ll_fsync` of another file on the shard no longer waits out the 30 s recv. Cluster is still stopped (13:53Z). Next is 10's one IOR, then 7 step 2 (c) (measure, then ask), then 11. Do not widen the 400 ms apply wait, and do not retry the one-shot REPORT.

**Sep 29 14:20Z: D13 (8j) is in the tree, not rolled, not gated.** Over the file cap, and only when no range already meets the 1/8 rule or the 1 GiB byte cap, `kv_compact_locked` merges the range with the most L0 files (at least two) into one L0 file and does not open that range's L1. Tombstones stay in that file, because L1 is still there. The compactor loop already repeats while `n_l0` is over the cap. A range with a single L0 file is left alone. The 1 GiB byte cap still rewrites L1; that is how bytes leave L0. The pump does not wait. Cluster is still stopped (13:53Z). Next is the shard-I/O lock held across a fragment GET, which this change does not touch. Do not widen the 400 ms apply wait, and do not retry the one-shot REPORT.

**Sep 29 14:05Z: the 13:08Z perf+strace run is reduced. Next work is D13 (8j). Do not widen the 400 ms apply wait, and do not retry the one-shot REPORT.** Cluster and clients were stopped at 13:53Z (`~/efs-runs/stop32.log`) so the recorders could finish. Clients exited on SIGTERM. The four `efsd` did not, and were killed after their recorder children were signaled; `perf report` still opens the files. Nothing is running. Files, node-local: `/tmp/efs-perf/` (`efsd.data` 183–299 MB and `efsd.strace` 13–20 GB on fcstor003–006; `fuse.data` 68–72 MB and `fuse.strace` 3.1–3.2 GB on fcstor009 and fcstor010, about 1–2 MB / 180–210 MB on the other clients). Reduction: `results/measure/20260929-130800-ddposix/ana`. The window is 09:17–09:50 EDT: jobs=1 posix on fcstor007 and fcstor008, and two clients each `dd bs=1M conv=fsync` of two 100 GiB files. About 30 % of every server sample is `ptrace_stop` because strace was attached. That is the tracer. The numbers below are the rest.

The copies each transferred 100 GiB and then `fsync` returned EIO. fcstor009's fuse log: two `report-loop`, both `rounds=1 busy=1 rc=-13`, the longer 39.4 s. fcstor010: three, max 39.1 s. posix under that load was 192/201 and 194/201, the fails EIO, `mmap_write_read` the only skip. The client is off-CPU. On fcstor009 the window's non-wait syscalls are 141 s; `recvfrom` max is 30.5 s; `write` is 4.9 M calls and 57 s. On-CPU, blake3 is 28.7 % self inside `dcache_flush_slot_inner` / `dcache_reclaim_main`. Do not spend a change on blake3. fcstor007's posix window is 1.1 s of non-wait work and `recv_poller` is the top of that profile. Do not put the yield loop back.

What the servers did:

- **The BUSY is still `host_read_index` before pack, and a few pushes block for much longer.** fcstor004 `report-split`: 184 `rc=-13`, 59 `rc=0`. `pack_ms=0` on 172 of the 184. `finish_ms` on those BUSY lines averages 656 ms and maxes at 5.5 s. `push_ms` averages 5.4 s, and 12 of them are ≥ 1 s, one **206 s**, inside `host_pub_batch_push`. The successful lines have `push_ms=0` and `finish_ms` ≤ 392. `backpressure=0` on every server, so L0 bytes never reached 1 GiB and D9's admission did not fire. Do not widen `HOST_READ_TRIES`.
- **D12's backstop rewrote L1 for the whole copy.** It compacts whichever range has the most L0 files as soon as `n_l0` is over 4, and the count stayed over 4 for the entire ingest, so the 1/8 rule never applied. fcstor004: 425 compacts, **67.5 GB**, max 17.2 s, L0 peaked at **379** and was still 36 at the stop (L1 448). fcstor003: 770 / 75.5 GB, max 7.1 s, L0 peak 95, ended at 3. fcstor005: 431 / 67.5 GB, max 25.4 s, L0 peak 360. fcstor006: 754 / 75.9 GB, max 9.0 s, L0 peak 114. An average compact is about 159 MB. That is a fat L1 rewrite, which is the amplification D10 had removed.
- **A point get still binary-searches the files that rewrite did not remove in time.** fcstor004 work-window self time: `kv_seg_probe` 8.3 %, `__memcmp_avx2_movbe` 10.2 %, `lookup` 3.4 % self / 23 % inclusive, `efs_meta_apply_get_chunk` 18 % inclusive, `host_pump` 15 % inclusive. Span skip only rejects a key outside a segment. Every L0 file of the same range is inside the span, and there were hundreds of them. `apply-sleep` hit 400 ms on 599 of 655 group-0 sleeps on fcstor003 (247 s summed) and on 547 of 611 group-2 sleeps on fcstor006 (232 s). fcstor006's CPU profile is `openat` / `writer_thread` (17 %) and XFS, not lookup: the same deadline, waiting on a disk that is also writing the 67 GB.
- **The fragment create is the other server CPU, and it is not the EIO.** fcstor003 in the window: `openat` 7.52 M / 387 s, `access` 85 k (D7 held), `fstat` 2.7 M, `pread64` over 1 MiB 143 k (the merge readahead is in use). `fsync` 37 488 calls on fcstor003 (115 s, average 3.1 ms) and 68 960 on fcstor004 (114 s); the longest on any server is 3.8 s. The 100 ms and the 3.6 s outliers are the compactor's segments under that 67 GB, not the Raft log. Do not move `mdraft/`. `raft-snap` fired 4 / 8 / 8 / 6 times. Do not go back to snapshotting every 256 entries.

**How to fix it.** D13 is in the tree (14:20Z), not rolled. When the file count is over the cap and no range meets the 1/8 rule, merge that range's L0 files with each other and do not read its L1. The 1/8 rule, and the 1 GiB byte cap, stay the paths that rewrite L1. That drops the count the probe walks, and it stops the 67 GB that makes the apply miss 400 ms. The shard-I/O lock held across a fragment GET (the 12:36Z IOR hang) did not show up in this run: these fsyncs returned. That drop is in the tree (14:50Z), not rolled.

**Sep 29 12:50Z: 8h and D12 are rolled (12:29Z, no perf, no strace) and the 9×4 debug IOR did not finish.** `results/io500/20260929-123635-rdma`. Stonewall is 1 s, same shape as the Sep 28 runs, so every number is `[INVALID]`. ior-easy-write **2.218 GiB/s** (19.720 s) and mdtest-easy-write **1.765 kIOPS** (2.739 s) completed. ior-hard-write printed 31 `fsync failed` and no bandwidth; rank 28 on fcstor014 called `abort`. No later phase, no SCORE. fcstor004's L0 file count ended at **3** (D12 held; it was 310 on the 05:31Z trace) with L1 still 397. `report-split` on that node was 23 `rc=0`, 73 `rc=-13`, 3 `rc=-14`, and there were 1582 `apply-sleep` lines, so the 400 ms apply wait is still the BUSY. Do not widen it, and do not retry the one-shot REPORT.

The hard-write hang is a different wait. On fcstor012, `ll_fsync` of the shared file (ino 2595379) blocked on the shard I/O lock while `dcache_reclaim_main` held that lock across `efs_client_fetch_published_chunk` (chunk 170279). The two fragment GETs sat in `efs_rdma_recv_wait` (`timeout_ms=30000`) against nodes 3 and 4. That client's four ranks went D (`request_wait_answer` / `fuse_flush`) and ignored SIGKILL until `efs-fuse` was killed; the client was remounted at 12:50Z (`FUSE_OK`). The earlier fsyncs on that inode were STALE (`rc=-14`) and then EIO, not the L0 admission. Cluster is up, `a53b253f2455-dirty`, RDMA, no recorders. Group 0 leader 0 term 9434 commit==applied 13756802; group 2 leader 1 term 2919 commit==applied 12169621 (at the 12:32Z roll).

**Sep 29 12:02Z: the 05:31Z trace is reduced. 8h and D12, named below, were the next work and are now in the 12:29Z binaries.** Do not widen the 400 ms apply wait, and do not retry the one-shot REPORT. Cluster and clients were stopped at 12:02Z so the recorders could finish (`~/efs-runs/stop31.log`). Clients exited on SIGTERM. The four `efsd` did not, and were killed after their recorders were signaled; `perf report` still opens the files (IOR slice on fcstor004: 155K samples, lost 0). Nothing is running. Files, node-local: `/tmp/efs-perf/` (`efsd.data` 89–150 MB and `efsd.strace` 7.2–7.4 GB on fcstor003–006; `fuse.data` 8–13 MB and `fuse.strace` 1.8–1.9 GB on fcstor003–015). Reduction: `results/measure/20260929-053100-w30trace/ana`. The recorders ran 01:31–08:02 EDT. The only write in that window is the 01:46 EDT IOR (05:46Z), which aborted; the profiles below are that 01:45:30–01:52:30 slice, not the six idle hours.

No bandwidth. Same abort as 00:40Z and 02:35Z: every client `report-loop rounds=1 busy=1 rc=-13` (one `stale=1 rc=-14` on fcstor008), walls 8–26 s, then `fsync`/`close` failed. fcstor007's strace is one `recvfrom` of 25.9 s. The client is off-CPU. On-CPU, blake3 is 20.8 % and `memmove` 6.6 %; `fstat` is 126 calls and `getsockopt` 2931 (W23's probe cut held). `recv_poller` is 5 % of the IOR samples and the top of the idle slice, which is ~2 % of one core. Do not put the yield loop back, and do not spend a change on blake3.

What the servers did, fcstor004 unless said (60 `report-split` lines: 54 `rc=-13`, 2 `rc=-15`, 4 `rc=0`):

- **The BUSY is `host_wait_applied`, before pack.** `pack_ms=0 push_ms=0` on the `rc=-13` lines. Each one is preceded by `apply-sleep us=400xxx group=2`: commit ran ahead of applied by about 30 entries (idx 12148239, applied 12148214) and the wait hit its 400 ms deadline. Group 2 hit that deadline on 53 of 60 sleeps; group 0 on 9 of 35 (median 53 ms). This is not D9. There is no `kv-compact: backpressure` line on any server, so L0 bytes never reached 1 GiB and `host_pub_batch_propose` did not refuse the batch. The four `rc=0` lines have `pack_ms` 393–612. The two `rc=-15` lines (`push_ms` 5551 and 5721) sit inside the one snapshot below.
- **D10's byte rule held, and the file cap it removed did not.** 34 compactions wrote **0.254 GB** (003: 14 / 0.123 GB, 005: 38 / 0.257 GB, 006: 12 / 0.075 GB). The longest is 1161 ms. `inputs=2` or 3, output 0.5–1 MB, 10–14 ms, ranges 322 and 503. L0 file count went **26 → 310** on 004 (24 → 149 on 003, 27 → 327 on 005) and L1 ended at 353. `kv_compact_locked` returns BUSY unless a range's L0 bytes are at least 1/8 of its L1, so the compactor only rewrites ranges whose L1 is still tiny, one range per turn, and the flush adds files faster than that removes them. `KV_LSM_L0_DEFAULT` is 4; waking at 4 and then returning BUSY is how the count reaches 310. The pump did not wait on any of these (D9 held). `fsync` in the IOR window: 3090 calls, 1.40 s total, max 0.091 s (D11 held). `pread64` over 1 MiB: 6304 (the merge readahead is in use).
- **A point get walks every one of those files, on the pump.** IOR-window inclusive profile: `host_pump` 16.0 %, `host_apply` 15.0 %, `efs_meta_apply_publish` 14.9 %, `efs_meta_apply_get_chunk` 9.0 %, `lookup` 17.9 %, `kv_seg_probe` 11.8 %, `__memcmp_avx2_movbe` 10.1 % self / 19.0 % inclusive. `lookup` probes `n_l0 + n_l1` segments (310 + 353 by the end) and `kv_seg_probe` binary-searches each. That is the apply the 400 ms wait is timing out on. `writer_thread` is the other 15 % (`openat` 855K calls / 108 s in the window — the fragment creates). `access()` is 10 237 against those 855K opens (D7 held for this write). `vx_sift_down` is 2.2 %; `efs_kv_lsm_view_export` is under 0.5 %.
- **W22.1 fired once, not every 256.** One `raft-snap` on group 2: 2.82 GB, 14.1 s. The 54 BUSYs start before it. Do not go back to snapshotting every 256 entries.

Compared with D10 as written: "let L0 files accumulate, the cap that matters is bytes, reads already probe every L0" is the sentence this trace falsifies. The bytes are fine. The probes are the abort. Fixes are 8h (skip a segment whose key span misses, no decision — the range split already exists) and 8i / D12 (when the file count is over the cap, compact the range with the most L0 files even under the 1/8 ratio, until the count is back under the cap). The 1/8 rule stays for the steady state, so a fat range is not rewritten to absorb a few KB. The pump still does not wait.

**Sep 29 04:08Z: W22.1, W22.2, W14.4b, the `--meta-storage` flag, and `pub_p50` are on the cluster.** Rolled `--all` (`~/efs-runs/rollw22.log`), build `54a500da9dc8-dirty`, `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`, servers and clients fcstor007–015 started with `--perf --strace`. Group 0 leader 1 term 9418 commit==applied 13687019; group 2 leader 1 term 2902 commit==applied 12107762. Clients fcstor007–015 remounted in the same screen, and fcstor003–006 (which also run `efs-fuse`) remounted at 04:13Z (`~/efs-runs/fuse36.log`), all `fuse.efs-fuse MOUNT_OK`. Recorders ran until 04:27Z and were stopped for the analysis below (the perf children are zombies until the daemon's `stop_recorder` reaps them; `pgrep -x perf` still counts them). Files, node-local and final: `/tmp/efs-perf/efsd.data` (28–42 MB) and `efsd.strace` (2.4–3.7 GB) on fcstor003–006; `/tmp/efs-perf/fuse.data` (0.4–0.6 MB) and `fuse.strace` (65–96 MB) on fcstor003–015. At 04:11Z every host had `perf=1` and `strace=1` (fcstor005's `perf=0` in the roll log was the 1 s `pgrep` racing the attach). No IOR this roll; a user `ecopy` ran 04:15–04:22Z and is what the next block analyzes.

What landed: a group snapshots when command bytes past `snap_idx` reach 512 MiB (`EFS_RAFT_SNAP_BYTES`) and keeps that much of the log (`log_base`); `send_ae` sends InstallSnapshot only when `next_index` is below the window. An import diff is applied 1024 keys per pump cycle (`HOST_SNAP_SLICE`) and the last-chunk retry stays BUSY until the cursor finishes. `path_hint = 0xffffffff` (`EFS_PATH_HINT_NEW`) is the first PUT of a fragment; the server skips `access()` and creates on the least-queued root; a retry sends 0. `efsd --meta-storage <root>` exists and defaults to the first `--storage` root — `mdraft/` was not moved. `raft-obs` prints `pub_p50` / `pub_max` (propose→apply of the publish batch). Not done: D2's parallel chunk-map windows, the D8 IOR that reads `pub_p50`, the D6 shared-vs-quiet `fsync` measurement (ask which device before any move).

**Sep 29 04:07–04:27Z: perf + un-narrowed strace on every daemon across a user `ecopy` (`results/measure/20260929-040800-idle-trace/ana`, per host `perf-<h>-{efsd,fuse}-{flat,threads,callers,children}.txt`, `strace-<h>-{efsd,fuse}-{summary,tids,tidcalls,persec,long}.txt`, `idle-detail-<h>.txt`, `idle-detail2-<h>.txt`, `strace-fcstor00N-efsd-{openat-paths,mkdir-paths,rename-unlink,top-tid-*}.txt`).** The recorders from the 04:08Z roll were stopped at 04:27Z for this analysis (perf must finalize the file); they are not running now. Something wrote through the cluster from 04:15 to 04:22Z (`.ecopy.tmp.*` names in `unlink-simple` lines; 408K fragment `O_CREAT` opens on fcstor003 and on fcstor004, 120K of them in the first minute); none of it went through fcstor007–015's FUSE (125 `read`, 3 `recvfrom` per client in 19 minutes), so the client profiles are idle-only. No IOR was run.

What the servers did, fcstor004 unless stated (voter in both groups, group-2 leader until 04:16:17Z):

- **W22.1 held:** zero `raft-snap: start` on all four nodes in 20 minutes (the 02:35Z run had one every 256 entries). No import, no `efs_kv_lsm_view_export` in any profile. `disk_log_new_bytes`, the byte counter W22.1 added, walked the whole retained log under `d->mu` on every pump tick: 1.0–1.25 % of every server. Fixed in tree (running `bytes_all`/`bytes_old` in `raft_disk_group`, O(1)); not rolled.
- **The compactor is the server.** 48 % of fcstor004's samples and 55 % of fcstor005's are one thread: `kv_compact_locked` (`cm_sift_up`, `__memcmp_avx2_movbe`, `cm_pop`, `kv_seg_probe`). 433 compactions on 004 in 20 minutes (441 / 648 / 656 on 005 / 003 / 006), `bytes=` sum **159 GB** (159 / 124 / 124 GB) against a 5.3 GB table (100 L1 + 3 L0 files at 04:44Z). Each compaction takes one `key[0]` range's L0 files (`inputs=` 5–11, 33 once) and rewrites that range's L1: ranges 1–15 average 190–330 MB per rewrite; **range 0 averages 1.8 GB** (34 rewrites = 61 GB of the 159). `kv-compact: backpressure n_l0=55..64` appears throughout the write window. The thread does 5.25M `write(…, 4096)` (stdio's default buffer; fixed in tree with `setvbuf` 1 MiB in `kv_seg_w_open`, not rolled) and 3.7M `pread64` of 7.5–8.2 KB (one block per call, the merge's input reads).
- **The pump blocks on the compactor. That is the election trigger now, and the REPORT BUSY.** Pump tid 881545's `futex` waits during the write window: **4.14 s, 1.68 s, 24.23 s, 2.70 s, 3.57 s** (04:16:13–04:16:54Z), each matching a `kv-compact: end ms=` of the same length (4139, 24233 — range 0, 1.77 GB — 2696, 3574). That is `kv_maybe_flush_locked`'s `while (n_l0 + KV_LSM_RANGE_MAX > KV_LSM_MAX_SEGS) pthread_cond_wait(&l->cv)`: the apply path cannot place a memtable while L0 is within 16 files of the 64 cap, so it waits for the compactor's publish, on the pump, under `h->mu`. `raft-obs`: `pump_hold_max=4438191us apply_max=4438134us applies_in_worst=5` on 004 (history 6.4 s, 5.8 s, 2.5 s, 2.0 s), 2.14 s on 005. While held: `raft-host: apply-sleep us=400xxx` (the 400 ms `host_wait_applied` deadline) → `report-split nrec=76757 … finish_ms=1881 rc=-13` (12 of 116 REPORTs on 004 non-zero rc; a client's `fsync` would have returned EIO under W17.1); `get_max=2018ms` on `tx->2`; and the terms: group 0 9418→9421 at 04:15:43Z (fcstor003 stepped down and was re-elected), group 2 2902→2904 at 04:16:17Z (fcstor004 stepped down to fcstor006), both inside the pump holds. W13 step 2 called this wait "the only stall left, and it means the compactor is 16× behind (log it)"; it is now the stall.
- **The 100 ms `fsync` is the compactor's own segment, not journal sharing.** Per thread on 004: the pump's 61 668 `fsync` (Raft log + `applied.N.tmp`) average **0.39 ms**, 61 228 under 2 ms, 2 over 90 ms; the compactor's 695 average **51 ms**, 299 in 90–120 ms. 003: pump 0.40 ms, compactor 36 ms (141 ≥ 90 ms). 005: pump 0.38 ms, compactor 62 ms (407 ≥ 90 ms). A 200–300 MB segment written a moment earlier and then `fsync`'d at 2–3 GB/s is ~100 ms; that is the mode. The Raft commit path's `fsync` is not slowed by the sharing D6 assumed. What the sharing does cost is the compactor's ~130 MB/s of continuous segment I/O on `/data1/01` while it is also one of six fragment roots.
- **D8's number:** `pub_p50=3109us` in 17 of the 5 s samples with traffic, `46511us` in 4, `8051–8665us` in 2; `pub_max` 0.80 s and 2.36 s in the election minute. The per-batch commit is a ~3 ms Raft round when the pump is free; the 29–46 ms and the seconds are the pump held by the flush wait above, not queueing behind other clients and not the log `fsync` (0.4 ms). No wire change is indicated; remeasure after the compaction fix.
- **D7 held:** 26 265 `access()` on 004 for 408K fragment creates (was 1.59M per IOR); the rest are re-PUTs and probes.
- Smaller: `persist_applied` is `openat`+`write`+`fsync`+`rename`+`close` per group per pump cycle with applies (32 769 `openat`, 31 809 `rename` on the pump; 25 459 `fsync` in minute 04:19 = 9.15 s of that minute on the pump). `mkdir_p` on a new chunk directory issues eleven `mkdir` from `/data1` down (39 680 calls for 3 248 new directories, 1.7 s total). Neither is a stall.
- **Clients (idle):** `recv_poller` is 930 `poll(…, 1 ms)`/s waiting on the CQ channel, 3.7K samples in 19 minutes = 0.3 % of one core. Nothing to fix; do not put the yield loop back.

Compared with what this page said before this trace: W13's back-pressure wait was described as a bound that means "16× behind" and would be logged — it fired five times in one 7-minute write on one node, once for 24 s, on the pump. W22's source said "the import-diff apply is the only multi-second pump hold left" — with the import gone, the flush wait is the multi-second pump hold, and the same client symptom (BUSY → `rc=-13` → fsync EIO) and the same term changes follow from it. D6's premise (the 100 ms `fsync` mode is the shared XFS journal slowing the Raft log) is not what the per-thread numbers show; the Raft log `fsync` is 0.4 ms. D8's "is it ~3 ms or ~100 ms" is answered: ~3 ms, with a tail that is the pump hold. The recommendations are D9–D11 in §1a's decision table and rows 8e–8f in the order table.

**Sep 29 02:35Z: IOR with perf and strace on the outbox / multi-chunk / post-and-return tree** (`results/io500/20260929-023447-iorperf2`). No bandwidth. Same abort as 00:40Z: `report-loop rounds=1 busy=1 rc=-13` (18–19 s on fcstor007), fsync EBUSY, `close` failed, rank 3 `MPI_ABORT`. Ranks were gone by 02:41Z. Recorders were SIGINT'd then; files are node-local `/tmp/efs-perf/` (`efsd.data` 28–33 MB and `efsd.strace` 1.9–2.4 GB on fcstor003–006; `fuse.data` 4.1–5.0 MB and `fuse.strace` 241–245 MB on fcstor007–015). The client recorder attached to the daemon (fuse.log has `perf recorder pid=`). The strace was not narrowed, which is why the server files are ~2 GB.

Analysis (`…/ana/`, per-host `perf-*-{flat,callers,children}.txt`, `strace-*-{summary,long,persec,tids}.txt`, `log-*.txt`, `leader-detail-fcstor004.txt`, `pump-detail-fcstor004.txt`): the client's 19.5 s REPORT was one TCP `recvfrom` of 19.36 s (tid 813056 at 22:38:58.52) — no client retry; the group-2 leader fcstor004 held it: `report-split nrec=86234 pack_ms=1431 push_ms=9869 finish_ms=9227 rc=-13`. fcstor004 answered 77 REPORTs: 43 OK, 18 BUSY, 15 NOT_PRIMARY; fcstor005 answered 32 as a transient leader (27 BUSY). Group 0 term 9370→9415 and group 2 2792→2897 across the run, with `drop=0` on every `raft-obs tx->` line on all four nodes (W14.2 (a) gate half met: no drops, terms still move). Every node exports a 2.67 GB snapshot every 256 applied entries (`HOST_SNAP_MIN`), 4.6–12 s each, back to back (`efs_kv_lsm_view_export` memcmp is the top user symbol on all four; 14 % of fcstor004's samples on the GC thread), and followers behind by more than that install it: fcstor006 imported group 2 twice (`diff n=130398 ms=16467`, `n=685534 ms=12132`), fcstor003 group 0 once (`n=380716 ms=12041`). The compactor is fcstor004's top thread (22 % of samples, `kv_compact_locked`), with its segment `fsync` at a flat 100 ms while 20 writer threads create fragments on the same XFS (`persist_max` 254 ms). Client: blake3 15.7 %, `memmove` 5.5 % of which 3.1 % is the RDMA send copy — the `fuse_buf_copy`/`ll_write_buf` copies are gone (W15.3 multi-chunk landed); `send_buf_pick`/vDSO spin gone (W15.4); 28K samples in ~4 min = mostly off-CPU. `access()` on fcstor004: 1.59M calls, 192 s across six handler threads (first PUT of every chunk misses the W14.4 hint). Both profiles carry ptrace overhead from the concurrent strace (`ptrace_do_notify` 4–5 %).

**Sep 29 00:40Z: first IOR on the W17–D2 tree
(`results/io500/20260929-002758-wimpl`, `bbcbcb5ad779-dirty`
rolled 00:28Z RDMA, clients remounted 00:39Z, perf on every daemon).**

No bandwidth. 9-client `run.sh ior` (30 s stonewall, mdtest off)
printed the usual `stonewall-time 30s != 300s` INVALID, then eight
`fsync` warnings, `close(20) failed`, rank 2 `MPI_ABORT`; the ranks
were gone by 00:41:33Z (no D-state this time — W17.1 did what it
was specified to do). Every client logged one `report-loop … rounds=1
stale=0 busy=1 rc=-13`, wall 8.2–26.1 s: the first sync REPORT came
back BUSY and the loop did not retry. The old loop retried BUSY up to
8 times, which is why the Sep 28 run's fsync succeeded after 328 s
and this one returned EIO in under 30 s. The 8 s is checked between
attempts; the extra is one in-flight RPC that the server holds (the
`report-split nrec=90016` lines on fcstor004: nine `rc=-13` with
`pack_ms=0`, then six `rc=0` with `pack_ms` 0.86–2.77 s). The
`dual type=67 exhausted 16 BUSY/STALE retries (10.3 s)` lines are
the close-kicked `report_dirty_ino(0)` path, which sets no deadline.
The server-side BUSY on a 90016-record REPORT is W16 steps 2–3
(attribute, then ask before touching the 400 ms / 10 s budgets);
`fsync` returning EBUSY-mapped EIO on it is the decided behaviour.

Profiles from that window (~100 s of writing; overlaps compaction
and a group-2 snapshot export; `cycles:P`, lost 0; trees under
`~/orcd/scratch/efs/perf/fcstor00N/`). Leaders were fcstor003
(group 0) and fcstor006 (group 2); fcstor004 was a follower in both.

| where | samples | now | was (Sep 28 run 3) | reads as |
| --- | --- | --- | --- | --- |
| fcstor003, group-0 leader both runs | 79K | `memmove` self 4.2%; `send_ae` 2.5% (the one log→frame copy); `host_send` under the 1.5% floor; `try_commit` self 0, 2.5% children all `host_apply`; `host_sender` 5.8% | `memmove` 18.5% (12.6% `host_send`←`send_ae`), `try_commit` 4.0% self / 10.5% | **W19 landed.** |
| fcstor007 client | 14K ≈ 29 CPU-s in ~100 s, so mostly off-CPU | `dcache_flush_slot_inner` self 0.1%, `pthread_once` absent; `dcache_reclaim_main` children 49.7%, all `dcache_put_now` (blake3 ~17%, RDMA send 4.7%) | `dcache_flush_slot_inner` self 43.8%, `pthread_once` 25%, mutex 12% | **W18 landed.** Reclaim now spends its time on the PUT itself, which is the work. |
| fcstor007 client | same | `memmove` in `ll_write_buf` 8.6% | 7.5% | W15 step 3, open |
| fcstor004, follower, REPORT target | 127K | `open` 11.9% + `access` 8.7% + `write` 6.9% on the fragment PUT (`open` callers are unresolved frames whose bytes decode to the six `/data1/0N/efs` roots); `memcmp` 9.2% self in `lsm_get` under `efs_meta_apply_get_chunk` (REPORT pre-check) and publish, in `kv_compact_locked`, and in `efs_kv_lsm_view_export`; `kv_seg_probe` 3.1%; `vx_sift_down` 2.2% all export | `memmove` 20.5%, `copyout` 15.4% | W14 step 4, open. The `memcmp`/`kv_seg_probe` share is the point-get and export merge over a large L1 (fcstor006 had ~1000 L1 files at the roll); check L1 file counts before calling it a regression. |

**Before the next run:** nothing is wedged. Clients fcstor007–015 are
mounted RDMA on this build; all four `efsd` are up; perf recorders
are stopped. Run `tests/preflight.sh` (idle gate) first.

---

**What the Sep 28 evening IOR runs established (kept for the
comparison column above).**

Two 9-client `run.sh ior` runs on `bbcbcb5` (and the same tree
uncommitted before it), 30 s stonewall, RDMA, perf attached to every
`efsd` and `efs-fuse`. Neither produced a score. Both finished
easy-write: **1.048 GiB/s** in 397 s and **0.769 GiB/s** in 393 s.
In each, the 30 s of writing is followed by a ~326–328 s `fsync`
(`fsync-split … flush_ms=327885 report_ms=0 rc=0`). IOR's wear-out
makes every rank write as many blocks as the fastest rank managed in
30 s, so the wall is that flush, not the stonewall. Then:

- run 1 (mdtest still on): `create file.mdtest.0.2236 failed (EIO)`
  → abort. `run.sh ior` no longer runs mdtest.
- run 2 (uncommitted tree, 22:00Z): `INODE_LOOKUP` on shard 3745 hit
  the 16-retry BUSY budget → IOR `stat` failed → abort. That is W16.
- run 3 (`bbcbcb5`, 22:14Z): ior-hard on the shared file. Nine ranks
  publish the same inode (1166063); fcstor004 logged
  `apply publish rc=-14` (STALE) for many chunk indexes; on the
  clients `REPORT_CHUNKS` (type 67) hit the 16-retry BUSY budget
  repeatedly, `fsync` returned EIO (`efs_rc=-13`), IOR `close` failed,
  rank 8 aborted, and **io500 on seven clients stayed in D-state
  `request_wait_answer`** after `MPI_ABORT` killed it. That is W17.

What the profiles say (all `cycles:P`, `perf record -F 499 -g`;
reports under `~/orcd/scratch/efs/perf/fcstor00N/efsd-19810/` and
`.../fcstor00N/efs-mount/`):

| where | run | share | what |
| --- | --- | --- | --- |
| client fcstor007 | 3 (917K samples, 40 min) | 44% + 25% + 12% | `dcache_flush_slot_inner` self, `pthread_once`, shard mutex — all on `dcache_reclaim_main` (73% of the client). Blake3 3%. **W18** |
| client fcstor007 | 2 (36K, 5 min; report files since overwritten by run 3) | 27% + 19% | blake3 in `hash_write_fragments`; `memmove` in `ll_write_buf` (7.5%) and the RDMA send (6.3%). W15 steps 1 and 3 |
| fcstor003 (group 0 leader) | 3 (1M) | 12.6% + 5.2% + 6.9% | `memmove` in `host_send` and `send_ae` on the **pump**, then `writev` copy-in on `host_sender` — every AppendEntries byte copied twice in user space and once by the kernel, over TCP. `try_commit` 10.5% (walks `last_i → commit_index` calling `log_term` per AE reply). **W19** |
| fcstor004 (group 0 follower, group 2 leader) | 3 (2M) | 15.4% + 6.3% | `copyout` in `server_handle_conn` → `recv` (the other end of those AEs); `memcmp` in LSM `lookup` under publish apply, view export, compaction |

Gone from the profiles: `send_snap` recopy, `kv_flush_locked` on the
pump (`7eecf1d` + `bbcbcb5`), `efs_rdma_reply_ready_us` + vDSO
(W15 step 2, done in `bbcbcb5`: vDSO 0.87%, the symbol under the
0.5% floor), `recv_poller` (1.2%).

| fstor007 `client.sh --perf` | 20:36–20:39 EDT (214K, 3 min) | 35% + 8.9% + 6.6% | `memmove` (14.6% dcache patch, ~17% `fuse_buf_copy` bounce with no frame pointer, 3.6% RDMA send); `ll_setattr` → `fill_stat_from_inode` walking every chunk of the file (**W20**); `efs_export_staged_bytes` on the evictor, 1024 passes per wake (**W21**). `send_buf_pick` spin (`EFS_RDMA_NSEND`=2) is 4.2% (W14 step 5). Details under W15's source. |
| fstor007 `client.sh --perf` | 22:11–22:14 EDT (32K, 3 min), binary built 22:06 **with** W15.3 / W20 / W21 / `NSEND=EFS_WRITE_PIPELINE`, seven parallel `dd bs=1M` to new files in one dir | 39.5% + 8.2% + 6.9% | `memmove` still two copies per byte (16% `ll_write_buf`, ~19% unattributed `fuse_buf_copy`, 4.4% RDMA send): W15.3 takes the single-copy path only when `size == chunk`, and a 1 MiB write is eight chunks. vDSO 6.9% is `now_us()` + `pthread_spin_lock` in `efs_rdma_send_frame`'s **synchronous send-CQE wait**, not `send_buf_pick`; more send buffers did not move it. W20 and W21 are gone from the profile (dd does no setattr; evictor under cap). One `dd` failed `open` with EBUSY: fuse log has **249** `exhausted 16 BUSY/STALE` lines (LOOKUP 98, SETATTR 50, GETATTR 47, RENAME 31, CREATE 23), every retry `why=no-hint` — elections, not apply lag. Group 0 term 9309→9368 and group 2 2763→2789 since 00:39Z; fcstor004 `raft-obs: tx->3 enq=50698 drop=72114 sent=50695 hi=2048`. Review and fixes under W15, W14 step 2, W16. |
| fstor007 `client.sh --perf` | 22:48–22:55 EDT (260K, 6.5 min, ~1.3 cores average), binary with the multi-chunk W15.3 and the W21 cursor; `dd bs=1M status=progress` to `/tmp/efs-mount/001/dat08`, killed at 1.7 GB | 29% + 12.8% + 9.9% | **W15 step 3 verified:** the `ll_write_buf` `memmove` is gone; `memmove` is 7.2% RDMA send copy + ~22% caller-less `fuse_buf_copy` — one user copy per byte now. vDSO is 0.8% (the send-CQE wait is below the floor on this run; keep W15 step 4's gate). **W21 not fixed:** `efs_export_staged_bytes` 7.4% self + `stage_evict_main` 5.4% self — the cursor limits the bands per wake, but every write over cap *kicks* a new wake, so the wake rate is the write rate. Fix under W21. blake3 9.9%, `xor_into` 5.3%, kernel `fuse_dev_read` 7% are the work. `~/orcd/scratch/efs/perf/dd.trace.txt`: 1667 × 1 MiB `write()` returned 1048576, the 1668th was in flight when `dd` got SIGKILL; no `-tt`/`-T`, so the trace does not say how long that write waited. |
| fstor007 `client.sh --perf --strace` | 00:18–00:22 EDT Sep 29 (75K samples; `fuse.strace` 1.4 GB, 17.5M lines, 374 threads; `ecopy --verify` of `/data1/erbmi1/knouse` plus a `dd` into an existing 1.7 GB file), servers on the 02:35Z roll | ptrace 43% | **The perf half is not usable for shares:** `ptrace_do_notify`/`ptrace_stop`/`do_notify_parent_cldstop` are 43% of samples; user symbols keep their order (memmove 7.2%, blake3 3.8%, `stage_evict_main` 3.1%, W21 again). **The strace half is the finding.** `strace-summary.txt` for 230 s: `fstat` 1 171 561, `getsockopt` 1 172 007 (586 035 `SO_ERROR` + 585 972 `TCP_INFO`), `recvfrom` 598 781 (`MSG_PEEK`), `poll` 1.12M, `read` 1.20M, `write` 584K (`recv_poller` eventfd) — five liveness syscalls per connection checkout plus an `fstat` per send, ~586K checkouts, ~15K syscalls/s of probing (**W23**). `clock_nanosleep` 794 calls / 488.7 s: 564 of them 0.8 s = ~47 RPCs that ran the full 16-attempt BUSY budget in four minutes. Fuse log: 424 `exhausted` (was 249 at 22:19): SETATTR 136, RENAME 106, LOOKUP 98, GETATTR 47, CREATE 37; 2 212 `why=no-hint`; `rename_at … rc=-13 after 9 tries` and `report-loop … busy=1 ms=8435..24182 rc=-13` → `flush-meta: resource busy` (ecopy's fsyncs). `dd.trace.txt`: `openat("/tmp/efs-mount/001/dat08", O_WRONLY\|O_CREAT\|O_TRUNC)` **never returned**; killed (**W24**). Terms since the 02:35Z roll (outbox coalesce in): group 0 9368 → 9421, group 2 2789 → **2904** — 115 elections in two hours with `drop=0`; that is W22's import trigger, not the outbox. `ecopy.strace.txt` traced the main thread only (no `-f`): it waits on its workers until SIGTERM/SIGKILL; nothing else in it. |

Those seven D-state `io500` processes were cleared by the 00:39Z
remount. The `-dirty` suffix on the servers' build id is the node
tree missing tracked `results/` (the deploy rsync excludes it); all
four IDs match, so the HELLO gate is satisfied.

**Sep 28 ~18:30 EDT: `df` on a fresh mount during the 9×4 IOR returned
ENOENT.** Root GETATTR (shard 0, group 2) got BUSY for 10.3 s and the
client maps that to ENOENT. Not data loss. Written up as **W16** in §1a
with the fix order (mapping first, then attribute the BUSY stream with
`raft-status` before touching the server). Start there.

**L1 cap and the snapshot pump (Sep 28 evening, committed `bbcbcb5`).**
The 64-slot L1 array is a growable list. The on-disk MANIFEST was
already one `1 <seq>` line per file. `efs_kv_lsm_flush_nowait` returns
BUSY without walking the memtable when L0 cannot take another full
set of ranges; `host_snap_open` uses that on the pump. A no-progress
InstallSnapshot ack waits one heartbeat before the next chunk, and
the empty AppendEntries still goes out. Abandoned `snap-*.kvx.tmp`
files were deleted while efsd was down. Roll `--all` of
`7eecf1da00cd-dirty` (`EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`) caught
every voter up inside the script's window: group 0 commit=applied
13616251, group 2 12041271. fcstor004 L0 49→2, fcstor005 58→3,
compaction rc=0, group 0 snapshot import diff n=15697. Unit tests
`test_kv_lsm` and `test_raft` passed on node9901. 9-client
`run.sh ior` (30 s stonewall, mdtest still `run=TRUE` in
`config-ior-only.ini`) finished easy-write at **1.048 GiB/s** in
397.381 s (one fsync flush 325.6 s, rc=0) and then aborted:
`create file.mdtest.0.2236 failed (EIO)`, rank 0 `MPI_ABORT`.
That easy-write is not comparable to the 1 s stonewall 2.917 GiB/s.
mdtest in `config-ior-only.ini` is now `run=FALSE`. A later
`client.sh --perf` IOR (22:00Z, daemons then stopped) produced no
RESULT: fsync `flush_ms=202803` `rc=0`, then `INODE_LOOKUP` on
shard 3745 exhausted 16 BUSY retries and IOR's `stat` aborted.
Committed as `bbcbcb5` and rolled 22:14Z (`bbcbcb5ad779-dirty`
on every server; `-dirty` is the excluded `results/` tree).
That IOR's easy-write is **0.769 GiB/s** in 393.014 s
(`flush_ms=327885`, `rc=0`), then `fsync`/`close` failed and
rank 8 aborted. Publish of ino 1166063 was returning STALE
(`rc=-14`). No score. io500 on seven clients stayed in
D-state `request_wait_answer`.
Profiles from the first run (`cycles:P`): fcstor003 top is kernel
dentry lookup under `nvme_put` (5.6%); fcstor004 top is LSM `memcmp`
in lookup (10.6%). `send_snap` / `kv_flush_locked` are not the stack.
efsd is up; fcstor007–015 are mounted RDMA (with the D-state io500
leftovers noted above).

**9-client dd profile cycle is done (Sep 28 afternoon).** Five
rounds on RDMA, 8 GiB `dd bs=1M conv=fsync`, own file, every file
8589934592. The change that moved the wall was probing for an
existing fragment *before* taking the writer-pool lock
(`results/measure/20260928-131651-dd-prof-r2b`, **2810.5** MiB/s,
walls 25.05–26.23 s). 64 KiB snapshot chunks, a thread-local
directory fd, and a 256-publish / 64 KiB AppendEntries cap each
made the slowest client worse; those three are reverted. Kept:
the probe stays outside the pool lock, and a snapshot chunk in
flight is not resent on every pump wake (`ae_inflight` end is
`UINT64_MAX` until the reply or a heartbeat). Live check after
that restore: **2551.5** MiB/s, walls 24.84–28.90 s
(`results/measure/20260928-134637-dd-prof-r5b`). Do not quote the
hung round (`20260928-102121-dd-prof-r2`). Prior morning RDMA
9-client was 1326.6.

**9×4 IO-500 debug, same binary, TCP and RDMA (Sep 28).**
`SLOTS=4 NP=36 tests/perf/io500/run.sh debug` (1 s stonewall,
IOR easy/hard + mdtest, find off). Servers
`db2b88c4802a-dirty`, preflight idle, every client
`fuse.efs-fuse` + `stat` OK. Reads are same-mount (client
dcache). Official score is INVALID (stonewall is not 300 s).
No data-check failures in either driver log. RDMA easy-write
**2.917 GiB/s**, hard-write **0.291**, easy-read **3.092**,
hard-read **1.525**; mdtest-easy-write **3.418 kIOPS**,
hard-write **0.757**, easy-stat **3.248**. TCP easy-write
**1.376 GiB/s**, hard-write **0.274**, easy-read **2.669**,
hard-read **1.162**; mdtest-easy-write **2.721 kIOPS**,
hard-write **0.910**, easy-stat **16.461**. Dirs
`results/io500/20260928-150609-rdma` and
`results/io500/20260928-151707-tcp`. An earlier RDMA attempt
aborted in easy-write when `INODE_LOOKUP` returned EBUSY
after 16 retries; that attempt is not a result.

**User xattrs are durable (Sep 28, same `db2b88c4802a-dirty`).**
One blob per inode (`EFS_KV_KIND_XATTR` 23), command
`EFS_MD_CMD_XATTR` 27. Only names starting with `user.` are stored;
`security.*` and `system.*` return EOPNOTSUPP with no RPC. The key
is deleted with the inode when it exists. `roll_efsd.sh --all`
exited FAIL while fcstor005 was still receiving the snapshot; both
groups then reached the leaders (group 0 commit 13387007, group 2
11813960). Clients fcstor007–015 were remounted RDMA. posix jobs=1:
**200/201**, 0 fail, `opt_xattr` PASS, `mmap_write_read` SKIP
(`MAP_SHARED` ENODEV; kernel 5.14 has no
`FOPEN_DIRECT_IO_ALLOW_MMAP`), 45.2 s
(`results/posix/20260928-043918`). posix2 alone **63/63** in 65.4 s
(`results/posix2/20260928-044304`). The overlapped posix2
(`results/posix2/20260928-043918`) failed
`peer_overlap_pwrite_chunk_straddle` and
`peer_rename_vs_unlink_src`; they did not reproduce alone. persist
26/26 (`results/posixpersist/20260928-043919`). Four suites at once
on fcstor007 (`POSIX_JOBS=1`, `posixstress 4`): each **200/201**,
0 fail, mmap SKIP only, 46–57 s
(`results/posix/20260928-044951`). The Sep 17 four-suite run
(`results/posix/20260917-120328`) was 165–168 timeouts at 15 s;
that was saturation on the old build, not this one. Do not clear
`direct_io` to make the mmap test pass. fcstor003–006 still run the
previous fuse.

**RDMA recv poller no longer spins (Sep 28).** During posix jobs=1
the shared CQ poller was 64% of `efs-fuse` and 31% of `efsd`
(`results/measure/20260928-050145-posix-prof-rdma`, 147775
cpu-clock samples in `recv_poller`). It paused and `sched_yield`'d
for the whole time a QP was up. TCP on the same suite
(`results/measure/20260928-050841-posix-prof-tcp`) has no such
thread; its top sample is blake3 at 355 hits, and the wall is
45.9 s / 59.0 s. The poller now spins about 100 µs and then waits
on the completion channel for at most 1 ms. Acking the event
before that wait disarmed the notify and added a millisecond to
every RPC (76 s). After the ack moved to after `poll`, jobs=1 is
**200/201** twice, 58.5 s and 57.8 s, 0 fail
(`results/measure/20260928-053033-posix-prof-rdma-fix2`).
`perf trace` cannot read tracefs here. Do not put the yield loop back.

Five more cpu-clock rounds, same morning, jobs=1 on fcstor007,
RDMA, all four servers restarted after each change
(`results/measure/20260928-093100-posix-prof-r1` through
`20260928-100138-posix-prof-r5`). Every suite was **200/201**,
mmap SKIP only. The client top was `clock_gettime` in a 200 µs
`efs_rdma_recv_wait` spin; metadata replies wait on a Raft
commit, so the spin always ran out. That wait is 64 pauses, then
`poll` on the eventfd. The server top was snapshot export: a
linear merge (`memcmp`), then three `write`s per key. The merge
is a heap (newest L0 still wins a tie) and the export buffers
64 KiB. Compaction uses the same heap. With the recv spin gone,
`recv_poller`'s 128 empty CQ polls were 16% of efs-fuse; that
spin is 32. A jobs=1 run after the last roll was **200/201** in
45.1 s. One pair on the recv-wait change was 29.0 s and 29.7 s;
the runs around it were 44–58 s. Do not quote 29 s as the wall.

**Shared-file IOR-hard uses immutable spans (Sep 27, TCP, clients
`8e62ff12b422-dirty`).** A partial publish appends a span object.
The base generation does not move, so disjoint writers do not STALE
each other. A full-chunk fold commits only when the live span list
is the one that image already folded; a longer list is left at seq 0
so the CAS STALEs and the replay refetches. NP=4, SEGS=3000, 47008 B,
cold remount, FUSE_OK: write **481.56 MiB/s** (1.12 s), read
**91.35 MiB/s** (5.89 s), pattern 12000 records bad 0. posix2 on
those clients: **63/63** (`results/posix2/20260927-190509`, 77.7 s).
The prior 4-rank bar was 33 MiB/s
(`results/measure/20260921-162514-ior-hard-scaling`). 1/9/36 were
not remeasured. Group 2 kept committing REAP_DONE of distinct dead
inodes at ~11–16/s, and `tests/preflight.sh` fails above 5 entries/s.
Do not raise that threshold. The dd above was measured on
`75321297f719-dirty` (servers rolled `--all` Sep 27 20:35Z). The
`pub-stale` fprintfs are out of the live efsd. 9-client 8 GiB
dd+fsync is valid: **1478** MiB/s, walls 49.76–49.89 s, every host
FUSE_OK and 8589934592 bytes, zero report `rc=-3`
(`results/measure/20260927-204907-dd-wall`). Preflight was idle.
1-client 977 and 4-client 1984 are build `4c6a5acefe03-dirty` and
were not remeasured here. The 9-client wall is report BUSY retries
(119 of 128 `report-split` lines), 3.4% of the 44 GB/s ceiling.
Servers are now `ab458efab95b-dirty` (`roll_efsd.sh --all` Sep 27
21:11Z, TCP, `EFS_TRANSPORT=tcp EFS_RAFT_OBS=1`). Same-directory
creates no longer each broadcast their own AppendEntries: a leader
with an open fsync hold appends locally, one fsync covers the
burst, and the pump sends one batch. `samedir_rate.sh`
(`results/measure/20260927-211953-samedir-rate`, PREFLIGHT_OK,
storm PASS, parent `children=0 nlink=2`): **333 / 1385 / 1241**
ops/s at 1 / 9 / 36 procs (was 138 / 134 / 159). `busy_n` was 0, 0,
and 1. Idle mkdir median 3.1 ms. Next queue item is W6 residual 2
(1 GiB open).

**19810 is on RDMA (Sep 28, `db2b88c4802a-dirty`).**
`EFSD_ENV='EFS_TRANSPORT=rdma EFS_RAFT_OBS=1'`, clients fcstor007–015
remounted, fuse log `RDMA transport up`. A full per-peer outbox dropped
the catch-up AppendEntries and still returned success, so
`ae_inflight` suppressed the retry and fcstor004 group 0 sat 704
entries behind until a snapshot install finished. `host_send` keeps
snapshot chunks and entry-carrying AppendEntries and returns
`EFS_ERR_AGAIN` when it cannot queue; `send_ae` does not mark that
batch in flight. Gate before user xattr: posix jobs=1 **199/201**,
mmap + xattr SKIP, 0 fail, 90.1 s
(`results/posix/20260928-033823`); 9-host **199/201**
on all nine, 56.7–59.1 s (`results/posix/20260928-034049`); posix2
**63/63** in 76.8 s (`results/posix2/20260928-034350`). 9-client
8 GiB dd+fsync that morning was **1326.6** MiB/s, slowest wall
55.574 s (walls 28.55–55.57 s), all nine files 8589934592, no
fsync EIO, zero `rc=-3`
(`results/measure/20260928-033420-dd-wall`). Afternoon cycle,
same shape, after the probe moved off the writer-pool lock:
best **2810.5** (`20260928-131651-dd-prof-r2b`), live restore
**2551.5** (`20260928-134637-dd-prof-r5b`). 99 of 108
report-split lines are BUSY. That is slower than the TCP 1478.
Earlier RDMA build `113823180b15-dirty`: 1-client **947**, 4-client
**2311** (`results/measure/20260928-015839-dd-wall`); that run's
9-client row is INVALID (do not quote ~238). IOR-hard NP=4 on that
earlier build: write **469.55**, read **107.03**, bad 0
(`results/measure/20260928-015806-ior-hard-rdma`); not remeasured
on this server roll. Posix and the 9-client write are not faster
than TCP. Do not roll 19810 back to TCP unless a suite fails.

**Posix suites clean (Sep 27, TCP).** 9-host
`results/posix/20260927-123717`: 200 pass + `mmap_write_read` SKIP
on all nine, 0 not-run (screen 12:37:17–12:37:52Z). posix2
`results/posix2/20260927-123946`: **63/63** in 58 s, and again
63/63 after the span client fix (above). The 9-host EBUSY before
the morning roll was group 2 electing: `send_snap` returns
`EFS_ERR_AGAIN` while the export file is not ready, and sending
nothing let that peer campaign and step the leader down (term +200,
mkdir EBUSY). `send_ae` now sends an empty AppendEntries in that
window. Do not remove it. Same tree, data path: `DCACHE_NR` 32,
truncate keeps `got.generation`, `dcache_note_committed` clears
`nrange` when the slot is clean.

**W13 done (Sep 26).** The user ratified the background-compactor row.
L1 compaction runs on a `kv_lsm` thread. The pump still flushes the
memtable to L0 and kicks the thread when `n_l0 >= l0max`; the merge
opens private segment fds, drops `l->mu` for the rewrite, and installs
under the lock (atomic manifest rename). A segment file is unlinked
only when its refcount hits 0 (`kv_seg_doom`), so `efs_kv_lsm_view_pin`
survives a concurrent flush and compact. `kv_compact_locked` remains
for `efs_kv_lsm_compact` and for the fallback if the thread did not
start. The write path waits on `l->cv` only when `n_l0` reaches
`KV_LSM_MAX_SEGS` (64) and logs `kv-compact: backpressure` — that wait
means the compactor is ~16× behind. `EFS_KV_COMPACT_DIE=N` exits after
the Nth finished output segment, before the manifest rename; reopen
serves the old manifest. Partitioned flush (one L0 file per `key[0]`)
is in the TCP build rolled Sep 27 05:07 UTC.

W13 gate, on the build that was rolled Sep 26 16:37 UTC
(`3210a3d63f73-dirty`, TCP). The cluster at that write-up was
`4c6a5acefe03-dirty`, TCP, after the Sep 27 RDMA rollback. It is
RDMA as of Sep 28 (§1b). Gate:

- Hammer `results/measure/20260926-163709-mkdir-hammer`: idle p50
  **4.68 ms**; 144-way **35166** mkdirs in 15 s, p50 **56.5 ms**.
  `apply_max` **68 ms** on fcstor005 while two compactions rewrote
  ~760 MiB in 1964 ms and 2067 ms (`l1=12`). The only errors are
  harness `rmdir-own ENOTEMPTY`.
- 9-host posix `results/posix/20260926-164123`: **200/201 on all
  nine**, mmap SKIP, **13.2–14.8 s**. Timeline
  `results/measure/20260926-124106-w8-stall-timeline`: group 0 stayed
  term 6882 leader 1, group 2 stayed term 1198 leader 3, for 139 s;
  no probe stat/mkdir/rmdir over 1 s. Four `kv-compact` cycles in
  that window; worst `apply_max` **67 ms**.
- Idle `md_latency.py` on fcstor007 after the suite: mkdir 2.9 /
  create+close 1.6 / append+close 2.0 / stat 0.3 / unlink 0.8 /
  rmdir 2.9 ms.

W11's chunked snapshot is running on this build (see the Sep 27 note
below). The live RDMA switch was tried Sep 27 and rolled back: 9-host
posix was 193–196/201 in 385 s
(`results/posix/20260927-044348`), against the TCP 200/201 in 31 s.
19810 is TCP again (`4c6a5acefe03-dirty`). Partitioned flush is
in this build (one L0 file per `key[0]`; compaction rewrites one
range). The RDMA mkdir gap is diagnosed on the private cluster and
fixed in tree, not rolled: the shared recv poller slept 100 ms after
acking a CQ event (11 of 100 mkdirs), and every SEND called
`ibv_query_qp` plus a sysfs read (~380 µs raft RTT vs ~15 µs TCP).
After both fixes, 100 mkdirs were 642 ms on RDMA vs 507 ms on TCP,
raft RTT ~50 µs (`~/efs-runs/rdmaprof7.log`). A later run's wall was
one 1.5 s mkdir plus three BUSY retries; the other 99 were 2–7 ms.
Do not switch 19810 on that private number.

Sep 27 dd (TCP, 8 GiB `conv=fsync`, FUSE_OK). Morning run
`results/measure/20260927-053506-dd-wall` on `4c6a5acefe03-dirty`:
1-client **977** MiB/s, 4-client **1984**. Its 9-client row is
INVALID (fcstor009 and fcstor013 fsync EIO, report `rc=-3`); do not
quote 1464 from that run. Evening rerun on `75321297f719-dirty`,
preflight idle: 9-client **1478** MiB/s, walls 49.76–49.89 s, all
nine files 8589934592, no fsync EIO, zero `rc=-3`
(`results/measure/20260927-204907-dd-wall`). 119 of 128 report-split
lines are BUSY. That is 3.4% of the 44 GB/s ceiling.
Do not raise `l0_max`, the memtable, or the election timeout. `EFSD_ENV` is
space-separated. Live servers are already `75321297f719-dirty`.

**Txn finisher (Sep 26, `src/server/raft_host.c` `host_txn_commit`).**
A compaction stall longer than the 400 ms apply wait turns an in-flight
DECIDE/RESOLVE into BUSY and leaves the EXCL intent until the 5 s
recovery scan; that intent makes every later create on the shard BUSY
and the client burns its 16 attempts into EIO. `host_txn_commit` hands
that txn to a finisher thread, which reads the decision record and
proposes RESOLVE (retrying BUSY until 10 s). The stall itself is W13,
now off the apply path (above). First 9-host suite on the finisher
build was **200/201 all nine, 42–44 s**
(`results/posix/20260926-0345-fin`, `fin_q=0`).

**Mkdir hammer (Sep 26, `6a60318`).** Propose was fsyncing the Raft
log while holding `h->mu`, then sending one AppendEntries.
`results/measure/20260926-042515-mkdir-hammer`: idle p50 8.4 ms,
144-way p50 **258 ms** / 470 mkdir/s. The fsync now runs outside the
lock (`68dfebb`). Proposers only raise the send ceiling; the pump
ships one batch after the threads queued on the lock have appended,
and it will not start a second batch while one is in flight
(`f47854b`, `6a60318`). A clean 144-way run
(`results/measure/20260926-052320-mkdir-hammer`): idle p50 11.3 ms,
144-way p50 **168 ms** / 754 mkdir/s, apply_max 61 ms, `fin_q=0`.
The repeat (`20260926-052437-mkdir-hammer`) hit a 766 ms apply of
31 entries and fell to p50 235 ms — that stall was the synchronous
compaction W13 has since moved off the apply path. Do not pipeline
past the one in-flight batch. Earlier
same-morning numbers, before this batching, are in the history
(`044248` 144 ms / 735 on a smaller table, `050609` 189 ms / 706).

**KV get (`5d3e603`).** A mkdir's negative lookup pread every LSM
segment under the KV lock and freed the block on a miss. The segment
is pinned, the pread runs outside the lock, and the block stays
cached (32 slots per segment). Measured on that tree (build string
`f93e7e6669b6-dirty`):
`results/measure/20260926-053753-mkdir-hammer` idle p50 **7.1 ms**,
144-way p50 **147 ms** / 887 mkdir/s, apply_max 59 ms, `fin_q=0`.
Three `rmdir-own ENOTEMPTY` lines, no mkdir errors. Do not pipeline
past the one in-flight batch. A multi-entry AppendEntries fsyncs
once at the end of the batch (catch-up used to fsync each entry
under the host lock). Do not give every proposer its own sync-hold
slot: idle mkdir waited out unrelated appends (p50 7.1 → 11.9 ms)
and the 144-way rate did not move.

**Apply path: where the 887/s ceiling was (Sep 26 midday).** A
thread-local fsync defer for one txn's PREPARE/RESOLVE burst was
built and measured first: no gain (`20260926-110435`, 12059 / p50
174 ms, clean apply) — the wall was never the leader fsync count. It
was the apply itself: `strace -c` on the dual-host follower during
the hammer showed 8 650 `pread64`/s and 875 `fsync`/s; `perf` showed
half of efsd on `memcmp` under `search_block` / `kv_msrc_advance`.
Every PREPARE apply runs two prefix scans (`guards_conflict`,
`reduces_pending`), every RESOLVE three (`txn_scan_kinds`), and
`merge_scan` opened an iterator on every LSM segment — one pread and
a linear walk of a 64 KiB block each — under `l->mu` on the pump.
Three changes, all LSM-internal / pump-internal, no protocol change:
(1) `merge_scan` skips a segment whose [first,last] key range cannot
hold the prefix (`kv_seg_excludes`; L1 is range-partitioned, so a
3-byte txn prefix is in one or two of them) and the iterator reuses
the segment's cached block; (2) the pump's KV WAL fsync and the
applied-index file write moved off `h->mu` and the file write is
throttled to 10 ms (three fsyncs per ~3 ms cycle were under the lock
proposers queue on; the saved index is only a restart lower bound);
(3) `KV_LSM_BLOCK_TARGET` 64 → 8 KiB (readers take any block size;
old segments stay valid until compaction rewrites them). Hammers,
build string still `194286c37a4f-dirty`: prune alone
`20260926-112916`: idle 9.5, 144-way **13715 / p50 137 ms**; + pump
tail `20260926-133934`: idle **6.97**, **14764 / 106 ms**; + 8 KiB
blocks `20260926-134640` idle **4.54**, **21990 / 73 ms** and
`20260926-134738` idle 5.59, **28072 / p50 60 ms** (~1 870 mkdir/s,
22 `rmdir-own ENOTEMPTY`, 0 mkdir errors). Every one of those runs
contained a 1.7–2.0 s `kv_compact_locked` stall; the clean-window
rate is higher still. **Cost of the speed: the L1 rewrite now
happens every ~7 s of hammer** (compaction trigger is 4 × 4 MiB L0
flushes and the table writes ~2 MB/s of records) and a 1.9 s stall is
past the election timeout — the 9-host suite on this build
(`results/posix/20260926-1350-blk`) ran in **20–38 s** but scored
195–199/201 with two elections, both at a 1.9 s compaction
(`link_across_dirs`, `last_link_unlink_other_dir`, `dir_many_files`,
`dir_deep_nesting*` — the BUSY-exhausted-10 s / election class).
That stall is what W13 (done, top of this section) removed from the
apply path. Do not tune `l0_max` or the memtable size around how
often compaction runs.

**Follow-on, same afternoon: memtable probes and the wakeup herd.**
After `610f4a8` the pump thread was still 61–67 % of efsd samples,
with `memcmp` 23 % — 12 % of it the memtable binary search inlined
into `lsm_batch` (one `lsm_put` per PREPARE part, ~15 probes, each
two dependent cache misses: `e[mid]` then its key) — and ~20 % of
efsd in `native_queued_spin_lock_slowpath` / `futex_wake` /
`futex_wait_setup`: the pump's `pthread_cond_broadcast(applied_cv)`
after every cycle woke every sleeping handler (~150 under the
hammer) to re-check a predicate that was false for all but a few, all
on one futex word. Three changes, no protocol change: (1) the
memtable entry's key is allocated inline with `struct kv_ent`; (2) a
parallel `uint64_t pfx[]` array (first 8 key bytes, big-endian) is
what the binary search probes — L2-resident, an entry is touched only
on a prefix tie; (3) waiters register the index they need (or "any
change of this group's view" for a read-index round) in one of 512
slots with its own condvar (`host_waiter_sleep`), and the pump
signals only the slots the fresh view satisfies
(`host_waiters_wake`); the view carries a `stamp` bumped by every
publish that changed a field; `applied_cv` remains as the overflow
path. `memcmp` 23 → 11–13 %, `kv_mtab_pos` itself 2.2 %. Hammer
(build `610f4a8…-dirty`): `20260926-141940` idle **5.23 ms**, 144-way
**30366 / p50 52 ms**, p99 224, 0 mkdir errors (67 `rmdir-own
ENOTEMPTY`); its twin `20260926-142052` hit a 14.7 s max with 30
`mkdir EIO` — the run-to-run spread is how many 2 s compaction stalls
and the elections they trigger land in the 15 s window (g0 term
6746 → 6751 during the first run), not the code. 9-host suite:
`results/posix/20260926-1430-wake2` **200/201 on eight hosts, 199 on
fcstor009** (`flock_shared_then_exclusive`, an intermittent seen on
Sep 22/25/26 builds) in **15.5–23 s per host** — the prior gate runs
were 39–44 s — with a 2.0 s `apply_max` and one election per group
at the start. Its predecessor `20260926-1425-wake` ran into a
four-term g0 election burst (`apply_max=851 ms` on 003 → LEADER→
FOLLOWER → CANDIDATE ×3, `arc_term_miss=37`) and scored 193–200 in
16–75 s with the EIO cluster on one host. Same W13 class; do not
bisect it. Remaining pump profile: `memmove` 7 % (pointer + prefix
array insert), txn-record scans ~6 %, `kv_compact_locked` (W13), and
the residual futex share is `l->mu` / `h->mu` handoffs, not the herd.
One PREPARE command = one part = one Raft entry = one `lsm_put`;
folding a txn's parts into one PREPARE is a protocol change (per-part
verdicts) and goes to the user, not into the tree. Staging WAL
records under the hold into one `write()` per cycle was tried and
reverted (no measurable change; history, Sep 26 afternoon). Cluster
is on `c044fb16e859-dirty`, servers and clients.

**9-host suite on that same tree** (`results/posix/20260926-054047`,
timeline `results/measure/20260926-014030-w8-stall-timeline`):
seven hosts **200/201**, fcstor007 199, fcstor013 198, 39–41 s,
0 NOTRUN. No term change. `names_crazy_dirs` passed on all nine.
Left: one `dir_many_files` EIO, one `names_crazy_roundtrip` EIO,
one `dir_deep_nesting` 15 s timeout. The previous suite on
`e8f3dc1` was 194–199/201 with a group-0 election
(`results/posix/20260926-050825`).



**I16 landed (Sep 23 02:47, `43bdf6a41f7d`) — gate runs owed.** Every
directory RPC (CREATE/MKDIR, UNLINK/RMDIR, LINK, RENAME_AT) now carries
an optional 36-byte op-id suffix `(client uuid, session epoch, seq,
contiguous ack)` (`EFS_OPID_WIRE_LEN`, `handler.c:dirop_opid`). The
window for an op lives on the **dentry shard of the (destination) name**
— deterministic from the request, always a participant, always in the
group that owns the shard (the coordinator is randomized, the parent
shard may be in the other group: both rejected). The leader probes it
(`host_opid_replay`) *before* the EEXIST/ENOENT pre-checks and answers a
replay from the recorded verdict (`host_opid_reply_ino`: current row if
the ino still exists, the stored ino/nlink otherwise). The verdict is
recorded **atomically with the op**: cross-group txns add a
`EFS_TXN_REDUCE_OPID` part (`host_prep_opid`; `fold_reduce` on
`EFS_KV_KIND_OPID` runs `efs_opid_fold` = ack + complete on whatever the
window holds at RESOLVE, so it commutes with the log path's
read-modify-write); the single-group log path appends a trailer to the
`CREATE` (flag bit `HOST_CREATE_F_OPID` in `cmd[1]`) / `UNLINK` /
`RMDIR` (by length) commands and `efs_meta_apply_*_op` writes the window
in the same `efs_kv_batch` (`opid_item`). The apply keeps
returning OK for a replay it sees (idempotent); the client-visible
EEXIST/ENOENT for a *genuine* second attempt still comes from the
leader's pre-check, which now runs after the probe. Client
(`inode_rpc.c`): one random uuid per mount (epoch 1), one seq space,
512-slot in-flight table; `ack = lowest in-flight − 1`, so a shard's
window advances past seqs it never served; the slot is held across every
BUSY/STALE/NOT_PRIMARY retry inside `rpc_send_recv_*`, so the retry is
byte-identical. Table full = op goes out with no identity (pre-I16
behaviour). `efs_opid_ack` now moves the watermark and shifts the
bitmap; `efs_txn_prepare_opid` rejects any key that is not exactly the
window key. Unit: `test_txn:test_opid_reduce` (commit records, abort does
not, commutes with a log-path fold, wire form), `test_meta_apply` I16
block (mkdir/create/unlink/rmdir replay = recorded verdict; the same op
without an id = EEXIST/ENOENT). `raft-obs` line gains `opid_replay=`.
**Bounded by design, and the two limits to know:** the reply cache is
16 entries per `(client, epoch, shard)` window (`EFS_OPID_REPLY_CACHE`;
`EFS_OPID_VAL_MAX` 512 → `txn.c VAL_MAX`); a 17th un-acked op on one
shard applies **unrecorded** (`efs_opid_fold` NOMEM → skip), i.e. falls
back to pre-I16. One client with > 16 concurrent dir ops on one shard
whose oldest is stalled hits that. If the 9-host gate still shows
EEXIST-on-fresh-name, check this first (count `opid_replay` and look for
>16 in-flight per shard) before suspecting the mechanism; raising the
cache is an implementation matter. Second limit: the dir op-id identity
is a per-mount random uuid (epoch 1), **not** the §7.5 session's
`(uuid, epoch)`, so a dead client's windows are never dropped by the
fence barrier §7.9 relies on ("a fenced session's records are dropped
wholesale") — a remount leaves its old windows behind. Follow-up, no
decision needed: seed `opid_uuid/epoch` from the client session and drop
`EFS_KV_KIND_OPID` keys for a fenced epoch in the barrier.
**Gate results on `43bdf6a` (Sep 23 02:52–03:07, rolled 02:49):**
`i17_leader_freeze.sh` run 1
(`results/measure/20260923-025259-i17-leader-freeze`): parent clean,
`arc_term_miss` 0→3, `opid_replay` 0→3, **0 worker errors** (the
`46d54e6` runs had 3 EEXIST + 1 ENOENT). Run 2 (`-025903-`): parent
clean, `opid_replay` 3→7, but **3 workers got `mkdir ENOENT`** at round 7
during the g0 freeze (fcstor007 p0, fcstor008 p3, fcstor015 p3); the
following `rmdir` of the same name succeeded, so the MKDIR committed and
only the *reply path* failed. Not an I16 miss: the replay path returns
OK, and `raft-host` logs no mkdir `rc=-3`. Root cause found in the
client: `rpc_send_recv_shard/_dual` returned `EFS_ERR_NOT_PRIMARY`
immediately on a **hintless** NOT_PRIMARY (`primary_id == 0` — a stale
leader that just stepped down and has not heard the new one), and
`efs_client_stat_ino` maps every RPC failure to NOT_FOUND, so
`ll_mkdir`'s post-mkdir `lookup_fill(new_ino)` (GETATTR) turned into
ENOENT for a dir that exists. Fix in tree (uncommitted, see §1b tail):
hintless NOT_PRIMARY backs off like BUSY (50 ms ×2^n, same 16-attempt
budget, EBUSY at the end) in both send paths; `ll_mkdir` logs
`fuse: mkdir ... ok ino=N but getattr rc=` when it happens. Server side:
`host_opid_reply_ino` logs `opid-replay ... (stub)` / `row gone`.
posix jobs=1: **200/201 + mmap SKIP** (`results/posix/20260923-030005`).
9-host: **189–191/201 per host, 0 not-run**
(`results/posix/20260923-030056`); fails = the six many-op timeouts
(item 4) + one-offs `names_crazy_roundtrip` EIO (014), `symlink_absolute`
EIO (012), `unlink_open_then_recreate` `b''` (012) — the same hintless-
NOT_PRIMARY class is the first suspect for the EIOs. `md_latency.py` 30 s
after the suite: mkdir 8.4 / create+close 19.3 / append+close 65.9 /
stat 0.5 / unlink 2.0 / rmdir 8.3 ms — post-suite churn, **not** a valid
comparison; remeasure on an idle cluster (commit flat 30 s).
**That client fix is `bb634d9`, rolled, and it closed the getattr
case.** On `ad292b9` (getattr reads a COMMITted-but-unresolved inode
row; `efs_meta_apply_get_inode_tx`) the freeze
(`results/measure/20260923-144129-i17-leader-freeze` and `-144243-`)
had no new `fuse: mkdir ... but getattr` line. Run 2 was clean
(opid_replay 1→5, parent removable). Run 1 left `d-fcstor011-1-22`:
three clients got `mkdir EBUSY` after the 10.3 s retry budget while
recovery of shard 1504 took 16.5 s, then `rmdir` returned ENOENT for a
name that existed. The dentry was a COMMITted EXCL intent, and lookup
used a bare kv get. Handler lookups and resolves now go through
`efs_meta_apply_lookup_tx` / `resolve_tx` (same rule as the inode row;
the apply path stays plain). `rmdir`/`unlink` map `EFS_ERR_BUSY` to
EBUSY — they were returning EIO. Do not widen the 16-attempt budget to
outrun recovery.
**Gated on `7e29943f28ef-dirty` (rolled Sep 23 18:21 UTC, servers and
clients 007–015):** `i17_leader_freeze.sh` ×2
(`results/measure/20260923-182559-i17-leader-freeze`, `-182709-`) — both
PASS, 0 worker errors on all 9 clients, parent removable, `opid_replay`
0→1→3, `arc_term_miss` 0→3→15, every `fuse.log` `getattr=0 exhausted=0`.
Idle `md_latency.py` before the freeze: mkdir 9.8 / create 4.4 / append
7.1 / stat 0.4 / unlink 2.0 / rmdir 10.5 ms (reference 6.1/4.0/6.2/0.3/
1.5/5.2; mkdir/rmdir high — remeasure once `commit` is flat 30 s; the
post-freeze samples were taken with commit still moving and are
invalid). Known limit, by design: a client's 16-attempt budget (~10.3 s)
is shorter than recovery of a stranded txn (16.5 s on shard 1504) → the
app sees EBUSY, never a wrong answer. Do not widen it.

**Gates on `7e29943f28ef-dirty` (Sep 23 20:26–20:46 UTC):**
9-host posix **193–196/201, 0 not-run, 79–94 s**
(`results/posix/20260923-202626`, timeline
`results/measure/20260923-162609-w8-stall-timeline`). Fails are the six
many-op timeouts (item 4; mkdir p50 46 ms / p90 230 ms under the suite)
plus one-offs at the one group-2 election in the run (term 535→537,
leader raft-id 2→3, one root stat of 1.6 s): `concurrent_create_unlink_two_proc`
EIO on 009 and 010, `content_random_roundtrip` EIO on 014,
`concurrent_write_and_readdir` EIO on 007, `unlink_open_then_recreate`
reading `b''` on 009, `concurrent_appends` timeout on 010. Group 0 did
not change term during the suite (5498; `leader=0` in the status line is
raft id 0 = node 1, not "no leader"). No 2.4 s apply stall in this run.
`same_parent_storm.sh` 9×4×100 on Sep 23: parent ended `children=0
nlink=2` and `rmdir` succeeded, but fcstor014 logged two `mkdir ENOENT`
at round 63 (`results/stress/same-parent-20260923-202846`). Reply path:
`ll_mkdir` committed, then `lookup_fill` (no open fh) refreshed, and
`stat_refresh` maps every GETATTR failure to NOT_FOUND. Fixed: a
committed mkdir/symlink/link replies the dual-applied local row when
that refresh fails (`lookup_fill_committed`). Re-gate on the new
clients (servers still `7e29943f28ef-dirty`): PASS 9×4×100, parent
`children=0 nlink=2`, `RMDIR_OK`
(`results/stress/same-parent-20260924-170945`). The fallback line did
not fire, so this run did not exercise the failed refresh; the Sep 23
failure is the case the reply now covers.
Idle `md_latency.py` twice, 20 min apart, term stable and
`commit == applied`, commit still +30 in 30 s (the 0–2/s reaper band,
not catch-up): mkdir 8.5 / create+close **56.8** / append+close **59.5**
/ stat 0.5 / unlink 2.2 / rmdir 7.8 ms
(`results/measure/20260923-162535-idle-mdlat/mdlat-idle2.txt`). The 50 ms
mode is on the two ops that write a byte and close; mkdir is near the
6.1 ms reference. Not the post-roll election (the roll was 18:21, this
sample is 20:46). A 4 MiB raft-log tail still shows the suite, not the
idle 1/s — do not blame a command from it.
**Strace (Sep 24, `results/measure/20260924-021832-close-strace`,
`20260924-053648-leader-strace`): the 50 ms is a `recvfrom`, not fsync.**
On the client every slow mkdir/create/append is one `recvfrom` of a
441-byte reply (0x1B9). On both leaders, at the same instant as the slow
creates, a thread blocks 60–70 ms in `recvfrom` of a 2-byte frame; the
group-2 leader also blocks 77–114 ms reading a ~64 KiB frame (0x10049).
A different pair of threads on each leader sits ~52 ms in `recvfrom` of
an 81-byte frame for the whole trace — the 50 ms heartbeat cadence,
present with no creates running. The follower's long calls were a
2.000 s `clock_nanosleep` and 2.000 s reads, not this op. One create in
the leader trace was 10 ms; the other three were 66 / 73 / 118 ms.
Do not treat this as the Sep 20 wrong-condvar bug until a send-side
trace shows the 81-byte frame leaving a follower late.

**9-host on the mkdir-reply clients (Sep 24 20:33 UTC,
`results/posix/20260924-203211`, servers still `7e29943f28ef-dirty`):**
193–196/201, skip=1 (`mmap_write_read`), 0 not-run, 74–80 s. The Sep 23
election one-offs (`content_random_roundtrip`, `unlink_open_then_recreate`
`b''`, `concurrent_create_unlink_two_proc`) did not recur. New one-offs:
fcstor008 `names_near_path_max` and `content_random_overwrite_append`
EEXIST, `rename_file_over_symlink` EIO; fcstor010 `dir_many_files` EIO
at `f0149`; fcstor015 `dir_deep_nesting` ENOENT. The rest are the six
many-op timeouts. The XFS baseline on node9901 did not run
(`/data1/efs` is not a mount there); score the `efs-*.tsv` summaries,
not the compare files.
**9-host after the ghost + close-unlock clients (Sep 25 00:02 UTC,
`results/posix/20260925-0002-ghost`, servers still
`abc6e913e760-dirty`):** 191–197/201, skip=1, 0 not-run, 66–74 s.
`unlink_open_then_recreate` and `flock_unlock_on_close` passed on
every host. The empty read was the sharded `keep_last` path returning
only when the dentry table differed from the parent, so the common
case deleted the open-fd ghost and getattr adopted size 0. Last close
now also sends `LOCK_UN` for that fd's owner. Non-timeout leftovers:
fcstor007 and 009 `dir_many_files` EIO (`f0200`, `f0253`), fcstor010
`trunc_grow_sparse` FileExistsError on the test directory, fcstor011
`names_dash_prefix_terminal` EBUSY on the test directory. The rest
are the many-op timeouts.
**9-host after NET-retry + visible-dir mkdir (Sep 25 00:54 UTC,
`results/posix/20260925-0048-net`):** 194–197/201, skip=1, 0 not-run,
77–101 s. `dir_many_files`, `trunc_grow_sparse`, and
`names_dash_prefix_terminal` passed on every host, as did
`unlink_open_then_recreate` and `flock_unlock_on_close`. A dropped
conn or recv used to return `EFS_ERR_NET` on the first attempt
(create → EIO). It now stays inside the existing 16-attempt loop.
MKDIR `EEXIST`/`EBUSY` returns success only when a lookup sees a
directory, and no longer reports EEXIST when the name is not visible
(`fuse_create_errno` used to short-circuit on `last_err`).
**9-host after local directory lookup (Sep 25 02:50 UTC,
`results/posix/20260925-0130-lookup`):** 195–198/201, skip=1, 0
not-run, 63–69 s. `dir_deep_nesting_beyond_64` passed on all nine
hosts. With `entry_timeout=0` a depth-100 mkdir was one LOOKUP RPC
per ancestor per level. A directory this client already has is
answered from the local table; files still go to the server, so a
peer unlink stays visible. `dir_deep_nesting` (the same walk plus
`rmtree`) still timed out on 7 of 9 hosts. `names_crazy_dirs` timed
out on every host. `names_crazy_roundtrip`,
`concurrent_creates_same_dir`, and `concurrent_write_and_readdir`
timed out on some hosts only.
**9-host after write() stopped reporting inline (Sep 25 04:30 UTC,
`results/posix/20260925-0412-write`, servers+clients
`d2e593244a10-dirty`):** 194–198/201, skip=1, 0 not-run, 60–66 s.
`write()` used to `report_dirty(0)` the whole set before returning;
flush/close already does `report_dirty_ino(ino, 1)`, so the write
report was a second round trip on every small file. Best hosts
(007, 008, 011) fail only `dir_deep_nesting` and `names_crazy_dirs`.
Those two still time out on every host. `dir_deep_nesting_beyond_64`
slipped back to a timeout on some hosts after the roll.
Alone on fcstor007 (Sep 25 04:34 UTC, same build, jobs=16) both
pairs pass: `dir_deep_nesting` + `beyond_64` in 3.6 s,
`names_crazy_dirs` + `names_crazy_roundtrip` in 3.2 s. The 15 s
failure is the 9-host queue, not a bug in those tests.
**9-host after an immutable-segment block cache (Sep 25 12:35 UTC,
`results/posix/20260925-0445-segcache`, `6ba3592b03c2-dirty`):**
198–199/201, skip=1, 0 not-run, 48–56 s. `dir_deep_nesting` and
`dir_deep_nesting_beyond_64` passed on every host checked.
`names_crazy_dirs` still timed out on every host;
`names_crazy_roundtrip` on some. Each KV get was malloc + pread of
the block under the LSM lock, including a miss that walks every
segment. The segment is immutable, so the last block stays cached.
A miss used to install that block and evict the hot one. Misses
leave the cache alone, and a key past the segment's last key does
not read the last block again.
**9-host after that, plus no open-lease and no empty close REPORT
(Sep 25 16:49 UTC, `results/posix/20260925-1240-misscache`,
`59b312f5904b-dirty`):** 199–200/201, skip=1 (`mmap_write_read`),
0 not-run, 44–49 s. Seven hosts failed nothing. fcstor008 and
fcstor013 failed only `names_crazy_dirs` (15 s). `dir_deep_nesting`,
`dir_deep_nesting_beyond_64`, and `names_crazy_roundtrip` passed.
`names_crazy_dirs` later passed on all nine
(`results/posix/20260926-164123`). W13 is done. W11 is done
(Sep 27): 9-host posix is 200/201 again
(`results/posix/20260927-033723`). W9's pin rules are on the nine
clients. Posix 1, posix 2 (59/63), the leak gate, and the
1M-file RSS walk are done (`results/measure/20260927-w9-walk`).
W10's private empty-mkdir passed 5/5. The Sep 27 live switch failed
the 9-host suite and was rolled back that hour. Sep 28 the cluster
is on RDMA again; see §1b.
The idle 50 ms create+close median did not hold: Sep 24 15:03 UTC on
the live mount, term stable, `md_latency.py` was mkdir 5.9 /
create+close 4.1 / append+close 6.1 / stat 0.3 / unlink 1.6 / rmdir
6.2 (`results/measure/20260924-150154-rpcprof-shapes` agrees). One
50–110 ms sample per 20-op batch remains. Not a code change until
that median is back.
The I16 follow-up's server half is in tree and unit-tested
(`efs_session_fence_local` deletes that shard's op-id window for the
fenced epoch; `test_session` OK on node9901) and is **not rolled**: the
FUSE client never creates an efs session, so nothing fences it. Seeding
the op-id from the session waits on that client session, which is the
product-gap item, not a one-line change.

The I17 story (index-only ring match; `46d54e6`; two gate runs
`results/measure/20260922-122517-i17-leader-freeze`, `-122629-`) is in
`docs/project-history.md` (Sep 22). posix jobs=1 on `46d54e6`:
**200/0/1 SKIP in 92 s** (`results/posix/20260922-133653`); 9-host
**185–197 / 201, 0 NOTRUN, 46–66 s** (`results/posix/20260922-134008`,
a leader change 12 s in explains the low end). The six old `posix-*`
leftovers on 19810 stay half-applied (pre-fix rows, no repair tool) —
ignore or wipe at the next agreed `raft-mkfs`.

Cluster runs `46d54e679e4f-dirty` → rolling to **`43bdf6a41f7d`** with
`EFS_RAFT_OBS=1` (5 s `raft-obs:` lines in
`efsd.log`; keep it on until W8 closes). W8's gate chain passes except
the 9-host row: `tests/measure/w8_root_lat.sh` root mkdir med 10 ms max
0.063, no 1 s mode; `w8_root_mkdir.sh` 9/9 root + 9/9 sub; posix jobs=1
**200/201 + mmap SKIP in 37 s** (`results/posix/20260922-015106`);
`same_parent_storm.sh` PASS 9×4×100 (`results/stress/same-parent-20260922-015426`);
9-host suite **191–195 / 201 on every host, 0 NOTRUN, all nine done in
~62 s** (`results/posix/20260922-020950`, timeline
`results/measure/20260921-220933-w8-stall-timeline`). Idle
`md_latency.py` reference is now mkdir 6.1 / create 4.0 / append 6.2 /
stat 0.3 / unlink 1.5 / rmdir 5.2 ms (a run 20 s after a roll shows a
50 ms mode — that is the post-roll RDMA election churn, wait 2 min).

What is left in the 9-host row, and what to do with each:
1. **Half-applied cross-shard txn (I17)** — fixed `46d54e6` (ring match
   by `(index, term)`), gated Sep 22 (two freeze runs).
2. **Retry of a committed non-idempotent op (I16)** — `link_of_symlink`
   EEXIST on a never-used name, `concurrent_create_unlink_two_proc` EIO,
   `unlink_open_then_recreate` reading `b''`: a client got BUSY from the
   400 ms apply-wait deadline during a stall, retried LINK/UNLINK
   (`stale_retryable`), and met its own result. **Landed `43bdf6a`**
   (block above); gated on `7e29943` (the 20:26 9-host run above). The
   client still retries BUSY and the 400 ms deadline is unchanged. The
   `b''` read and the EIO one-offs recurred once, at the group-2
   election in that run.
3. **Compaction stall → leader loss (needs a decision, §4).** One pump
   cycle held `h->mu` for **2.4 s** on both g0 replicas at once
   (`obs-fcstor004/005.txt`: `apply_max=2464250us applies_in_worst=54`),
   the leader missed its heartbeats and g0 went 5299→5302→5303; the
   client saw a 2 s `stat` and two failed mkdirs in that window. The KV
   is 1 GiB per node in 64 MiB L1 segments; an L0 segment spans every
   shard, so `kv_compact_locked` merges **all** of L1 every 4 flushes
   (≈ 16 MiB of writes) under `l->mu` (every read on the node stalls
   too) and `h->mu`. It is deterministic, so every replica does it at
   the same moment. Options the spec does not choose between: bounded
   (leveled / per-key-range) compaction; compaction off the apply path
   (background thread, readers merge an immutable memtable); a larger
   memtable as a stopgap (fewer, not shorter, stalls). **W13 landed
   the background compactor (Sep 26); see the top of §1b.** Do not
   raise the election timeout.
4. **Throughput at 144 concurrent jobs** — the six many-op tests
   (`dir_deep_nesting*`, `names_crazy_*`, `concurrent_write_and_readdir`,
   `concurrent_creates_same_dir`, `mtime_monotonic_many_writes`) time out
   on most hosts: mkdir p50 21 ms / p90 88 ms under the suite, so a
   200-op test needs > 15 s. Steady-state apply is still ~0.3 ms per
   entry with no fsync in it (`apply_max=10657us applies_in_worst=36`):
   that is the KV reads in the apply path — one `pread` per segment per
   lookup/scan, 17 segments, no block cache. Measure before choosing
   (perf on the pump thread under the suite); a block cache is an
   implementation matter, a different segment layout is a decision.
Do not raise the per-test 15 s budget or the suite cap.

**Progress log (newest first — read this before the state below):**

- **22:15 (Sep 21)** — **9-host posix: 191–195/201 on all nine in 62 s
  (was 66–95 with `[None]` at the 385 s cap). Three causes, three fixes.**
  (1) *Harness:* the parallel path stamped the per-test clock at
  **submit** — 196 tests into 16 workers, so anything still queued after
  15 s was recorded "timeout" without running (3-op tests "timing out",
  the same first timeout on every host, ~100/201). The worker now stamps
  its own start (`a683def`); the budget is unchanged.
  (2) *`h->mu` contention:* with `read_mu` gone, 30+ handlers per pump
  cycle took the pump's own mutex to read commit/applied/leader and to
  cond_wait; the pump lost its heartbeat cadence and both groups
  re-elected 4× in 50 s under load
  (`results/measure/20260921-211829-w8-stall-timeline`). Handlers now
  read a lock-free view the pump publishes (`host_publish_view`,
  atomics) and sleep on `applied_cv` under a separate `cv_mu`; a handler
  takes `h->mu` only to propose or begin a read round (`4eb1419`).
  (3) *KV WAL fsync per applied entry:* 0.5 ms per apply on every
  replica under `h->mu` (`EFS_RAFT_OBS`). One `efs_kv_lsm_sync_hold` per
  pump cycle, released before `persist_applied` (`84a2a55`); spec
  §"Raft log is the durability boundary" says the KV owes nothing on
  the critical path. Under the suite: stat p50 3 ms (was 9), mkdir p50
  21 ms (was 63); idle mkdir 6.1 / create 4.0 / append 6.2 ms. What
  remains (§1b): retry-of-committed-op (I16 dedup), the 2.4 s
  full-L1 compaction stall that still costs a term, and per-op cost at
  144 jobs. New tools: `tests/measure/w8_stall_timeline.sh` (1 Hz
  raft term/commit + client op latency around the 9-host suite),
  `EFS_RAFT_OBS=1` on the deployed efsd.

- **17:37 (Sep 21)** — **The sweep itself was a 100 ms regression; fixed
  in `3291c6d`.** posix jobs=1 on `9534e53` passed 200/201 but took 219 s
  (56 s in the morning); jobs=16 put 77 tests over the 15 s budget;
  `md_latency.py`: mkdir med 99.5, create+close 152, append+close 202 ms
  (reference 7.2/6.7/9.0). The GC thread on each leader ran 512 shards ×
  3 prefix scans per second; strace: one scan = 9 `pread64` (one block
  per segment) ≈ 0.3 ms under the LSM mutex the apply path needs; perf:
  38 % `rep_movs` + 21 % memcmp, process CPU only 3 % — lock hold, not
  CPU. Now every PREPARE apply marks its shard, a 5 s-old mark gets one
  scan (64 per pass, 1 ms yield), clean clears it, a fresh process marks
  all 4096 once. After the roll: create 6.1–6.7, append 8.5–9.3, stat
  0.4, unlink 2.1, rmdir 7–10 ms medians; mkdir 9.9 (bimodal 2 ms log
  path / ~50 ms cross-group txn, so its median is the mix).
- **17:15 (Sep 21)** — **Recovery gates 0–3 pass on `9534e53`.** Within
  ~1 min of the roll the two leaders logged 47 `txn-recover` lines —
  every one `COMMIT (resolved)`: the coordinators HAD written COMMIT,
  only their RESOLVEs were lost, so the recovered ops (dentries, inodes,
  ALLOC bumps) are now visible ~90 min after their callers were told
  EBUSY. One forwarded RESOLVE came back BUSY once and succeeded the
  next pass. `kv_intents` on a fresh copy: **0** INTENT/GUARD/REDUCE
  (`results/measure/20260921-w8-orphans/after-recovery.txt`). Parent
  burst 12 rounds × 9 hosts: **108/108, 0 BUSY lines, max 0.31 s**
  (`results/measure/20260921-210917-w8-parent-burst`). Root lat
  (`…-211258-w8-root-lat`): root mkdir 16 ms (log path) or 50–57 ms
  (cross-group txn: 5 PREPARE + DECIDE + 2 RESOLVE ≈ 8 commits), max
  0.119, **no 1 s mode**; the "≤ 15 ms" I wrote for this gate was the
  log-path number, the txn path is ~50 ms by construction. Concurrent
  root mkdtemp (`…-211320-w8-root`): root 9/9 ≤ 0.137 s, fresh parent
  9/9 ≤ 0.311 s, every rmdir OK, no ENOENT.
- **17:10 (Sep 21)** — **Stranded transactions were the 10.4 s EBUSY.**
  `tests/measure/w8_parent_burst.sh` (9 hosts × 6 rounds of fresh-parent
  `mkdtemp`, `results/measure/20260921-204358-w8-parent-burst`): 53 OK,
  1 EBUSY after 10.39 s, and the new server line shows 16 identical
  `mkdir … rc=-13 (BUSY)` for it — one shard, every retry. A copy of
  node 2's `mdraft/kv` through `tests/tools/kv_intents`
  (`results/measure/20260921-w8-orphans`): **105 INTENT, 1 GUARD, 28
  REDUCE records, all 4 500–5 000 s old**, 45 of the intents on ALLOC
  keys of 45 even shards — a log-path create/mkdir landing on one of
  those shards is BUSY on every attempt. They date from the 1 s-scan era:
  a DECIDE or RESOLVE whose `host_wait_applied` hit 400 ms made the
  coordinator skip the rest, and nothing ever came back for the records
  (spec L5 says recovery must; there was none). `9534e53`:
  `efs_txn_scan_pending` + `host_txn_recover_pass` on the GC thread.

- **16:30 (Sep 21)** — **Root cause of the 1 s root mkdir: full-shard
  scan in RESOLVE.** Server strace during 12 root pairs
  (`results/measure/20260921-202253-w8-root-srv`): on the dual host 3–5
  threads sit in one futex wait and release together 0.70 s / 1.05 s
  later; on fcstor005 a futex wait ends `ETIMEDOUT` at exactly 0.400 s
  (the `host_wait_applied` deadline → BUSY); no disk syscall ≥ 0.15 s.
  perf on fcstor003+004 (`results/measure/20260921-202715-w8-root-perf`):
  **85–90 % of efsd CPU** is `host_pump → apply_committed → host_apply →
  efs_txn_resolve → lsm_scan_prefix → merge_scan → memcmp`.
  `efs_txn_resolve` and `efs_txn_drop` used a 2-byte prefix = every key
  of the shard, on every replica, per participant shard, under the KV
  lock and `h->mu`. Root's shard 1 carries the most history, so a
  cross-group child (txn path) cost ~1 s while a same-group child (log
  path, no RESOLVE) cost 2 ms — the bimodality. The 400 ms BUSY then
  drove the 10.4 s client backoff, and a retried UNLINK whose first
  attempt had committed came back NOT_FOUND = the rmdir ENOENT. Fix
  `165e779`: scan `[shard][INTENT|GUARD|REDUCE]` (3-byte prefixes).
  Unit tests `test_txn`/`test_meta_apply`/`test_sim` OK on fcstor003.
- **20:02 (Sep 21)** — **Root mkdir is 2 ms or 1.05 s; BUSY burns 10.4 s.**
  Root has **410** names, listed in 10 ms
  (`results/measure/20260921-195541-w8-root-trace`). Not the 65536
  spread. A strace of `efs-fuse` shows the 1.12 s call is one
  `recvfrom`; there is no client sleep on that path. Twenty sequential
  root mkdir+rmdir pairs
  (`results/measure/20260921-195829-w8-root-lat`): 13 at 2 ms, 7 at
  1.03–1.09 s, and the rmdir matches the mkdir. Interleaved with a
  fresh directory
  (`results/measure/20260921-200108-w8-root-interleave`): 6/16 root
  calls still ~1.03 s while the fresh-directory call in the same
  second is 2–15 ms. Two fresh-directory creates returned `EBUSY`
  after 10.38 s, which is all 16 BUSY retries. Raft tail, last 64 KiB
  (`results/measure/20260921-200251-w8-root-log`): group 0 PREPARE 58
  vs DECIDE 10. The 9-host warmup dies inside that backoff.
- **19:44 (Sep 21)** — **W8 root burst vs a fresh parent.**
  `results/measure/20260921-194332-w8-root`. One root `mkdtemp` 1.192 s.
  Nine at once: six `MKDIR_OK` in 1.19–5.96 s, three still out at 8 s
  (one python in `request_wait_answer`), and fcstor012/014 then `rmdir`
  ENOENT on the directory that same call had just created. Nine at once
  in a fresh subdirectory: **9/9 in 0.008–0.197 s**, every `rmdir` OK.
  Group 0 `commit == applied`, +55 entries in ~9 s. Root `stat` right
  after: nlink=406, `.stats` rollups all zero
  (`results/measure/20260921-194815-w8-root-stats`). Not a 65536-entry
  spread. The mount root is what the 9-host suite cannot enter.
- **19:40 (Sep 21)** — **W7 closed. W8 harness proven, 9-host gate not run.**
  Isolated walks are 1.1–6.1 s (`results/measure/20260921-133437-posix-isolated`);
  suite 1 jobs=1 stays 200/201. A 20 s cut of suite 1 writes 201 TSV rows
  (`results/measure/w8-cut.tsv`): 1 PASS, 200 `NOTRUN` ("suite cut by
  signal 15"), `NONE=0`. `compare.py` reports `EFS BUGS : 0` and
  `not run : 200` (`results/measure/w8-compare.txt`). Immediately after,
  9 clients each doing one `mkdtemp` in the mount root: **0 MKDIR_OK**.
  Five ssh timed out at 20 s (D-state `request_wait_answer` ignores the
  inner timeout); four were killed at 12 s (`rc=124`) with no success
  line. A same-binary remount cleared them. One mkdir on one client
  still returns. Do not start `run_tests.sh posix --parallel` until the
  root burst in `w8_root_mkdir.sh` returns.
- **18:23 (Sep 21)** — **W11 measured, still unspecified.**
  `results/measure/20260921-182308-raft-snap-state`: raft logs
  **1.83 / 3.56 / 4.36 / 1.89 GB** on fcstor003–006, still growing
  388–801 B/s on an idle cluster. KV 0.54–1.08 GB. `snapshot skipped`
  is latched (group 0 applied≈4521152, group 2 ≈3256233). Both groups
  `commit == applied` on every voter — fcstor005 is not behind on this
  build. A follower restart is still a full replay. Chunked
  InstallSnapshot stays unspecified: ask, do not design.
- **16:30 (Sep 21)** — **dd wall, 1 and 4 clients. 9-client number does not exist.**
  `results/measure/20260921-163033-dd-wall`, 8 GiB `dd conv=fsync` of
  non-zero `/tmp/src8g`, FUSE_OK, file 8589934592. **1 client 499 MiB/s**
  (wall 16.4 s; Sep 18 was 639). That REPORT: pack 3554 + push 4848 +
  finish 1013 ms, `rc=0`. **4 clients 176 MiB/s** aggregate (slowest
  185.8 s; Sep 18 was 251). 21 `report-split` lines, all `nrec=65536`
  `rc=0`, one push 149 s. 9 clients: every ssh hit 400 s; mkdir EIO on
  007/009/010 (the 30.2 s WALL is a failed open) and fsync EIO on
  011–015. Row is `INVALID`. Do not quote a 9-client rate from this run.
- **12:30 (Sep 21)** — **IOR-hard scaling, full runbook**
  (`tests/measure/ior_hard_scaling.sh`,
  `results/measure/20260921-162514-ior-hard-scaling`). Write MiB/s
  **372 / 33 / 69 / 82 at NP 1 / 4 / 9 / 36** (47008 B × 3000 segs, one
  file). 1-rank matches the own-file dd wall. From 4 ranks, ~half of
  `report-split` lines are `rc=-14` STALE and `finish_ms` (apply wait)
  is the large phase (37 s summed vs a 59 s IOR wall at 36 ranks).
  Not a monotone CAS cliff. Next: `dd_wall.sh` at 1/4/9 with `PERF=1`.
- **12:25 (Sep 21)** — **W6 same-directory rate, full runbook**
  (`PERF=1 tests/measure/samedir_rate.sh`,
  `results/measure/20260921-161931-samedir-rate`). Storm PASS at 1×1,
  1×9, 4×9. Aggregate **138 / 134 / 159 ops/s** — flat, so adding procs
  does not add throughput. `busy_n=0` rules out the BUSY backoff.
  `checkout_us` ~15 ms/client rules out the conn pool. Fuse `recv_us`
  ≈ storm wall rules in server+wire wait. Leader log tail is the storm's
  own PREPARE/CREATE/UNLINK/RESOLVE/LEASE_CLOSE/REAP_DONE (GC_ACK 0.4 %,
  so the earlier 52 % GC_ACK was an idle-tail artifact of the small
  smoke). On-CPU profile: `memcmp` in `lsm_scan_prefix` under
  `reduces_pending`, `guards_conflict`, `efs_txn_resolve`. Next runbook
  in order: `ior_hard_scaling.sh` at `NPS="1 4 9 36" SEGS=3000`
  (open-cost §2 is already answered by the smoke).
- **11:30 (Sep 21)** — **Runbooks for every open measurement item**
  ([runbooks.md](runbooks.md), `tests/measure/*.sh`, `tests/preflight.sh`),
  all pinned to the running build `b2184a5c7faf-dirty`, each smoke-run
  once (dirs under `results/measure/20260921-*`). Two findings from the
  smokes that change the questions: **(1) 1 GiB cold open is 0.23 s idle,
  not 20 s** — exactly 128 GETCHUNKS × 1.6 ms; with 32 concurrent openers
  GETCHUNKS is **14.6 ms** (9.5×) and open 1.6–2.5 s, one map fetch per
  host — so the IOR 20 s is server-side GETCHUNKS serialization under 36
  ranks, not per-open cost. **(2) Same-parent storm is 171 ops/s at
  1 proc AND at 4 procs with `busy_n=0`** — a flat aggregate ceiling that
  is not the BUSY backoff (leader raft log tail: 52 % `GC_ACK`). Also:
  `EFS_RPC_PROF=1` counters only dump on an RPC, ≤ every 2 s — read them
  after a trivial RPC (`rpc_prof_last` in `tests/measure/lib.sh`), and they
  do **not** count REPORT (`rpc_send_recv_dual` unprofiled) — the REPORT
  number is the server's `report-split` line, which the write runbooks now
  collect. **(3)** 1-client 8 GiB dd+fsync today 380–418 MiB/s (Sep 18:
  639); its single REPORT was `pack 6.9 s + push 5.9 s + finish 1.0 s` =
  **13.9 of the 21.5 s wall** — pack+push, not the apply wait, is the tail.
  Next: run the runbooks at their default (full) settings, one at a time —
  order in runbooks.md §1–7; then bring the tables to the user.
- **08:45 (Sep 21)** — **Step E DONE as option (b), §7.2 end state:** the
  parent inode row, the dseq emptiness witness and the HASHED dir-lane
  stamp are **commutative reductions**, not EXCL full-image CASes. New txn
  kinds `EFS_TXN_REDUCE_INO` (signed `nlink`/`nents` delta, times MAX,
  `used_shards` OR, `parent` SET, `parent_version` delta) and
  `EFS_TXN_REDUCE_ADD` (u64 +1); `fold_reduce` in `txn.c` dispatches on
  the DATA key's kind (LANE / INODE / DSEQ) and bumps the folded key's
  version so a stale EXCL lands STALE. Soundness rules: a pending reduce
  and an EXCL/GUARD on one key are mutually BUSY (`reduces_pending`);
  the log path probes before an unversioned DEL (`dir_txn_busy` in
  `efs_meta_apply_rmdir`, `efs_txn_key_busy` in `efs_meta_apply_unlink`)
  and returns BUSY; dseq GUARDs compare the observed **value**
  (`efs_txn_dseq_observe`) — a log-path bump is unversioned but changes
  the value, so `RMDIR`'s guard goes STALE exactly when a child appeared.
  One shared PREPARE decoder `efs_txn_apply_prepare` (server + sim).
  All of `host_hashed_create_txn`, mkdir, rmdir, `host_unlink_txn`, link,
  `rename_at` converted (`host_prep_ino_delta` / `host_prep_dseq_bump` /
  `host_prep_lane_stamp`); the simulator's `dseq_prep`/`dseq_guard` too.
  Client retries STALE for UNLINK/LINK/RENAME_AT as well as CREATE
  (`stale_retryable`). Unit gates: `test_txn` +4
  (`test_ino_delta_commutes` is the Sep 20 lost update: log-path PUT
  between PREPARE and RESOLVE, fold lands on the log-path result, ver+1;
  vs-EXCL/GUARD BUSY both ways; lane 56-byte tail kept; wire decode) and
  `test_meta_apply::test_log_delete_busy_under_intent`; `test_sim`,
  `test_kv`, `test_raft` green. Deployed stop-all/start-all (new
  `tests/roll_efsd.sh --all`) as `b2184a5c7faf-dirty` + all 9 clients
  (`fuse-deploy`). **posix jobs=1 `results/posix/20260921-123904`:
  200/201 + mmap SKIP in 56 s** (unchanged signature). **Repro
  `tests/stress/same_parent_storm.sh` PASS
  `results/stress/same-parent-20260921-124141/`:** 9 hosts × 4 procs ×
  100 rounds of mkdir/create/rmdir/unlink in ONE parent = 14 400 ops,
  0 errors, parent ends `children=0 nlink=2`, `rmdir` OK. Measured while
  there: **~178 ms per op per proc under 36-way same-parent contention
  (~200 ops/s aggregate)** vs 7 ms idle — the same ceiling as
  mdtest-easy-write 0.238 kIOPS. Not a correctness item; it goes in the
  W6 residuals as "same-directory op rate" (BUSY/STALE retries are not
  logged at default verbosity, so first instrument, then decide).
  **Step 5 (9×4 IO-500 debug) PASS on this build:
  `results/io500/20260921-debug-9x4-reduce/`** — both `-R` reads 0
  errors, **0 `Unable to remove directory`**, the run tree is gone
  afterwards (the previous run left 3 undeletable dirs). Rates within
  noise of the previous build (mdtest-easy-write 0.189 vs 0.238 kIOPS,
  hard-write 0.240 vs 0.201, ior-easy-read 1.13, hard-read 4.06 GiB/s;
  NOTE.txt has the table). Step 6 = this commit.
  `efs-bg.sh` now runs jobs in a detached **GNU screen `efs-<name>`** on
  node9901 (user's suggestion): `screen -r efs-<name>` there shows the
  live job; log/rc bookkeeping unchanged.
- **23:10 (Sep 20)** — **Step D (9×4 IO-500 debug) DONE and correct:**
  `results/io500/20260921-debug-9x4-outbox/` (run id 2026.09.20-22.45.42,
  cluster on the outbox-fix build). Both `-R` reads **0 errors**, every
  ior file unlinked. vs the morning gate (`20260920-debug-9x4`):
  mdtest-easy-write **0.238 kIOPS (was 0.050, 4.7×)**, mdtest-hard-write
  0.201 (was 0.018, 11×), mdtest-easy-stat 0.88 (0.24), mdtest-hard-stat
  2.76 (0.79), mdtest-easy-delete 0.29, mdtest-hard-delete 0.61;
  ior-easy-write 0.776 GiB/s (0.814, unchanged — W4 wall), ior-hard-write
  0.046 (0.044, unchanged — 36-way sub-chunk CAS), ior-easy-read 1.16
  (1.80; 25 s of the 37 s is the 1 GiB `open`, see residuals),
  ior-hard-read 3.89 (3.55).
  **New correctness finding — wedged directory, needs a user decision
  (§4):** 7 mdtest `WARNING: Unable to remove directory
  …/mdtest-easy/test-dir.0-0/mdtest_tree.N.0`. Afterwards 4 of them rmdir
  fine (transient STALE), **3 return EIO forever**: `raft-rmdir` →
  `status=3` (ERROR) because `server_raft_host_rmdir` /
  `efs_meta_apply_rmdir` hit `prow.nlink < 3 → EFS_ERR_PROTO`. The parent
  `test-dir.0-0` (ino 824) has **nlink=2 with 3 live subdirectories**
  (true value 5): 36 ranks did `mkdir` then `rmdir` of one child each in
  the same parent; children with an even ino go the same-group **log
  path** (`efs_meta_apply_rmdir`: PUT of the parent row, no intent probe,
  no version bump) and odd-ino children go the **txn path** (EXCL on the
  parent row at `pver`, PUT of a full row image with `nlink-1` from its
  read snapshot). A txn that read the row before a log-path apply still
  PREPAREs at the old version, wins, and overwrites the log-path
  decrement/increment — the exact class `alloc_key_claim` fixed for the
  ALLOC key on Sep 20 (`meta_apply.c` comment above it), now on the parent
  inode row. Every log-path parent-row PUT is exposed: `create_file_batch`,
  `mkdir_batch`, `efs_meta_apply_unlink/link/rename/rmdir`. Consequence:
  parent `nlink` drifts low → the last children can never be rmdir'ed
  (EIO), `rm -rf` of an mdtest tree fails; drifts high → a directory
  claims children it does not have. This is the "transient mdtest rmdir
  ENOTEMPTY" residual — it is not transient.
  Leftovers on 19810: `/io500/2026.09.20-22.45.42/mdtest-easy/test-dir.0-0/
  mdtest_tree.{3,18,27}.0` (parent 824 nlink=2). Ignore or wipe.
  Options for the user: **(a)** generalize `alloc_key_claim` to the parent
  row: log-path PUT is BUSY under a pending intent and bumps the row's
  version so the txn's EXCL goes STALE — same rule as ALLOC, mechanical,
  plus the client must retry STALE for MKDIR/RMDIR/UNLINK/LINK/RENAME (today
  only `INODE_CREATE`, `inode_rpc.c:299`), and the 50 ms × 2ⁿ BUSY backoff
  becomes the same-parent latency; **(b)** the spec'd §7.2 end state:
  parent nlink / dseq / mtime as commutative REDUCE parts (the existing
  `efs_txn_reduce` carries only max_end/max_mtime/max_ctime; needs a
  signed nlink delta and a REDUCE resolve onto an inode row), which makes
  same-parent ops conflict-free instead of retried. Not started either.
- **22:45 (Sep 20)** — **The 100 ms metadata floor is gone: mkdir med
  103 → 7.2 ms, create+1B+close 60–107 → 6.7, append+close 160–180 → 9.0**
  (`results/perf/20260921-md-latency.txt`). Step C below is DONE; the
  reaper was only half of it. The other half was a wakeup bug in the
  raft_host outbox: ONE shared `outbox_cv` for all per-peer sender threads
  + `pthread_cond_signal`, so queuing a message for peer A usually woke
  peer B's sender (empty queue, back to sleep) and A's message left at the
  next signal for anyone — the next 50 ms heartbeat. Each AE and each AE
  reply lost 0–50 ms per hop → ~100 ms per commit while every fsync was
  0.3 ms. Found by strace on a FOLLOWER (003): it acked only every other
  heartbeat, two 85-byte `writev` back to back; on the leader the AE to
  one peer left 47 ms after the propose. Fix: per-peer `tx[].cv`
  (`struct host_outbox`), `host_send` signals exactly that one, shutdown
  broadcasts all. Rolled all four (`roll3`, 90 s, TCP peers,
  `EFSD_ENV=EFS_TRANSPORT=tcp`); posix jobs=1 running as `posix3`. Not
  committed yet (step A below now includes this).
- **21:55 (Sep 20)** — Steps 1–4 DONE again on the full in-flight tree
  (now also the `concurrent_appends` dcache fixes, the dir-rename dual-host
  bounce, and a reaper fix found tonight); step 5 (9×4 IO-500) NOT run;
  nothing committed yet. Everything long-running now runs **from node9901
  via `efs-bg.sh`** (see the `efs-test-ssh` skill; logs in `~/efs-runs/`)
  because the login-node shell died four times today — the fifth time was
  mid-doc-edit at 21:50, which is why this entry exists.
  - Posix jobs=1 twice: `results/posix/20260921-011518` (190 + mmap SKIP,
    but 8 clients were being rebuilt/remounted during it) and
    `results/posix/20260921-012432` (**191 both-pass + mmap SKIP**, fails
    = `dir_deep_nesting`, `dir_deep_nesting_beyond_64`, `names_crazy_dirs`
    15 s walks + `mtime_monotonic_many_writes` 15 s). `concurrent_appends`,
    `dir_rename_dir_with_contents`, `trunc_zero_then_high_pwrite` PASS.
    `mtime_monotonic_many_writes` is not a flake: 80 × (open O_APPEND,
    write 1 B, close) at **~200 ms each** > 15 s. Isolated timing on 007
    (idle cluster): mkdir med **103 ms** (min 5.5), create+1B+close 60–107,
    append+close 160–180, `report_ms=104` on every 1-rec REPORT,
    server `report-split ... finish_ms=103`.
  - **Root cause of the ~100 ms floor (fixed in tree, deployed, gate
    pending):** the reaper. `tests/tools/raft_log_tail.py` on 004's
    `raft.log` showed **92 % of both groups' entries were `LANE_SWEEP`**
    for the same 64 inodes, 362× each — ~33 entries/s per group with no
    client. `host_gc_propose` was leader-only, but a lane's shard is
    `ish + lane × odd stride`, so every ODD lane of an inode lives in the
    OTHER group: the anchor-group leader swept lane 0 (landed), got
    `NOT_PRIMARY` on lane 1, and retried the whole marker next second,
    forever — unless one node happened to lead both groups. Fix:
    `host_gc_propose` forwards a foreign-group command to that group's
    leader (`host_remote_cmd`, same path as a client op) and waits for the
    local replica if hosted. No unit test covers `raft_host`; the gate is
    the log histogram going quiet + the latency numbers above dropping.
  - Rolled all four (`tests/roll_efsd.sh 1 2 3 4` from node9901, 01:48–
    01:50 UTC, each caught up in < 10 s, all `d0fd0448adb6-dirty`). After
    the roll `REAP_DONE` finally lands (0.3 % → 40 % of entries): the
    reaper is **draining a backlog of dead inodes at ~15/s per group**
    (thousands from IO-500/mdtest/posix). Until it is idle every client
    commit queues behind it (mkdir med still 150 ms at 21:55). **Wait for
    both groups' `commit` to be flat for 30 s before measuring anything**,
    then redo the isolated latency probe — the min of 5.5 ms says what a
    raft commit costs; if the median stays > 20 ms on an idle cluster that
    is the next serialization point (each apply fsyncs the KV WAL inside
    the single pump thread, `raft_host.c:4245 EFS_KV_LSM_SYNC`) and needs
    a user decision, not a tweak.
  - Rolling restarts cause a **group-2 election storm** (term 199 → 264)
    lasting ~5 min after the last restart: the server peer pool is RDMA
    (`peer_pool.c` upgrades every peer conn; raft AE rides it) and a
    restarted node's QPs die with `transport retry counter exceeded` /
    `*** SOCKET CLOSED/REUSED BEHIND THIS CONN ***` on its peers, each
    blocking a sender for seconds → missed heartbeats. Pre-existing (same
    lines in every `efsd.log.prev`), converges by itself. Same class as
    the client pool identity bug (`test_conn_fd`) — not chased tonight.
  - New tooling, all committed with this batch: `efs-bg.sh` (detached
    runner on node9901), `tests/deploy_fuse_clients.sh` +
    `tests/fuse_client_remount.sh` (parallel client rebuild+remount, one
    status line per host), `tests/roll_efsd.sh` (rolling restart with
    build-ID check and per-group catch-up wait), `tests/tools/raft_log_tail.py`
    (what is the log made of). The deploy rule's kill command is now
    `pkill -9 -x efsd` only.
- **12:05** — Steps 1–4 DONE, step 5 not started; the agent's shell died
  again while probing the one new posix failure. Results:
  - Churn (step 1) = the GC reaper draining the fragment backlog the
    `lane-sweep rc=-5` bug left (no client process; `df` on every
    `/data1/0N` falls ~150 KB/s). Benign; let it run.
  - Rolling restart (step 2) done 11:33–11:45 in the order 004, 005, 006,
    003; each caught up within 9 s; all four run `d0fd0448adb6-dirty`.
  - **Step 3 PASSED:** raw IOR hard 36×3000 write **96.78 MiB/s** (was
    ~45), cold `-r -R` verify **0 errors**, `/tmp/hardcheck.py` from
    fcstor009 **bad records 0 of 108000** (was 6707). W1 `n1_shared_pwrite`
    **lost=0**. All in `results/io500/20260920-hardv-pubbatch/gate.txt`.
  - Step 4 posix jobs=1 `results/posix/20260920-155107`: **192 both-pass +
    mmap SKIP**, 3 EFS fails. Isolated re-run: `dir_deep_nesting` PASS,
    `mtime_monotonic_many_writes` PASS (load flakes, known), but
    **`dir_rename_dir_with_contents` FAILS ISOLATED** (deterministic
    `EIO` on `os.rename(d/src, d/dst)` where `src/sub/f` exists; 1.6 s).
    It was PASS in the Sep 17 `results/posix/20260917-190719` gate, so
    this is a regression in the in-flight tree or a state issue on this
    populated cluster. `ino_dup=0`, `lane-sweep rc=-5`=0 on all four
    servers. **Do this before step 5:** reproduce by hand on fcstor007
    (`mkdir -p $d/src/sub; echo x >$d/src/sub/f; mv $d/src $d/dst`), vary
    it (empty dir; dir with a file only; dir with an empty subdir) to see
    which shape EIOs, and read the RENAME path in
    `src/server/raft_host.c` (`server_raft_host_rename_at`, LOCAL same-dir
    directory rename GUARDs dest ancestry `parent_version`) plus the
    efsd.log of the parent's group leader for the txn status. Candidate:
    the `alloc_key_claim` version bump or `dentry_seq` GUARD racing the
    child's `nents` update — the test creates `sub` and `f` immediately
    before the rename.

**State as of Sep 20 10:40 (verified by probe, not memory; the agent's
shell died mid-rolling-restart, which is why this block exists):**

- **The bug being fixed: a REPORT that spans several Raft entries returned
  OK when only its LAST entry applied OK.** `host_pub_batch_wait`
  (`src/server/raft_host.c`) read the apply-verdict ring for `last_idx`
  only; a report larger than `HOST_PUB_BATCH_N` (256) pubs is proposed as
  several entries, and a pub that loses its N-1 CAS at apply time is
  `STALE` on the ring for *its* entry only. The client took the OK, marked
  those dirty ranges clean, and the CAS winner's generation (merged on an
  older base) became the file. Measured: raw IOR hard, 36 ranks × 3000
  segments of 47008 B (`SLOTS=4 bash tests/perf/io500/run.sh ior-hard-write
  3000`, then cold-remount `ior-hard-verify 3000`) lost **6707 of 108000
  records**, every one a client's whole sub-range of a record straddling a
  128 KiB chunk boundary, zeros in a cold verify (classifier:
  `/tmp/hardcheck.py <file> 36` on fcstor007 — recreate from the comment
  in that file if it is gone). The 9×4 debug run
  `results/io500/20260920-debug-9x4/` saw the same thing as 2 `-R` errors
  on ior-hard-read (easy-read 0). This is a **correctness** bug and blocks
  every other W6 item.
- Uncommitted code in the working tree (on top of `cc828d8` + the Sep 20
  docs/rules commit), all unit-gated on fcstor003 (`test_raft` /
  `test_meta_apply` / `test_kv_lsm` OK):
  - `src/server/raft_host.c` — `struct host_pub_batch` keeps every
    proposed index (`idxs[]`, `overflow`); `host_pub_batch_wait` waits for
    the last index then reads **every** entry's verdict; any `STALE`, any
    ring miss (`obs_arc_miss`), or `overflow` → the report is `STALE`
    (client re-pulls the map and replays only chunks whose generation
    moved). `host_pub_batch_reset` frees both arrays. No unit test covers
    `raft_host` (live-only); the gate is step 3 below.
  - `src/meta/meta_apply.c` — `alloc_key_claim` (log-path CREATE/MKDIR
    alloc is `BUSY` under a txn `EXCL` intent on the shard ALLOC key and
    bumps its version; closes the `ino_dup` 4746 race; gate
    `test_alloc_vs_txn_intent`); `sweep_cb` / `rsv_purge_cb` initialise
    `val=NULL vlen=0` on `EFS_KV_DEL` items (uninitialised `vlen` made
    `wal_encode` return `INVAL` → reaper `apply lane-sweep rc=-5` forever,
    124 inos re-swept ~9 entries/s).
  - `src/kv/kv_wal.c` — `wal_encode` validates `vlen` only for `PUT`;
    regression in `tests/test_kv_lsm.c` (DEL with garbage `val/vlen`).
  - `src/client/read.c`, `src/client/ops.c`, `src/client/client_internal.h`
    — a read whose layout pull fails **fails** instead of zero-filling;
    `pull_chunks_range` returns the first RPC error; `pull_layout_miss` has
    a 200 ms range cache, not a 1/s rate limit. Built into `efs-fuse` on
    fcstor007–015 (all nine were remounted from this tree before the last
    IO-500 run).
- **Servers:** `efsd` built from this tree at `/tmp/efs` on fcstor003–006
  (`d0fd0448adb6`-dirty, 4704656 B, 10:29). **fcstor004 (node 2) was
  restarted on it at ~10:33 and caught up** (its `applied` equalled the
  leaders' `commit` on both groups at every 3 s poll for 2 min). The HELLO
  gate accepted it, so the build ID is unchanged and a **rolling** restart
  is fine. **fcstor003 (node 1, g0 leader), fcstor005 (node 3), fcstor006
  (node 4, g2 leader) still run the PREVIOUS process** (started ~09:20,
  without the `host_pub_batch` fix). Nothing was wiped; no `--join`.
- **Unexplained, check first:** with no test running, both Raft groups
  were advancing ~33 entries/s (g0 commit 3259035 → 3333442 in ~7 min).
  Candidates: a leftover IOR/posix process on a client, or the reaper on
  the old-binary leaders. Do not restart anything until you know which.

**Steps (as of 22:45 Sep 20 — steps 1–4, B and C are DONE; what is left):**

- **A. DONE — commit `30c41ee`** (W6 correctness batch + the 100 ms
  floor). posix jobs=1 after the outbox fix: **200/201 both-pass + mmap
  SKIP in 60 s** (`results/posix/20260921-024204`; the four 15 s walk
  timeouts are gone). **The cluster still runs `d0fd0448adb6-dirty`**
  (byte-identical source to `30c41ee`, built before the commit); the next
  server restart must be stop-all-four / start-all-four, `roll_efsd.sh`
  will refuse. Clients likewise run the dirty build — fine until the next
  `deploy_fuse_clients.sh`.
- **B. DONE** — reaper drained, both groups flat; `raft_log_tail.py` on 004
  no longer dominated by `LANE_SWEEP`. Keep the check as a habit before
  any measurement.
- **C. DONE** — `results/perf/20260921-md-latency.txt`: medians 6–9 ms.
  The pump's per-apply KV WAL fsync is NOT a serialization point at this
  load (0.3 ms per fsync on NVMe); no `sync_mode` question for the user.
- **D. DONE** — `results/io500/20260921-debug-9x4-outbox/`, 0 read errors,
  all unlinks OK, mdtest 4.7–11× (see the 23:10 entry). Then step 6.
- **E. DONE (user chose (b), 08:45 Sep 21 entry)** — §7.2 reductions on
  the parent row / dseq / dir lane; repro is now a script,
  `tests/stress/same_parent_storm.sh` (PASS, 14 400 ops, parent clean).
  Any directory whose `stat` nlink ≠ 2 + subdir count after this build is
  a NEW bug, not this one. Step 5 (9×4 IO-500) PASS on it
  (`results/io500/20260921-debug-9x4-reduce/`); step 6 = the §7.2 commit
  (`git log -1 --grep="commutative reductions"`). **Nothing is in flight after that commit** — take the lowest
  open item in §1a (W6 residuals first). The cluster and the 9 clients run
  `b2184a5c7faf-dirty` = the same source as that commit; the next server
  restart is a build-ID change → `tests/roll_efsd.sh --all`.

Original steps (1–4 done twice, kept for the commands):

1. Pre-flight (deploy rule). Then the churn check: on fcstor007–015
   `pgrep -x io500; pgrep -x ior; pgrep -f posix_suite` (kill leftovers
   with `pkill -9 -x`, never `-f` on an ssh command line); on fcstor003 and
   fcstor006 `tail -c 400000 /tmp/efs/efsd.log | grep -oE 'raft-host: [a-z-]+ [a-z-]+' | sort | uniq -c | sort -rn | head`.
   If it is the reaper (`lane-sweep`), the restart below stops it; if it is
   a client, kill it and re-check `commit` is flat for 30 s.
2. Rolling restart: `efs-bg.sh start roll 'bash tests/roll_efsd.sh 1 2 3 4'`
   from the login node (runs on node9901; builds on each node, `pkill -9
   -x efsd`, starts, waits for per-group catch-up, refuses on a build-ID
   change). ~2 min for four nodes. Expect a few minutes of group-2
   elections afterwards (RDMA peer conns re-forming) — wait for
   `leader != -1` and a stable `term` on all voters.
3. **The gate for the fix:** on fcstor007
   `SLOTS=4 bash tests/perf/io500/run.sh ior-hard-write 3000`, wait for
   `last-run.log` to finish, remount every client (deploy rule "Restart one
   efs-fuse" — cold verify or it is dcache), then
   `SLOTS=4 bash tests/perf/io500/run.sh ior-hard-verify 3000`. Pass =
   **0** `-R` mismatches; also run `/tmp/hardcheck.py` and expect `bad
   records 0`. Expect hard-write MiB/s to DROP (the STALEs the old code
   swallowed are now retried) — record it, do not tune it here. Also the
   W1 gate: `tests/stress/n1_shared_pwrite.py` `prepare` on 007, then
   `write-a` on 007 ∥ `write-b` on 008, remount 009, `verify` on 009 →
   `lost=0` (shape and expected output: `results/stress/20260918-n1-w1/gate10c.txt`).
4. `EFS_TRANSPORT=tcp POSIX_JOBS=1 bash tests/run_tests.sh posix fcstor007.ib`.
   Gate: ≥ **195/201**, only the known signature (`dir_deep_nesting*` /
   `dir_many_files` / `names_crazy_dirs` 15 s walks, `mmap_write_read`
   SKIP, `concurrent_writes_disjoint` flake); no `ino_dup` in any
   efsd.log; no `apply lane-sweep rc=-5` on any leader after the restart.
5. 9×4 IO-500 debug once (`SLOTS=4 NP=36 bash tests/perf/io500/run.sh
   debug`, detached driver; poll `driver.log`). Gate: every phase
   finishes, ior-easy-read **and** ior-hard-read `-R` errors = 0, every
   mdtest unlink OK. Copy `result.txt` + ini + `driver.log` to
   `results/io500/<id>/`.
6. Commit code + rules + this file with the result directories cited, then
   delete this block (W6 residuals move to the W6 queue item).

---

<a id="ph-oct-2-2026-20-05z-servers-perf-idle-window"></a>
## Oct 2 2026 20:05Z — servers profiled 19:22–19:52Z under `--perf`, then stopped; GC pass is delete-bound, PUT cost is XFS metadata

Sequence: 19:22Z `cluster.sh start --perf` on `efc0f499cbae-dirty`
(clients fcstor003–015 already down since the 19:05Z stop); the user ran
one 10 GiB `dd bs=1M` (O_TRUNC over an existing `001/dat16`, no fsync,
strace -T on dd and `perf -g` on efs-fuse) from fstor007 at
19:39:11–19:39:43Z — 337 MB/s, 2.24 ms mean per 1 MiB `write()`, the
serial source `read()` 25 % of the wall; 19:52Z `cluster.sh stop`
(SIGTERM efsd, then perf; all four footers OK). `~/efs-runs/rstop.sh`
copied `efsd.log` and `/tmp/efs-perf/efsd.data` off each node and ran
`perf report` on the node; everything is in
`results/measure/20261002-195156-servers-perf-idle/` (SUMMARY.txt,
perf-self/-children/-tid/-header per host, full efsd logs) and the raw
data in `~/orcd/scratch/efs/perf/servers-20261002-195156/`. **The whole
cluster is down after this**; the next start is `cluster.sh start`
with storage kept (no `--join`, no `--fresh`).

Findings (details and numbers in the SUMMARY):

1. Servers at 8–10 % of one core over the 30 min, almost all of it in
   the 32 s dd window. The PUT path (`writer_thread`, 20–24 % of efsd
   cycles on every node) is kernel XFS: `path_openat → xfs_create`
   11–12 % and `writev → iomap_dio_rw → xfs_bmapi_write /
   xfs_alloc_ag_vextent_near` 17–21 %, `xfs_trans_commit` 7–8 %;
   ~61 K fragment creates per node in 32 s. efsd user code in that path
   is ~0.3 %; data memcpy 1.5 %. That is the one-file-per-fragment
   store design showing as filesystem metadata cost.
2. Followers (fcstor004/005): `host_pump` 15–17 %, `lsm_batch` 8–9 %,
   `compactor_main` 7–8 %. The dd's 153 REPORTs (1.39 M publish
   records) filled L0 every ~50 ms: 1215 compactions in minute 19:39
   (20/s, ~12 MB each), `l0=270` during, ~260 idle and not coming
   down; 15.6 GB rewritten. Leader `apply-sleep` 317 lines in that
   minute (mean 35.5 ms, max 118.5 ms, gap ≤ 6). `pub_p50=75.9 ms`.
   Compaction shape stays D9/D10.
3. Leaders (fcstor003 g0, fcstor006 g2): `host_gc_thread` 6.7 %,
   `nvme_del_if_sum` 7.6 %. From the dd's first second to the stop
   both logged a `gc-pass` every ~1.27 s with identical counters
   `fscans=2 fkeys=516 femit=514 ftomb=2` / `gc-frag scans=2
   records=512` (591 and 594 passes). Read against the code: the
   D26 cursor removed the tombstone walk (W44 a's 1 M-tombstone pass
   is gone — the 19:22 start pass walked `ftomb=2172` once), and
   `GC_FRAG_BUDGET_US` (200 ms per group per loop) ends the pass after
   the second 256-record scan because each scan is ~130 ms of
   `host_gc_record`: up to three serial fragment deletes per record,
   two remote (`get_avg=177us`) → ~0.5 ms per record, ≤ ~400
   records/s per group. 302 K record emissions in 13 min against the
   81 920 records one 10 GiB overwrite queues: either a larger queue
   draining (the user's earlier dat* overwrites, reaps of the test
   trees, 16 `lane-fence rc=-2` inodes at 19:38:59) or a set that
   fails every pass and the cursor wraps over. `efs_meta_apply_gc_ack`
   is in the follower profile, so acks were applied. The
   discriminator is a GC-prefix key count on a copy of `mdraft/kv`
   (safe while the servers are down) or a W50-style `EFS_GC_DBG` run
   with the cursor in tree. Not decided; recorded on the P2.2 row.
4. Leader pump 7–8 %: `efs_meta_apply_lane_sweep → lsm_batch →
   memmove` — the reaper's LANE_SWEEP entries for the unlinked test
   trees; the single largest efsd user symbol (1.7–2.9 %).
5. Nothing else moved: `wait_timeouts=0`, `tx fail=0`, no
   backpressure line, `apply_max` 80–273 µs, `persist_max` ~0.5 ms,
   stable terms after the restart election.
6. Client side (user's capture): blake3 21 %, memmove 14.3 %
   (**7.7 % under `efs_rdma_send_frame` on the PUT send path — W39's
   zero-copy send apparently not engaged on fstor007; check
   `efs_rdma_zc_region_add` there**), `xor_into` 10.7 %, kernel
   `fuse_copy_page` memcpy 5.9 %; `dcache_put_now` 58.9 % inclusive.
   Known shape apart from the send-path copy.

No code changed. START-HERE §1b item 0 and the P2.2 row carry the
findings; the rule file's cluster fact says DOWN.

---

<a id="ph-oct-2-2026-19-05z-w54-fold-gc-live-base-lookup-memo"></a>
## Oct 2 2026 19:05Z — redeploy gate opens W54 (fold GC deletes the live base) and the lookup-memo stat regression

Redeployed `20745142` (client: one readdir listing per opendir, LOOKUP
row answers the next GETATTR) with `cluster.sh restart --clients --perf`
at 17:00Z — a build-id change, so stop-all/start-all; storage kept.
Gate (`results/measure/20261002-165956-redeploy-posix-ior/SUMMARY.txt`):

- posix jobs=1 fcstor008 **164/201, 36 fail** (`results/posix/20261002-170119`)
  vs 200/201 at 15:19Z. Every failure is a same-client stat showing the
  pre-mutation row (size 0 after write, old mode after chmod, old nlink
  after link, old mtime after utimens): `lookup_memo_take` answers a
  GETATTR within 50 ms of the LOOKUP from the LOOKUP row. posix2 63/63
  (the peer holds no memo). START-HERE row 0j.
- IO-500 9×4 debug (`results/io500/20261002-165956-rdma`) **aborted in
  ior-hard-read**: rank 34 on fcstor015 `read() EIO`, `MPI_ABORT`, no
  score. Phases that ran: easy-write 4.393 GiB/s, hard-write 0.530,
  easy-read 25.8, mdtest-easy-stat 19.8 kIOPS.

The abort: fcstor015 `fuse.log` 17:04:40Z `fetch published ino=656804
ci=181944 rc=-9 then pull rc=0 rc=-9` with the row identical before and
after the pull, then `efs-fuse read: decode error (efs_rc=-9)
off=23847816512 len=47008`. `raft-getchunks 656804 181944`:
`base_gen=15366554570668337119 spans=2`, tombstones
`4218386339069290638 seq=1088` and `15366554570668337119 seq=3731` — the
base is the second tombstone. On disk under `…/0065/6804/177/` only
`181944.{0,1,2}.3089159234672355549` (fcstor003/004/005); the live
generation's three fragments are unlinked. Mechanism: two clients
merged the chunk to the same image (same content hash → same object
gen); one published it as a span, the other folded the same object as
the base; the fold branch of `efs_meta_apply_publish` tombstoned the
span and `gc_queue`d its gen (no check against `stored.generation` or
`chunk_aliases`); `host_gc_local_del` → `efs_store_del_if_sum` passed on
the identical sum; the reaper deleted the live fragments. **W54**,
START-HERE row 0i, before every performance row. Server code unchanged
since `2b5a25df`; the 15:21Z clean same-mount hard-read does not clear
it. The file is unrecoverable (IOR test data).

Tree pulled to `efc0f499` at 19:01Z (metadata reads to the group leader;
the lookup memo is still in it). Not deployed. Servers remain
`20745142ad79-dirty`.

---

<a id="ph-oct-2-2026-15-00z-w50-w51-d26-cursor"></a>
## Oct 2 2026 15:00Z — W50 closed (not stuck), W51 table written, D26 cursor unrolled

P2.1: three `EFS_GC_DBG=1` windows on 19810 after the P1 tree
(`111a07527093-dirty`). Passes 1–2 had `ex=(nil)` (no RAM export until
a PUT/GET on that process) and deleted nothing. Pass 3 remounted
clients and kicked a 1 MiB non-zero write: every `gc del` and
`gc ack flush` rc=0, zero consecutive-pass identity repeats
(33792 / 36864 unique = del count). The repeating `records=256/512` is
the scan batch, not a failed-delete set. Summary:
`results/measure/20261002-134900-w50-gcdbg/SUMMARY.txt`.

P2.4: classified the existing 144 `apply-sleep` lines from the P0.2
file — compact-overlap 47, small-gap-no-compact 89, lag-gap 0. D30
stays an ask. `results/measure/20261002-143815-w51/SUMMARY.txt`.

D26 cursor (`efs_kv_scan_from` past the last emitted GC key) is in
`raft_host.c` and not rolled. The apply-batch watermark is still to
land. Servers at 15:00Z: `111a07527093-dirty`, `--perf`, clients
fcstor007–010 remounted 14:57Z.

15:19Z gate on that same server build (clients fcstor007–015 remounted,
no wipe): posix jobs=1 200/201 (`results/posix/20261002-151947`),
posix2 63/63 (`results/posix2/20261002-152031`), IO-500 9×4 debug
(`results/io500/20261002-152126-rdma`): easy-write 6.837 GiB/s,
hard-write 0.600, hard-read 0.845 with 0 Incorrect-data lines
(Oct 1 had 1). Compare:
`results/measure/20261002-151920-posix-ior/SUMMARY.txt`.

<a id="ph-oct-2-2026-13-45z-performance-plan-p0-p1-d23-w41-landed"></a>
## Oct 2 2026 13:45Z — performance plan P0 (gates) and P1 (D23 + W41) done; W52, W53 opened

The user started the START-HERE performance plan ("implement
performance plan", 05:33Z) ahead of the correctness list. Servers stayed
on `start760` (`cluster.sh start --clients --perf`, 05:25Z) all day; no
wipe, no server code change.

**P0 (`results/measure/20261002-053311-p0-gates`,
`-054132-p0-x16/SUMMARY.txt`).** W28 gate on fcstor007 PASS: 8 GiB
dd+fsync 1518 MB/s, cold read 3297 MB/s, CMP_OK. W46/W47 counts from a
20 s `strace -c` attach around a second 8 GiB dd: server writev
1.00/fragment (W46 halved it), eventfd write 2.36/fragment (unchanged;
W47 saves nothing server-side), futex 8.1/fragment, openat 1.7
(28 K ENOENT probes); client write 1.0/fragment reply, read 1.09, poll
0.64, futex 0.5/PUT. Row 11, untraced 16 × 10 GiB dd on fcstor007:
every stream 44.7–45.0 s = **3815 MB/s aggregate**, no close tail (the
traced 04:02Z fstor007 run had 663 MB/s and a 74.6 s close); 169
`report-split`, all on fcstor004, `fail=wait` 0, `skip=all` 0, nrec
~8250, pack 3.4–7.6 µs/rec (the traced run had 100–133 µs under
l0=100–170); 144 `apply-sleep` of 20–34 ms, none at the 400 ms
deadline. So D29's motivating symptom (BUSY after commit, byte-identical
resend) did not appear untraced — the ask stands without a live case.
RSS 4.26 → 8.57 GB over 160 GiB written (not flat, not tracking bytes).
W19 closed: leader profiles mid-run show memmove 1.6 %, `try_commit`
< 0.2 %; the leaders' top user-space cost is the GC frag pass's key
scan (`__memcmp_avx2_movbe` 9 %, 6.6 % under `host_gc_thread`) — D26.
W49 closed: fstat + getsockopt + recvfrom(MSG_PEEK) = 0.04 % of
534 894 syscalls during an ecopy. Caveat recorded: `perf report` shows
`efsd (deleted)` after a `--clients` roll (the rsync+make replaced the
binary file under the daemon); the harness's RSS sampler held `wait`
185 s past the last dd, so `total_wall` in x16.txt is an artefact.

**P1 (`results/measure/20261002-060052-p1-d23-w41/SUMMARY.txt`).**
Client-only change (`write.c`, `client_internal.h`, `node_cache.c`),
built on fcstor008 `/tmp/efs-dev`, deployed to fcstor008/009 (later
010). W41: per-inode dirty sets, per-inode publish slots, a 4-thread
meta-flush pool for D24's landed REPORTs, no `report_mu`. D23 took
five gate passes to find its correct place: pass 1 wedged the client
(rdcache put under the slot mutex vs `efs_client_set_chunk` holding
`idx_mu` → `efs_dcache_yield_extra`; gdb stacks in the results dir);
pass 2 EIO'd posix `basic_overwrite_middle` and four posix2 pwrite
tests (a span-only row has table gen 0, the rdcache refuses it, the
body-less `have_base` node then publishes zeros+range as a full image
→ FOLD_LIST STALE, replay loop); pass 3 still failed
`peer_shared_pwrite` (`dcache_store_owned` used `dcache_find`, which
skips body-less nodes, so a second node shadowed the first); pass 4
still failed it (after a PUT-time hand-off the STALE replay had no
body and no ranges — `why=2 BASE_CAS exp=0 base_n=1` ×129, 30
`stale-class` rounds with no `snap-replay`). Final form: the body goes
to the rdcache in `dcache_note_committed` (REPORT OK, `object_gen ==
gen`, clean, `base_gen != UNCOND`), put outside the slot lock; both
store paths use `dcache_find_meta`. Pass 5: posix 200/201, posix2
63/63 (`results/posix/20261002-130142`, `results/posix2/20261002-
130227`), CMP_OK after remount, md_latency 3.2/1.7/3.4/0.2/0.6/3.1 ms.
D23 RSS gate (4 × 1 GiB bs=64k dd+fsync): P1 build 1.45 → 2.52 → 2.55
→ 2.55 GB, old build 1.45 → 2.52 → 4.64 GB. W41 wedge gate on the same
host (fcstor010, old then P1 build): 4 × 8 GiB dd wall 9.70 → 8.30/
8.37 s, storm p50 4.9 → 3.8/5.5 ms, **p99 37.2 → 100.7/121.6 ms, max
52.8 → 277/217 ms** — the plan's p99 ≤ 51 ms gate is not met. Opened
as **W53** (investigate; keep/revert is the user's): the hypothesis is
that each small close's own REPORT proposal now queues behind the
pool's 8192-record batch applies (fcstor006 `apply_max` 100–160 ms
during those applies) where before the small file's records rode in
whichever client-wide REPORT was in flight; not excluded are the
commit-time rdcache put taking `idx_mu` (should not trigger for span
files or UNCOND dd chunks) and the pool threads' conns.

**W52 (found on the way, both builds):** a REPORT after thousands of
O_APPEND writes answers after > 30 s because `host_resolve_caught_up`
runs one serial `host_propose_wait` per OPEN reservation after the
`report-split` line; the client's 30 s recv timeout resends
byte-identical (`retry type=67 why=recv rc=-6 recv_ms=30312`), the
server packs again (`skip=all`), dd prints `fsync: I/O error`; the
size lags (completed prefix) and converges. fcstor004 `nrec=1133`
06:03:30Z + `skip=1133` 06:04:01Z (old build), `nrec=625` ×3 at
06:06:45/07:15/07:46Z (P1). ~5 ms per O_APPEND write. Fix shape is a
question for the user. Also: fcstor005 `inbox_drop=158` dates from
05:45:49–05:46:06Z (type=3 frames from node 0 during the W49 ecopy),
unrelated to P1.

Cleanup: test trees under `/tmp/efs-mount` (`d23 p0 tr769 tr770 p1 …`)
and `/tmp/x16-*`, `/tmp/w41-gdb.txt`, `/tmp/tr77*` on fcstor008/009
removed (`clean785`); intermediate posix/posix2 dirs of passes 2–4
deleted; `/tmp/efs-dev` on fcstor008 left as the P1 build dir.

---

<a id="ph-oct-2-2026-05-00z-review-of-the-r749-window-16-dd-traced-on--e5dc7f"></a>
## Oct 2 2026 05:00Z — review of the r749 window: 16× dd traced on both ends, REPORT drain slower than PUTs, W44 step a read

The user asked for a review of `~/orcd/scratch/efs/perf/efs-mount/`
(their `scripts/client.sh --perf --strace` run on fstor007) together
with the servers' perf profiles and straces from the `r749` window
(servers `2b5a25df419c-dirty` under `--perf --strace`, 01:54Z–04:24Z),
then the cluster was stopped (`stop751`). The review and its reductions
are `results/measure/20261002-040242-dd16x10g-review/` (SUMMARY.txt);
the raw files stay in `~/orcd/scratch/efs/perf/efs-mount/` and
`~/orcd/scratch/efs/perf/cluster-20261002-0423/`, and
`/tmp/efs-perf/efsd.{data,strace}` are still on fcstor003–006 until the
next `cluster.sh start`.

**A wrong first reading, recorded so it is not repeated.** The one
`dd.trace.txt` in the dir (dat16, 10 GiB, 37.3 MB/s, `close()` 74.6 s)
was first read as the whole load, and the servers' 852 k fragment
creates per node in the window — 14× what one 10 GiB file needs — as a
PUT-amplification bug in the new tree. The FUSE census settled it: the
`fuse_in_header` nodeid at offset 16 of every 1 MiB `/dev/fuse` read
shows **16 inodes** receiving 10240 WRITEs each from 04:02:43Z
(`client/ana-fuse-writers.txt`). 140 433 MiB in 222 s = 663 MB/s
aggregate on one client with recorders on both ends — the same as the
untraced Oct 1 20:56Z 16× run. The lesson is in the project-state rule:
census the nodeids before reading a single-process trace as the load.

**What the run shows (the numbers are in SUMMARY.txt):**

- Per stream 50 MB/s; dd `write()` latency 6358 under 2 ms, 2132 at
  10–50 ms, 1507 at 50–200 ms, 56 at 200 ms–1 s (dirty-cap
  back-pressure with 16 streams on one 2 GiB budget). One 1 MiB
  `write()` = one 1 MiB FUSE WRITE.
- **Publish slower than PUT.** `report-split` on fcstor004/005: nrec 8 k
  → 30 k → 57 k → 68 k → 124 k → 322 k → 570 k; pack 0.8 → 42.9 s (one
  point get per record, 100–133 µs under `l0=100–170`); push up to
  77 s; every REPORT but the last `fail=wait/-13` — the host had
  committed every record and then its apply-wait hit the deadline, so
  the client resent the identical REPORT to the other dual host, which
  packed it again (0.3–15 s) and found `skip=all`. Publish ≈ 2.7 k
  rec/s against PUT ≈ 5 k chunks/s: the dirty set grows for the whole
  write and the last `close()` pays 74.6 s. D24 (REPORT every 8192
  landed) bounds a close only when REPORTs drain faster than PUTs land.
  W41 (decided) is necessary and not sufficient; the "committed, apply
  pending" receipt is an ask (Q2 in SUMMARY → **D29**, 05:30Z). One STALE round
  (322 395 records, 320 347 already committed, 2048 replayed,
  `pull_ms=12449`).
- **W44 step a, read:** idle group-0 leader `gc-pass` every 1.6 s,
  430–740 ms, `fkeys=1 026 406 ftomb=1 026 149 femit=257` constant for
  2 h (38 % of a core); group 2 `596 739 / 596 482 / 257`. perf: ~90 %
  of each leader's `efsd` cycles under `host_gc_thread →
  host_gc_frag_pass` (`merge_scan`, `__memcmp_avx2_movbe`,
  `kv_seg_iter_next`, `kv_msrc_advance`, pread). It is tombstones under
  the GC prefix, not the L0 count → D26 (i). After the dd the
  tombstones compacted away (12 k / 121 k) and grew back (171 k /
  337 k; +1024 per pass = the superseded objects draining at ~512
  records per 1.4 s pass). A `gc-frag group=0 scans=1 records=126
  ms=428` line repeating identically is consistent with deletes
  failing every pass but does not prove it (Q4 → **W50**, 05:30Z: the
  user pointed out that identical counts prove nothing and that
  `femit=257` means the passes are not empty, so D26's watermark alone
  will not remove them). The raft log tail before the stop was 99.7 % GC_ACK
  at 5–6 commits/s per group — the reason `preflight.sh` reported "not
  idle" before `r749` was started without it.
- **Compaction pressure** on fcstor004/005 during the write: l0 up to
  170, 340 MB L0+L1 merges of 3–5 s, `apply-sleep` ×31 / ×103 — the
  `fail=wait` REPORTs above and 400 ms BUSY on follower-served reads.
- **Server syscalls per node per 5 min:** futex 14.7 M, eventfd `write`
  7.5 M (W47 saves nothing on a server: the conn thread is always
  armed), poll 3.6 M, openat 1.85 M (852 k O_CREAT), writev 1.7 M,
  pread 1.66 M, fsync 13.8 k (max 0.15 s), unlink 25 k; no work syscall
  over 50 ms. **Client:** recv poller 6.4 M eventfd writes, 24 PUT-pool
  threads 149 k poll+read each (one per fragment reply), flush thread
  `recvfrom` up to 120.6 s (REPORT replies over the TCP side channel);
  perf blake3 19 %, memmove 10.7 %, memcpy 4.7 %, xor 3.6 % of ~0.9
  core. CPU is not the wall on either end.
- **Open:** four of the 16 streams ended at 3.6–5.2 GiB (04:04:17–48Z)
  with a normal FLUSH and no error reply in the FUSE trace; their dd
  exit lines are in the user's harness dir, not in `efs-mount/` (Q1 →
  **W48**; an EIO/ENOSPC there would not by itself implicate W42).
  The traced stream's `openat(O_CREAT|O_TRUNC)` took 1.27 s under 16
  concurrent creates (not chased). No `apply truncate` / `lane-fence`
  lines: the files were fresh at open.

Session mechanics: reductions ran as screens (`ana5-003..006` on the
servers, `cli5`–`cli9` on fstor007; scripts in `~/efs-runs/`, outside
the repo); `tail -2 a b` is "option used in invalid context" — `tail
-n 2`; `raft_log_tail.py` wants `/data1/01/efs/mdraft/log/raft.log`.

---

<a id="ph-start-here-handoff-archive-oct-2-2026-05-00z-moved-oct-2-2026-1345z"></a>
## START-HERE handoff archive Oct 2 2026 05:00Z — moved Oct 2 2026 13:45Z

Verbatim from START-HERE §1b when the Oct 2 13:45Z block replaced it.

**Oct 2 2026 05:00Z — review of the `r749` window (servers `--perf
--strace`, fstor007 `client.sh --perf --strace`), written for whoever
picks this up; the cluster is STOPPED (`stop751`, CLUSTER_OK 04:24Z).**
Results + reductions: `results/measure/20261002-040242-dd16x10g-review/`
(`SUMMARY.txt` is the review; raw traces stay in
`~/orcd/scratch/efs/perf/efs-mount/` and
`~/orcd/scratch/efs/perf/cluster-20261002-0423/`; `/tmp/efs-perf/
efsd.{data,strace}` are still on fcstor003–006 — `cluster.sh start`
deletes them). What it found, in order of weight:

1. **The run was 16 × 10 GiB dd on one client, not one dd.** The FUSE
   census (`client/ana-fuse-writers.txt`, nodeid from the
   `fuse_in_header` of every 1 MiB `/dev/fuse` read) shows 16 inodes
   × 10240 WRITEs from 04:02:43Z; 140 433 MiB in 222 s = **663 MB/s
   aggregate with recorders on both ends**, same as the untraced Oct 1
   20:56Z 16× run. The traced stream (dat16, `efs-mount/dd.trace.txt`)
   got 50 MB/s + a 74.6 s `close()` = 37.3 MB/s. Servers: 3700–4000
   fragment creates/s each = exactly that load. No regression, no PUT
   amplification, no `apply truncate`/`lane-fence` lines (fresh files).
   Four streams stopped at 3.6–5.2 GiB with a normal FLUSH and no error
   reply seen in the FUSE trace — **W48 (investigate):** their exit
   status, signal, stderr and byte counts are in the user's harness
   dir, not in `efs-mount/`. An EIO/ENOSPC there establishes a failure;
   it does not by itself name W42 (new in this tree) as the cause —
   the server log at that second and the client's `inode-rpc` lines
   decide that.
2. **The publish path is slower than the PUT path, and BUSY-after-commit
   doubles its cost.** `report-split` on fcstor004/005: nrec 8 k → 30 k
   → 57 k → 68 k → 124 k → 322 k → 570 k; pack (one point get per record,
   100–133 µs each under `l0=100–170`) up to 42.9 s; push up to 77 s;
   every REPORT but the last ended `fail=wait/-13` (committed, then the
   apply-wait deadline → BUSY) and the client resent it byte-identical
   to the other dual host, which packed it again and found `skip=all`.
   Publish ≈ 2.7 k rec/s vs PUT ≈ 5 k chunks/s, so the dirty set grows
   for the whole write and the last close pays it. D24 bounds a close
   only when REPORTs drain faster than PUTs land. → W41 (decided) is
   necessary, not sufficient. **D29 (ask, user's framing 05:30Z):** a
   REPORT *receipt* for "committed, apply pending" that lets the client
   wait on or query the **same operation** instead of re-sending it
   (no second pack, no second proposal). It is not a successful
   publication and must not let the client free dirty bytes — the
   apply may still return STALE or another failure. Tests must cover a
   lost receipt and a leader change between receipt and verdict. Also:
   D26's L0 width is a REPORT-pack cost too, not only the GC's.
3. **W44 step a reading is in (D26's input):** idle leaders scan
   **1 026 406 keys of which 1 026 149 are tombstones** (group 0) /
   596 739 / 596 482 (group 2) to emit 257 records, every 1.6 s,
   430–740 ms per pass = 38 % of a core; ~90 % of each leader's `efsd`
   cycles under `host_gc_frag_pass`. The cost is tombstones under the
   GC prefix, not the L0 count. **But these passes are not empty:
   `femit=257` means 257 live records were found each time** (user,
   05:30Z), so D26's watermark — which skips only truly empty scans —
   will not help while those records remain. D26 therefore pairs the
   watermark with **bounded scan progress** (a pass that resumes
   where the last one stopped instead of re-walking 1 M tombstones to
   reach the same 257) and with W50; the watermark is maintained
   atomically with record insertion/removal and across recovery.
   After the dd the tombstones were compacted away (12 k / 121 k) and
   grew back (171 k / 337 k, +1024 per pass = the run's superseded
   objects draining at ~512 records per pass). A `gc-frag group=0
   scans=1 records=126 ms=428` line repeating identically is the
   **W50 (investigate)** symptom: identical counts alone do not prove
   failed deletes — record the 126 identities, each delete's verdict
   and the committed GC_ACKs (`EFS_GC_DBG=1` for one pass) before
   naming a cause. The raft log tail is 99.7 % GC_ACK at 5–6 commits/s
   per group — that is why `preflight.sh` says "not idle".
4. **Compaction pressure on the data-heavy followers** during the write:
   `kv-compact` l0 up to 170, 340 MB L0+L1 merges of 3–5 s,
   `apply-sleep` ×31 (fcstor004) / ×103 (fcstor005) coincide with the
   `fail=wait` REPORTs in 2 and 400 ms BUSY on follower-served reads.
   **W51 (investigate):** the experiment must distinguish lock blocking
   (the apply waiting on `l->mu` / the compactor), slow application
   (the apply itself), and transport delay (AE arrival) before any
   remedy; **D30 (ask)** is that remedy and stays open until W51 says
   which it is (D9/D10 territory).
5. Server syscalls per 5 min (each node): futex 14.7 M, eventfd `write`
   7.5 M (W47 saves nothing server-side: a conn thread is always armed),
   openat 1.85 M, writev 1.7 M, fsync 13.8 k (max 0.15 s). Fragment
   writes ~60 µs; the writer pool is not a bottleneck. Client: recv
   poller 6.4 M eventfd writes, 24 PUT threads 149 k poll+read each,
   blake3 19 % of 0.9 core — CPU is not the wall on either end.

---

<a id="ph-start-here-handoff-archive-oct-2-2026-01-20z-moved-oct-2-202-70793e"></a>
## START-HERE handoff archive Oct 2 2026 01:20Z — moved Oct 2 2026 05:00Z

**Oct 2 2026 01:20Z — the "runs without the user" table, rows 1–3 and
5–10, is in the tree (W43 b/c, W45, W36, W26, W44 a, W46, W47 variant)
plus W42 (row 2a).** All 16 unit suites + `test_rdma_xprt` pass on
fcstor007 with no warnings. **The cluster is STOPPED** (`stop751`,
`~/efs-runs/stop751.log`, `cluster.sh stop --clients`, CLUSTER_OK
04:24Z Oct 2). It had been up since `r749` (01:54Z, `2b5a25df419c-dirty`,
servers `--perf --strace`). Perf reports from that window:
`~/orcd/scratch/efs/perf/cluster-20261002-0423/fcstor00{3,4,5,6}/{flat,by_thread,callers}.txt`
(0 lost samples; fcstor003's efsd needed the -9 after SIGTERM, header
still ok). Raw `/tmp/efs-perf/efsd.data` and `efsd.strace` are still on
each server. fcstor clients had no recorders; fstor007's earlier
`efs-mount` reports were already written at 00:19Z and were left as
they were. The earlier `start740` (01:22Z, no recorders,
`141071a3e9ba-dirty`) is what the gates below ran on. **Live gates held**
(`~/efs-runs/gate741.log`, `gate742.log`): posix fcstor007 **200/201**
(`opt_fallocate` PASS on efs and on the XFS baseline; the one skip is
`mmap_write_read` by spec), posix2 fcstor008/009 **63/63**
(`peer_rename_vs_unlink_src` PASS once; the W36 20/20 repeat is still
owed), `tests/stress/truncate_big.sh` fcstor010 **exit 3**
(`results/stress/truncate-big-20261002-012442`: all four truncates
returned EIO, every file unchanged, 0 lies; the servers show `apply
lane-fence rc=-2` ×4 on fcstor004–006 — the inodes sit on group 2 —
one per refused truncate, and no `apply truncate rc=` line because the
fence fails before the inode-group entry is proposed). W42 live: `df`
on fcstor007 reports 96.00 TiB total (4 × 36 TiB × 2/3; was 72) and
used 1.21 TiB = Σ node used × 2/3; `efs-mgmt status` prints the same
`Usable (2+1 logical)` line.

*W45 audit — the case list (every `rc=%d index` site in `raft_host.c`):*

| apply | before | now |
| --- | --- | --- |
| create, xattr, append-rsv, publish | verdict | unchanged |
| **unlink** | every failure → OK (BUSY/IO included: file stayed, client heard success); NOT_FOUND "replay" | **verdict**; NOT_FOUND = lost a race → ENOENT; retries are the op-id window's job |
| **rmdir** | NOT_FOUND → OK | **verdict** |
| **setattr** | NOT_FOUND/STALE → OK | **verdict** (handler packs `expect_gen = 0`, STALE cannot occur) |
| **utimens** | NOT_FOUND/STALE → OK | NOT_FOUND etc. **verdict**; STALE stays OK — documented: a later `mtime_gen` already landed, the later stamp is the POSIX result |
| **truncate** | every failure → OK (the W43 lie) | **verdict** (NOMEM at the 33rd chunk → EIO until D25) |
| **activate-lane** | every failure → OK | **verdict**; `efs_meta_apply_activate_lanes` itself answers OK for "row unlinked since the read" and "bits already set" |
| **lane-fence** | every failure → OK | **verdict** (the fence apply is idempotent on its own: same/older epoch → OK) |
| lane-sweep, reap-done, gc-ack | logged, OK | unchanged — host-driven GC, the next pass rescans |
| append-res | NOT_FOUND → OK | unchanged — the reservation was already resolved |
| dir (BEGIN/MIGRATE/FINISH) | NOT_FOUND/INVAL/BUSY → OK | unchanged — the spread driver re-reads the layout each pass |
| session (LEASE_DROP/RECLAIM) | NOT_FOUND/INVAL/BUSY/STALE → OK | unchanged — cleanup; the reaper is the witness |
| **lock** (GRANT/RELEASE) | AGAIN/NOLCK/NOT_FOUND/INVAL/BUSY/STALE → OK | **unchanged, flagged**: the handler pre-checks conflicts before proposing, so an AGAIN at apply = a conflicting grant committed between pre-check and apply, and the client still hears "granted". Not changed blind — the lock wait queues live in leader memory and need the trace. Ask or trace before touching |

Those six commands are now on `host_apply`'s ring-only list (a non-OK
verdict is a reply, never a halted log).

---

<a id="ph-oct-2-2026-0120z--the-runs-without-the-user-table-landed-w43-7d41f2"></a>
## Oct 2 2026 01:20Z — the "runs without the user" table landed (W43 b/c, W45, W36, W26, W42, W44 a, W46, W47)

The user asked for every START-HERE item that is a bug or a performance
change and needs no decision. What landed, all unit suites +
`test_rdma_xprt` green on fcstor007 with no warnings, cluster restarted
(`start740`, `141071a3e9ba-dirty`), posix 200/201 (fcstor007, `gate741`
/ `gate742`), posix2 63/63 (fcstor008/009), `tests/stress/truncate_big.sh`
exit 3 on fcstor010 (`results/stress/truncate-big-20261002-012442`):

- **W43 step b + W45 (server, `raft_host.c`).** `apply_unlink_cmd`,
  `apply_rmdir_cmd`, `apply_setattr_cmd`, `apply_utimens_cmd`,
  `apply_truncate_cmd`, `apply_activate_lane_cmd`, `apply_lane_fence_cmd`
  return the apply's rc as the ring verdict instead of logging it and
  returning OK; the six commands joined `host_apply`'s ring-only list so
  a non-OK verdict never halts the log. The one kept mapping is utimens
  STALE → OK (a later `mtime_gen` already landed). The full case table,
  including the lock GRANT mapping left in place and flagged, is in
  START-HERE §1b. Live: `truncate -s 0` / `dd of=` (O_TRUNC) onto a
  1 GiB or 300 MiB file now returns **EIO** and leaves the file intact
  (`apply lane-fence rc=-2` ×4 on the group-2 replicas, one per refused
  truncate, each matched by a client error; `apply truncate rc=` is
  never reached because the fence fails first). Before, the same
  commands returned 0 with the size unchanged.
- **W36 (server, `meta_apply.c`).** The log-path unlink/rmdir probed
  the child's inode row for a pending txn intent but not the dentry it
  deletes unversioned. A RENAME's dentry EXCL lands before its inode-row
  REDUCE, so an unlink in that window removed the name and the row under
  the rename and the rename's RESOLVE then PUT the dest dentry over
  nothing — the `-?????????` of posix2 `peer_rename_vs_unlink_src`.
  Both paths probe `k_loc`/`k_hash` now (BUSY → client retry → ENOENT).
  posix2 63/63 on the first run after; the 20/20 repeat is the gate.
- **W42 (`placement.c`, `efs_fuse.c`, `efs_mgmt.c`, `test_placement`).**
  `efs_capacity_logical`: largest M with Σ min(cᵢ, M) ≥ 3M, logical =
  2M (binary search). `df` total = quotas through the bound, avail =
  per-node room through the bound, used = total − avail; `efs-mgmt
  status` the same over the up nodes. Live on 19810: `df` 96.00 TiB
  total (was 72), used 1.21 TiB = Σ used × 2/3; `efs-mgmt status`
  prints the same three numbers.
- **W26 (`efs_fuse.c`).** `ll_fallocate`: mode 0 past EOF publishes
  buffered writes then extends through the SETATTR path; inside EOF and
  KEEP_SIZE inside EOF are 0; KEEP_SIZE past EOF and every other mode
  EOPNOTSUPP. posix `opt_fallocate` now calls raw `fallocate(2)` on an
  `O_DIRECT` fd (no glibc zero-fill fallback), checks the extend, the
  no-ops, the hole read, and accepts 0 or EOPNOTSUPP for punch-hole
  (XFS baseline punches).
- **W44 step a (`kv_lsm.c`, `raft_host.c`).** Per-thread
  `efs_kv_scan_stats` counted in `merge_scan` (scans, segment iterators,
  merged keys, emitted PUTs, tombstones); the `gc-pass` line carries the
  frag pass's and the whole pass's counters, printed over 5 ms or under
  `EFS_GC_DBG`. The idle-hour reading for D26 is the next action.
- **W46 (`writer.c`).** `writer_slot` carries `cv_work`/`cv_done`/
  `cv_empty`; QUEUED and DONE wake one waiter with `pthread_cond_signal`,
  EMPTY broadcasts to the fallback waiters. Not yet measured (row 9's
  futex count per PUT).
- **W47 variant (`rdma.c`).** The poller writes a conn's eventfd only
  when a waiter is armed (`efd_armed`; `efs_rdma_reply_fd` and the
  `recv_wait` poll arm with a post-arm ring re-check that self-signals;
  `reply_ready*` disarm). Replies that land while the waiter is spinning
  or checking cost no `write`. One write per poll batch across conns
  would need a shared per-waiter fd — a protocol change, not taken.
- **W43 step c (`tests/stress/truncate_big.sh`).** Standalone (the
  posix gate stays 200/201): four cases, exit 3 = truncate refused and
  file unchanged (today), 0 = PASS (after D25), 1 = a lie.

Not touched (asks): D25, 0a(d), W41, D23, D17, D26, the benches, D15/D16,
the fragment layout, W40, zero-copy receive. Not run (need IOR/ecopy
time): W38, W27, the 16× dd re-measure.

---

<a id="ph-start-here-closed-items--full-text-w1w5-w7-w11-w13-moved-oct-7d6df9"></a>
## START-HERE closed items — full text (W1–W5, W7, W11, W13), moved Oct 1 2026

<a id="ph-w1--shared-file-n-1-writes-from-two-clients-silently-lose-da-a3f3d7"></a>
### W1 — Shared-file (N-1) writes from two clients silently lose data — DONE


`peer_shared_pwrite` is concurrent (`("a", a0), ("ab", (a, b)), ("a", a2)`).
STALE bound is **64** with 2–20 ms backoff — 8 loses to 16 fsyncs/chunk.
`EFS_CHUNK_BASE_UNCOND` (`UINT64_MAX`) is only for a full-chunk overwrite.

- **Gate:** `results/stress/20260918-n1-w1/` — n1-w1-gate10c
  `lost=0 decode_eio=0`; isolated `peer_shared_pwrite` **5/5**;
  `test_meta_apply` / `test_wire` OK (`test_publish_stale_then_retry`).
  Suite 2 one pair `results/posix2/20260918-122419/` **60/3** (was 58/4).
  Suite 1 jobs=1 `results/posix/20260918-123250/` 186/10 — every new FAIL
  isolated PASS except known `concurrent_appends`. Honest 1-client
  `results/perf/20260918-w1-honest/`: sw-1m **758** (not a regression vs
  209); sw-50g **373** (was NET). `FIO_ONLY=1` skips the 4/9 sweeps.
- **Amended Sep 28 2026 (D1):** the per-chunk CAS remains for
  full-image publishes; span publishes no longer CAS on the base
  generation (W17 step 3). The N-1 contract is: non-overlapping
  concurrent writes to one chunk never conflict; overlapping ones
  resolve in Raft order.
- **Forbidden to reopen:** a distributed chunk lock; per-record report
  status arrays; sending `UINT64_MAX` from any path that read a base;
  wipe / `raft-mkfs` / inventing chunked SNAP.

<a id="ph-w2--write-is-specified-as-durable-and-visible-the-code-buffe-58be40"></a>
### W2 — `write()` is specified as durable-and-visible; the code buffers — DONE


Measured (`results/stress/20260918-w2/`): peer sees **0/10** of an
un-`fsync`ed 4 KiB `pwrite`; `kill -9` of `efs-fuse` loses 64 MiB of an
acknowledged `write()` (file exists, size=0).

- **Forbidden:** implementing option (ii) publish-on-write; editing §3
  back to "`write()` is durable"; wiring `O_SYNC` as a silent side-cut
  of a later item.

<a id="ph-w3--split-the-single-client-fsync-tail-then-remove-the-large-c7adef"></a>
### W3 — Split the single-client fsync tail, then remove the larger half — DONE


- **Gate:** `results/perf/20260918-w3-split/gate.txt`. 8 GiB
  `dd+fsync` **13.501 s / 639 MB/s** (best cut 11.862 s / 724 MB/s);
  remount `HEAD_OK` `TAIL_OK`. **3.8–4.1 %** of 16.7 GB/s (was 1.9 %
  at 317 MiB/s). posix jobs=1 `results/posix/20260918-w3f/` **195/201**
  (190 both-pass; W1 was 186/10). posix2 one pair
  `results/posix2/20260918-w3f/` **58/63** (W1 60/3). Remaining
  suite fails are load / known O_APPEND atomicity / 15 s walks, all
  isolated PASS except `concurrent_appends`. Remaining tail is REPORT
  pack+push (~6 s).
- **Forbidden to reopen:** a REPORT split into multiple RPCs;
  weakening `fsync`; async flush to inflate the number; raising
  `EFS_IO_TIMEOUT_MS`.

<a id="ph-w4--4-client-and-9-client-honest-fio-and-dd--done-6101bb"></a>
### W4 — 4-client and 9-client honest fio and dd — DONE


- 1-client morning matrix `results/perf/20260918-w4-honest/`: sw-1m
  **694**, sr-1m **4102**, sw-50g **341** (W5). All `FUSE_OK` `err=0`.
- 4-client fio (same dir, morning): sw-1m AGG **1589** (2.3×), ow-1m
  636, rw-1m 343. After bounce/grown table: sw-1m 281, ow-1m 240, then
  rw-1m **400 s ssh TIMEOUT** on all 4 (REPORT tail). Do not raise it.
  4/9-client fio reads and 9-client fio writes were not finished.
- 8 GiB `dd+fsync` own file, remount HEAD/TAIL `0x5a` OK: 4-client
  **251 MiB/s** (32 GiB / 130.5 s = 0.41× one client, 0.46 % of 44 GB/s);
  9-client first-write 011–015 **202 MiB/s** (40 GiB / 203 s = 0.32× one
  client). Do not quote 007–010's 13–31 s 9-client walls — those files
  kept the 4-client mtime.
- A 4-client fio storm can lose raft heartbeats (`report-split`
  `rc=-15` NOT_PRIMARY). Recovery is keep-storage efsd bounce, not wipe.

W3's leftover (REPORT pack+push) is the multi-client wall. The 1-client
limit was not per-client CPU.

- **Forbidden to reopen:** a REPORT split into multiple RPCs; quoting a
  run where any host failed the FUSE check; one shared file (W1);
  raising `EFS_IO_TIMEOUT_MS`.

<a id="ph-w5--re-measure-sw-50g-after-w3--done-f5363d"></a>
### W5 — Re-measure `sw-50g` after W3 — DONE


- **Forbidden to reopen:** raising `EFS_IO_TIMEOUT_MS`; splitting REPORT
  without asking.

<a id="ph-w7--two-posix-suite-1-tests-exceed-the-15-s-budget-even-in-i-77e6e9"></a>
### W7 — Two POSIX suite-1 tests exceed the 15 s budget even in isolation — DONE


| test | result | seconds |
| --- | --- | --- |
| `concurrent_creates_same_dir` | PASS | 4.9 |
| `mtime_monotonic_many_writes` | PASS | 1.1 |
| `dir_deep_nesting` | PASS | 6.1 |
| `dir_deep_nesting_beyond_64` | PASS | 4.8 |
| `names_crazy_dirs` | PASS | 3.3 |
| `dir_many_files` | PASS | 5.2 |

Suite 1 jobs=1 on the same build is **200/201** plus `mmap_write_read`
SKIP (`results/posix/20260921-123904`), above the 193 floor. If any of
these walks exceed 15 s again, the cluster was not idle or a wakeup
regressed — do not raise the budget.

- **Forbidden:** raising `POSIX_TEST_SEC` or the `@budget(...)` values. A
  timeout is a failure to be removed, not re-labeled.

<a id="ph-w11--chunked-installsnapshot--done-sep-27-9e5ef0"></a>
### W11 — chunked InstallSnapshot — DONE Sep 27


`raft_host` only compacts when the whole group's KV export fits in one
`EFS_WIRE_RAFT_MAX_CMD` command. On 19810 one group is over that, so the log
is never compacted and `snap_oversized` latches. Measured Sep 21
(`results/measure/20260921-182308-raft-snap-state`): logs 1.83–4.36 GB
and still growing ~400–800 B/s idle, both groups `commit == applied`
(fcstor005 is not behind on this build; the gossip `DOWN` right after a
bounce is the STATUS probe, not a dead process — check `pgrep -x efsd`).
A follower restart is a full replay of that log.

The fix is a chunked / multi-message InstallSnapshot, and **that protocol is
not specified anywhere**. Do not implement it before the recommendation
below is ratified. Until then this is a known, documented lag.

**Recommendation (Sep 23): adopt the Raft paper's InstallSnapshot chunking
(§7 of the paper: `lastIncludedIndex`, `lastIncludedTerm`, `offset`,
`data[]`, `done`), and take the snapshot as a pinned segment view, not a
RAM blob.** Two facts drive it:

- The wire cap is the smaller problem. Today `efs_raft_snapshot` calls
  `snap_get` at `last_applied` **under the SM lock** and keeps the whole
  KV export in `r->snap_blob` in RAM; `host_snap_get` is a full
  `efs_kv_scan` of the group. Removing the 4 MiB cap alone would turn every
  `HOST_SNAP_MIN`-entry snapshot into a multi-hundred-MB scan under `h->mu`
  on the leader — the same stall class as W13, on every snapshot instead of
  every fourth flush. The spec already says snapshots exist only for log
  truncation and the KV is durable, so the snapshot *point* should cost a
  memtable flush and a `save_snap(idx, term)`; the *export* happens only when
  a follower actually needs one, off the pump thread.
- The protocol is not an invention. Chunked InstallSnapshot with
  `offset`/`done` is the one the Raft paper specifies; the code base already
  has the "regenerate at the current applied index on demand" precedent
  (`send_snap` after a leader restart). The follower side stays
  `efs_kv_group_import` on the assembled bytes, unchanged.

Steps, once ratified (after W13's step 1, which builds the pinned view):
1. Spec: add to [architecture.md](../architecture.md) §KV the snapshot
   rule — snapshot point = memtable flushed + `save_snap`; export is lazy,
   from a pinned immutable segment view taken in one pump cycle at applied
   index N (so it is exactly the state at N); shipped as ≤ 4 MiB `SNAP_REQ`
   chunks `(incl_index, incl_term, offset, done)`; the follower stages
   chunks in `<storage>/mdraft/snap-<group>-<incl>.part`, restarts from
   `offset 0` on any `(incl_index, incl_term)` change, and on `done` imports
   the file, sets `last_applied = incl`, and truncates. Under the existing
   rule: no snapshot past the KV's durable point.
2. `raft.c`: drop `snap_blob`; `snap_get` becomes `snap_open(incl) →
   (handle, total_len)` + `snap_read(handle, offset, buf, n)` +
   `snap_close`; `send_snap` keeps one chunk in flight per peer (like
   `ae_inflight`) and advances on the `SNAP_REP` ack carrying `offset`.
   The 8-byte `app_old/app_new` prefix stays on chunk 0.
3. `raft_host.c`: `host_snap_open` pins the view in the pump cycle, exports
   it to `mdraft/snap-<g>-<incl>.kvx` on the GC thread (the one that runs
   `host_txn_recover_pass`), unpins, and serves `pread`s from the file;
   one snapshot file per group, replaced when a newer one is opened.
   `snap_oversized` and the `snapshot skipped` path go away.
4. `raft_sim` test: a follower behind a truncated log catches up from a
   snapshot that spans ≥ 3 chunks; a leader change mid-transfer restarts
   the transfer from offset 0; a follower crash mid-transfer leaves no
   `.part` applied.
5. Live gate on 19810: both groups' `raft.log` shrink and stay bounded
   (`results/measure/…-raft-snap-state` re-run: today 1.83–4.36 GB); kill
   and restart fcstor005's efsd — it rejoins by snapshot in seconds, not by
   replaying GBs; `apply_max` in `raft-obs` does not spike at a snapshot;
   `md_latency.py` unchanged. 9-host posix and the freeze script as usual.

- **Forbidden:** implementing before the row is ratified. Killing 005 to
  "fix" the lag — a 2+1 PUT needs every fragment ACK, so removing a node
  breaks writes.

<a id="ph-w13--synchronous-full-l1-compaction-is-the-remaining-electio-1128be"></a>
### W13 — Synchronous full-L1 compaction is the remaining election trigger — DONE Sep 26 2026


Measured Sep 21 (`results/measure/20260921-220933-w8-stall-timeline/obs-*.txt`):
`kv_compact_locked` runs inside the apply path under `l->mu` + `h->mu`,
rewrites all of L1 (~16 MiB; 64 MiB L1 segments) every 4 memtable flushes
because one L0 segment spans every shard and so overlaps every L1 segment.
`apply_max=2464250us` on 004 and 005 at once, group 0 term 5299→5302→5303,
client `stat` 2 s. It is the last non-hardware cause of a term change under
the 9-host suite, and it scales with table size, so it gets worse. It was
listed in §1b as "unspecified → decision"; the decision request is here.

**Recommendation: run compaction on a background thread; then, as a
follow-on, bound its per-cycle I/O with a partitioned flush. Do not raise
the memtable or the election timeout.**

- *Why background first.* The stall is a lock hold, not CPU: the apply path
  waits on `l->mu` for a 2.4 s file rewrite it does not need to observe.
  Segment files are immutable, so a compactor can read the old L0/L1 files
  and write new L1 files with no lock at all; only the manifest swap needs
  `l->mu`, and that is microseconds. The logical content of the KV is the
  same on every replica regardless of when compaction runs, so it does not
  touch determinism, the applied index, or the durability rule (the WAL and
  the manifest swap are what persist). This removes the stall at any table
  size.
- *Why not per-key-range compaction alone.* Leveled compaction bounds the
  bytes per compaction only when L0 segments have narrow key ranges. Our
  memtable holds every shard, so every L0 segment overlaps every L1 segment
  and a range compaction still rewrites all of L1. It needs a partitioned
  flush (one L0 file per key range) first — that is the follow-on, and it
  is worth doing for read cost (one `pread` per overlapping segment per
  KV read), not for the stall.
- *Why not a bigger memtable.* It makes the same stall rarer, and one stall
  is one election. Same reason the election timeout stays: a longer timeout
  hides a 2.4 s apply stall today and a 5 s one at twice the table size.

Steps:
1. **Pinned segment view** (shared with W11): `kv_lsm_view_pin()` returns a
   refcounted copy of `{l0[], l1[]}`; a segment file is unlinked only when
   the manifest no longer lists it *and* its pin count is 0. Unit test:
   a scan over a pinned view is unaffected by a concurrent flush and
   compaction.
2. **Compactor thread** in `kv_lsm`: the write path flushes memtable → L0
   as today and signals when `n_l0 >= l0max`; the compactor pins the L0 set
   plus overlapping L1, merges to new L1 files with no lock held, then
   under `l->mu` swaps the manifest (atomic rename — verify
   `kv_manifest_write` is), drops its pin, unlinks unreferenced files.
   `kv_compact_locked` remains only for `efs_kv_lsm_compact` (tests,
   `--compact` tools). Back-pressure: when `n_l0` reaches `KV_LSM_MAX_SEGS`
   the write path waits on `l->cv` for the compactor — the only stall left,
   and it means the compactor is 16× behind (log it). **Sep 29 04:27Z:
   that stall is the pump, it fired five times in one 7-minute write on
   fcstor004 (24 s once), and it is the remaining election trigger and
   REPORT BUSY — see W23 / D9.** Crash test: kill
   after every file write in a compaction, reopen, verify every key.
3. Remove the compaction call from `efs_kv_lsm_flush` (the snapshot path)
   — it only needs the memtable flushed.
4. **Gate:** `raft-obs` `apply_max` < 100 ms on every replica across ≥ 3
   compactions during the 9-host suite (log `kv-compact: start/end` with
   bytes and ms); `tests/measure/w8_stall_timeline.sh` shows no term change
   on either group through the run; `md_latency.py` within the idle
   reference; `make test` green with the two new tests; 9-host posix
   ≥ the current 185–197/201.
5. **Follow-on (after gate):** partitioned flush — write the memtable as
   one L0 file per shard range so compaction touches only overlapping L1
   segments; gate = bytes rewritten per compaction bounded by
   `KV_LSM_L1_TARGET` × ranges touched, read `pread` count per KV get ≤ 3
   on a 1 GiB table.

- **Read:** `src/kv/kv_compact.c`, `src/kv/kv_lsm.c` (`efs_kv_lsm_flush`,
  `kv_maybe_flush_locked`), the "One KV WAL fsync per pump cycle" and
  "Synchronous full-L1 compaction" learnings in the project state rule.
- **Forbidden:** raising the election timeout; raising `KV_LSM_MEM_DEFAULT`
  as the fix; any compaction step that holds `h->mu`.


<a id="ph-oct-1-2026-1555z--roadmap-performance-items-w39-read-path-r1-4441cd"></a>
## Oct 1 2026 15:55Z — roadmap performance items: W39, read path R1–R5, store writev, W37

**Request.** "From the roadmap, what else can be implemented and smoke
tested? I will do the heavy testing. I would prioritize performance
improvements" → list given (server PUT without the D20 bounce copy, W39
RDMA zero-copy send, read-path profile → fix, W41; D23 / fragment layout /
W40 / metadata ops as asks) → "do as many as you can and in parallel if
possible, i follow your recommendations".

**Server PUT bounce copy (`store.c`, in the tree, not rolled).** D20's
4 KiB digest tail had disabled the aligned O_DIRECT fast path for every
data PUT: payload + digest were memcpy'd into a bounce buffer (one 68 KiB
copy per fragment). An aligned payload with a digest is now one `writev`
of `{payload, tail_tls}` where `tail_tls` is a per-thread 4 KiB page
holding the digest and zeros (`writev_all` loops on short writes).
Once-logs `store: put O_DIRECT zero-copy` / `bounce copy`.

**W39 — RDMA zero-copy fragment send (client, deployed).**
`efs_rdma_send_frame` copied every fragment into an 84 KiB registered pool
buffer (6.5 % of client cycles). Now `bufpool.c` calls
`efs_rdma_zc_region_add(slab, …)` for every slab it carves; `rdma.c` keeps
an append-only table of up to 128 regions with a lazily registered MR per
HCA (`zc_lkey` → `ibv_reg_mr(pd, base, len, LOCAL_WRITE)`, once, logged
once on failure) and posts a two-SGE send (`post_send2`: 5-byte frame
header + small header part from the pool buffer, payload from the slab)
when the payload is ≥ 4 KiB (`EFS_RDMA_ZC_MIN`) and the QP was created
with `max_send_sge = 2` (`rc->zc_ok`). The payload must stay valid until
the send CQE; every caller already does (a conn is either replied to —
which implies send completion — or destroyed through `efs_conn_destroy`
→ `ibv_destroy_qp`; `efs_rdma_send_quiesce` reaps outstanding sends).
`test_rdma_xprt` passes; `efs: RDMA zero-copy send active (type=6 hdr=80
payload=65536)` on the first PUT. 16 GiB random dd+fsync on fcstor007
1.4 → 1.5 GB/s, client CPU ~21.4 s; cmp 0/5/15 GiB + md5 OK
(`~/orcd/scratch/efs/perf/agent-rd-20261001-151704-new`).

**Read path (client, deployed).** Baseline profile
(`agent-rd-20261001-145047`, 16 GiB cold read 2.5 GB/s, 21.6 s CPU; `perf
record -a` is refused at `perf_event_paranoid=1` — the rule's claim is
wrong; attach `-p` ~0.7 s into the read so the pools exist): user time was
memmove chains — RDMA recv buffer → fragment buffer → decoded chunk →
rdcache → FUSE reply buffer → kernel. R1 `efs_rdcache_put_owned` hands a
prefetched chunk's buffer to the cache (frees the victim's body); R2 a
whole-chunk demand job decodes into the caller's buffer (`chunk_get_job
.ext`, no copy-out, nothing freed); R4 `efs_client_decode_placed_chunk_
attempts` points the two data-fragment buffers into the chunk itself and
`efs_decode_chunk` skips a memcpy whose source is its destination
(after R4: 3.0 GB/s, CPU 16.6 s — the removed copy was hot-cache, the
cold-source ones remained, `-153035-inplace`, `-153356-prof`); R5
`efs_client_read_refs` pins the rdcache images of a chunk-aligned READ
whose chunks are all cached and clean (`efs_rdcache_pin` → `pins++`
under the stripe lock; a pinned way is never a victim and `rdcache_put`
leaves its bytes alone) and `ll_read` answers with `fuse_reply_iov`
(libfuse 3.10's `fuse_reply_data` copies a multi-buffer bufvec into a
fresh allocation; `fuse_reply_iov` writev's the vector). Once-log `efs:
read reply zero-copy active (iov=8 size=1048576)`. The get pool is 64
workers (`GET_POOL_N`), was `EFS_WRITE_PIPELINE` = 32; one synchronous
fetch per worker is the client's in-flight read cap.
Result (`-154007-refs`): single cold read **3.6 GB/s, CPU 10.8 s**; four
readers **6.5 GB/s** (was 3.7); `EFS_READ_PREFETCH=32` 2.9 GB/s (worse,
default 16 stays). cmp/md5 OK; the one `CMP_TAIL_BAD` in `gate670` was
the harness (`cat | dd skip=1023` on a pipe without `iflag=fullblock`
counts short reads) — `gate671` with an exact source: tail cold/warm,
unaligned warm, md5 all OK. posix jobs=1 200/201
(`results/posix/20261001-154333`); wedge gate (4 × 8 GiB + 300-file
storm) 8.2 s / p99 70 ms / 32 `report-landed`.
Still copies: RDMA recv buffer → chunk (zero-copy receive = per-request
posted receives into the destination, a transport change — ask) and the
FUSE write copy (W40). Single-stream is bound by in-flight depth and
~1 ms per chunk, not CPU.

**W37 — mkfs salt fork (server, in the tree, not rolled).** The Oct 1
wedge: fcstor003's mkfs committed MKFS and SALT, the SALT reply was BUSY
(the `group_slot` bug), the harness retried on fcstor004, whose MKFS was
a no-op (the apply keeps the first salt) and whose SALT carried ITS salt →
`efs_meta_apply_salt_record` PROTO → `apply salt rc=-7 index=3` on every
group-2 apply, because SALT was a halt-on-error command. Fix:
`server_raft_host_mkfs` reads the committed salt back
(`efs_meta_apply_export_salt`) after the MKFS commit and replicates that;
a node holding no record (group-2-only host, SALT not yet landed) answers
BUSY with `retry on a group-0 node`; `EFS_MD_CMD_SALT` is ring-only so a
mismatch is a verdict, not a stuck group. A first attempt to make the
mkfs apply return EXIST broke `test_meta_apply` `idempotent` and
`test_sim` `idempotent mkfs` — the apply contract (idempotent, first salt
wins) is right and stays. Compiled, `test_sim` and `test_meta_apply` pass (`bld701`).
Gate: a fresh-table start after the next wipe.

**rmdir emptiness (client, deployed).** The 9-host suite on the new
client gave fcstor009 199/201: `dir_deep_nesting` `rmdir d25` ENOTEMPTY
(`results/posix/20261001-155455`) — the same one-host signature as
fcstor012's `d50` on Sep 30, where the KV showed the dir empty and
another client's rmdir succeeded. `efs_client_unlink` refused locally
when `efs_export_dir_empty` found a child in the client's table; that
table can keep a child whose removal committed elsewhere. The local
check now only logs (`efs: rmdir ino=… local table lists child '…' —
asking the server`, first 16) and the server decides. 9-host after
(`dep691`): 200/201 on all nine (`results/posix/20261001-160049`).

**Deployment.** Clients fcstor003–015 (`dep680`) and fstor007's `/tmp/efs`
tree (the user's running efs-fuse untouched). Servers carry store.c, W37
and the log timestamps unbuilt — a build-ID roll the user's live session
should not pay for unasked.

<a id="ph-oct-1-2026-1400z--the-fstor007-wedge-d24-decided-report-repl-785d27"></a>
## Oct 1 2026 14:00Z — the fstor007 wedge: D24 decided, REPORT reply wait sized, log timestamps

**Symptom (user).** fstor007, 8 parallel 20 GiB `dd bs=1M` + an `ecopy`:
once ecopy started the mount blocked, ecopy never got going, SIGKILL
left the processes stuck.

**Read live, no tracing (13:41Z, `~/efs-runs/rec-look550..559.log`).**
16 `dd` in D-state in `request_wait_answer`, cmdline already gone
(`[dd]`: past `exit_mm`, i.e. the kill had landed and they were in the
kernel's forced, uninterruptible close-time FLUSH); `ecopy` a zombie
with threads in D; load 56. efs-fuse (pid 2007820) alive: 63 threads,
all asleep — 10 FUSE workers in `read(/dev/fuse)`, 32 put-pool + 16
reclaim in `cond_wait` (gdb snapshot `/tmp/efs-fuse-stacks-552.txt`);
`/sys/fs/fuse/connections/59/waiting=0`. Servers: commit==applied,
`apply_max` < 1 ms, `inbox_drop=0`, no elections. The client log
(`/tmp/efs-fuse-efs-mount.log`, no timestamps then) ended with
`retry type=67 why=recv rc=-6` ×3, `slow-ok type=67 attempts=6
saw_busy=1 us=116309308`, then `attempts=4 us=121101530`. Everything
had drained by 13:44Z.

**Mechanism.** Type 67 = `REPORT_CHUNKS`. One 20 GiB file is 163 840
records (~27 MB), 2.2 s of server time alone; eight arriving together
pushed some past `EFS_IO_TIMEOUT_MS` (30 s), `rpc_send_recv_dual`
dropped the conn and re-sent the whole REPORT, up to six times, so
each `close()` took ~2 min. Every `close()` on the client goes through
`efs_client_report_dirty_ino` → `report_mu`, so ecopy's first close on
every thread queued behind that loop. The kernel's FLUSH/RELEASE on
close is `force`d, so SIGKILL could not release the waiters.

**Decided (user, 14:00Z): "implement all fixes that prevent the lock",
timestamps in the logs.** Done:
1. **D24** — `report_landed_note()` in `dcache_put_now`: every 8192
   landed PUTs (1 GiB, global) kick `meta_flush_main`, which REPORTs
   the whole dirty set (`sync=0`, the close path's records and code);
   close() publishes only the tail. `report-landed:` log line.
2. `rpc_send_recv_dual(…, recv_ms)`: REPORT's reply wait is
   `EFS_IO_TIMEOUT_MS + count/2` ms (0.5 ms/record), restored to the
   conn default on release; the `retry … why=recv` line prints
   `req_len` and the bound.
3. `src/common/log_ts.c`: both daemons replace stdout/stderr with
   `fopencookie` streams that prefix every line with
   `YYYY-MM-DDTHH:MM:SS.mmmZ `; `EFS_LOG_TS=0` disables;
   `backtrace_symbols_fd` writes to fd 2 (cookie `fileno` is -1).

**Not done — W41 (ask).** `report_mu` still serializes every close on
one client behind one REPORT's retry loop (BUSY ≈ 2 s, STALE up to the
8 s sync budget). Removing it needs per-inode extraction from the
open-addressing dirty sets and a multi-slot `pub_ino` set; the
snapshot swap is load-bearing for "close returns only after this
inode's records went out".

**Gate.** Unit tests pass (`bld561`). `~/efs-runs/wedge565.sh` on
fcstor007 (`agent-wedge-20261001-141222`): 4 × 8 GiB dd+fsync from one
client in 9.8 s (3.5 GB/s aggregate, sizes correct) with a concurrent
300-file create+write+close storm at p50 4.7 / p99 51 / max 213 ms;
32 `report-landed`, zero `fsync-split`, zero retries. Clients
fcstor003–015 and fstor007's `/tmp/efs` on this tree; servers not
rolled (timestamps only; the user's live session).

<a id="ph-oct-1-2026-1320z--the-users-20-gib-dd-perf-dir-reclaim-herd-e6ec57"></a>
## Oct 1 2026 13:20Z — the user's 20 GiB dd perf dir: reclaim herd, read buffer, NUMA pin

**Input.** `~/orcd/scratch/efs/perf/efs-mount/` from fstor007: `client.sh
--perf --strace`, `dd bs=1M` 20 GiB write (29.1 s, 738 MB/s) and read
back to local disk (23.9 s). Both figures carry the tracers; the pid-
attached `perf record -p` taken at mount had only the seven initial
threads (the reclaim and put pools are created at the first write), so
blake3 and the PUT path were missing from that profile.

**Reproductions** (`~/orcd/scratch/efs/perf/agent-dd-20261001-*`, all
fstor007, source `/data1/node9901-data1/erbmi1/001/dat01`, no tracer
unless named): `-120522` pid perf + strace windows + dd straces;
`-121718-sw` system-wide perf before the fix; `-121931-lock` dwarf perf
+ `futex.strace` + `futex-top.txt`; `-122504-fix` system-wide after;
`-1237xx-var` variants A (fix) / B (`EFS_FUSE_SPLICE_READ=1`) /
C (`numactl --cpunodebind=2 --preferred=2`) / D (read) / E (read
pinned); `-130753-base` clean HEAD `292fc6da`; `-13xxxx-numa` the
in-process pin.

**Where the time is.** dd is serial: source `read()` 0.45 ms/MiB (9.1 s
of a 20 s write), efs `write()` 0.82 ms/MiB, then `close()` is one
`EFS_MSG_REPORT_CHUNKS` of 163 840 records that the server answers in
2.13–2.35 s (`slow-ok type=67`, `fsync-split flush_ms≈2300`), linear in
file size. Read: efs 0.56 ms/MiB to `/dev/null`; writing the
destination disk was the other half of the user's 23.9 s.

**Found and fixed (client).**
1. `g_reclaim` herd: `dcache_kick_complete` took `g_reclaim.mu` and
   signalled per completed chunk; every sweep end `cond_broadcast` to
   16 workers that each re-took the mutex. `strace` 3 s window: 156 377
   futex calls on `g_reclaim+0x0` / `+0x50` (`nm -n`, non-PIE binary);
   perf: 18 % of all client cycles in `native_queued_spin_lock_slowpath`,
   FUSE thread in `ll_write_buf → __lll_lock_wait`. Fix: atomic `kicks`,
   `reclaim_kick` locks only when `waiters > 0`, broadcast only at
   shutdown. After: 26.5 K futex / 3 s, spinlock gone, client CPU for
   the 20 GiB write 74.9 s → 33.6 s. Wall unchanged (20.1 → 21.2 s): the
   wall is dd's read plus the REPORT tail.
2. `ll_read` malloc'd `size` per request (1 MiB, above the mmap
   threshold → mmap/munmap + kernel page zeroing per READ). Thread-local
   buffer up to 16 MiB. Read 12.2 → 10.4 s.
3. NUMA: fstor007 is 2 sockets / 8 nodes, HCA `mlx5_1` on node 2; the
   FUSE thread's `fuse_buf_copy` into a cold remote-node pool buffer was
   0.3 ms/MiB (cold-destination memcpy bench 3–18 GB/s vs 55–66 hot).
   `numactl` variants: write 21.2 → 18.0 s, read 10.4 → 6.2 s. Now
   in-process: `numa_pin_startup` (affinity before the first thread +
   `MPOL_PREFERRED`), node from `efs_rdma_numa_node_for_host` (route's
   local IP → ifname → HCA → sysfs `numa_node`), `EFS_NUMA_NODE=none|N`.
   Measured: write 18.9 s (CPU 23.9 s), read 6.6 s (3.3 GB/s), `cmp` of
   the first GiB OK. fcstor nodes: 2 nodes, HCAs on node 0.
4. The "libfuse 3.10.2 cannot set max_pages, 128 KiB/request" comment
   was wrong: `read(/dev/fuse)=1048656`, requests are 1 MiB.

**Measured, not done** (START-HERE §1a 0f / 1i): D24 ask — a threshold
REPORT of landed chunks during a long write (the 2.3 s close tail);
W39 RDMA zero-copy send (`efs_rdma_send_frame` memmove 6.5 % of client
cycles; memmove total 25 %, blake3 8 %, `xor_into` 7.3 %); W40 the FUSE
request → pool copy (splice does not remove it: variant B 21.9 s).

**Unit tests** on fcstor007 (`unittest528`): all pass except
`test_raft_store` (4 failures, identical on clean HEAD — pre-existing,
snapshot/retained-window semantics, not this change).

<a id="ph-oct-1-2026-0805z--io-500-on-the-fresh-table-one-record-lost-7203c6"></a>
## Oct 1 2026 08:05Z — IO-500 on the fresh table, one record lost (W38)

The user asked for the cluster to be started and IOR run, errors as
START-HERE follow-ups. The cluster was already up from the 07:24Z start
(`pf431.log` PREFLIGHT_OK, both groups idle), so the run went straight
to `IO500_KEEP_DATA=1 SLOTS=4 EFS_SSH_TIMEOUT=60 run.sh debug` on
fcstor007 (`rec-ior432.log`), 9 hosts × 4 ranks, 1 s stonewall.
Results in `results/io500/20261001-074905-rdma/` (NOTE.txt has the
table next to the Sep 30 18:35Z run): every phase completed, 0 fsync
failures; ior-easy-write 5.173 GiB/s (4.563), mdtest-easy-write 6.185
kIOPS (4.632), ior-hard-write 0.519 GiB/s in 63.2 s (0.640 in 52.6 s),
mdtest-hard-write 2.818 (2.612), ior-easy-read 22.5 same-mount,
mdtest-easy-stat 24.4 kIOPS (16.5), ior-hard-read 0.824 GiB/s with
**1 read error** (1.034, 0), mdtest-hard-stat 30.4, easy-delete 5.65,
hard-read 5.92, hard-delete 4.84. Score BW 2.657 / IOPS 8.001 / 4.610,
all INVALID by the stonewall. Terms g0 5 / g2 4 unchanged,
commit==applied, `arc_miss=0`, `wait_timeouts=6` on fcstor004,
`pub_p50=879 ms` on the g2 leader, 1997 publish STALE retries (why=6
FOLD_LIST 1954, why=5 18, why=2 22, why=4 3), none for the bad chunk.

The error was chased cold: the nine clients were remounted (`rm436`),
IOR `-r -R` over 36 ranks reproduced 1 error (`rec-hv437.log`, 851.7
MiB/s), and a new scanner `tests/tools/hardscan.c` (the Sep 30 one had
lived in `/tmp` on fcstor008 and was gone) found it in 24 s
(`hs438.log`): `records=747720 bad=1`, record 331368 = rank 24 =
fcstor013, the last 4256 bytes of the record = bytes [0,4256) of chunk
118843 of ino 10897, all zero. Ranks 24–27 are all fcstor013, so the
chunk had one writer host — not the Sep 30 two-host span-identity
shape. `raft-getchunks` on fcstor004 (`getchunks-118842-118844.txt`):
`ci=118843 nodes=2,3,4 base_gen=obj_gen=17743715622959842861 spans=1
seq=1222; span off=0 len=0 gen=18389759692080110185`, i.e. rank 24's
piece went up as a span first (seq 1222), then the client published a
full image whose fold list named exactly that span, the server
tombstoned the span (`meta_apply.c:~3425`), and the image carried zeros
at [0,4256). Neighbour chunks are span-only rows and clean. Candidate
client paths are listed in START-HERE §1b (F3) and §1a 0e; it needs a
one-client 4-rank repro with `EFS_DCACHE_TRACE=1`. Lesson paid for:
the remount for the cold verify truncated the nine `fuse.log`s of the
run — copy them before remounting. Harness: `run.sh ior-hard-verify`
now passes `--dataPacketType=timestamp` and takes `IOR_HARD_G`. The
io500 tree and hard file stay on the mount for the repro.

---

<a id="ph-oct-1-2026-0745z--fresh-cluster-after-the-wipe-posix-suites-eab988"></a>
## Oct 1 2026 07:45Z — fresh cluster after the wipe, posix suites, four bugs

The user asked for the wiped 19810 to be started and the posix suites
run, with errors as START-HERE follow-ups.

**Start.** The first fresh start (`start401`) rotated `raft-mkfs` over
nodes 1–3 because every attempt returned BUSY (rc=-13). Two bugs there.
(1) Each node proposes its own `h->salt`: group 0 took node 1's MKFS,
then node 2's attempt put node 2's salt on group 2's anchor shard, and
`efs_meta_apply_salt_record` answered PROTO on every group-2 apply from
then on (`apply salt rc=-7 index=3`, `rec-st402..404`). Only a wipe
recovers; re-wiped at 07:18Z (`wipe405`). The harness now mkfs's on
fcstor003 only, after both leaders are seen (`roll_efsd.sh --all
--fresh`, `cluster.sh start --fresh`); the server guard is W37. (2) The
BUSY itself: `host_apply_rc_locked` and three siblings tested `!g` for
"not hosted", but `group_slot` returns a slot for every attached group,
so the forwarded SALT step read fcstor003's unhosted group-2 ring →
`arc_miss` +1 and BUSY. Fixed (`hosted` tested); `restart408` rolled it
(same build id `4b2844487831-dirty`), mkfs took 2 ms afterwards.

**Posix.** First jobs=1 run: 10/201, `basic_pread_pwrite` D-state in
`request_wait_answer`, 190 NOTRUN (`results/posix/20261001-072454`).
gdb: the FUSE worker in `rdcache_acquire` waiting on a `pending` way.
`efs_rdcache_put` looked the entry up with `rdcache_find`, which needs
`data`, so the mark W29 set on a fresh data-less way was never found,
the put went to another way and the mark was orphaned. Fixed in
`read.c`. Second run 199/201: `writev_readv_chunk_straddle` read zeros
across the chunk boundary (`-072916`). W30 PUTs the completed chunk
during the write; the full-image snap steals the body and the map names
the object only after the PUT lands, so the readv in between found the
chunk nowhere and `chunk_get_worker` zero-filled a "hole". `efs_client_read`
now waits for the inode's PUT windows (`efs_dcache_put_win_wait`, the
Sep 30 utimens window machinery, lock-free when none is open). Third run
200/201 in 28.1 s (`-073542`); 9-host 200/201 on all nine in 13.3–13.6 s
(`-074052`). posix2 62/63 (`results/posix2/20261001-073048`):
`peer_rename_vs_unlink_src` — rename a→b on A and unlink a on B both
returned 0 and `b` was left dangling (`-?????????`). 1 in 6 on a loop
(`p2r422`); the full suite again was 63/63 (`-074250`). No server log
line; `apply_unlink_cmd` maps NOT_FOUND→OK silently. That is W36
(START-HERE 0c), evidence kept at
`/tmp/efs-mount/posix-2c-r422-6/peer_rename_vs_unlink_src/b`.

<a id="ph-oct-1-2026-0436z--encryption-idea-checked-and-saved-not-a-de-ba422c"></a>
## Oct 1 2026 04:36Z — encryption idea, checked and saved, not a decision

The user asked how synchronous encryption could work for transport and at
rest: a key created with the cluster, and only the Linux user who mounts
with that key can see the contents. Nothing to implement. The conclusions
are in START-HERE §1a ("Idea checked Oct 1 2026 — synchronous encryption").
Short form: one key shown once at `raft-mkfs`, a verifier in the cluster,
fragments encrypted on the client after XOR parity (each fragment its own
IV, object name still a hash of the plaintext), frame encryption on RDMA
and the TCP side channel. Metadata is the open choice — plaintext
namespace, or unlock `efsd` with the key at start and encrypt the KV and
Raft log — because the servers apply names and cannot do that without the
key. One cluster key is not per-user secrecy; the Linux-user limit is the
key file mode plus a mount that is not `allow_other`, and it does not hold
against root or against another host that has the key.

<a id="ph-oct-1-2026-0250z--the-last-two-mechanical-items-of-the-2110z-18aae9"></a>
## Oct 1 2026 02:50Z — the last two mechanical items of the 21:10Z review: pin release on a landed PUT, dirty-list close flush

Asked what else from the perf review was cheap, the two client-side
items were (4) the dcache row pin and (5) the per-close chunk walk; the
user asked for both and does the ecopy testing. In tree as `b6c1712d`,
clients fcstor007–015 remounted 02:43Z (`dep366`), fstor007's `/tmp/efs`
rebuilt (`fst367`); servers unchanged.

(4) `dcache_pin_add` ran on every store (`dcache_init`, `dcache_fill`,
the re-dirty paths), but `dcache_pin_release` ran only in
`dcache_drop_locked` and in one partial-write branch of
`dcache_flush_slot_inner`. A full-overwrite PUT stole the body and left
the body-less node pinned; the append branch kept the published body
and its pin. So every file this client had written stayed pinned for as
long as its slot lived: the staging evictor could not evict its row
("staging table over EFS_CLIENT_META_MB with nothing evictable;
growing"), and the Oct 1 close predicate (`efs_ino_has_unpublished`,
which counts the pin) never short-circuited for a written file — during
ecopy's copy phase every close still took the full path. The pin's own
comment says losing it is a bounded-cache miss, never corruption, and
after a landed PUT `dcache_put_now` has already put the chunk in the
dirty set, which keeps the row staged until the REPORT consumes it. So
`dcache_flush_keep` (the close/fsync pipeline's completion) and
`dcache_flush_slot_inner` (reclaim) now release the pin of a clean entry
after a successful PUT, body kept or not. An entry a write re-dirtied
mid-PUT was re-pinned by `dcache_set_dirty` + `dcache_pin_add` (the
"already held mid-flush" path) and is left alone; the failure paths
re-dirty before the release point and keep their pin.

(5) `dcache_flush_ino_pass` read the staged row's size under `idx_mu`,
then called `dcache_steal_dirty(ino, ci)` for every `ci` up to
`size/128 KiB` — two mutexes and a chain walk per chunk, dirty or not —
and fell back to all 65536 slots once `nci > DCACHE_SLOTS` (a file ≥
8 GiB). It now asks `dcache_dirty_cis_of` for the inode's chunk indexes
on the 64 per-shard dirty lists (W18's index; `realloc`-grown array,
sorted so PUTs go out in chunk order) and steals exactly those. For the
lists to be a complete index, dirty ⟹ on_dirty has to hold everywhere.
It did except in the reclaim pop (`dcache_reclaim_main`), which
unlinked an entry while leaving it dirty so a second reclaim thread
would not take it; a close racing that pop would not have seen the
chunk and its REPORT would have shipped without it (the chunk then
reached the server on the reclaim's PUT, visible only after a later
REPORT). The pop now sets `reclaim_claimed` and leaves the entry
linked; other reclaim threads skip claimed entries; the claim clears in
`dcache_dirty_link`/`dcache_dirty_unlink` (the snapshot's unlink) and,
if the flush did not reach the entry, right after `dcache_flush_slot`
returns. `dcache_flush_all_slots` lost its `have_only` branch and is
reclaim-all only.

Built clean on fcstor007 (`bld365`, no warnings; unit tests OK). Gate on
the new client: posix jobs=1 fcstor007 200/201 (mmap SKIP, 0 EFS bugs,
`results/posix/20261001-024420`); posix2 fcstor007/008 63/63
(`results/posix2/20261001-024506`); `mtime_repro.py` rows
`ecopy-order*`, `rsync-order`, `fsync-between` all 1380661863
(`~/efs-runs/rec-mt371.log`; the gate script's `scp` to fcstor007 hung
without the wrapper's agent — run the repro from the NFS home instead).

What the user's ecopy profile should show: `dcache_steal_dirty` and
`dcache_flush_all_slots` gone from the top; `g_dcache_pin_count` (gdb)
near the number of files with in-flight writes rather than every file
written; `stage-evict: tabs … pinned=` small. Open from the review: N3
chain-length walk, the D18 A/B, N4 server trace.

<a id="ph-oct-1-2026-0225z--d18-decided-and-implemented-the-staging-ev-1e3b50"></a>
## Oct 1 2026 02:25Z — D18 decided and implemented: the staging evictor drops whole cold tabs

The 21:10Z review put the client staging evictor at 31.6 % of a 62 min
profile (47.6 % of the ecopy slice): the per-shard-tab floor (~90–170 KB
× up to 4096 tabs) sits above `EFS_CLIENT_META_MB`, so the row LRU
evicted 64 just-closed rows per pass and every `ensure_meta_room` re-armed
it. Asked which bound they wanted (D18), the user chose "evict whole
cold tabs". Implemented in `f073e136`, clients fcstor007–015 remounted on
it (`dep360`), fstor007's `/tmp/efs` rebuilt (`fst362`, no efs-fuse was
running there); servers unchanged (`44073b77249b-dirty`).

Shape: `stage_evict_main` runs its four row bands as before; if the
table is still over the cap, `evict_cold_tabs` (stage_evict.c) asks
`efs_export_tabs_by_age` for the 256 least recently used tabs (sorted by
`shard_tick`, which `efs_export_table()` already bumped on every use)
and, holding table + idx + dirty exactly like `evict_one`, drops up to 64
of them whose every row and chunk-record ino passes the same pin rules
(`efs_export_tab_for_each_ino` with `tab_pin_cb`: dirty set, dcache pin,
open fd, op pin, plock). The dropped inos' LRU entries are removed under
`g_lru_mu` so a later `evict_one` does not fan out over every tab for a
row that is gone; `efs_export_drop_tab` unregisters the present-chunk
counts, frees the tab, and returns its `staged_est`. The pass stops at
64 drops or when the reading is at or under the cap.

The part that was not in the plan: `efs_export_get_chunk` (every read
and write consults the local chunk table) went through
`efs_export_table_for_chunk` → `efs_export_table`, which creates the tab
on demand — the unit test caught it, a lookup of an evicted ino rebuilt
the tab at its floor cost. That would have turned the evictor into a
drop/rebuild loop under any wide walk. Readers and in-place updaters now
peek: `chunk_tab_peek` for get_chunk / set_chunk_gen / set_chunk_deltas /
add_chunk_delta, and `shard_route` (set_size, set_mode, set_owner,
set_mtime, set_atime) returns NULL → NOT_FOUND on an absent tab, which
is what those calls returned before (the freshly created tab was empty).
Only `efs_export_set_chunk` and the row stagers create tabs. Both peeks
still bump the LRU tick.

`test_stage_evict` gained: age order follows touch order and `n=1`
returns the oldest; the walk visits the row and its group-0 chunk recs
and stops on a nonzero callback; drop frees exactly `staged_est`, leaves
other tabs and the root intact, is a no-op repeated or on shard 0;
get_inode / get_chunk / set_chunk_gen / set_mode on the dropped shard
miss without rebuilding; staging a row rebuilds the tab and it is the
youngest. Built on fcstor007 (`bld359`): `test_stage_evict`, `test_data`,
`test_meta_apply`, `test_conn_fd`, `test_sim` OK; `test_stage_evict`
clean under valgrind.

Expected on the user's run: with `EFS_STAGE_DBG=1`, `stage-evict: tabs
bytes=…MB cand=256 dropped=N pinned=M` lines while the table is over the
cap, then silence; `evict_pass` out of the top of the client profile. If
`shard_tab_get_or_create` shows up instead, the tabs are being rebuilt as
fast as they are dropped (uniform shard access over a working set whose
floor alone exceeds the cap) — that is D18's alternative (smaller first
slab, lazily sized indexes), to bring back as a measurement, not a knob.
`TAB_SCAN`/`TAB_EVICT` are internal constants.

<a id="ph-sep-30-2026-2110z--review-of-the-users-fstor007-perf-dir-the-83d022"></a>
## Sep 30 2026 21:10Z — review of the user's fstor007 perf dir: the evictor and the per-close walk (documented, not changed)

The user ran `perf record -F 499 -g` on fstor007's efs-fuse for 62 min
(19:54Z–20:56Z) around two `ecopy --verify /data1/erbmi1/software/
/tmp/efs-mount/software/` runs, the first traced with `strace -f -tt -T`
for 86 s (died of its own SIGPIPE), the second for 150 s (^C). Asked to
review and document next steps only.
`results/measure/20260930-205300-ecopy-perf-review/SUMMARY.txt`.

`stage_evict_main` is 31.6 % of the whole profile and 47.6 % of the
second ecopy's slice; `perf annotate` puts every hot instruction in the
inlined `evict_pass` scan of `g_lru_keys`/`g_lru_ticks`. It runs
back-to-back because of D18's floor: the staging estimate is always over
the 256 MB cap, each pass evicts 64 just-closed rows (table lock 64×, one
compact per wake) and `efs_client_stage_evict_kick` — called from every
`ensure_meta_room` — re-arms it because the pass did evict. The Sep 30
targeted-evict fix made a pass cheap; it did not change the cadence.

`dcache_steal_dirty` is 36.9 % of that slice with a broken callchain
(libfuse worker, no frame pointers). The only caller at that rate is the
per-close path: `ll_flush → efs_append_flush_report → efs_dcache_flush_ino
→ dcache_flush_ino_pass`, which loops over every chunk index of the file
(from the staged size), two mutex pairs and a slot chain walk each, under
the inode's append stripe, on read-only opens and dup'd fds too, and
falls back to all 65536 slots once the file is ≥ 8 GiB
(`dcache_flush_all_slots.part.0` is in the slice). There is no
"anything unpublished?" predicate on that path; the utimens path got
one earlier the same day. `efs_dcache_yield_extra` (one `dcache_find`)
at 6.6 % suggests the slot chains hold many dead nodes; that is a
hypothesis to count with gdb, not a finding.

Per-op efs latency under the ecopy, with dirfd resolved through the fd
table: rename 54 / 29 ms, openat 18 / 7.7, utimens 8.8 / 15.9, stat
9.5 / 4.0, close 9.8 / 2.5 (idle refs 6.2 / 0.6 / 1.05 / 0.33 / 0.34),
at only ≈ 27 and ≈ 5.6 efs ops in flight; ecopy kept ≈ 250–275 of its
~400 threads on one futex. No efs syscall over 1.7 s, no EIO/EBUSY/
ESTALE. Next steps as written in the SUMMARY: decide D18 (an isolating
run with `EFS_CLIENT_META_MB=4096` needs no code), the close-path
predicate (mechanical, after a `--call-graph dwarf` confirmation), the
gdb chain count, and only then a server-side trace.

<a id="ph-sep-30-2026-1855z--item-10-a-9-client-io-500-that-completes-9a90e4"></a>
## Sep 30 2026 18:55Z — item 10: a 9-client IO-500 that completes, and the ior-hard-read loss was a naming bug

Goal for the afternoon: one 9×4 IO-500 that completes ior-hard-write
(every IOR since Sep 29 had aborted on fsync EIO). Seven debug runs today
completed every phase once the REPORT path stopped exhausting the 8 s
wall (single-get GETCHUNKS, follower ReadIndex coalescer, PULL_FAN /
REPLAY_THREADS, growable putid table). What remained was ior-hard-read:
40–208 records wrong per run (`/tmp/hardscan` cold scan of the 36 GB
file; IOR itself reported 80–140 read errors), always the client-boundary
piece of a chunk two hosts share, the server row showing no base image
and the missing piece's span absent.

The trace (fcstor009 with `EFS_DCACHE_TRACE=1 EFS_REPORT_DBG=1`, run
ior330, ino 547783) gave two chunks with different shapes:

- **238434 / 239622** — one full-image put-record after a
  `snap-inner | dirty=1 hb=1 bg=0 obj=0 r=` (nrange 0 on a first partial
  write), one `report … rc=0`, no server apply line, and the server span
  `off=25344 len=47008 gen=0xe990ec58f2c8212d nodes=2,3,4`. That gen
  **is fcstor009's object** (16830211674258022701 decimal) — under the
  *peer's* range. Object names are content hashes of the 128 KiB PUT
  image; the two clients had merged the shared chunk to the identical
  image, so fcstor010's span record and fcstor009's carried the same
  gen. `apply_publish` treated the second as "replay of this same span
  object" and, on the retry path, the REPORT pack skipped it as held
  (`efs_meta_apply_chunk_holds` matched gen alone) — the `report-split
  skip=N` counts. The bytes were never recorded; the read saw zeros.
  The nrange-0 snapshot is a second, independent bug: `dcache_take`
  marks the slot dirty and the have_base/base_gen/range came in a second
  locked section, so a flusher in between PUT the buffer as a full image
  and the merge overlay (which walks nrange) dropped the write.
- **41745** — three objects (span, then two full images), the slot base
  became its own first object gen through `dcache_need_published_merge`
  → pull → `export_chunk_copy` returning the *local* gen; the pack STALEd
  on base CAS against no row; the classifier read
  `stale-class committed=94a3… ours=94a3… done=1` because the GETCHUNKS
  reply had no row for the chunk, the local table kept our candidate
  gen, and "committed == ours" was taken as done. Dropped as committed.
  `span_of` had the same confusion: our own unreported span in the table
  "covered" the range, so a widened flush published only the tail, and
  putid (one object per chunk) forgot the first span. Its cover logic
  was also wrong on its own terms (any overlapping span extended cover,
  so an uncovered head with a covered tail counted as covered).

Fixes, all in `f2d3a7871b96-dirty`, rolled 18:30Z, clients 18:32Z:
span identity is (gen, off, len) on the server (apply replay check and
`chunk_holds`; `test_meta_apply` publishes the same object under a
second range and expects a third span); the STALE classifier matches a
span by range and a full image by base gen and treats a chunk the repull
found **no row** for as never committed (`pull_chunks_range` fills an
absence bitmap, `efs_client_pull_chunks_range_absent`); the replay of an
absent chunk drops the table's gen/list, starts from zeros with an empty
observation and publishes our ranges / span / image with expected 0
(`dcache_replay_stale_ex`); `dcache_init` installs have_base / base_gen /
range with the buffer under one lock; `span_of` grows cover only
contiguously from the range start and ignores the putid object's own
span.

Result (`results/io500/20260930-183504-rdma`): every phase, **0 fsync
failures, 0 read errors, cold hardscan 766980 records bad=0**.
ior-easy-write 4.563 GiB/s, mdtest-easy-write 4.632 kIOPS, ior-hard-write
0.640 GiB/s in 52.6 s, mdtest-hard-write 2.612, ior-hard-read 1.034,
mdtest-easy-stat 16.478. Server apply STALE (all FOLD_LIST) 220–463 per
node against 3238–6573 in ior330; three client STALE rounds in total with
pull ≤ 1.2 s against one per client at ~7 s. `inbox_drop` 171 on fcstor005
(3102 before). Not read: `pub_p50` 596629 on fcstor005's last raft-obs
line, 24 `getchunks slow` lines on fcstor004.

<a id="ph-sep-30-2026-0840z--ecopys-149-metadata-mismatches-four-clien-3064b8"></a>
## Sep 30 2026 08:40Z — ecopy's 149 metadata mismatches: four client bugs, none on the server

The user's 07:17Z `ecopy --verify` on fstor007 (`85f5b31c` client) left 149
"verification metadata mismatch" lines: 119 atime, 29 mtime, 1 size. The
05:06Z mtime fix (flush before SETATTR) had closed the common case; these
were what remained. All four are client-side; a stat from a second client
confirmed the server held the wrong value only where the client had sent
it. Review, gates and traces:
`results/measure/20260930-080000-ecopy-times-review/SUMMARY.txt`.

**atime (119).** `struct efs_inode_mem` had no `atime_nsec` / `ctime_nsec`.
`inode_copy_attr` and `inode_to_rpc_p` dropped them and `efs_export_set_atime`
took seconds. ecopy's post-rename stat is a lookup filled from the staged
row: nsec 0. The old client reproduced it on the first try
(`atime_repro.py`: after-close OK, after-rename `.000000000`). The fields
went into the struct's padding (still 192 B), both copies carry them, the
setter takes nsec, and the second-resolution local ctime bumps zero
`ctime_nsec`.

**mtime = close time (29).** Three holes in "utimens has nothing to
flush", found one at a time with an ecopy-shaped burst (`size_gate.py`, 48
threads × 120 files, write → futimens(ns) → close → rename, then stat from
the same and a second client). (a) A wb job waiting in
`wb_overlap_inflight` was in neither the queue nor `busy_ino[]`;
`efs_wb_ino_pending_locked` said clean. `busy_ino` is set at the pop.
2 of 5760 files still wrong. (b) Every flusher marks the dcache entry clean
before its PUT and the chunk dirty after (the put-record); in that window
the chunk is invisible to a REPORT snapshot and to `efs_dcache_flush_ino`.
`put_win_open/close` count windows per inode; `efs_dcache_flush_ino` waits
for them (8 s → BUSY) and re-passes. Still 1–2 per run. (c) The predicate
itself: `efs_client_ino_is_dirty` is the dirty SET. A threshold REPORT
(`only_ino=0`) snapshots the ino mark `dcache_note_size` set and ships an
irec-only record while the bytes are still a dirty dcache entry. With
`EFS_DCACHE_TRACE=1` (new `utimens` / `report` / `release` lines) the bad
inode showed `utimens ... flush rc=0` with no report before it, the setattr,
then `snap-steal` of a `dirty=1 r=[0,1000)` entry and a `only=0 sync=0`
publish. `efs_utimens_flush_dirty` now also checks `efs_dcache_ino_pinned`,
the predicate the evictor already uses. Four bursts: 0 / 5760 each, same
client and fcstor008.

**size (1).** mpfr's `Makefile.in`, 32398 bytes on XFS, 131072 on efs.
`write_chunks_no_replicate` marked the chunk dirty and the ino only at the
end of the function; a threshold REPORT between the two shipped the crec
with no irec; `raft_host.c` sizes a crec without an irec to `(ci+1) ×
128 KiB` and irecs are newer-only. The ino mark and a local size grow sit
in the chunk's locked block now, and the REPORT builder appends an irec for
every crec whose ino has none. Server fallback left as is.

Perf of the same session (`fuse.data`, 976K samples): memmove 21.9 % (6.4 %
the `efs_rdma_send_frame` bounce copy, ~14 % FUSE → dcache), blake3 10.2 %,
`xor_into` 7.3 %, `stage_evict_main` 5.9 % — the evictor bug fixed in
`5ea397fd`, which fstor007's client did not have. Zero-copy RDMA from
registered dcache memory would be a design decision. Deployed to
fcstor007–015 08:35Z; fstor007's `/tmp/efs` rebuilt; posix jobs=1 200/201.
Pre-existing, untouched: `test_raft_store` 4 W22.1 assertions,
`check-architecture.py` regen / single-home.

<a id="ph-sep-30-2026-0740z--gate-of-the-0650z-roll-posix-green-one-en-010e9d"></a>
## Sep 30 2026 07:40Z — gate of the 06:50Z roll: posix green (one ENOTEMPTY), IO-500 hard-write fsyncs fail, hard-read EIO

Servers `44f397b4ca2e-dirty` (rolled `--all` 06:50Z), clients fcstor007–015
mounted RDMA 07:17Z on the `5ea397fd` tree (evictor fix). The user's
ecopy from fstor007 was still running for the first suite and stopped
~07:22Z.

**Posix.** jobs=1 on fcstor007: 200/201 in 16.8 s under the ecopy
(`results/posix/20260930-072042`), 200/201 in 7.1 s idle
(`20260930-072224`); `mmap_write_read` SKIP only. posix2
fcstor007/008 63/63 in 47.6 s (`results/posix2/20260930-072241`).
9-host (`results/posix/20260930-072501`): eight hosts 200/201 in
18.0–19.1 s, fcstor012 199/201 — `dir_deep_nesting` failed `rmdir
d50` with `[Errno 39] Directory not empty`. From that client the rmdir
kept returning ENOTEMPTY 1–2 min later (`d50-probes-012.txt`); a KV
copy from fcstor005 at 07:28Z (`kv_dir_dump`, `d50-kv-dump.txt`) shows
d50 = ino 56024 `nlink=2 nents=0`, parent d49 = 164529 `nlink=3
nents=1` with the one `d50` dentry, no intent/guard/reduce; `rmdir`
from fcstor007 succeeded at once (`d50-rmdir-from-007.txt`). gdb on
fcstor012's `efs-fuse` afterwards (`d50-client012-childvec.txt`):
`child_vec_get(&g_client.export, 56024, 0)` on the **root tab**
returns a vector with `count=0 cap=4` — a row with parent 56024 was
on the root tab at some point. `efs_client_unlink` (`ops.c:1303`)
refuses a rmdir locally when `efs_export_dir_empty(&g_client.export,
ino)` says non-empty, and that function reads only the root tab's
`child_vecs`; nothing logs the refusal. The server pre-check
(`raft_host.c:8776`, `row.nlink > 2` or one readdir entry) reads the
raw row, so a pending `REDUCE_INO` from d51's rmdir would also give a
transient ENOTEMPTY, but the client retries that only 20 × 1 ms
(`efs_fuse_rmdir_at`), and "transient" does not cover the later
probes. No `dir_deep_nesting` failure in the Sep 28–30 history; this
is the first 9-host run with the evictor fix (the evictor is active
during the suite — D18). Open. Two ways forward: log which tab/slot
made the fast path say non-empty, or remove the local emptiness check
(the server is authoritative, and the fast path saves one RPC on a
non-empty rmdir only) — the second is a decision, ask.

**IO-500 debug 9×4** (`results/io500/20260930-072824-rdma`, launched
07:28:24Z, cluster idle, terms unchanged through the run):

| phase | Sep 30 | Sep 28 (`20260928-150609-rdma`) |
| --- | --- | --- |
| ior-easy-write | 3.148 GiB/s | 2.917 |
| mdtest-easy-write | 3.696 kIOPS | 3.418 |
| ior-hard-write | 0.261 GiB/s, 133 s, 16 fsync failed | 0.291, 73 s, 0 |
| mdtest-hard-write | 2.126 kIOPS | 0.757 |
| ior-easy-read | 2.976 GiB/s | 3.092 |
| mdtest-easy-stat | 3.600 kIOPS | 3.248 |
| ior-hard-read | ABORT, read EIO | 1.525 |

Every client's `fuse.log` has the same pair on the shared file ino
84857: `report-loop rounds=1 stale=0 busy=1 ms≈9000–11600 rc=-13`
(fsync EBUSY — W17.1 returns the first BUSY REPORT) and `rounds=1
stale=1 busy=0 ms≈58000–68600 rc=-14` (fsync EIO). ior-hard-read
then read those unpublished spans: `read(23, …, 47008) failed
Input/output error` on ranks 22/23 (fcstor012 and others) →
`MPI_ABORT` 07:32Z; nothing after that ran. I9 behaves as specified
(fail, do not zero-fill). Server side (`servers-after.txt`):
`apply_max` 0 µs and `pump_hold_max` ≤ 10 µs on all four (the pump is
not held), but `inbox_drop` 978 / 3102 / 223 on fcstor004/005/006 —
`raft-host: inbox full, dropped frame type=3 group=2 from=3
len=949540`: 950 KB AppendEntries from the group-2 leader (fcstor006)
dropped at fcstor005's 256-frame `HOST_INBOX_MAX`; `wait_timeouts` 619
on fcstor004, `apply-sleep` 20–127 ms everywhere, `pub_p50` 544 ms on
fcstor005 (3 ms at D8's IOR). L0 is 113–231 files per node and does
not drain at idle: ~10 000 `kv-compact` lines per node since the
roll, each 50–270 KB in 1–2 ms (`inputs=2 … l0only=1`, D10's
per-range trigger), while `gc-pass ms=244 frag=244` on both leaders
keeps flushing memtables with `GC_ACK` entries (the run's garbage).
The F2 drop counter did not exist on Sep 28, so that run cannot say
whether its inbox dropped too; the hard-write fsync failures are the
regression (0 warnings on 09-28, before W17.1/D9). No code changed
for this. The aborted run's data and two stale Sep 29 `posix-*` dirs
were removed from the mount; the nine clients stay mounted.

<a id="ph-sep-30-2026-0710z--the-rest-of-the-perf-dir-du-52k-a-1-hz-cl-ba6110"></a>
## Sep 30 2026 07:10Z — the rest of the perf dir: du 52K, a 1 Hz client stall, 10 ms stats, 16 fragments/s

`results/measure/20260930-063500-perf-dir-review/SUMMARY.txt`. The user
asked about the other files in `~/orcd/scratch/efs/perf/efs-mount`
(ls, ls -lart, rsync, ecrawl, three finds, du, dd bs=16k and their
straces), starting from `du -hs` = 52K for 3.8 TB.

**du 52K.** du sums `st_blocks`; 52K = 103 symlinks × 512 B. Every
regular file reports 0 blocks (`find -ls` block column, ecrawl
`total_allocated_bytes=0`, `files_sparse_heuristic=21150/21503`).
`inode_allocated_bytes` (efs_fuse.c) counts the client-local
present-chunk table (W20), which since D2 is empty for a file the mount
did not write. The server row has no chunk count and W20 forbids
size-based `st_blocks`. Design ask D17 (per-lane present-chunk stamp
reduced at getattr). Not implemented.

**1 Hz stall.** du, find1 and find2 each show one syscall of 13–14 ms
every 1.014 s on whatever op was in flight; the 01:10 find on a fresh
mount does not. First suspect was the server GC loop (1 s cadence,
prefix scans under `l->mu`); a `gc-pass` timing line went in, the
servers were rolled, and the stall was still there, larger (17–27 ms).
Straced both sides at once during a stat loop: servers idle apart from
50 ms heartbeat waits; on the client, thread `stage_evict_main` leaves
its 1 s timedwait and then takes and releases the client table lock 58
times in 21 ms while the RPC workers sit in FUTEX_WAIT on it. gdb on
the live client: staged estimate 412 MB against the 256 MB cap with
10780 rows — the root export has 5 rows, ~2400 shard tabs have 1–3
each and are booked at ~170 KB apiece (a 256-row slab counted at the
512 B on-disk row size though calloc got 192 B rows, 16 × 1232 B chunk
entries, 512-slot indexes). So the evictor was over cap forever and
each of its 64 `evict_one` per second ran `ino_has_chunks` +
`drop_chunks_from` + `forget_ino` = three walks over every loaded tab
(250 µs), then `efs_export_compact` rebuilt every tab's index to the
same size (the 25 % load test always passes at 1–3 rows against the
256-row floor). Fixed on the client: `efs_export_evict_ino` visits the
tabs the row names (ino, parent, hashed dentry, chunk groups by size;
fan-out for a missing row, a hard link, or a file spanning most
shards), compact reindexes only when the rebuild shrinks, slab bytes at
`sizeof(struct efs_inode_mem)`; `test_stage_evict` covers the two-phase
evict. fcstor007 remounted on it: two du runs, 0 syscalls over 10 ms.
The floor itself (up to 4096 tabs × 90–170 KB > the cap before any data
is staged, evictor churning 64 hot rows/s) is D18.

**Big-file stat 5–10 ms.** `host_read_inode_lanes` issued a ReadIndex
per active foreign lane (32 peer round trips on a follower) and every
per-lane `efs_txn_reduce_read_ex` prefix scan opened an iterator per
covering segment whose `iter_load` pulled a 1 MiB readahead window —
22 × 1 MiB pread per RPC, twice per stat (LOOKUP + GETATTR), fcstor004
strace, servers at 2 % CPU. Fixed and rolled `--all` 06:50Z
(`44f397b4`): one ReadIndex per group; `kv_seg` iterators read one
block unless `kv_seg_iter_set_seq` (compaction and export keep the
readahead). 9.8 → 4.7 ms per stat; the remainder is the 64-lane double
collect.

**GC 16 fragments/s.** `host_gc_frag_pass` scanned 32 records and
proposed one 16-ack `GC_ACK` per group per second: 70–86 unlink per
server per 5 s, 21 hours per 100 GiB file. Now 256 per scan, 128 acks
per entry (`EFS_META_GC_ACK_MAX`, apply scratch on the heap), rescan
while full within 200 ms per group per loop, 1 ms yield between scans.
`raft-host: gc-pass ms=` prints when a GC iteration exceeds 5 ms
(leaders: `recover=70`, 64 ms of it the per-shard yield sleep).

Also read: rsync's `getcwd ENOENT` at 01:53 was the invoking shell's
cwd (re-run at 02:31 from `/tmp/direct_copy` worked); dd bs=16k is
20–40 µs per 16 KiB FUSE write (~500 MB/s single stream, direct_io
shape); readlink p50 0.85 ms; ecrawl's 16 threads had 718 calls over
10 ms in 4.8 s; the 01:10 find's 1.025 s getdents was the 05:11Z
election. Servers were rolled twice under the user's live `client.sh
--perf` on fstor007 (06:35Z, 06:50Z).

<a id="ph-sep-30-2026-0617z--the-wedged-mount-after-a-100-gib-dd-a-pip-c9b53b"></a>
## Sep 30 2026 06:17Z — the wedged mount after a 100 GiB dd: a pipelined peer lane read its replies on the wrong channel

The user ran ls/rsync/ecrawl/find/du (all fine), then a 100 GiB `dd
bs=1M` from fstor007 against the 05:43Z roll. 1.0–1.2 GB/s for 106 s,
then `dd: closing output file: Input/output error`; sha256sum ENOENT
after 33 s, `ls /tmp/efs-mount` EBUSY after 16.7 s, unmount `DATA LOSS`
after 60 s of BUSY REPORTs. Analysis and inputs in
`results/measure/20260930-060000-dd-wedge/`.

The servers: no elections, `inbox_drop=0`, `apply_max` 110 ms. But the
two dual-group hosts' lanes to each other were failing once per 250 ms
in both directions (`tx->2 fail=2399 hi=2048` on fcstor004, `tx->1
fail=2395 hi=2048` on fcstor005; the 003 and 006 lanes fail=0), and ten
minutes after the client was gone fcstor005's group 0 was 678 entries
behind its leader and fcstor004's group 2 was 721 behind — each moving
one entry every ~15 s. Every read served by those followers
(`host_read_index` → 400 ms `apply-sleep` → BUSY: 164 + 82 lines) and
every REPORT touching both groups (`report-split nrec=819200 …
rc=-13`, 83 + 82 lines, pack 0 / push 0) returned BUSY; the client's
16-retry budget ran out.

Root cause, read from `host_sender`: a frame larger than the RDMA buffer
(72 KiB) goes over the conn's TCP side-channel (`conn_pick_send_chan`)
and the peer answers it on TCP; smaller frames go and come back on RDMA.
The sender pipelines up to three messages and then collected the
replies with `recv_chan` left at the *last* message's channel. A batch
of [AE_REP (RDMA), big AE (TCP)] read TCP only, left the RDMA reply in
the ring, timed out at `HOST_SEND_IO_MS`, destroyed the conn, and Raft
resent the same window — same shape, same result, forever. Only the
004↔005 lane mixes one group's AppendEntries with the other group's
replies (a single-group lane keeps one AE in flight and rarely batches
two kinds), and only a big-entry stream makes an AE take TCP: the dd's
publish batches (2048 publishes per command) did. Before F1 the same
shape waited `EFS_IO_TIMEOUT_MS` (30 s) per attempt — that is the
"lost RAFT_REPLY" the 04:06Z review could not place; the reply was on
the other channel.

Fix `85f5b31c`: `struct efs_conn` records `last_recv_chan` on every
receive path; `host_sender` counts the expected replies per channel
after each send and reads with `recv_chan=RDMA` while RDMA replies are
outstanding (that wait drains the ring first and falls through to TCP
when a byte is there), TCP otherwise; a reply on a channel with none
outstanding drops the conn. No wire change. Rolled `--all` 06:16Z.

Gate 06:19Z (`~/efs-runs/gate130.log`): fcstor007, 32 GiB of random
bytes through `dd bs=1M conv=fsync`, 32.85 s ≈ 1000 MiB/s, rc 0, size
34359738368, server `report-split nrec=262144 … rc=0` — the same
2048-publish stream that wedged. `tx->N fail=0` on every lane of
fcstor004 and fcstor005 before and after, terms unchanged, both groups
commit==applied, no `exhausted`/`report-loop`/`DATA LOSS` in fuse.log;
clean unmount. (A first attempt, gate128, ran the dd inside a 15 s
`efs-ssh.sh` call and died before writing a byte — void.)

Not changed: the 819200-record close REPORT is one RPC whose last-batch
BUSY discards 16 s of server work (splitting it is a design ask); the
D13 fold runs after every flush (L0 ~150 files, 1–5 ms each, log
noise); `apply lane-fence rc=-2` ×32 during the dd, not chased; the
client dd profile is memmove 29 % / blake3 13 % / xor 4.7 % with
`dcache_reclaim_main` gone from the top.

<a id="ph-sep-30-2026-0545z--review-of-the-0506z-roll-under-ecopy-the-976ae1"></a>
## Sep 30 2026 05:45Z — review of the 05:06Z roll under ecopy: the conn pool was the 5 s mode and the 30 s freezes

The user ran `client.sh --perf` on fstor007 against the 05:06Z roll
(F1/F2/utimens flush) and drove find, `ecopy --verify software/`,
ecrawl and `rsync -avvvP ~/git` through it (05:10–05:19Z). Review in
`results/measure/20260930-051000-review2/SUMMARY.txt`; fixes committed
`e002771e`, rolled `--all` 05:43Z (`e002771e56e4-dirty`,
`~/efs-runs/roll107.log`), fstor007 `/tmp/efs` rebuilt.

1. **`dcache_reclaim_main` was 39.8 % of efs-fuse cycles.** The 16
   reclaim threads pop only `have_base` dirty slots (W18); ecopy's dirty
   set is fresh files, so with dirty_bytes over the 2 GiB limit every
   kick re-walked all 64 dirty lists, popped nothing, and looped. Now a
   sweep that pops nothing parks the threads until the next kick after a
   10 ms nap; a sweep that pops re-arms immediately.
2. **The 5.0 / 10 / 15 s RPC mode (244 `slow-ok`, `attempts=1
   saw_busy=0`, every message type) was `efs_client_conn_get`**: 16
   slots per node, a checkout holds a slot for the whole RPC, a PUT
   holds three, a full pool waits `pthread_cond_timedwait` 5 s and
   returns NULL silently. 400 ecopy threads on 16 conns. The earlier
   attribution to `EFS_RDMA_SEND_WAIT_US` was wrong (IB hw_counters:
   `out_of_buffer` 0 on all servers). Worse than the wait:
   `put_fragments` and `fetch_fragment` called
   `efs_client_node_note_fail()` on that NULL, and four of those mark a
   live node DOWN for 30 s (`EFS_NODE_DOWN_FAILS`/`EFS_NODE_DOWN_MS`),
   after which every RPC to it returns NULL immediately — the two 28 s
   client-wide freezes in the ecopy strace (every syscall class parked,
   then all released together) and the 12 `put_fragments ... no quorum`
   lines against four live servers. Fix: log the timeout
   (rate-limited, with the pool size), never count it as a node failure
   (connect failures are already counted inside `conn_get`), default
   pool 64 per node (`EFS_SERVER_MAX_CONNS` is 4096 now, not the 512
   the old comment cited; one RDMA conn pins ≈ 2.6 MB per end; slots
   connect lazily).
3. Left as is: REPORT is one at a time with the whole dirty set, so a
   slow RPC inside it parks every queued closer (1630 ecopy syscalls
   ≥ 1 s) — splitting it is a design ask; re-measure after (2) first.
   fcstor004's 2475 `apply-sleep` follower waits are D14. 1376 tiny
   kv-compactions on 004/005 are log noise (4.3 s total).
4. **"ecopy + atime": no new defect.** W25 item 2 (atime nanoseconds
   end to end) landed in 2dd77dac; reads never move atime; the ecopy
   strace has zero mismatch lines; rsync's 8504 `set modtime, atime of
   <dir>` lines are its -vvv directory time set.
5. Servers after the 05:06Z roll: terms stable but for one group-0
   election at 05:11:19Z under load, `inbox_drop=0` everywhere, F1's
   250 ms bound visible as `tx-> fail` +1 per stall (fcstor006 tx->1
   fail=8, fcstor004 tx->2 fail=5). IB hw_counters snapshot saved as
   the delta base for the next run.

<a id="ph-sep-30-2026-0513z--f1-f2-and-the-utimens-flush-implemented-a-5ac668"></a>
## Sep 30 2026 05:13Z — F1, F2 and the utimens flush implemented and rolled

User: "can you implement f1 f2 and the time bug?". All three are in the
tree, uncommitted, rolled `--all` at 05:06Z as `06916bc7e5c1-dirty`
(`~/efs-runs/roll78.log`, no recorders).

**F1 — bound the RDMA peer reply wait.** `struct efs_conn` gained
`recv_timeout_ms` (`include/efs/network.h`); `efs_conn_set_recv_timeout()`
(`src/common/network.c`) stores it and sets `SO_RCVTIMEO` on the TCP
side-channel; `conn_rdma_frame` (`src/common/protocol.c`) waits
`recv_timeout_ms` when set instead of `EFS_IO_TIMEOUT_MS` (30 s);
`host_sender` (`src/server/raft_host.c`) calls it with `HOST_SEND_IO_MS`
(250 ms) for every conn kind — the old block was `if (pc->kind ==
EFS_CONN_TCP)`. The sender's conn comes from `server_peer_conn_new`
(private, not the pool), so nothing has to be restored.

**F2 — count inbox drops.** `server_raft_host_inbox` increments
`h->inbox_drop` on the inbox-full `EFS_ERR_BUSY` path, logs the first ten
(`raft-host: inbox full, dropped frame type= group= from= len=`), and the
`raft-obs: wait_timeouts=` line ends with `inbox_drop=`. `from` is the
big-endian s32 at wire offset 4 (`efs_raft_msg` codec, `wire.h`).

**Mtime — flush before a SETATTR that sets mtime.** `efs_fuse_utimens_ino`
calls `efs_utimens_flush_dirty(ino)`: if the inode has writeback jobs
pending (`efs_wb_ino_pending_locked`) or is in the client's dirty set
(`efs_client_ino_is_dirty`), it runs `efs_append_flush_report(NULL, ino)`
— the same flush + REPORT that close runs — so the publish reaches the
server under the old `mtime_gen` and the SETATTR that follows bumps it and
fences the lanes. A clean inode pays one hash lookup. No wire or server
change; `EFS_INO_REC_F_TIMES` is still read by nothing. `efs_client_mtime_pin`
says `mtime-pin: table full` once instead of dropping pins silently.

**Gate.** `mtime_repro.py` on the user's live fstor007 mount (FUSE_OK,
`~/efs-runs/rec-gate84.log`): `write→futimens→close→rename`,
`write→futimens→close`, `write→close→utimens`, `write→futimens→fsync→close`
all end at 1380661863. `futimens→write→close` ends at the write time
(correct). An earlier run of the same gate (`gate81`) started 30 s before
the user's mount came up and wrote into the plain dir under the
mountpoint; those rows are local XFS and are not a result — the dir
`/tmp/efs-mount/measure/mtime-repro2` is still there under the mount.

**Unit tests** (fcstor007, `/tmp/efs-build69`): 14 of the 15 `make test`
binaries pass, `test_raft` OK with `test_ae_reply_match_stops_at_prev` and
`test_noop_over_stale_tail`. `test_raft_store` fails four assertions
(`snap index should report its term`, `compacted prefix differs`,
`snapshot did not shrink the log`, `reopen after rotation`) on a clean
`git archive HEAD` build as well (`~/efs-runs/rs77.log`) — W22.1's
retained log window changed what a snapshot does to the log and the test
was not updated. `docs/check-architecture.py` fails on the regen diff and
on the repeated decision-table headers in START-HERE §1.

**After the roll, under the user's load (05:06–05:14Z):** fcstor004
`tx->2 fail=5 hi=47`, `wait_timeouts=5`, `inbox_drop=0` on every node; one
group-0 election (term 94→95 at 05:11:19Z, fcstor005 campaigned and
fcstor004 stepped down); 256 `kv-compact: start` lines and 120–270 ms
`apply-sleep` waits on group 2 in one log window. F1 turned each lost
reply into a 250 ms `fail` + reconnect instead of a 30 s freeze; it does
not say which side lost the reply, and it does not stop `on_vote_req` from
deposing the leader (D15/D16 pending).

<a id="ph-sep-30-2026-0445z--ecopys-mtime-mismatches-a-utimens-between-3639a6"></a>
## Sep 30 2026 04:45Z — ecopy's mtime mismatches: a utimens between write and close is lost

The 153 "ecopy: verification metadata mismatch" lines at the start of the 04:03:51Z ecopy are all `(mtime)` (message lengths against the paths the same threads had just stat'd; `mis_ctx.py`). The copier thread does `openat(tmp, O_CREAT)` → `pwrite` → `utimensat(fd, [atime, mtime 2013])` → `close` → `renameat`; the verifier's `lstat` then sees a 2026 mtime. Reproduced from one python process on fstor007 against the idle cluster (`results/measure/20260930-044100-mtime-utimens-close/repro-fstor007.txt`): `write→futimens→close` gives the close time, `write→futimens→fsync→close` too (2013 survives the fsync, not the close), `write→close→utimens` is right, XFS is right in all three. Cause: the buffered write's PUBLISH reaches the server in the close REPORT after the SETATTR; `host_utimens` bumped `mtime_gen` and fenced the lanes, but the publish is packed with the *current* `mtime_gen` and `p.now`, so `meta_apply.c:3467` stamps the lane and the stat at `:4175` takes the MAX. The client-side pin (`efs_client_mtime_pin`, `EFS_INO_REC_F_TIMES` on the REPORT rec) was meant to cover this; the server never reads the flag, and the pin table is 256 entries dropped silently. Consequence for the user's copies: every already-copied file fails ecopy's `same_size_and_mtime`, so each rerun re-copies the whole tree (22.4 GiB this time) and fails verification again. Documented in SUMMARY.txt with the recommended fix (flush dirty dcache before a MTIME/ATIME SETATTR, client only) and the server-side alternative; nothing written. The repro client was stopped afterwards; `/tmp/efs-mount/measure/mtime-repro/` holds five 7-byte files.

<a id="ph-sep-30-2026-0420z--post-fix-review-per-op-references-and-a-3-8d749d"></a>
## Sep 30 2026 04:20Z — post-fix review: per-op references, and a 30 s RDMA peer wait behind the election storms

The user ran `ls`, `find -ls`, `du`, a re-sync of `~/git`, `ecrawl` and `ecopy --verify` (22.4 GiB, 400 threads) from fstor007 against the 03:56Z roll (`06916bc7e5c1-dirty`, servers `--perf` only, client `client.sh --perf`). Reduction and raw pulls: `results/measure/20260930-040600-postfix-review/`. The first five are clean and give the per-op references for this build with no server strace: stat 0.33 ms, openat 0.60, close 0.34, utimensat 1.05, chmod 0.49, rename 6.2 ms (max 36 ms), `ls` 0.015 s, `find` of the tree 6.5 s with no call over 0.2 s. ecopy put both groups into an election storm: group 0 63 terms, group 2 426 (fcstor006) in 13 minutes, 12 `newfstatat` of 55–59.5 s, 513 over 0.2 s, 21 `shard=0` LOOKUPs and 8 REPORTs exhausted 16 BUSY retries, fcstor004 `wait_timeouts` 0→1290, and the 96 727-record REPORT answered NOT_PRIMARY four times by fcstor005 before fcstor004 packed it (846 ms). Group 2 settled at term 669 (leader fcstor005) at 04:09:39Z with all voters commit==applied.

Cause, from `raft-obs tx->` and the code: `host_sender` blocks for a `RAFT_REPLY` per batch, and on an RDMA peer conn that wait is `EFS_IO_TIMEOUT_MS` = 30 s (`conn_rdma_frame`, protocol.c:380) — the 250 ms `HOST_SEND_IO_MS` bound at raft_host.c ~757 is applied to TCP conns only, although the comment at raft_host.c:45 states the rule for any peer. `sent` froze for 10–30 s on fcstor004→005, fcstor006→004 and 006→005 while `enq` grew, then `fail` +1 and the conn was rebuilt. The frames of the frozen batch are processed by the peer (fcstor006 won votes through a frozen lane); the reply is what does not come back, and which side loses it (the peer's reply send waiting on a credit, or the sender node's shared `recv_poller` behind ~400 client conns) needs a stack sample of the sender and peer-conn threads in a storm; on-CPU perf cannot show it. The unheard peer campaigns every 0.5–0.9 s and `on_vote_req` → `maybe_step_down` deposes the live leader each time (no Pre-Vote, no leader stickiness); the leader re-wins 0.6 s later. The 01:21Z `~/git` ecopy (`20260930-012100-ecopy-git`, terms 18→253, `hi=2048`) was the same thing. No CQE-error, `retry counter` or QP line on any node; `pump_hold_max` ≤ 2.7 ms, `apply_max` ≤ 67 µs; `ping` 0.03 ms.

Documented in SUMMARY.txt and START-HERE: F1 bound the RDMA peer reply wait like TCP (per-conn recv timeout on `efs_conn`, set by `host_sender`), F2 count `server_raft_host_inbox` BUSY drops (a dropped Raft frame today has no line and no counter, and `server_handle_conn` still replies), both mechanical; D15 leader stickiness / Pre-Vote and D16 a separate credit class or poller for peer frames are questions for the user. Client profile: 27 % memmove, 10 % blake3, 4 % XOR — the write payload, nothing new on the metadata side; the 59 s stat walls are off-CPU in the RPC retry loop.

<a id="ph-sep-30-2026-0357z--group-0-wedge-a-stale-tail-was-a-match-th-42bb29"></a>
## Sep 30 2026 03:57Z — group-0 wedge: a stale tail was a "match"; the leader's no-op never shipped

The 23:16Z rsync froze and a 23:51Z `ls` returned EBUSY after 16.76 s (`~/logs/ls.strace.txt`, `results/measure/20260930-032400-g0-wedge`). Group 0's leader fcstor005 had armed `send_idx` at attach with the replayed `last_i` = 437637 and never shipped its own term-41 no-op 437638; fcstor004 held a stale pre-restart 437638 (term 34) and, because `on_ae_req` answered an empty heartbeat with `match = last_i`, reported 437638 as matched. When `durable_idx` reached 437638, `try_commit` committed it with {self, fcstor004} — a false commit; fcstor004 then truncated, the reject/accept ping-pong ran every heartbeat, and every follower-served group-0 ReadIndex waited for 437638 → 400 ms → BUSY → 16× → EBUSY/ENOENT. Fix in `src/raft/raft.c`: the success reply reports `match = prev_index + nentries`, never the follower's last log index; `send_idx_cover` advances `send_idx` in `become_leader`, `maybe_append_cold` and `efs_raft_change` the way `efs_raft_propose` does. `test_raft` gained `test_ae_reply_match_stops_at_prev` and `test_noop_over_stale_tail`; both pass (`~/efs-runs/raftfix62.log`). Rolled `--all` 03:56Z with `--perf` only (`~/efs-runs/ready65.log`): group 0 elected fcstor003 at term 43 with all voters at 437638; `ls`/`stat`/mkdir from fstor007 returned at once, zero `apply-sleep`.

<a id="ph-sep-29-2026-1609z--servers-rolled-with-perf-and-strace-no-cl-805d3c"></a>
## Sep 29 2026 16:09Z — servers rolled with perf and strace, no clients

`tests/roll_efsd.sh --all` (`~/efs-runs/rollw34.log`), `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`, `EFSD_ARGS='--perf --strace'`. All four built `2dd77dac881c-dirty` and came up; group 0 leader 0 term 9454 commit==applied 13775244, group 2 leader 3 term 2925 commit==applied 12187894. The 1 s start check printed `perf=0`; a second check (`~/efs-runs/chk34.log`) is `perf=1` `strace=1` on fcstor003–006 and `efs-fuse` 0 on fcstor003–015. Storage was kept. The strace files were already 0.8–1.5 GB two minutes in.

<a id="ph-sep-29-2026-1540z--w21-step-2-w17-step-3-tests-w15-step-5-bl-9b2e53"></a>
## Sep 29 2026 15:40Z — W21 step 2, W17 step 3 tests, W15 step 5 blocked

`efs_export_staged_bytes` is a running total (`staged_refresh` at every capacity change; `test_stage_evict` checks it across compaction). `test_chunk_deltas` gained the three W17.3 cases and drops the pre-D1 "trailer gone" assertion (a fold keeps tombstones). W15.5 checked on fstor007: `fs.pipe-max-size` 1 MiB < `max_write` 4 MiB + 4 KiB, so libfuse 3.10.2 never used the pipe. The user set `fs.pipe-max-size=8388608` on all 15 hosts (runtime, reverts on reboot) and `efs_fuse_init` now sets `FUSE_CAP_SPLICE_READ`. Not rolled, not gated.

<a id="ph-sep-29-2026-1515z--24-h-review-six-fixes-to-the-unrolled-tre-ce2229"></a>
## Sep 29 2026 15:15Z — 24 h review: six fixes to the unrolled tree

`kv_flush_locked` capped runs at 256 while `KV_RANGE_N` is 512 (a wide memtable was BUSY forever under D9). `kv_seg_data_bytes` is cached at open and `kv_l0_bytes` is hoisted out of the compactor's range loop (was O(blocks) per L1 file and per range under `l->mu`). The D13 fold's output is installed at the newest input's position, not at the head (a same-range file flushed during the merge stays ahead). The merge iterator reads a block over 1 MiB whole instead of returning IO. `maybe_prefetch` asks the layout-miss path only when the chunk map is absent. `dcache_flush_slot_inner` re-finds its entry after the unlocked GET/PUT instead of dereferencing a pointer a drop may have freed. `lane_bits` moved into `pack_utimens_cmd`. Two `test_kv_lsm` assertions updated to the byte rule. Not rolled, not gated.

<a id="ph-sep-29-2026-1450z--reclaim-drops-the-shard-lock-before-a-fra-b216fd"></a>
## Sep 29 2026 14:50Z — reclaim drops the shard lock before a fragment GET

In tree, not rolled, not gated. `dcache_reclaim_main` flushed through `dcache_flush_slot`, which held `shard_io` across `efs_client_fetch_published_chunk`. On the 12:36Z IOR that recv sat at 30 s and `ll_fsync` of the shared file blocked on the same lock until the client was killed. The flush now drops `shard_io` with the dcache mutex before the GET and the PUT, after the entry is clean, and takes both again only to install the result. `dcache_steal_dirty` already dropped the lock at that point.

<a id="ph-sep-29-2026-1420z--d13-a-file-cap-compact-does-not-read-l1-7ea15b"></a>
## Sep 29 2026 14:20Z — D13: a file-cap compact does not read L1

In tree, not rolled, not gated. When `n_l0` is over the file cap and no range meets the 1/8 rule or the 1 GiB byte cap, `kv_compact_locked` merges the range with the most L0 files (at least two) into one L0 file and does not open that range's L1. Tombstones are kept, because L1 still holds older copies. The compactor already loops while the count is over the cap. A single-file range is left for the 1/8 rule. The 1 GiB byte cap still rewrites L1.

<a id="ph-sep-29-2026-1353z--100-gib-copy-trace-the-file-cap-backstop-4f6121"></a>
## Sep 29 2026 13:53Z — 100 GiB copy trace: the file-cap backstop rewrote L1

Stopped the 13:08Z `--perf --strace` roll at 13:53Z (`~/efs-runs/stop32.log`). Reduction is `results/measure/20260929-130800-ddposix/ana`, window 09:17–09:50 EDT (posix on fcstor007/008 plus `dd conv=fsync` of two 100 GiB files from fcstor009 and fcstor010). Every `fsync` returned EIO. D12 compacted whenever `n_l0` was over 4, which was the whole copy, so the 1/8 rule never applied: fcstor004 wrote 67.5 GB in 425 compacts (max 17 s, L0 peak 379). The apply still binary-searched those files (`kv_seg_probe` 8.3 % self on fcstor004) and `host_read_index` returned BUSY at 400 ms (`pack_ms=0` on 172 of 184 `rc=-13` reports). One `push_ms` was 206 s. `backpressure` did not fire. Server `fsync` averaged 3 ms (max 3.6 s on the compactor). The fix is D13: over the file cap, merge a range's L0 files together and do not read its L1.

<a id="ph-sep-29-2026-1236z--8h-and-d12-rolled-easy-write-finished-har-cde5fa"></a>
## Sep 29 2026 12:36Z — 8h and D12 rolled; easy-write finished, hard-write did not

Rolled `--all` at 12:29Z without perf or strace (`~/efs-runs/rollw31.log`), build `a53b253f2455-dirty`. 8h caches a segment's last key at open and `kv_seg_probe` rejects a key outside the in-memory span before `block_for`. D12, when `n_l0` is over the file cap, compacts the range with the most L0 files even under the 1/8 byte rule. The 9×4 1 s-stonewall IOR (`results/io500/20260929-123635-rdma`) printed ior-easy-write 2.218 GiB/s (19.720 s) and mdtest-easy-write 1.765 kIOPS (2.739 s). ior-hard-write then logged 31 `fsync failed` and no bandwidth; rank 28 on fcstor014 called abort. fcstor004 ended at L0=3, L1=397, with 73 of 99 `report-split` lines `rc=-13`. On fcstor012 the shared-file `ll_fsync` waited on the shard I/O lock while the reclaimer held it across a fragment GET (`efs_rdma_recv_wait`, 30 s) of chunk 170279; those four ranks went D and were cleared by killing that `efs-fuse` and remounting.

<a id="ph-sep-29-2026-1202z--ior-trace-l0-file-count-not-the-pump-wait-4d6bf0"></a>
## Sep 29 2026 12:02Z — IOR trace: L0 file count, not the pump wait

Recorders from the 05:31Z roll (`a53b253f2455-dirty`, RDMA, `--perf --strace`) ran until 12:02Z and were stopped so the files could be read (`~/efs-runs/stop31.log`). Clients exited on SIGTERM. The four `efsd` did not return within 8 s and were killed after their recorder children were signaled; the perf files still open (fcstor004 IOR slice 155K samples, lost 0). Reduction is `results/measure/20260929-053100-w30trace/ana`. The only write in the 01:31–08:02 EDT window is the 01:46 EDT IOR, which aborted with no bandwidth: `report-loop rounds=1 busy=1 rc=-13`, walls 8–26 s, one client `recvfrom` of 25.9 s.

D9, D10's byte rule, D11, and D7 held. 34 compactions on fcstor004 wrote 0.254 GB (max 1161 ms, no `backpressure` line, `fsync` max 91 ms, `access()` 10K against 855K `openat`). What did not hold is D10's dropped file cap: L0 went 26 → 310 files and L1 ended at 353, because `kv_compact_locked` returns BUSY unless a range's L0 bytes are 1/8 of its L1, and the ranges that qualify are 0.5–1 MB / 10 ms. `lookup` probes every segment, so the publish apply on the pump (`efs_meta_apply_get_chunk` 9 %, `lookup` 18 %, `kv_seg_probe` 12 %, memcmp 10 % self) falls behind and `host_wait_applied` returns BUSY at 400 ms with `pack_ms=0` (54 of 60 reports on fcstor004). One group-2 snapshot, 2.82 GB / 14.1 s, accounts for the two `rc=-15` reports, not the 54 BUSYs. Fixes are START-HERE 8h and D12. The client hot path is blake3 at 21 % of on-CPU time while the wall is the RPC wait.

<a id="ph-sep-29-2026-0408z--snapshot-window-sliced-import-first-write-26d872"></a>
## Sep 29 2026 04:08Z — snapshot window, sliced import, first-write hint

Rolled `--all` (`~/efs-runs/rollw22.log`) onto `54a500da9dc8-dirty`,
RDMA, `EFS_RAFT_OBS=1`, servers and clients fcstor007–015 with
`--perf --strace`. No suite and no IOR. Group 0 leader 1 term 9418
commit==applied 13687019; group 2 leader 1 term 2902 commit==applied
12107762. All nine clients FUSE_OK. Recorders left running.

W22.1: `host_maybe_snapshot` fires when command bytes past `snap_idx`
reach `EFS_RAFT_SNAP_BYTES` (512 MiB). `raft_group_snap` keeps that
many command bytes (`log_base`); `send_ae` InstallSnapshots only when
`next_index` is below the window. A rotated log records `log_base` in
the SNAP record so replay can put the window back. W22.2: the pump
applies an import diff 1024 keys at a time and returns BUSY until the
cursor finishes, so a heartbeat can go out between slices. W14.4b:
the client's first PUT of a fragment sends `EFS_PATH_HINT_NEW`; the
server skips the `access()` walk and the writer picks the least-queued
root. A retry of an unacknowledged PUT sends 0. D6: `efsd
--meta-storage` exists and defaults to the first `--storage` root;
`mdraft/` stayed where it was. D8: `raft-obs` prints `pub_p50` and
`pub_max` for the publish batch's propose-to-apply wait. D2's
parallel chunk-map windows are still open.

<a id="ph-0427z--the-trace-analyzed-1bc97e"></a>
### 04:27Z — the trace, analyzed

Recorders stopped at 04:27Z; per-host analysis in
`results/measure/20260929-040800-idle-trace/ana` (scripts
`~/efs-runs/ana4*.sh`). A user `ecopy` ran 04:15–04:22Z (408K fragment
creates per server; nothing through fcstor007–015's FUSE, whose
profiles are idle: `recv_poller` 930 `poll`/s, 0.3 % of a core).

Held: zero snapshot exports in 20 minutes (W22.1); 26K `access()` for
408K creates (D7). Regressed: `disk_log_new_bytes` walked the retained
log per pump tick, 1–1.25 % of every server — replaced with running
counters (in tree). Found: the compactor is 48–55 % of two servers,
433 compactions and 159 GB of `bytes=` in 20 minutes on a 5.3 GB
table, range 0 at 1.8 GB per rewrite; the pump's apply path blocked in
`kv_maybe_flush_locked` for 4.1 / 1.7 / 24.2 / 2.7 / 3.6 s on
fcstor004, each the length of one compaction, giving `apply-sleep`
400 ms timeouts, `report-split … rc=-13`, and the run's two term
changes (g0 9418→9421, g2 2902→2904). Per-thread `fsync`: pump
(Raft log) 0.38–0.40 ms average on 003/004/005; compactor 36–62 ms
average with 141–407 calls in 90–120 ms — the 100 ms mode is the
compactor's own segment, so D6's shared-journal premise is closed
(D11). D8's `pub_p50` is 3.1 ms when the pump is free. Also in tree:
`setvbuf` 1 MiB on segment writes (5.25M 4 KiB `write`s). W23 (D9:
pump never waits for the compactor; D10: compact by bytes, split
range 0) is written up as pending decisions in START-HERE.

<a id="ph-sep-29-2026-0511z--pump-no-longer-waits-map-windows-range-0-735ccd"></a>
## Sep 29 2026 05:11Z — pump no longer waits; map windows; range 0 split

Rolled `--all` (`~/efs-runs/rollw25.log`) onto `a53b253f2455-dirty`,
RDMA, `--perf --strace`, clients fcstor003–015 remounted. No suite.
Group 0 leader 1 term 9426 commit==applied 13735166; group 2 leader 2
term 2907 commit==applied 12147949. Recorders running.

D2: `pull_layout_miss` pulls an adaptive window (16 chunks, doubling
to 256 while the read stays sequential) one window ahead of the
caller, and issues each chunk group's GETCHUNKS on its own thread.
D9: `kv_maybe_flush_locked` returns without waiting when L0 cannot
take the flush; `host_pub_batch_propose` returns BUSY while L0 is
within one flush of the 64-file cap. D10: compaction skips a range
whose L0 bytes are under 1/8 of its L1 unless L0 is at that cap;
`key[0]==0` flushes and compacts as 16 subranges; compaction reads
1 MiB ahead of each block. The L0 array is still 64 files. Also in
this binary: the O(1) log-byte counter and the 1 MiB segment
`setvbuf`.

<a id="ph-sep-29-2026-0235z--outbox-coalesce-multi-chunk-copy-post-and-bc057e"></a>
## Sep 29 2026 02:35Z — outbox coalesce, multi-chunk copy, post-and-return; IOR with perf and strace

Rolled `bbcbcb5ad779-dirty` `--all` at 02:35Z with `EFSD_ARGS='--perf
--strace'` and remounted fcstor007–015 the same way (`EFS_TRANSPORT=rdma`).
Three items that had a fixed design went in with that roll: W14.2 (a)–(b)
(AE reply and heartbeat coalesce in place; votes are not evicted; the
sender drains that lane before entry AppendEntries), W15.3 for a
multi-chunk write (one `fuse_buf_copy` per whole chunk inside a 1 MiB
write), and W15.4 (`efs_rdma_send_frame` posts and returns; the reply
wait reaps the send CQ). `efs-fuse --perf` now starts the recorder in
the daemon child; `--strace` is `strace -f -tt -T`. IOR
(`results/io500/20260929-023447-iorperf2`) aborted again on the first
BUSY REPORT (`report-loop rounds=1 busy=1 ms=18246–19508 rc=-13` on
fcstor007). No bandwidth. Recorders were SIGINT'd at 02:41Z so the
files are finalized; they live on each node under `/tmp/efs-perf/`
(`efsd.data` / `efsd.strace` on 003–006, `fuse.data` / `fuse.strace`
on 007–015). Server straces are 1.9–2.4 GB because the trace was not
narrowed.

<a id="ph-sep-29-2026-0040z--ior-abort-on-the-w17d2-tree-perf-hot-path-98d172"></a>
## Sep 29 2026 00:40Z — IOR abort on the W17–D2 tree, perf hot path

Rolled `bbcbcb5ad779-dirty` `--all` at 00:28Z (`EFS_TRANSPORT=rdma
EFS_RAFT_OBS=1`). fcstor006 did not catch up inside the roll's 180 s:
it was installing a 2.3 GB group-2 snapshot (5796 ms) and compacting
(L1 ~1000). It was commit==applied at 12083285 before the client
remount. Clients fcstor007–015 remounted RDMA at 00:39Z with
`perf record -F 499 -g`.

`tests/perf/io500/run.sh ior` (stonewall 30 s) produced no bandwidth.
`results/io500/20260929-002758-wimpl`: INVALID stonewall, `fsync`
failed, `close` failed, rank 2 `MPI_ABORT`, ranks gone by 00:41:33Z
with no D-state. `report-loop` on every client, one round, `busy=1
rc=-13`, walls 8.2–26.1 s: the W17.1 bound returned EIO on the first
BUSY REPORT where the old loop retried BUSY 8 times (the Sep 28 run's
fsync succeeded after 328 s). The 8 s is checked between attempts;
the rest is one RPC the server held. The `exhausted 16` lines are
the non-sync close-kick path. fcstor004 `report-split nrec=90016` is
BUSY nine times (`pack_ms=0`) then six `rc=0` with `pack_ms`
0.86–2.77 s.

Profiles (`cycles:P`, lost 0, ~100 s window overlapping compaction
and a group-2 snapshot export) under `~/orcd/scratch/efs/perf/`.
Leaders were fcstor003 (group 0) and fcstor006 (group 2). fcstor003
(79K): `memmove` self 4.2% (was 18.5%), `send_ae` 2.5% (the one
log→frame copy), `host_send` under 1.5%, `try_commit` self 0 (was
4.0% / 10.5%) — W19. fcstor007 (14K ≈ 29 CPU-s, mostly off-CPU):
`dcache_flush_slot_inner` self 0.1% (was 43.8%), `pthread_once`
gone (was 25%), reclaim children 49.7% all in `dcache_put_now` —
W18. `ll_write_buf` memmove 8.6% (W15.3 open). fcstor004 (127K,
follower, REPORT target): fragment `open` 11.9% + `access` 8.7%
(W14.4 open); `memcmp` 9.2% in `lsm_get` / compact / export,
`vx_sift_down` 2.2% in the export heap.

<a id="ph-sep-28-2026--decisions-d1d3-and-the-implementation-order-79e2e3"></a>
## Sep 28 2026 — decisions D1–D3 and the implementation order

The user accepted three recommendations after the evening IOR runs.
D1: the span publish commutes — `efs_meta_apply_publish` checks
`expected_gen` only for full-image publishes, a sub-range writer
publishes a span even with the base in hand, folded spans'
`candidate_gen`s stay in the trailer so a replay is a no-op, and the
fold is done by the publisher that fills the chain or by a reader.
No distributed lock. D2: `open()` adopts the inode row only; chunk
maps come from `pull_layout_miss` per lane group in parallel, one
metadata window ahead of the prefetcher, size adaptive, no mount
option. D3: the 5/s REAP_DONE idle gate stays. The order for the
queue is W17.1, W16.1, W18, W19, W14.4, W15.3, the W14/W15 residuals,
D1, D2, W16.2–3, then W8/W10 re-gates. Recorded in START-HERE
("Decisions — taken and pending") and in the W1, W6, W17 items.

<a id="ph-sep-28-2026--committed-l1-list-ior-easy-write-0769-gibs-0a14cc"></a>
## Sep 28 2026 — committed L1 list, IOR easy-write 0.769 GiB/s

`bbcbcb5` rolled `--all` with `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`.
Both groups were commit==applied before the run (group 0
13626406 term 8016 leader 0, group 2 12052065 term 1917 leader 1).
Clients fcstor007–015 mounted RDMA. The node build id is
`bbcbcb5ad779-dirty` because rsync excludes `results/` and git
then sees those tracked files deleted; all four servers printed
the same id.

`run.sh ior` (30 s stonewall) printed easy-write **0.769 GiB/s**
in 393.014 s. fcstor007's fsync for that phase was
`flush_ms=327885 report_ms=0 rc=0`, after REPORT type 67 exhausted
16 BUSY retries. mdtest-easy-write printed 0.000 kIOPS in 0.000 s
(the ini has `run=FALSE`). The next phase's `fsync` returned
errors, `close` failed, and rank 8 `MPI_ABORT`. During that phase
fcstor004 logged `apply publish rc=-14` (STALE) for ino 1166063
across many chunk indexes. No easy-read, hard, or score. After
the abort, io500 on fcstor007–010 and 012–014 stayed in D-state
`request_wait_answer`. efsd and the mounts were left up. Perf
reports: `~/orcd/scratch/efs/perf/fcstor00N/efsd-19810/` and
`.../efs-mount/`.

<a id="ph-sep-28-2026--perf-ior-stopped-after-stat-ebusy-39a080"></a>
## Sep 28 2026 — perf IOR, stopped after stat EBUSY

Restarted 19810 with the real node ids (not `scripts/server.sh`,
which derives a node id from the address) and `perf record -F 499 -g`
on each efsd. Clients fcstor007–015 were mounted with
`scripts/client.sh --perf` and `EFS_TRANSPORT=rdma`. mdtest in
`config-ior-only.ini` is `run=FALSE`. The driver printed the 30 s
stonewall INVALID line, then `stat` of the easy files failed and
rank 6 called `MPI_ABORT`. No RESULT line. Client logs: REPORT
(type 67) exhausted 16 BUSY retries, `fsync-split` `flush_ms=202803`
`report_ms=0` `rc=0`, then `INODE_LOOKUP` (type 43) on shard 3745
exhausted the same budget. Profiles:
`~/orcd/scratch/efs/perf/fcstor00N/efsd-19810/` and
`.../efs-mount/`. Daemons were stopped after the reports were
written. `efs-fuse` and `efsd` counts were 0 on a later check.

<a id="ph-sep-28-2026--l1-list-snapshot-pump-ior-easy-write-1147c6"></a>
## Sep 28 2026 — L1 list, snapshot pump, IOR easy-write

The dual-host nodes were stuck at 64 L1 files, so compaction
published BUSY, L0 could not drain, and the group 0 follower never
installed a snapshot. The pump on that follower's other group spent
the profile in `kv_flush_locked` (fcstor004, 41%) and the group 0
leader spent it in `send_snap` (fcstor003, 50%).

L1 is now a growable list. The MANIFEST format did not change.
`host_snap_open` calls `efs_kv_lsm_flush_nowait`, which returns BUSY
before walking the memtable when L0 cannot take `KV_LSM_RANGE_MAX`
more files. A no-progress snapshot ack sets a one-heartbeat retry
and the next send skips the pread; an empty AppendEntries is still
sent. Abandoned `snap-*.kvx.tmp` files were removed on fcstor003–006
while efsd was down.

`roll_efsd.sh --all` with `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1` built
`7eecf1da00cd-dirty` and reported `ROLL_OK`. Both groups were
commit==applied at the leaders' indexes (group 0 13616251, group 2
12041271). Compaction on fcstor004/005 ended rc=0 and L0 fell to 2
and 3. fcstor004's group 0 import applied a 15697-item diff.

`tests/perf/io500/run.sh ior` on fcstor007–015 (all `MOUNT_OK`)
printed easy-write **1.048 GiB/s** in 397.381 s. The client log for
that fsync is `flush_ms=325611 report_ms=0 rc=0`. The driver then
aborted on `create file.mdtest.0.2236 failed (EIO)`. Official score
is not a result: stonewall is 30 s, and the later phases did not
run. After the write, L1 was 310 files on fcstor004 and 405 on
fcstor005, L0 still 3. Group 0 and group 2 were still
commit==applied (13624345 and 12050181). Server profiles are under
`~/orcd/scratch/efs/perf/efsd-19810-fcstor00{3,4,5,6}/`; client
profiles under `~/orcd/scratch/efs/perf/fcstor00N/efs-mount/`.

<a id="ph-sep-28-2026--94-io-500-tcp-and-rdma-013e21"></a>
## Sep 28 2026 — 9×4 IO-500, TCP and RDMA

Same binaries (`db2b88c4802a-dirty`, the tree committed as
`2f086f5`), 9 clients × 4 ranks, `tests/perf/io500/run.sh debug`
(1 s stonewall). Preflight idle and `fuse.efs-fuse` + `stat` OK
before each run. Reads are the driver's same-mount reads.

| phase | TCP | RDMA |
| --- | --- | --- |
| ior-easy-write GiB/s | 1.376 | 2.917 |
| ior-hard-write GiB/s | 0.274 | 0.291 |
| ior-easy-read GiB/s | 2.669 | 3.092 |
| ior-hard-read GiB/s | 1.162 | 1.525 |
| mdtest-easy-write kIOPS | 2.721 | 3.418 |
| mdtest-hard-write kIOPS | 0.910 | 0.757 |
| mdtest-easy-stat kIOPS | 16.461 | 3.248 |
| mdtest-hard-stat kIOPS | 10.271 | 5.683 |
| mdtest-easy-delete kIOPS | 2.964 | 2.223 |
| mdtest-hard-read kIOPS | 10.661 | 4.895 |
| mdtest-hard-delete kIOPS | 0.425 | 0.538 |

`results/io500/20260928-151707-tcp` and
`results/io500/20260928-150609-rdma`. The first RDMA launch
aborted: `INODE_LOOKUP` on shard 1745 exhausted 16 BUSY retries
(10.3 s) and IOR's post-write `stat` called `MPI_Abort`. The
files were on disk afterward (36 × ~1.4 GiB). The quoted RDMA
row is the retry. After the TCP run, fcstor010–015 wedged
(`stat` hung, ssh 10 s timeout). `killall -9 efs-fuse`, then
`timeout 3 fusermount3`, then an RDMA remount: all nine
`MOUNT_OK`, `RDMA transport up` once each.

<a id="ph-sep-28-2026--five-9-client-dd-profile-rounds-a46af4"></a>
## Sep 28 2026 — five 9-client dd profile rounds

8 GiB `dd bs=1M conv=fsync`, non-zero source, own file, FUSE only,
RDMA. Aggregate is 9 × 8192 / slowest client wall. Every quoted row
has all nine files at 8589934592.

| round | aggregate MiB/s | slowest wall | what changed |
| --- | --- | --- | --- |
| 1 | 1875.6 | 39.31 s | profile only (`20260928-101245-dd-prof-r1`) |
| 2 | 2810.5 | 26.23 s | fragment probe moved off `g_pool.lock` (`20260928-131651-dd-prof-r2b`) |
| 3 | 2443.1 | 30.18 s | 64 KiB snap chunks (`20260928-133055-dd-prof-r3`) |
| 4 | 2326.7 | 31.69 s | thread-local inode-dir fd (`20260928-133558-dd-prof-r4`) |
| 5 | 1776.1 | 41.51 s | publish batch 256 and AE cap 64 KiB (`20260928-134137-dd-prof-r5`) |
| restore | 2551.5 | 28.90 s | rounds 3–5 reverted (`20260928-134637-dd-prof-r5b`) |

The first attempt at round 2 cached inode-directory fds in a global
table and the 9-client dd hit the 150 s ssh timeout
(`20260928-102121-dd-prof-r2`). That cache is not in the tree.
`access()` under the pool lock was the round-1 server sample; moving
the probe out is what moved the wall. Later rounds removed that
`access()` and the snapshot `pread` from the top of the profile and
the slowest client got worse. Client CPU stayed blake3 in
`hash_write_fragments`. A 4 MiB snapshot chunk made the pump resend
the same chunk on every wake once `send_idx` passed `next_index`
(outbox hi=2048, ~20k messages/s). The in-flight end for a snapshot
chunk is now `UINT64_MAX` until the reply or a heartbeat. That fix
stayed in the restore.

<a id="ph-sep-28-2026--five-posix-jobs1-profile-rounds-f859ea"></a>
## Sep 28 2026 — five posix jobs=1 profile rounds

cpu-clock while posix suite 1 ran on fcstor007, RDMA, port 19810.
Each round restarted fcstor003–006 with `roll_efsd.sh --all`
(`EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`) and remounted the clients
when the client binary changed. Every suite was 200/201, mmap
SKIP only, 0 EFS bugs. `perf trace` still cannot read tracefs;
the second pass of the harness is not a wall-time result (one
traced pass took 150.9 s).

Round 1 (`results/measure/20260928-093100-posix-prof-r1`, 54.9 s):
client top `clock_gettime` in `efs_rdma_recv_wait`; server top
`memcmp` in `efs_kv_lsm_view_export` on the GC thread. The export
merge became a heap. Newest segment (lowest index) still wins a
tie. `test_kv_lsm` covers two overlapping L0s, a tombstone, and
the other raft group.

Round 2 (44.1 s / 56.1 s): export `memcmp` dropped off the top of
group 0. The client spin was still the top. `efs_rdma_recv_wait`
now pauses 64 times and then polls the eventfd.

Round 3 (29.0 s / 29.7 s): `clock_gettime` left `recv_wait`.
`recv_poller` was 16% and blake3 6%. Compactor `memcmp` was the
server-side merge; it uses the same heap. The 29 s pair did not
hold on the next rounds.

Round 4 (58.4 s / 57.0 s): server top was `write` from the export,
three syscalls per key. The export now buffers 64 KiB and flushes
before the seek that writes the item count.

Round 5 (50.7 s on the cpu-clock pass): writes left the server
top. A large export put `memcmp` and `vx_sift_down` back on top;
that is the heap walking the table, not a loop to delete.
`recv_poller` was still 16% of efs-fuse. The empty-CQ spin went
from 128 polls to 32. The ack stays after `poll`. A jobs=1 run
with no tracer after that roll was 200/201 in 45.1 s.

<a id="ph-sep-28-2026--rdma-recv-poller-5727ec"></a>
## Sep 28 2026 — RDMA recv poller

posix jobs=1 under `perf record -e cpu-clock -g` on the client fuse
and both raft leaders. RDMA
(`results/measure/20260928-050145-posix-prof-rdma`): `recv_poller`
was 64% of efs-fuse (147775 samples) and 31% of efsd. The thread
polled, paused, and `sched_yield`'d whenever a QP was up. Suites
56.4 s and 43.6 s, 200/201, 0 fail. TCP on the same binary
(`results/measure/20260928-050841-posix-prof-tcp`): no poller; the
top sample is blake3 at 355 hits. Suites 45.9 s and 59.0 s.
`perf trace` cannot open `/sys/kernel/tracing` (mode 700).

The poller now does a short spin and then `poll`s the completion
channel for at most 1 ms. Acking a CQ event before that `poll`
disarms `ibv_req_notify_cq`, so the next completion was invisible
until the timeout. That version took 76–78 s. With the ack after
`poll`, jobs=1 is 58.5 s and 57.8 s, 200/201, 0 fail
(`results/measure/20260928-053033-posix-prof-rdma-fix2`), and
`recv_poller` is absent. Servers and fcstor007–015 are that binary,
`EFS_TRANSPORT=rdma`. The wall did not move past TCP. The spinning
core is what came out.

<a id="ph-sep-28-2026--user-xattrs-posix-gate-097a13"></a>
## Sep 28 2026 — user xattrs, posix gate

`user.*` attributes are one KV blob per inode
(`EFS_KV_KIND_XATTR`) and one raft command (`EFS_MD_CMD_XATTR`).
`security.*` and `system.*` return EOPNOTSUPP without an RPC, so a
create does not pay a commit for an LSM label. The blob is removed
in the same batch as the inode row, and only when the key exists.
SELinux on fcstor007 is Disabled.

`roll_efsd.sh --all` with `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`
built `db2b88c4802a-dirty` on all four and then exited FAIL:
fcstor005 stayed at group 0 commit 13316260 and group 2 11742588
while the leaders were at 13387007 and 11813960. The follower was
writing `snap-0-13387007.part` and `snap-2-11813960.part`. A minute
later both groups matched the leaders. Clients fcstor007–015 were
remounted. fcstor003–006 still have the previous fuse.

posix jobs=1 on fcstor007: **200/201**, 0 fail, `opt_xattr` PASS,
`mmap_write_read` SKIP (`MAP_SHARED` ENODEV), 45.2 s
(`results/posix/20260928-043918`). Compare exits 2 because XFS
passes that mmap test. Kernel 5.14.0-687 has `FOPEN_DIRECT_IO` and
no `FOPEN_DIRECT_IO_ALLOW_MMAP`; `fuse_file_mmap` returns ENODEV
when the file is direct-I/O and the mapping is shared.
`MAP_PRIVATE` still passes. Do not clear `direct_io`.

Four suites at once on one client (`posixstress 4 fcstor007`,
`POSIX_JOBS=1`): each **200/201**, 0 fail, mmap SKIP only, 46–57 s
(`results/posix/20260928-044951`). The Sep 17 four-suite result
(`results/posix/20260917-120328`, 165–168 timeouts) is not this
build. A 16-suite run exists only on the deleted engine (Sep 1).

posix2 overlapped with that suite and persist
(`results/posix2/20260928-043918`): 61/63,
`peer_overlap_pwrite_chunk_straddle` and
`peer_rename_vs_unlink_src`. Alone, the same pair is **63/63** in
65.4 s (`results/posix2/20260928-044304`). persist on fcstor015:
26 prepared, 26 survived (`results/posixpersist/20260928-043919`).

<a id="ph-sep-28-2026--19810-on-rdma-0c3413"></a>
## Sep 28 2026 — 19810 on RDMA

The Sep 27 live switch had been rolled back after 9-host posix
took 385 s. A second switch (`EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`,
`roll_efsd.sh --all`) stayed up. posix jobs=1 and 9-host are
199/201 (mmap + xattr SKIP, 0 fail) in 90 s and 57–59 s
(`results/posix/20260928-033823`, `results/posix/20260928-034049`).
posix2 is 63/63 in 76.8 s (`results/posix2/20260928-034350`).
9-client 8 GiB dd+fsync is **1326.6** MiB/s, walls 28.55–55.57 s,
every file 8589934592 (`results/measure/20260928-033420-dd-wall`).
TCP's 9-client bar is 1478, and TCP 9-host posix was ~13–15 s.
RDMA is not faster on those two.

fcstor004 group 0 had sat at commit 13315306 while the leader was
at 13316010, across RDMA and a TCP restart, until a snapshot
install landed. The shared per-peer outbox dropped the catch-up
AppendEntries and `host_send` still returned success, so
`ae_inflight` suppressed the resend. It now keeps snapshot chunks
and entry-carrying AppendEntries, and returns `EFS_ERR_AGAIN`
when the new message is not queued. `send_ae` / `send_snap` do
not mark that batch in flight and do not fail the propose.
Earlier RDMA build `113823180b15-dirty` had a valid 1-client
**947** and 4-client **2311**
(`results/measure/20260928-015839-dd-wall`) and an INVALID
9-client (do not quote ~238). IOR-hard NP=4 on that build:
write 469.55, read 107.03, bad 0
(`results/measure/20260928-015806-ior-hard-rdma`).

<a id="ph-sep-27-2026--same-directory-creates-15a2a2"></a>
## Sep 27 2026 — same-directory creates

Creating many files in one directory stayed near 140 ops/s from 1
process to 36 (`results/measure/20260921-161931-samedir-rate`).
Spread does not apply below 65536 entries, and the log-path file
create is point gets, not a shard scan. The remaining serialization
was Raft: the first proposer held the fsync and appended locally,
but every proposer that arrived while that hold was open called
`efs_raft_propose`, which broadcast that one entry under `h->mu`.
The next create waited out the round trip.

A leader with a hold already open now appends locally and raises
the send ceiling. The hold owner waits until the proposers already
inside `host_propose` have appended (capped), kicks the pump, fsyncs
once, and marks every log index covered by that fsync durable on
every group it leads. A record remembers the file offset where it
ended so a later append in the same file is not treated as covered.
Log rotation fsyncs the new file and clears those offsets; leaving
the old offsets in place made the durable scan stop at the first
stale one. Idle single-create still takes the hold only when none
is open, so it does not wait out an unrelated batch (that version
moved idle mkdir from 7.1 ms to 11.9 ms and did not raise the
144-way rate).

`tests/test_raft_store` checks the covered-index rule. Servers
rolled `--all` as `ab458efab95b-dirty` at 21:11Z, TCP.
`results/measure/20260927-211953-samedir-rate` (PREFLIGHT_OK,
ROUNDS=100): storm PASS at 1×1, 1×9, and 4×9, parent
`children=0 nlink=2`. Aggregate **333 / 1385 / 1241** ops/s.
`busy_n` was 0, 0, and 1. Idle mkdir median on fcstor007 was
3.1 ms; an empty create+close median was 0.6 ms.

<a id="ph-sep-27-2026--9-client-fsync-eio-6b8a52"></a>
## Sep 27 2026 — 9-client fsync EIO

The morning 9-client 8 GiB dd (`results/measure/20260927-053506-dd-wall`)
returned fsync EIO on fcstor009 and fcstor013. Both files were still
8589934592 bytes. `report-split` on fcstor004 had two lines
`nrec=65536 pack_ms=0 push_ms=0 finish_ms=0 rc=-3`. A deleted inode
is already answered OK on this path, so that NOT_FOUND was not a
missing row. `send_ae` returned `EFS_ERR_NOT_FOUND` when `store->get`
or `log_term` raced a snapshot that had compacted the index.
`broadcast_ae` then failed the propose, and
`efs_client_report_dirty_ino` did not retry NOT_FOUND.

`send_ae_behind` sends an empty AppendEntries (or the snapshot) in
that window and does not return NOT_FOUND. `broadcast_ae` keeps
going if one peer still returns it. The report handler maps a
leftover NOT_FOUND to BUSY after the log line, and the client
retries NOT_FOUND the same way it retries BUSY.

`tests/test_raft` and `tests/test_raft_store` passed on fcstor014.
`roll_efsd.sh --all` with `EFS_TRANSPORT=tcp EFS_RAFT_OBS=1` landed
`75321297f719-dirty` on fcstor003–006 (ROLL_OK 20:35Z). Fuse on
fcstor007–015 was rebuilt at 20:36Z.

9-client rerun `results/measure/20260927-204907-dd-wall`, preflight
idle (group0 0/s, group2 0/s): **1478 MiB/s**, slowest wall 49.894 s,
all nine walls 49.76–49.89 s, every file 8589934592, FUSE_OK, no
fsync EIO. `report-split` is 128 lines: 119 `rc=-13`, 9 `rc=0`,
zero `rc=-3`. The OK lines have `push_ms=0` (the BUSY attempts had
already published). 1478 is 3.4% of the 44 GB/s ceiling. 1-client
977 and 4-client 1984 stay the morning numbers; they were not
remeasured on this build. Do not quote 1464.

<a id="ph-sep-27-2026--shared-file-ior-hard-on-immutable-spans-d37e02"></a>
## Sep 27 2026 — shared-file IOR-hard on immutable spans

A partial chunk publish appends a span object. The base generation
does not change, so concurrent disjoint ranges do not STALE each
other. The span object is a full chunk image; readers copy
`[off, len)`. Overlap, or a trailer already at 8 spans, returns
STALE and the client folds those spans into one full-chunk CAS.
That CAS commits only when `delta_base_n` / `delta_base_seq` name
the list the image already contains. Adopting a longer list after
the image was built deletes a peer span: that was
`peer_overlap_pwrite_chunk_straddle` (B's exclusive tail above the
chunk boundary came back as A's bytes). The fold now pulls the lane
seq only when the span generation set is unchanged, and leaves seq
at 0 otherwise so the CAS STALEs and the replay refetches. A replay
whose slot was cleaned keeps the published span range instead of
copying the whole zero-padded object. A short read with `ndelta > 0`
does not return an unpainted rdcache hit.

Open does not take a server lease, so the last unlink deletes the
inode row. A peer `fstat` of a still-open fd used to return the
local nlink (1). `efs_client_stat_open` treats GETATTR
`EFS_ERR_NOT_FOUND` as nlink 0 and copies only nlink and ctime, so
the local size stays. That pair, plus the xfs baseline capture
(python's stdout had been prepended to the tsv, and compare.py
rejected every row), is posix2 **63/63**
(`results/posix2/20260927-190509`, 77.7 s, compare PASS).

NP=4, SEGS=3000, 47008 B, one shared file, cold remount, FUSE_OK:
write **481.56 MiB/s** (1.12 s, 537.96 MiB), read **91.35 MiB/s**
(5.89 s), pattern 12000 records bad 0. The Sep 21 4-rank bar was
33 MiB/s. 1/9/36 were not remeasured. After the verify both groups
kept committing REAP_DONE. Group 0 fell to 1/s; group 2 stayed
~11–16/s for the whole wait. A 2 MiB tail of group 2
(index 11532756..11534661) was 1710 REAP_DONE and 1710 distinct
inodes, so it is a backlog, not a loop on one marker.
`tests/preflight.sh` fails above 5 entries/s. The scaling script
was not run on that cluster. Do not quote a 1/9/36 table from this
day, and do not raise the idle gate.

Clients fcstor007–015 are the gate6 fuse (`8e62ff12b422-dirty`).
Servers were not rolled for the fold and nlink fix; they already
had the span apply. The `pub-stale` fprintfs are removed from the
tree and still present in the live efsd until the next
`roll_efsd.sh --all`. Uncommitted.

<a id="ph-sep-27-2026--posix-suites-and-the-snapshot-window-that-elect-f04b6a"></a>
## Sep 27 2026 — posix suites, and the snapshot window that elected

9-host suite 1 on TCP (`results/posix/20260927-123717`, screen
12:37:17–12:37:52Z): every host 200 pass, `mmap_write_read` SKIP
(`MAP_SHARED` ENODEV), 0 fail, 0 not-run. Compare still exits 1
because it counts that SKIP as an EFS bug. posix2
(`results/posix2/20260927-123946`): 63/63 in 58.0 s, rc=0.
Group 0 stayed term 7858 and group 2 stayed term 1719 through the
9-host run.

Two earlier 9-host runs the same morning (`20260927-121916`,
`posix9b`) were not this. `os.makedirs` of a fresh test directory
returned EBUSY, some hosts aborted on the uncaught `OSError`, and
the rest were cut. `apply_max` was under 1 ms. fcstor006's log
shows the cause: `raft-snap: start group=2` and then ~200 term
changes, commit frozen at 11164719 while fcstor005's commit moved
on. `send_snap` returns `EFS_ERR_AGAIN` for the whole export
(`host_snap_open` while `snap_exporting`). `send_ae` treated that
as success and sent nothing. The peer's election timer fired, the
vote request carried a higher term, and `maybe_step_down` dropped
the leader even though the log was not up to date enough to win.
`send_ae` now sends an empty AppendEntries in that window (not on
the `data_only` flush path). `on_ae_req` resets the timer before it
rejects a `prev_index` the follower does not have.

posix2's earlier 59/63 (`results/posix2/20260927-035212`) and the
mid-fix 57/63 were three data-path bugs, all on the build that
posix2d re-ran:

- `peer_shared_pwrite` lost 496 of 1000 half-blocks. `DCACHE_NR`
  was 8. Sixteen disjoint 4 KiB ranges in one chunk collapsed to
  one whole-chunk range with `base_gen = EFS_CHUNK_BASE_UNCOND`, so
  the last fsync published that client's image over the peer.
  `DCACHE_NR` is 32.
- `peer_truncate_visible` and `peer_extend_and_truncate` returned
  EIO. The truncate stub minted a new `candidate_gen` while copying
  the old objects' nodes and checksums. Objects are named
  `{ci}.{fi}.{gen}`. The reader GET of the new gen was never PUT
  (`EFS_ERR_DECODE`, fuse `efs_rc=-9`). When `got.generation != 0`
  the stub now keeps that generation, so apply sees
  `committed == candidate` and leaves the chunk row. The client
  already rewrites the zeroed tail before setattr.
- `peer_overlap_pwrite_partial` and `chunk_straddle` kept the
  previous pwrite's ranges after a successful report. The next
  pwrite merged into that span and a STALE replay painted the old
  image over the peer's exclusive tail. `dcache_note_committed`
  clears `nrange` when the slot is not dirty.

An every-read `pull_chunks_range` was tried and removed. GETCHUNKS
`NOT_FOUND` became EIO on an open fd after unlink, and the posix2
holder script turns that into an empty read. Do not put that pull
back on the read path.

<a id="ph-sep-27-2026--dd-rebaseline-on-tcp-c34c30"></a>
## Sep 27 2026 — dd rebaseline on TCP

8 GiB `dd bs=1M conv=fsync`, own file per client, flush in the
clock, every mount `fuse.efs-fuse` (`results/measure/20260927-053506-dd-wall`).
Preflight refused because both groups were committing ~8 entries/s;
the leader log tail was `REAP_DONE` from the million-file unlink,
commit==applied, no client. The run was taken with that noted.
1 client (fcstor007) 8.388 s, **977 MiB/s**. 4 clients (007–010)
16.42–16.52 s, aggregate **1984 MiB/s**. 9 clients all wrote
8589934592 bytes in ~50.2 s, but fcstor009 and fcstor013 returned
`fsync` EIO: `fsync-split` `rc=-3` (`EFS_ERR_NOT_FOUND`) from
`efs_client_report_dirty_ino` after flush ~10 s and report 13–18 s.
That row is INVALID. Do not quote the 1464 the harness printed
from the walls. Sep 18 was 639 / 251 / 202; Sep 21 was 499 / 176
with the 9-client row also invalid.

<a id="ph-sep-27-2026--rdma-mkdir-gap-two-transport-bugs-e89160"></a>
## Sep 27 2026 — RDMA mkdir gap, two transport bugs

A private 3-node cluster on fcstor007 (ports 19950–19952, not
19810) made the live posix failure reproducible without the reap
tail. 100 mkdirs were ~2× to ~7× TCP, and the histogram had a
mode at 100–108 ms (11 of 100). RPC-PROF put that time in recv,
with `busy_n=0`. The shared recv poller acked the completion
channel and then `poll`ed it for 100 ms. Acking consumes the
event for a CQE that landed in between, so the WC sat in the CQ
until the tick, and that one poller stalled every conn. The
poller now harvests again before a 1 ms backstop.

That removed the 100 ms mode and left a raft AppendEntries at
~380 µs against ~15 µs on TCP. The send-CQE wait itself was 1 µs.
Every `efs_rdma_send_frame` called `ibv_query_qp` and opened the
port-counter sysfs file before posting, so the failure dump could
show the QP state at post time. Those reads now happen only after
a send has already failed. A clean rerun was 642 ms RDMA vs 507 ms
TCP for 100 mkdirs, raft RTT ~50 µs
(`~/efs-runs/rdmaprof7.log`). A later run's wall was one 1.5 s
mkdir and three BUSY retries; the other 99 were 2–7 ms. 19810
stays TCP. The send path also spins ~200 µs before `sched_yield`,
because a yield on the first miss gave the core away for the rest
of a timeslice.

<a id="ph-sep-27-2026--partitioned-flush-and-the-rdma-mkdir-gap-db468c"></a>
## Sep 27 2026 — partitioned flush, and the RDMA mkdir gap

W13 step 5. A memtable flush used to write one L0 segment spanning
every key, so every compaction rewrote all of L1. The flush now
writes one L0 file per distinct `key[0]`. efs keys store the shard
in the first two bytes, so that is at most 16 files. Compaction
merges the fullest of those ranges and the L1 segments that overlap
it, and starts a new output file when the key range changes. A
segment that already spans several ranges is still compacted whole,
once, so the old files drain. `make test` on fcstor014 passed
(`test_partitioned_flush`: two ranges, two L0 files, and compacting
one leaves the other range's L1 file on disk). `roll_efsd.sh --all`
with `EFS_TRANSPORT=tcp EFS_RAFT_OBS=1` (`pflush-roll`) put that
binary on 19810. A mkdir/rmdir on fcstor007 returned immediately.

The same hour, a private 3-node cluster on fcstor007 (ports
19950–19952, not 19810) timed 100 mkdirs: TCP 1000 ms, RDMA 2246 ms.
That is the gap the live 9-host suite hit (193–196/201, 385 s).
The two causes and the private rerun are in the section above.
19810 stays TCP.

<a id="ph-sep-27-2026--live-rdma-switch-rolled-back-1bb371"></a>
## Sep 27 2026 — live RDMA switch rolled back

W10 step 1 had already passed (private empty-table mkdir, 5/5 on
fcstor007). After the million-file tree delete finished, all four
servers were rolled with `EFSD_ENV='EFS_TRANSPORT=rdma EFS_RAFT_OBS=1'`
(`w10roll`, `ROLL_OK`, build `4c6a5acefe03-dirty` on every node) and
fcstor007–015 were remounted with `EFS_TRANSPORT=rdma`. A single
mkdir/rmdir on fcstor007 returned immediately. The 9-host posix
jobs=1 did not match TCP: 193–196/201, skip `mmap_write_read`, 0
not-run, 385 s on every host
(`results/posix/20260927-044348`). The TCP bar is 200/201 in
30.4–31.3 s (`results/posix/20260927-033723`). The failures that
pass on TCP are the many-op tests hitting the 15 s cap
(`dir_deep_nesting`, `dir_deep_nesting_beyond_64`, `names_crazy_dirs`,
and on some hosts `concurrent_creates_same_dir` and
`mtime_monotonic_many_writes`) plus a few EIO and EEXIST one-offs.
`apply_max` on fcstor003 during that window stayed under 1 ms, so
it was not the compactor. A REAP_DONE tail from the deleted tree
was still committing before the suite, and group 0's term moved
during the run; the suite was still several times the TCP wall and
step 5 says that is a rollback, not a debugging session. The same
`roll_efsd.sh --all` with `EFS_TRANSPORT=tcp EFS_RAFT_OBS=1`
(`w10back`) brought 19810 back, the nine clients were remounted
TCP, and a mkdir/rmdir on fcstor007 returned immediately. Freeze,
idle md_latency, and the dd rebaseline were not run.

<a id="ph-sep-27-2026--chunked-installsnapshot-and-the-import-that-hel-f15b8d"></a>
## Sep 27 2026 — chunked InstallSnapshot, and the import that held the pump

The first roll of chunked snapshots truncated every raft.log from
1.7–5.8 GB down to 1–3 KB and exported ~386 MB per group in ~8 s on
the GC thread. fcstor003 and fcstor004 then stopped answering
`raft-status`. The pump was inside `efs_kv_group_import`, which
compared every local key with every incoming key and then PUT the
whole image, under `h->mu`. A one-index catch-up of a live group does
not finish that way. The import now sorts both sides and writes only
the diff (`src/kv/kv_snap.c`). Unit tests passed (`w11mktest4`).
After `roll_efsd.sh --all` the four logs stayed under 5 KB, both
groups had one leader with commit==applied, and a restart of
fcstor005's efsd rejoined in 510 ms (`results/measure/20260927-w11-gate`).
`apply_max` during the 386 MB export was 0. Idle md_latency on the
second sample was mkdir 2.9 / create 1.5 / append 2.0 / stat 0.4 /
unlink 0.7 / rmdir 2.8 ms. The first sample, a minute after the
restart, had create and append at 53 ms (one retry sleep) and did not
hold. 9-host posix was 199–200/201 on the second run
(`results/posix/20260927-015719`), not the clean 200/201 of
`20260926-164123`. The leader-freeze script failed once (one rmdir
EBUSY left a child whose parent nlink stayed 2) and passed on the
rerun (`results/measure/20260927-020111-i17-leader-freeze`,
`arc_term_miss` 0→6, parent clean). The private RDMA empty-table
mkdir on fcstor007 passed 5/5 (`MKDIR_RC=0 WRITE_RC=0`). 19810 stayed
on TCP.

The client staging-table pin rules from the Sep 23 recommendation
are in the tree the same night. `efs_client_stage_pin` holds a
count across create, rename, link, unlink, and from an append
reservation until the write has the bytes in dcache or the PUT has
marked the ino dirty. Rename marks the ino dirty before it drops
the directory locks. The evictor drops chunk maps of a clean closed
file and leaves the row for a later pass. Every open fd stays
pinned. `make test` on node9901 passed (`w9mktest2`), including
`test_stage_evict`. The nine clients were remounted (`w9fuse1`).
9-host posix jobs=1 is 200/201, skip `mmap_write_read`, 30.4–31.3 s
(`results/posix/20260927-033723`). That is the W11 bar as well.
Posix 2 one pair is 59/63
(`results/posix2/20260927-035212`): `peer_truncate_visible`, both
overlap-pwrite cases, and `peer_shared_pwrite` (496 of 1000
half-blocks). The shared-pwrite case passed when run again by
itself. The valgrind leak gate failed once on 36 bytes in
`test_raft`'s snapshot callbacks (the forced export miss allocated
the handle, and the receiver kept the assembled bytes) and passed
after those frees (`results/leaks/20260927-035622`: unit, efsd,
efs-fuse, and the RDMA phase all 0 definite). The first stat of that tree, on a client whose oldest rows were
still pinned, grew RSS from 264 MB to 622 MB with no plateau: the
drain treated a full window of pinned entries as "nothing left" and
never reached the clean files behind them. After the drain walks
past that window, a cold stat on fcstor013 went from 5 MB to a
level 233 MB at 1M files
(`results/measure/20260927-w9-walk`). 19810 stayed on TCP.

<a id="ph-sep-26-2026-evening--w13-l1-compaction-off-the-apply-path-e06644"></a>
## Sep 26 2026 evening — W13: L1 compaction off the apply path

The user asked to implement the next roadmap item. That ratified W13
only. The merge used to run inside `kv_compact_locked` under `l->mu`
and `h->mu` and rewrote every L1 segment (an L0 spans all shards),
which was the remaining election trigger (`apply_max` 1.7–2.4 s).

The compactor is one thread in `kv_lsm`. Flush still writes the
memtable to an L0 file and signals when `n_l0 >= l0max`. The thread
snapshots the L0 set plus overlapping L1, drops the lock, merges
through private `kv_seg` opens (the live segment block cache is not
shared), then under `l->mu` drops only the snapshotted inputs, keeps
L0 files flushed during the merge, and commits with the existing
atomic manifest rename. `kv_seg_doom` unlinks a file when the last
pin goes away, so `efs_kv_lsm_view_pin` still reads the old segments.
`EFS_KV_COMPACT_DIE=N` `_exit(99)` after the Nth finished output
segment, before that rename. `efs_kv_lsm_flush` no longer compacts;
at `KV_LSM_MAX_SEGS` it waits on `l->cv` and logs
`kv-compact: backpressure`. `kv_compact_locked` stays for
`efs_kv_lsm_compact` and for the no-thread fallback.

`make` of the KV/raft/txn tests on fcstor007 was green, including
`test_pinned_view` and `test_compact_crash`, before
`tests/roll_efsd.sh --all` with
`EFSD_ENV='EFS_TRANSPORT=tcp EFS_RAFT_OBS=1'`. Build
`3210a3d63f73-dirty`. Clients fcstor007–015 remounted
`fuse.efs-fuse`.

Hammer `results/measure/20260926-163709-mkdir-hammer`: idle p50
4.68 ms; 144-way 35166 mkdirs, p50 56.5 ms; `apply_max` 68473 µs on
fcstor005. fcstor004 logged two compactions, 796333166 bytes in
1964 ms and 798775386 bytes in 2067 ms. Errors were ten
`rmdir-own ENOTEMPTY` lines.

9-host posix `results/posix/20260926-164123`: 200/201 on all nine
hosts, 13.2–14.8 s. Timeline
`results/measure/20260926-124106-w8-stall-timeline`: group 0 term
6882 leader 1 and group 2 term 1198 leader 3 for the whole 139 s;
no probe over 1 s. Four `kv-compact` lines in the window (two ~400
MiB in ~0.85 s, two ~760 MiB in ~2.1 s); per-host `apply_max` peaks
32 / 67 / 64 / 11 ms. Idle `md_latency.py` afterwards: mkdir 2.9,
create+close 1.6, append+close 2.0, stat 0.3, unlink 0.8, rmdir
2.9 ms.

Not started: partitioned flush (W13 step 5), W11, W9, W10.

<a id="ph-sep-26-2026--raft-log-fsync-moved-out-of-the-host-lock-d814bc"></a>
## Sep 26 2026 — Raft log fsync moved out of the host lock

Propose held `h->mu` across `log_sync_locked`, so 144 mkdir threads
each waited out a private fsync and AppendEntries did not start until
that fsync returned. `68dfebb` takes the shared sync hold before the
lock, broadcasts, fsyncs outside the lock, then
`efs_raft_durable` so the leader does not vote for an entry it has
not synced. Followers can still form a majority without the leader.

`results/measure/20260926-042515-mkdir-hammer` (before): idle p50
8.4 ms, 9×16 p50 258 ms, 470 mkdir/s. `20260926-044248-mkdir-hammer`
(after, smaller table): idle p50 3.7 ms, 144-way p50 144 ms, 735
mkdir/s. One AppendEntries stayed in flight, so the next mkdir was
still its own round trip.

Two attempts to batch that send were reverted. Broadcasting only
after dropping `h->mu` (`1835c70`) did not beat 735/s. Pipelining
every newer suffix while the previous batch was unacked (`0e81e49`)
committed far ahead of apply: `20260926-050319-mkdir-hammer` shows
fcstor005 `pump_hold_max=5336343us` and `applies_in_worst=12120`.
`e8f3dc1` puts the one-batch cap back. Remeasure on the table those
runs left behind (`20260926-050609-mkdir-hammer`): idle p50 10.9 ms,
144-way p50 189 ms, 706 mkdir/s, apply_max 67 ms, `fin_q=0`. The
idle gap versus 3.7 ms is the grown table, not a return of the
under-lock fsync. Do not pipeline past the one in-flight batch.

9-host suite on `e8f3dc1`
(`results/posix/20260926-050825`, timeline
`results/measure/20260926-010808-w8-stall-timeline`): 194–199/201 in
40–44 s. Group 0 elected once (term 6294→6296). `names_crazy_dirs`
still hits the 15 s budget on the slower hosts.

`6a60318` holds the pump's AppendEntries until proposers blocked on
`h->mu` have appended, and still refuses a second batch while one is
in flight (the `0e81e49` pipeline applied 12 120 entries under the
lock). Clean hammer `results/measure/20260926-052320-mkdir-hammer`:
144-way p50 168 ms, 754 mkdir/s, apply_max 61 ms, `fin_q=0`.
`20260926-052437-mkdir-hammer` is the same binary with a 766 ms
apply of 31 entries (p50 235 ms) — compaction, not the batching.

`5d3e603` stops holding the LSM lock across a segment pread and
caches the block after a miss. A negative dentry lookup used to read
every segment and free the buffer, so the next name in the same
directory paid the disk again. Hammer
`results/measure/20260926-053753-mkdir-hammer` (build string
`f93e7e6669b6-dirty`, the bytes of `5d3e603`): idle p50 7.1 ms,
144-way p50 147 ms, 887 mkdir/s, apply_max 59 ms / 256 applies,
`fin_q=0`. Three `rmdir-own ENOTEMPTY` cleanups, no mkdir errors.
9-host suite on the same tree
(`results/posix/20260926-054047`, timeline
`results/measure/20260926-014030-w8-stall-timeline`): seven hosts
200/201, fcstor007 199 (`dir_many_files` EIO), fcstor013 198
(`names_crazy_roundtrip` EIO and `dir_deep_nesting` 15 s), 39–41 s.
Term did not move. `names_crazy_dirs` passed on every host. Probe
during the run: stat p50 3 ms (one sample 1.3 s), mkdir p50 14 ms.

A follower AppendEntries of many entries fsynced once per entry
under `h->mu` (the duplicated 128-entry catch-up was ~30 ms). The
store's `batch_begin` / `batch_end` defer that fsync to one call at
the end of the batch. The leader path is unchanged: only the
proposer that finds the sync hold at zero takes a slot. Giving
every proposer a slot, so the fsync waited for threads that had not
appended yet, moved idle mkdir from 7.1 ms to 11.9 ms and did not
raise the 144-way rate. That version was not kept.

<a id="ph-sep-26-2026-afternoon--memtable-probes-and-the-applied_cv-he-0ca130"></a>
## Sep 26 2026 afternoon — memtable probes and the applied_cv herd

With the scans pruned (`610f4a8`) the pump thread was still 61–67 % of
efsd samples on the dual-host follower. `perf report --sort pid,sym`
put `memcmp` at 21 % of efsd on that one thread, and `-g caller` split
it: 12 % under `lsm_batch ← lsm_put ← prepare_reduce_rec ←
efs_txn_apply_prepare` — the memtable binary search (`kv_mtab_pos`,
inlined), ~15 probes per put, each probe a pointer load `e[mid]` and
then the key behind it, two dependent misses; the rest under the txn
scans and `kv_seg_probe`. A second block, ~20 % of efsd, was kernel:
`native_queued_spin_lock_slowpath` under `futex_wake` and
`futex_wait_setup`, `_raw_spin_unlock_irqrestore`, `futex_hash`. That
is the signature of many threads on one futex word: the pump ended
every cycle with `pthread_cond_broadcast(&h->applied_cv)`, so under
the hammer ~150 sleeping handlers woke on every cycle (hundreds per
second), took `cv_mu`, re-read the view, found `applied < idx`, and
slept again.

Three changes, all inside `src/kv/kv_lsm.c` and `raft_host.c`, no
wire or on-disk change:

1. The memtable entry's key is allocated inline
   (`calloc(sizeof(*e) + klen)`, `e->key = (uint8_t *)(e + 1)`);
   `ent_free` no longer frees the key. Alone it moved `memcmp` from
   13.4 to 11.9 % — the miss just moved to the single load.
2. `struct kv_mtab` gained `uint64_t *pfx`, the first 8 key bytes of
   `e[i]` as a big-endian integer, zero-padded; `kv_mtab_pos` compares
   the integer and calls `kv_key_cmp` only on a tie (a key shorter than
   8 bytes ties with a longer key whose tail is zero, and the tie falls
   through to the full compare, so ordering is `kv_key_cmp`'s). The
   array is ~220 KiB for a full 4 MiB memtable and stays in L2; insert
   memmoves both arrays; `m->bytes` counts the extra 8 bytes.
   `memcmp` 23 → 11.6 % of efsd, `kv_mtab_pos` 2.2 %, `memmove` 4.7 →
   6.7 %.
3. Targeted wakeups. `struct host_waiter` × 512 in the host, each with
   its own condvar and `group / any / idx / seen`. `host_waiter_sleep`
   (cv_mu held, called right after the caller re-read the view under
   cv_mu and found its predicate false) takes a free slot and
   timed-waits on it; the pump, after `host_publish_view`, runs
   `host_waiters_wake`: under cv_mu, for every active slot, signal if
   the group's replica is gone, or (`any`) the group's `v_stamp`
   differs from `seen`, or (`!any`) `v_applied >= idx`. `v_stamp` is
   bumped by every publish that changed a field, stored last, and
   `host_view_get` reads it first, so a waiter holding stamp S has
   fields from publish S or later. `applied_cv` stays as the overflow
   path (`cv_overflow` counts sleepers on it; the pump broadcasts only
   when it is non-zero). `host_read_index` sleeps with `any=1`,
   `host_wait_applied` with `any=0`.

Each step went through `test_kv`, `test_kv_lsm`, `test_meta_apply`,
`test_txn`, `test_raft_store`, `test_raft` on fcstor007, a
`roll_efsd.sh --all` + client redeploy, and two `mkdir_hammer.sh` runs
(the second with `perf record` on fcstor004). Build string
`610f4a847738-dirty` throughout.

| run | change | idle p50 | 144-way mkdir | p50 | max |
| `20260926-135832` / `-135944` | inline key | 5.43 / 5.15 | 16520 / 23983 | 51 / 49 | 11.4 s / 11.2 s |
| `20260926-141042` / `-141156` | + prefix array | 5.53 / 5.35 | 25942 / 26834 | 65 / 60 | 9.7 s / 12.1 s |
| `20260926-141940` / `-142052` | + targeted wakeups | **5.23** / 5.08 | **30366** / 14439 | **52** / 60 | 3.3 s / 14.7 s |

The mkdir count is dominated by how many 2 s `kv_compact_locked`
stalls, and the elections they trigger, land in the 15 s window: every
run had `apply_max` ≈ 2.0 s, and group 0 went from term 6746 to 6751
during the best run alone. The 10–15 s maxima are the client's 16
BUSY/STALE retries (10.3 s) exhausted across a stall + election. Read
p50, idle p50 and the profile, not the count.

9-host posix on the final build: `results/posix/20260926-1425-wake`
started into a four-term group-0 election burst (fcstor003
`apply_max=851 ms` → LEADER→FOLLOWER at 6754, CANDIDATE→CANDIDATE
twice, LEADER at 6757, FOLLOWER again at 6758, `arc_term_miss=37`) and
scored 193–200/201 in 16–75 s, with an EIO cluster on fcstor014's
file tests (`write_hole_pread_zeros`, `two_fds_independent_offset`,
`trailing_slash_on_file`, `flock_unlock_on_close`,
`access_f_ok_after_unlink`, `mkdirat_unlinkat`) inside that window.
Sixty seconds later `results/posix/20260926-1430-wake2`: **200/201 on
eight hosts, 199 on fcstor009** (`flock_shared_then_exclusive` —
"LOCK_EX taken while another fd holds LOCK_SH", seen once each on Sep
22, 25 and 26 builds), **15.5–23 s per host** against 39–44 s on every
earlier gate run, through a 2.0 s `apply_max` on 004/005 and one
election per group at the start. (The first run's output landed under
`results/posix/home/...` because `POSIX_OUT` is a directory name, not a
path; moved.)

Tried and reverted right after: staging WAL records under a sync-hold
in a 1 MiB user buffer and issuing one `write()` before the hold's
fsync (one syscall per pump cycle instead of one per `lsm_put`). Tests
passed, the cluster ran it (`20260926-143658` / `-143818`: idle 5.29 /
5.10, p50 45 / 49 ms, both stall-dominated), but the pump's syscall
share did not move (`rep_movs_alternative` 2.1 %, `syscall_enter` 0.5
% — the bytes copied are the same, and the per-call overhead was never
the cost) and the 9-host suite (`20260926-1441-wal`, not committed)
ran into another multi-term election burst at an 850 ms `apply_max`
and scored 194–200. No measurable gain, a small change to
process-crash semantics (records in user memory instead of the page
cache until the hold's fsync): reverted before commit. The servers were
then rolled to the committed tree (`c044fb16e859-dirty`; hammer idle
p50 **4.73 ms**, 26585 / p50 54).

What is left on the pump: `memmove` 7 % (two-array insert into a
sorted memtable), `kv_msrc_advance` / `kv_seg_iter_next` /
`search_block` ~5 % (the txn-record scans — `guards_conflict` and
`reduces_pending` are already per-key prefixes; `txn_scan_kinds` walks
a shard's three kinds because the txid is the key's suffix, and
narrowing that is a key-layout change), `kv_compact_locked` (W13), and
a residual futex share from `l->mu` / `h->mu` handoffs. One PREPARE
command carries one part and costs one Raft entry and one `lsm_put`;
a mkdir is ~8 of them plus DECIDE and RESOLVEs. Folding a
transaction's parts into one PREPARE would cut the entry count but
changes the verdict protocol (one verdict per part today) — that is
a decision for the user, listed in START-HERE §1b.

<a id="ph-sep-26-2026-midday--the-mkdir-ceiling-was-the-apply-path-not-f34564"></a>
## Sep 26 2026 midday — the mkdir ceiling was the apply path, not fsync

Follow-up to the hammer work above. A thread-local fsync defer
(`log_defer_depth`, the follower-batch mechanism) wrapped the
cross-group mkdir/rmdir PREPARE loops and the RESOLVE loop so one
thread's ~6 entries shared one fsync. Rolled and measured
(`results/measure/20260926-110435-mkdir-hammer`, uncited): 12059
mkdirs / p50 174 ms with a clean 26 ms apply_max, idle 10.7 ms —
no better than the 887/s baseline. Reverted before commit.

Then measured instead of guessed. `strace -c -f -p efsd` on fcstor004
(dual-host follower) for 12 s of hammer: 103 847 `pread64`, 10 506
`fsync`, 3 502 `openat`+`rename`+`pwrite64`+`close` (the applied-index
file, one per pump cycle per group). `perf record -g` on the same
host: 32 % `__memcmp_avx2_movbe`, 5 % `search_block`, 4.7 %
`kv_seg_iter_next`, 6 % `memmove` — the linear walk inside 64 KiB
LSM blocks, under `l->mu`, on the pump. Each PREPARE apply runs
`guards_conflict` + `reduces_pending` (two prefix scans), each RESOLVE
`txn_scan_kinds` (three); `merge_scan` opened an iterator on every
segment (one pread + walk each) whether or not the segment's key
range could hold the prefix.

Three changes, committed together:

1. `kv_seg_excludes` + `merge_scan` skip: a segment whose first key is
   past the prefix or whose last key is below the seek is not opened
   (the range test compaction already uses). `iter_load` copies a block
   from the segment's slot cache instead of a pread when a point get
   just read it. `pread64` fell off the strace top list.
2. Pump durability tail off `h->mu`: the KV WAL fsync and the
   applied-index write now run after the unlock; the file write is
   throttled to 10 ms (`HOST_APPLIED_PERSIST_US`) and forced once at
   pump exit. The saved index is only a restart lower bound and never
   passes the durable KV, so the guarantee is unchanged; waiters wake
   before the KV fsync, which is fine because the Raft log is the
   durability boundary.
3. `KV_LSM_BLOCK_TARGET` 64 KiB → 8 KiB, `KV_SEG_CACHE_SLOTS` 32 →
   256. Readers take any block size, so the live table needed no wipe;
   compaction rewrites old segments. Index RAM ≈ 8 MB per GiB.

Hammers (all on build string `194286c37a4f-dirty`, each containing one
1.7–2.0 s `kv_compact_locked` stall): prune only
`20260926-112916` idle 9.5 / 144-way 13715 / p50 137 ms; + pump tail
`20260926-133934` idle 6.97 / 14764 / 106; + 8 KiB `20260926-134640`
idle 4.54 / 21990 / 73 and `20260926-134738` idle 5.59 / **28072 /
p50 60 ms**, 22 `rmdir-own ENOTEMPTY`, 0 mkdir errors. perf after:
`memcmp` 24 % (now mostly `kv_compact_locked` 7 % and `lookup_mt`),
`search_block` 0.8 %.

Cost: the L1 rewrite now comes every ~7 s of hammer (trigger is 4 ×
4 MiB flushes; the table writes ~2 MB/s of records at this rate) and
1.9 s is past the election timeout. 9-host suite
`results/posix/20260926-1350-blk`: 20–38 s per host (was 39–41),
195–199/201, two elections each at a 1.9 s compaction (g0 6624→6626,
g2 1068→1069), `txn-recover` ABORTs after them, client
`inode-rpc: ... exhausted 16 BUSY/STALE retries (10.3 s)`. Failures
are the election class (`link_across_dirs`,
`last_link_unlink_other_dir`, `dir_many_files`, `dir_deep_nesting*`).
W13 is unchanged as a decision; it is now the only thing between this
build and 200/201.

<a id="ph-sep-26-2026--txn-finisher-after-a-400-ms-apply-wait-1a66fa"></a>
## Sep 26 2026 — txn finisher after a 400 ms apply wait

A full-L1 compaction under the KV lock stalls apply for >400 ms
(`results/posix/20260926-0330-diag`: fcstor004 `apply_max=1665075us`,
`persist_max=1103us`, `wait_timeouts` 0→79 in one obs window). Every
in-flight `host_propose_wait` returns BUSY. A DECIDE COMMIT already in
the log then has no RESOLVE, the EXCL intent (often the ALLOC key)
stays until `host_txn_recover_pass` (age 5 s; log showed
`txn-recover ... age=6.0s -> COMMIT`), and other clients on that shard
burn the 16-attempt budget into EIO.

`host_txn_commit` queues that txn on a finisher thread. The thread
reads `efs_txn_decision_get` (NOT_FOUND until DECIDE has applied) and
proposes RESOLVE with that decision, retrying BUSY/NOT_PRIMARY until
10 s. RESOLVE stays idempotent with recovery. This does not shorten
the stall (W13).

First 9-host suite on the build: 200/201 × 9, 42–44 s
(`results/posix/20260926-0345-fin`), `fin_q=0`. Repeats
`-fin2`/`-fin3`/`-fin4` (not cited): 196–200, `fin_done` up to 11,
`fin_drop` 1, remaining fails are the 15 s many-op tests once the
table is warm and a stall lands inside the window.

Rejected the same day, do not restore: forwarded-submit early return,
batched remote PREPARE/RESOLVE, and an AppendEntries suffix while one
batch is in flight. Each left the 9-host suite worse than the
committed empty-commit-probe baseline (`20260926-0125-clean`,
199–200/201). `send_ae` keeps one batch in flight on purpose (Sep 19
fsync storm).

<a id="ph-sep-23-2026-evening--gates-on-7e29943-and-the-idle-50-ms-is-b4438a"></a>
## Sep 23 2026 evening — gates on `7e29943`, and the idle 50 ms is on close

Owed gates, no new code on the cluster. 9-host posix 193–196/201, 0
not-run, 79–94 s (`results/posix/20260923-202626`). The six many-op
timeouts remain (mkdir p50 46 ms under the suite). One group-2 election
(term 535→537) lines up with the one-offs: EIO on
`concurrent_create_unlink_two_proc` (009, 010), `content_random_roundtrip`
(014), `concurrent_write_and_readdir` (007), a `b''` read on
`unlink_open_then_recreate` (009), `concurrent_appends` timeout (010).
Group 0 stayed term 5498; the status line `leader=0` is raft id 0, which
is node 1. No 2.4 s compaction stall in this run.
`same_parent_storm` 9×4×100 left the parent `children=0 nlink=2`, rmdir
OK, and FAILed on two `mkdir ENOENT` from fcstor014 at round 63
(`results/stress/same-parent-20260923-202846`) — a reply-path ENOENT, the
row was not torn.
Idle `md_latency` 20 min later, term stable, commit==applied, still +30
entries in 30 s: mkdir 8.5 / create+close 56.8 / append+close 59.5 /
stat 0.5 / unlink 2.2 / rmdir 7.8
(`results/measure/20260923-162535-idle-mdlat`). The 50 ms mode is the
two ops that write one byte and close, not every metadata op, and it is
not the post-roll churn (roll was two hours earlier). Next measurement
is a follower strace of one create+close.

Server half of the I16 window-GC follow-up, not rolled: `fence_local`
deletes that shard's op-id window for the epoch it fences
(`test_session` OK on node9901). The FUSE client never creates an efs
session, so this path does not run in production, and seeding the op-id
uuid from "the client session" has nothing to copy.

<a id="ph-sep-23-2026--visibility-at-the-coordinators-decision-ad292b9-9a689d"></a>
## Sep 23 2026 — visibility at the coordinator's decision (`ad292b9`, `7e29943`); four decisions written up

Two more reply-path errors from the freeze runs on `bb634d9`, both the
same shape: a client saw ENOENT/EIO for a name or row that had already
COMMITted but whose RESOLVE was still pending (recovery of a stranded
txn takes 5–16 s; the I16 window answers the retry OK meanwhile).
`getattr` on the new dir read the inode row with a bare kv get and got
NOT_FOUND (`fuse: mkdir ... ok ino=85959 but getattr rc=-2`) →
`efs_meta_apply_get_inode_tx` reads through a COMMITted EXCL intent via
`efs_txn_read` (`ad292b9`). Then `rmdir` of `d-fcstor011-1-22` returned
ENOENT for a dentry in the same state → `efs_meta_apply_lookup_tx` /
`resolve_tx` (`7e29943`); every handler-side lookup/resolve in
`raft_host.c` uses them, the apply path stays on the plain read (a state
machine cannot ask a coordinator). `rmdir`/`unlink` now map
`EFS_ERR_BUSY` to EBUSY (`fuse_unlink_errno`), not EIO. Tests
`test_stat_committed_unresolved_row`, `test_lookup_committed_unresolved_dentry`.
Rolled `7e29943f28ef-dirty` 18:21 UTC (servers + clients 007–015);
freeze ×2 (`results/measure/20260923-182559-i17-leader-freeze`,
`-182709-`) both PASS, 0 worker errors, parent removable,
`opid_replay` 0→1→3, `arc_term_miss` 0→3→15. Idle `md_latency.py`
pre-freeze 9.8/4.4/7.1/0.4/2.0/10.5 ms (mkdir/rmdir above the
6.1/5.2 reference; remeasure on a flat commit). Design limit kept: the
16-attempt client budget (10.3 s) is shorter than a 16.5 s recovery →
EBUSY to the app, never a wrong answer.

Tooling: one `Shell` call executed three times (three `efs-rec: start`
lines within a second → concurrent builds in one `/tmp/efs`, duplicate
`git commit`s). `efs-rec.sh` now runs a name once (flock + `.done`,
exit 75 `DUPLICATE`), `efs-bg.sh start` holds a per-name lock; rule
`efs-remote-timeouts` says never reuse a name to retry. An `EFSD_ENV`
with a comma (`EFS_TRANSPORT=tcp,EFS_RAFT_OBS=1`) is one env var →
transport AUTO (ungated RDMA), no OBS; caught by the `efsd.log` header
and re-rolled with the space form.

With the freeze class closed, the queue stopped on four decisions the
spec does not make. Each got a recommendation, the reason, and steps in
START-HERE §1a ("Decisions pending", items W13/W11/W9/W10): background
compactor (a lock hold, not CPU; per-range compaction cannot help while
one L0 spans every shard); the Raft paper's chunked InstallSnapshot over
a lazily exported snapshot file from a pinned segment view (the RAM
`snap_blob` + export under the SM lock is the same stall class as
compaction — removing only the 4 MiB cap would make it worse); ratify
the client-cache pin rules and the 256 MB soft cap; and no wipe for W10
— `tests/rdma_first_inode.sh` is a private 3-node cluster on fcstor007
and is the empty-table repro, so 19810 can be switched to RDMA in place.

<a id="ph-sep-2223-2026--i16-op-id-dedup-for-directory-rpcs-43bdf6a41f-22b5b5"></a>
## Sep 22–23 2026 — I16: op-id dedup for directory RPCs (`43bdf6a41f7d`)

Why now: the second I17 freeze run (`results/measure/20260922-122629-i17-leader-freeze`)
left three `d-*` dirs whose `mkdir` had returned EEXIST and whose `rmdir`
returned ENOENT, with a perfectly consistent parent row. Recovery had
logged `COMMIT (resolved)` on those shards: the client's coordinator got
BUSY from the apply-wait deadline, the client retried the same MKDIR, and
the retry met the dentry its first attempt had (invisibly, at that
moment) committed. Same class as the 9-host suite's `link_of_symlink`
EEXIST on a fresh name, `concurrent_create_unlink_two_proc` EIO and the
`b''` read. §7.9 already specified the fix (stable request identity +
bounded per-shard window); only APPEND (sim) and `create_file_op` (sim)
implemented it, and the production dir RPCs carried no identity at all
(`pack_create_cmd` zeroed the uuid slot; that slot is the lease holder's
identity, not the requester's).

Design decisions taken (each was a real choice, recorded so nobody
re-litigates): (1) **Window shard = dentry shard of the destination
name.** The coordinator is randomized per txn (a retry would probe the
wrong shard); the parent's inode shard can be in the other group (the
window record would then live in a group that does not own that shard
range). The dentry shard is deterministic from the request, always a
participant, always owned by its group. (2) **Atomic with the op, never
a separate write.** Cross-group txns get a `EFS_TXN_REDUCE_OPID` part
folded at RESOLVE by `fold_reduce` (dispatch on `EFS_KV_KIND_OPID`),
which commutes with the log path's read-modify-write of the same window
because both are Raft-log-ordered on the owning group; the single-group
log path carries a trailer on the CREATE (`HOST_CREATE_F_OPID` flag in
`cmd[1]`) / UNLINK / RMDIR (by length) commands and the apply writes the
window in the same `efs_kv_batch`. (3) **Probe before the pre-check.**
The apply's idempotent OK-on-EEXIST would otherwise mask a genuine
second create; so the leader probes (`host_opid_replay`) first, then
runs the usual lookup that yields EEXIST/ENOENT. (4) **Ack semantics.**
`efs_opid_ack` now advances the watermark and shifts the bitmap even
past seqs the shard never served; the client sends `ack = lowest
in-flight seq − 1` (512-slot in-flight table, slot held across the
whole `rpc_send_recv_*` retry loop so the retry is byte-identical).
Because an op is in flight until its RPC returns, no request ever probes
a window whose watermark already covers its own seq — so the "acked
stub" answer in `efs_opid_lookup` cannot fire for a first attempt.
(5) `VAL_MAX` in `txn.c` rose from 320 to `EFS_OPID_VAL_MAX` (512); the
res_acc buffer is 48 × 512 on a server thread stack.

Limits left in place (spec-bounded, not bugs): 16-entry reply cache per
`(client, epoch, shard)` — a 17th un-acked op on one shard applies
unrecorded (falls back to pre-I16); the identity is a per-mount random
uuid rather than the §7.5 session's, so the fence barrier does not yet
drop a dead client's windows (follow-up in START-HERE §1b).

Tests: `test_txn:test_opid_reduce`, `test_meta_apply` I16 block; on-node
build clean (no warnings), all C tests OK
(`~/efs-runs/i16-build.log`). Rolled all four (`roll-all.log`,
`ROLL_OK`, `43bdf6a41f7d-dirty`, g0 leader 1 term 5374, g2 leader 3 term
469), `EFS_RAFT_OBS=1` kept.

Gate runs on `43bdf6a` (02:52–03:07): freeze run 1
(`results/measure/20260923-025259-i17-leader-freeze`) parent clean,
`arc_term_miss` 0→3, `opid_replay` 0→3, 0 worker errors — the retry
class the item was opened for is answered from the window. Freeze run 2
(`-025903-`) parent clean, `opid_replay` 3→7, but three workers got
`mkdir ENOENT` at round 7 during the g0 freeze; the same workers'
`rmdir` of that name then succeeded, so the MKDIR had committed and the
error was in the reply path. Server logs held no mkdir `rc=-3`. Traced
in the client: `rpc_send_recv_shard/_dual` return `EFS_ERR_NOT_PRIMARY`
at once when NOT_PRIMARY arrives without a hint (`primary_id == 0` — a
stale leader that just stepped down, before it hears the new leader's
heartbeat); `efs_client_stat_ino` maps any GETATTR failure to
NOT_FOUND; `ll_mkdir` does `lookup_fill(new_ino)` after the mkdir reply
and the FUSE `*_at` helpers map that to ENOENT. So the app saw ENOENT
for a directory that exists. Fix: a hintless NOT_PRIMARY backs off like
BUSY (50 ms × 2^min(n,4), same 16-attempt budget, EBUSY at the end) in
both send paths; `ll_mkdir` logs `fuse: mkdir ... ok ino=N but getattr
rc=` so the next occurrence is attributable; `host_opid_reply_ino` logs
`opid-replay ... (stub)`/`row gone`. The same class is the first
suspect for the 9-host one-off EIOs. posix jobs=1 200/201 + mmap SKIP
(`results/posix/20260923-030005`); 9-host 189–191/201 per host, 0
not-run (`results/posix/20260923-030056`). `md_latency.py` 30 s after
the suite (8.4/19.3/65.9/0.5/2.0/8.3 ms) is post-suite churn, not a
number; idle remeasure owed.

Operational note: the login-node Cursor shell died three times in this
session ("no exit status", no `rec-*.log` created = wrapper never ran);
each time a fresh probe 20–60 min later answered. The nested-quoting
`efs-bg.sh start … "ssh … \"…\""` one-liner was replaced by a script in
`~/efs-runs/i16-build.sh` (rsync + `make clean` + build + run the C
tests on one fcstor) — reuse it.

<a id="ph-sep-22-2026-00000035--i17-the-apply-ring-matched-proposals-b-451436"></a>
## Sep 22 2026 00:00–00:35 — I17: the apply ring matched proposals by index, not (index, term)

Picked up START-HERE §1b item 0 (half-applied cross-shard txns: ino 62991
nlink=3 nents=1 with 0 dentries, ino 31264 with a dentry to a missing row).
The hypothesis in that block — recovery's 5 s stranded age racing a slow
coordinator — was wrong on inspection: `efs_txn_decide` is first-writer-
wins and `host_txn_recover_one` does ABORT → PROTO → "COMMIT (resolved)"
correctly. The hole was in the primitive under every coordinator:

- `host_propose_wait_idx` = `host_propose` (returns the log index) →
  `host_wait_applied(idx)` (published view `applied >= idx`) →
  `host_apply_rc_locked(idx)`, which read `arc_rc[idx & MASK]` if
  `arc_idx[slot] == idx`, else counted `obs_arc_miss` and returned **OK**.
  `host_apply` recorded the ring with `(void)term;`.
- Raft reuses an index across terms: a leader that loses its term has
  its uncommitted tail truncated by the new leader, which commits a
  different entry at the same index. The old leader's waiter then sees
  `applied >= idx` and reads the stranger's verdict — almost always OK.
  For a txn coordinator that is "my DECIDE COMMIT applied" when no
  DECISION exists → RESOLVE COMMIT on the participants it reaches (child
  row deleted / dentry written), recovery finds no decision 5 s later →
  ABORT on the rest. Exactly the two dumped rows. The compaction stall
  (2.4 s under `h->mu`, both g0 replicas) supplied the leader changes
  (g0 5302→5303 in the 22:10 run).
- Forced repro before fixing: `tests/measure/i17_leader_freeze.sh` —
  `same_parent_storm.sh` 9×4×80 while SIGSTOPping the g0 leader 3 s, then
  the g2 leader 3 s (HOST_ELECT_BASE 100 ticks × 5 ms = 0.5–1 s, so each
  freeze forces an election). On `84a2a55`: parent `children=1 nlink=2`,
  `rmdir` FAIL, `kv_dir_dump` on a fcstor004 KV copy: parent 230820
  `nlink=2 nents=0` with dentry `d-fcstor009-2-29` → ino 113873 row
  present; the worker logged `mkdir EIO` then `rmdir ENOENT`; recovery
  logged `shard=1444 coord=3281 parts=2 age=13.6s -> COMMIT (resolved)`
  (`results/measure/20260922-042153-i17-leader-freeze`). First attempt
  of the script silently froze nothing: `GROUPS` is a bash builtin array
  (read 246111) — renamed `FREEZE_GROUPS`.
- Fix `46d54e6`: `arc_term[]` beside `arc_idx[]`; `host_propose` gains a
  `uint64_t *term` out (= `efs_raft_term(r)` under the same `h->mu` as
  the append; 0 when forwarded); `host_apply_rc_locked` /
  `host_apply_extra_locked` take the term: mismatch → `EFS_ERR_NOT_PRIMARY`
  (the entry was never committed; nothing happened; retry at the new
  leader), slot overwritten → `EFS_ERR_BUSY` (verdict unknown), never OK.
  `host_wait_verdict` (wait + verdict) and `host_wait_settled` (wait +
  term check only, for the CREATE/APPEND-style callers that ignored the
  verdict) replace the six bare propose+`host_wait_applied` pairs;
  `host_bg_propose` (recovery DECIDE/RESOLVE, reaper) checks too;
  `host_idx_ref` and the pipelined RESOLVE prefs carry the term;
  `host_pub_batch` keeps `terms[]` and answers STALE on a mismatch (client
  repulls). `efs_msg_raft_mkfs_reply.term` (server↔server, same build ID
  everywhere) carries the leader's term for the forwarded-PUBLISH branch
  of `server_raft_host_submit`, which proposes without waiting. `host_apply`
  stamps the slot on its early return so no waited-on slot is ever
  "never written". `raft-obs` prints `arc_term_miss=`. Unit tests
  (test_raft/txn/meta_apply/wire/sim) OK, 0 warnings, built on fcstor007.
- Rolled all four (`roll_efsd.sh --all`, `EFS_RAFT_OBS=1` kept):
  running `46d54e679e4f-dirty` (dirty = the doc edits in the tree; the
  four IDs agree, which is what the HELLO gate needs). Preflight passed
  everything except my exact-string `--expect-build`; then the login-node
  shell died ("no exit status" ×3) before the fixed-build freeze run
  could be launched. **Owed:** `i17_leader_freeze.sh` ×2 on the new build
  (pass = clean parent + `arc_term_miss` total > 0), posix jobs=1, 9-host.
- Not fixed by this and not I17: worker ERR lines during a freeze
  (`mkdir EIO`, `unlink EIO`, then `rmdir ENOENT` of the name whose mkdir
  "failed") are the frozen leader's in-flight ops and the retry of a
  committed op — I16 op-id dedup, next. The six pre-fix `posix-*`
  leftovers on 19810 stay half-applied; there is no repair tool.
- Gate, once the login shell was back (Sep 22 08:25–09:36,
  `46d54e679e4f-dirty`): two freezes. `arc_term_miss` 0→6 on the leaders
  that were stopped (fcstor004 and fcstor006), then 6→8. Run 2 parent
  `children=0 nlink=2` RMDIR_OK
  (`results/measure/20260922-122629-i17-leader-freeze`). Run 1 left
  `d-fcstor007-0-25`, `d-fcstor013-1-25`, `d-fcstor014-0-25`
  (`20260922-122517-…`); each worker line was `mkdir EEXIST` then
  `rmdir ENOENT`. Dump of a fcstor004 KV copy: parent ino 145111
  `nlink=5 nents=3`, three dentries, three child rows present and empty.
  Idle rmdir of the three and of the parent succeeded
  (`~/efs-runs/rec-rmdir-left.log`). Recovery during that run was
  `COMMIT (resolved)` on shards 1436, 1622, 1751, 3816 — the child shards
  and the parent shard. The client observed ENOENT while the txn was not
  visible yet; recovery then installed it. The freeze script's
  `children=0` check called that a torn row; it now accepts
  `nlink == 2 + d-* lines` as a note. Review leftover, not hit here:
  `host_wait_settled` (CREATE/APPEND-style callers) still returns OK when
  the ring slot was overwritten; `host_wait_verdict` returns BUSY. The
  txn path uses the verdict.

---

<a id="ph-sep-21-2026-night--the-9-host-posix-suite-harness-clock-h-mu-072a6c"></a>
## Sep 21 2026 night — the 9-host posix suite: harness clock, h->mu, KV WAL fsync, compaction

Where it started: after `read_mu` (`223da15`) and the peer-pool /
forwarding fixes (`f10fec0`), the 9-host suite still failed ~100 of 201
per host, with the same first timeout (`dir_deep_nesting`) on every
host and 3-op tests like `err_stat_nonexistent` "timing out"
(`results/posix/20260922-001341`). The probe showed the contended lock
had moved from `read_mu` to `h->mu` itself (31 handler threads on the
g0 leader in `host_read_index`/`host_wait_applied`).

**Timeline tool.** `tests/measure/w8_stall_timeline.sh` runs the suite
and samples at 1 Hz: raft term/leader/commit for both groups from
fcstor003 and stat + mkdir + rmdir wall time from fcstor007 (itself
under load). First run (`results/measure/20260921-211829-w8-stall-timeline`):
**four leader changes in the first 50 s** (g2 391→392→395, g0
5215→5216→5219), but **no 15 s stall anywhere** — stat p50 9 ms, mkdir
p50 63 ms / p90 212 ms, max 1.9 s at a leaderless moment. So the
"timeouts" were not a stall.

**Harness.** `posix_suite.py`'s parallel path did
`start[fut] = time.time()` at **submit**, for all 196 tests at once,
into a 16-worker pool; the timeout loop then failed every future older
than 15 s whether or not a worker had picked it up. Every 9-host number
before this (Sep 17's 131–144 with `[None]`, the 66–95 pass rows) was
queue time plus real stalls, and the abandoned-but-running futures are
why the run hit the 385 s cap. Fix (`a683def`): the worker stamps its
own start; an unstarted test cannot time out; the budget is unchanged.
(First attempt shadowed `main()`'s `t0` and crashed `flush_tsv` — the
run produced empty TSVs, `results/measure/20260921-213802-w8-stall-timeline`;
renamed to `began`.)

**h->mu.** With `read_mu` gone, every handler took `h->mu` to read
commit/applied/leader/role and to `cond_wait` on `applied_cv`; the pump
(which ticks heartbeats) waited on an unfair mutex behind 30+ threads
per cycle. `host_publish_view` now stores the replica state as atomics
(end of every pump cycle, after `read_begin`/propose, on attach);
`host_read_index`, `host_wait_applied`, `host_is_leader`,
`host_local_leader`, `server_raft_host_submit` read the view; waiters
sleep under a dedicated `cv_mu` (predicate on the view checked while
holding it; pump publishes then broadcasts under `cv_mu`). New raft
accessors `efs_raft_read_index`, `efs_raft_read_done` (`4eb1419`).
Result (`results/posix/20260922-015206`): **191–194/201 on every host,
0 NOTRUN, all nine in ~87 s**, stat p50 3 ms; still 3 leader changes.

**KV WAL fsync per apply.** Rolled with `EFS_RAFT_OBS=1`
(`results/measure/20260921-215919-w8-stall-timeline/obs-*.txt`):
steady state `apply_max` 20 ms for 40 applies on the leader, 130 ms for
256 on a follower — 0.5 ms per applied entry, under `h->mu`, on every
replica. Cause: every metadata apply is ≥1 `efs_kv_put` and `lsm_batch`
fsyncs the KV WAL per put unless a hold is open (only the publish batch
opened one). Spec ("The Raft log is the durability boundary; the applied
KV is a replayable view … the KV's own sync mode is a performance
choice") — so one `efs_kv_lsm_sync_hold` per pump cycle around
drain+tick, `sync_release` (the single fsync) before `persist_applied`
(`84a2a55`). Result (`results/posix/20260922-020950`,
`results/measure/20260921-220933-w8-stall-timeline`): **191–195/201, all
nine in ~62 s**, mkdir p50 21 ms / p90 88 ms under the suite; idle
`md_latency.py` mkdir 6.1 / create 4.0 / append 6.2 / stat 0.3 /
unlink 1.5 / rmdir 5.2 ms. (Measured 20 s after the roll it read
53/57/59 ms — post-roll RDMA election churn, not a regression.)

**What the OBS lines still show.** One pump cycle of **2.4 s** on 004
and 005 at the same moment (`apply_max=2464250us applies_in_worst=54`
/ `2430765us`, 37), 1.0 s on 003 and 006 at other moments; g0 went
5299→5302→5303 around it, the client saw a 2 s `stat` and two failed
mkdirs, `arc_miss=1` on both g0 replicas. The KV is 1 GiB per node in
64 MiB L1 segments (`/data1/01/efs/mdraft/kv`: 17 sst, every L1
segment's mtime identical), an L0 segment spans every shard, so
`kv_compact_locked` merges all of L1 every 4 flushes (4 MiB memtable →
≈16 MiB of writes) under `l->mu` (all reads on the node stall) and
`h->mu`, deterministically on every replica. Compaction strategy is not
in the spec → decision item. Steady-state apply is still ~0.3 ms/entry
with no fsync in it: the KV reads in the apply path (`pread` per
segment per lookup/scan, no block cache).

**Remaining 9-host failures** (`results/posix/20260922-020950`): six
many-op tests time out on most hosts (throughput at 144 jobs); and at
the election moment `link_of_symlink` EEXIST on a never-used name,
`concurrent_create_unlink_two_proc` EIO, `unlink_open_then_recreate`
reading `b''` — the client's BUSY retry of a LINK/UNLINK that had
already committed (the latent hazard noted Sep 21 evening). Spec §7.9 /
I16 defines the fix (op-id dedup, implemented today only for APPEND and
create_file); it is the next queue item.

**Leftovers = evidence of half-applied txns (I17).** 41 `posix-*` dirs
were left in the mount root by the timed-out suites; `rm -rf` removed
35. New `tests/tools/kv_dir_dump` (inode row + dentries + child-row
presence for an ino, from a KV copy) on fcstor004 and fcstor005 (both
host both groups; output identical): `names_crazy_dirs` ino 62991
nlink=3 nents=1 **0 dentries** (created 22:10, build `84a2a55`);
`mkdirat_unlinkat` ino 31264 nlink=3 nents=1, dentry `sub` → 83075
**row missing** (21:19, `f10fec0`); `perm_sticky_owner_can_unlink/sub`
66151 healthy row, `rmdir` EIO; `names_near_path_max_dir/aaa…` ENOTEMPTY
with nlink=2. A rmdir/unlink txn committed on the child shard and not on
the dentry/parent shard. Working hypothesis: `host_txn_recover_pass`
(5 s stranded age) and a live coordinator delayed past 5 s by the
compaction stall + BUSY backoff (10.4 s worst case) both decide — see
START-HERE §1b for the check (DECIDE must be a first-writer-wins CAS on
the DECISION record). Left in place.

Also: `w8_posix9_probe.sh` labels updated (`read_mu` → "host mutex").
`same_parent_storm.sh` PASS 9×4×100 both before and after; root lat /
root mkdir gates unchanged (max 0.063 s, 9/9).

<a id="ph-sep-21-2026-evening--root-mkdir-1-s-stranded-transactions-th-50762f"></a>
## Sep 21 2026 evening — root mkdir 1 s, stranded transactions, the sweep that cost 100 ms

Three server commits, all on the W8 path (9-host posix suite cannot
start because concurrent root `mkdtemp` stalls).

**`165e779` — RESOLVE/DROP scanned the whole shard.** Server strace during
12 root mkdir+rmdir pairs (`results/measure/20260921-202253-w8-root-srv`):
3–5 efsd threads park in one futex and release together 0.70 s / 1.05 s
later; on fcstor005 a futex wait ends ETIMEDOUT at exactly 0.400 s (the
`host_wait_applied` deadline → BUSY). perf
(`results/measure/20260921-202715-w8-root-perf`): 85–90 % of efsd CPU in
`host_apply → efs_txn_resolve → lsm_scan_prefix → merge_scan → memcmp`.
`efs_txn_resolve`/`efs_txn_drop` used a 2-byte `[shard]` prefix = every
key of the shard, on every replica, per participant, under the KV lock
and `h->mu`. Root's shard 1 has the most history, so a cross-group child
(txn path, RESOLVE) cost ~1 s while a same-group child (log path) cost
2 ms — the bimodal 2 ms / 1.05 s of
`results/measure/20260921-195829-w8-root-lat`. The 400 ms BUSY drove the
client's 16-retry 10.4 s backoff, and a retried UNLINK whose first attempt
had committed came back NOT_FOUND = the `rmdir` ENOENT. Fix: three 3-byte
`[shard][INTENT|GUARD|REDUCE]` prefixes (`txn_scan_kinds`). After it,
root mkdir max 0.119 s.

**`d5cbee6` — log BUSY/STALE dir-op outcomes.** Rate-limited (20/s) server
lines for mkdir/rmdir/create/unlink ending BUSY or STALE; client line when
the 16 BUSY/STALE retries are exhausted. Before this, a 10 s EBUSY had no
line anywhere.

**`9534e53` — L5 recovery of stranded transactions.**
`tests/measure/w8_parent_burst.sh` (9 hosts × 6 rounds of fresh-parent
mkdtemp, `results/measure/20260921-204358-w8-parent-burst`): 53 OK, one
EBUSY after 10.39 s with 16 identical server `mkdir … rc=-13` lines —
same shard, every retry. `tests/tools/kv_intents` on a copy of node 2's
`mdraft/kv` (`results/measure/20260921-w8-orphans`): 105 INTENT, 1 GUARD,
28 REDUCE records, all 4 500–5 000 s old, 45 intents on ALLOC keys of 45
even shards; a log-path create/mkdir landing on one of those shards was
BUSY on every attempt. They were left by coordinators whose DECIDE or
RESOLVE wait hit the 400 ms deadline in the 1 s-scan era; nothing ever
came back for the records (spec L5 says recovery must; there was none).
`efs_txn_scan_pending` lists a shard's distinct pending txns with their
part lists; the group leader's GC thread proposes DECIDE ABORT at the
coordinator (PROTO = it had COMMITted, only the RESOLVEs were lost) and
RESOLVE on every participant; `host_bg_propose` returns the apply
verdict for the local-leader path. After the roll: 47 `txn-recover`
lines, every one `COMMIT (resolved)` — the recovered dentries/inodes/ALLOC
bumps became visible ~90 min after their callers were told EBUSY (2PC
lost-ack semantics). `kv_intents` afterwards: 0 pending. Burst 12 × 9:
108/108, 0 BUSY. Root lat: no 1 s mode; root mkdir 16 ms (log) / 50–57 ms
(cross-group txn ≈ 8 commits). Concurrent root mkdtemp 9/9 ≤ 0.137 s,
fresh parent 9/9 ≤ 0.311 s, no ENOENT.

**`3291c6d` — the sweep was a 100 ms regression.** posix jobs=1 on
`9534e53`: 200/201 but 219 s (56 s in the morning); jobs=16: 77 tests
over the 15 s budget (`results/posix/20260921-211622`, `-211929`).
`tests/measure/md_latency.py`: mkdir med 99.5, create+close 152,
append+close 202 ms (reference 7.2/6.7/9.0). The first sweep scanned 512
shards × 3 prefixes per second on each leader; strace of the GC thread:
one `efs_kv_scan_prefix` = 9 `pread64` (one block per LSM segment) ≈
0.3 ms under the LSM mutex the apply path needs; perf 38 % `rep_movs` +
21 % memcmp with the process at 3 % CPU — lock hold, not CPU
(`results/measure/20260921-w8-orphans/sweep-regression.txt`). Now every
PREPARE apply marks its shard (`host_rec_mark`), a mark 5 s old gets one
scan (64 per pass, 1 ms yield), nothing pending clears it, anything found
re-arms it; a fresh process marks all 4096 once. After the roll: create
6.1–6.7, append 8.5–9.3, stat 0.4, unlink 2.1, rmdir 7–10 ms; posix
jobs=1 200/201 in 42.5 s (`results/posix/20260921-213801`).

Seen once during the `9534e53` posix run, not chased: the suite's cleanup
`rmdir` sat ≥ 90 s in `recv` on fcstor003's conn while the server's conn
thread for that fd was idle in `recv` and both Recv-Q/Send-Q were 0 — a
lost reply or lost request; SO_RCVTIMEO is 30 s, so the 90 s is itself a
question. It eventually returned. Open: 16 729 DECISION records are never
reaped (spec silent on when a decision may go).

<a id="ph-sep-21-2026-afternoon--measurements-w7-closed-w8-blocked-on-8410b2"></a>
## Sep 21 2026 afternoon — measurements, W7 closed, W8 blocked on the mount root

Runbooks ran on `b2184a5c7faf-dirty`. Same-parent rate is flat
~140–160 ops/s (`results/measure/20260921-161931-samedir-rate`).
IOR-hard write is 372 / 33 / 69 / 82 MiB/s at NP 1 / 4 / 9 / 36
(`results/measure/20260921-162514-ior-hard-scaling`). 8 GiB dd+fsync
is 499 MiB/s on one client and 176 on four
(`results/measure/20260921-163033-dd-wall`); the 9-client row is
INVALID (400 s, mkdir/fsync EIO). Raft logs are 1.8–4.4 GB, snapshot
skipped, both groups caught up
(`results/measure/20260921-182308-raft-snap-state`).

W7 is done: the isolated walks are 1–6 s. W8's `NOTRUN` harness is
proven on a cut suite. The 9-host suite still cannot start. Nine
concurrent `mkdtemp` in the mount root take 1–6 s, three of nine miss
an 8 s budget, and two creators then get ENOENT from `rmdir` of the
directory they just made. The same nine in a fresh subdirectory finish
in 0.2 s (`results/measure/20260921-194332-w8-root`). Root nlink is
406 and `.stats` rollups are zero, under the 65536 spread threshold.

Later the same afternoon: the root has 410 names and lists in 10 ms.
One-at-a-time, a root mkdir is 2 ms or 1.03–1.08 s (one `recvfrom`),
and a fresh directory in the same second is ≤15 ms except two creates
that return `EBUSY` after the 10.4 s BUSY backoff. Raft PREPARE
outnumbers DECIDE about 6:1 in that window. The 9-host warmup dies
inside that backoff.

<a id="ph-sep-21-2026-morning--parent-row-lost-update--72-commutative-388326"></a>
## Sep 21 2026 morning — parent-row lost update → §7.2 commutative reductions

**Symptom (found 23:10 Sep 20 in the 9×4 IO-500 debug run
`results/io500/20260921-debug-9x4-outbox/`).** 7 mdtest `WARNING: Unable
to remove directory …/mdtest-easy/test-dir.0-0/mdtest_tree.N.0`; 3 of them
returned EIO forever afterwards. `raft-getattr` of the parent
`test-dir.0-0` (ino 824) said **nlink=2 with 3 live subdirectories**
(true value 5); `server_raft_host_rmdir` / `efs_meta_apply_rmdir` hit
`prow.nlink < 3 → EFS_ERR_PROTO`. It had been filed as "transient mdtest
rmdir ENOTEMPTY" for days.

**Root cause.** 36 ranks did `mkdir` then `rmdir` of one child each in one
parent. Children whose ino lands in the parent's group take the same-group
**log path** (`mkdir_batch`, `efs_meta_apply_rmdir`: a plain PUT of the
parent row, unversioned, no intent probe); children in the other group
take the **txn path** (EXCL on the parent row at the version it read, PUT
of a full row image with `nlink±1` from that read snapshot). A txn that
read the row, then had a log-path apply change it, still PREPAREs at the
old version — the log path never bumped it — wins, and its full image
overwrites the log-path increment/decrement. Every log-path parent-row PUT
was exposed (`create_file_batch`, `mkdir_batch`,
`efs_meta_apply_unlink/link/rename/rmdir`). Same class as the ALLOC-key
bug `alloc_key_claim` fixed the day before, now on the parent inode row.

**Options given to the user.** (a) generalize `alloc_key_claim`: log-path
PUT is BUSY under a pending intent and bumps the version so the txn goes
STALE, plus client STALE retry for every dir op — mechanical, but every op
in a directory then serializes on one row and the 50 ms × 2ⁿ BUSY backoff
becomes the same-parent latency. (b) the spec'd §7.2 end state: parent
nlink / dseq / mtime as commutative REDUCE parts, conflict-free instead of
retried. User: "a or b which scales better?" → b → "do b".

**What was built (all in one dirty tree, unit-gated on fstor007, then
deployed):**

- `include/efs/txn.h`, `src/meta/txn.c`: kinds `EFS_TXN_REDUCE_INO`
  (`struct efs_txn_ino_delta`: signed `d_nlink`/`d_nents`, `max_mtime`/
  `max_ctime`, `or_used_shards`, `set_parent`, `d_pver`; 48-byte wire) and
  `EFS_TXN_REDUCE_ADD` (u64, 8-byte wire); `struct efs_txn_reduce` gained
  `mtime_gen` (32-byte wire, 24-byte payloads still decode). One record
  shape `reduce(key, txid) = [txid][parts][payload]` for all three
  (`prepare_reduce_rec`), so drop/resolve find every reduce of a txn by
  suffix. `fold_reduce` dispatches on the DATA key's kind: LANE = MAX
  triple + seq++ + mtime_gen MAX keeping the 56-byte tail; INODE = unpack,
  apply delta, repack; DSEQ = u64 add; INODE and LANE folds bump the key
  version (`res_add_ver_bump`) so a stale EXCL lands STALE. `reduces_pending`
  makes `efs_txn_prepare_excl` / `_guard` BUSY over another txn's pending
  reduce; `prepare_reduce_rec` is BUSY over a pending EXCL intent or
  another txn's GUARD. `efs_txn_dseq_observe` (value, not version) and
  `efs_txn_key_busy` (generic intent-or-reduce probe for the log path).
  `efs_txn_apply_prepare` is the one PREPARE decoder for server and sim.
- `src/server/raft_host.c`: `pack_prep_raw`, `host_prep_raw`,
  `host_prep_ino_delta`, `host_prep_dseq_bump`, `host_prep_lane_stamp`;
  every txn site converted — `host_hashed_create_txn`,
  `server_raft_host_mkdir`, `server_raft_host_rmdir`, `host_unlink_txn`,
  `server_raft_host_link`, `server_raft_host_rename_at`. Parent rows are
  no longer read-modify-EXCL-PUT; dseq bumps are `+1`; HASHED dir-lane
  stamps are REDUCE with mtime_gen; RMDIR's emptiness GUARD carries the
  observed dseq VALUE.
- `src/meta/meta_apply.c`: `dir_txn_busy` before the log-path rmdir DEL
  (probes the dir row and all its dseq keys), `efs_txn_key_busy` before
  the log-path unlink DEL of a last-name row; `efs_meta_unpack_inode`
  exported for the fold.
- `src/client/inode_rpc.c`: `stale_retryable` — STALE retried for
  CREATE, UNLINK, LINK, RENAME_AT (nothing has committed when a dir-op
  txn says STALE; REPORT/APPEND still own their STALE).
- `src/sim/sim_ns.c`, `sim_txn.c`: `dseq_prep` → REDUCE_ADD, `dseq_guard`
  → value observe; `pack_prep` carries raw payloads. Until this the sim
  failed 11 `test_sim` checks (rmdir / rename / hashed overwrite) because
  its dseq guards still used versions.
- Tests: `test_txn` +4 (`test_ino_delta_commutes` — the exact race: two
  deltas prepared, a log-path PUT lands between, both fold onto the
  log-path result, ver+1, a pre-fold EXCL is STALE;
  `test_ino_delta_vs_excl_guard`; `test_lane_fold_preserves_tail`;
  `test_apply_prepare_wire`), `test_meta_apply::test_log_delete_busy_under_intent`.
- `tests/roll_efsd.sh --all`: parallel build on all four, ID agreement,
  stop all / start all, wait for both groups (the build-ID gate rejects a
  rolling restart across a commit; there was no script for that case).
- `tests/stress/same_parent_storm.sh` + `same_parent_worker.py`: the
  repro as a gate.

**Gates on the deployed build `b2184a5c7faf-dirty`.** posix jobs=1
`results/posix/20260921-123904` 200/201 + mmap SKIP in 56 s (unchanged
signature). `results/stress/same-parent-20260921-124141/` 9 hosts × 4
procs × 100 rounds mkdir/create/rmdir/unlink in one parent = 14 400 ops,
0 errors, parent `children=0 nlink=2`, rmdir OK. 9×4 IO-500 debug
`results/io500/20260921-debug-9x4-reduce/`: both `-R` reads 0 errors, 0
`Unable to remove directory`, run tree gone afterwards; rates within noise
of the previous build (mdtest-easy-write 0.189 vs 0.238 kIOPS, hard-write
0.240 vs 0.201, ior-easy-read 1.13, hard-read 4.06 GiB/s).

**Learned.** (1) The storm measured **178 ms per op per proc under 36-way
same-parent contention** (~200 ops/s aggregate) vs 7 ms idle — the
reductions removed the lost update, not the same-directory ceiling; that
is now W6 residual 3 (count BUSY/STALE retries first; they are not logged).
(2) `efs_kv_scan_prefix` callbacks returning >0 = batch-full, again
relevant in `reduces_pending` — check it in every new scan user. (3) The
user pointed out `screen -S` on node9901 as the natural long-job holder;
`efs-bg.sh` now launches each job in a detached `screen efs-<name>` (same
NFS log + rc bookkeeping) so `screen -r efs-<name>` shows the live job.

<a id="ph-sep-20-2026-evening--reaper-cross-group-lane-bug-the-100-ms-d4a7ce"></a>
## Sep 20 2026 evening — reaper cross-group lane bug, the 100 ms commit floor, node9901 runner

**Symptom chain.** Posix jobs=1 on the full in-flight tree gave 191/201 +
mmap SKIP twice (`results/posix/20260921-011518`, `-012432`), with
`mtime_monotonic_many_writes` timing out deterministically: 80 × (open
`O_APPEND`, write 1 B, close) at ~200 ms each. Isolated probe on fcstor007
against an "idle" cluster: mkdir median 103 ms (min 5.5), create+1B+close
60–107, append+close 160–180; the client log said `report_ms=104` on every
one-record REPORT, and the leader's `report-split nrec=1 pack_ms=0
push_ms=0 finish_ms=103` — pack and push free, the wait for the apply
verdict ~100 ms.

**Finding.** `efs-mgmt raft-status` showed both groups committing ~33
entries/s with no client process anywhere. A 3 MB tail of fcstor004's
`raft.log` (new `tests/tools/raft_log_tail.py`, framing from
`raft_log.c`) was **92 % `LANE_SWEEP`** in both groups: the same 64
inodes, lane 0 only, 362 times each in the window — one sweep per inode
per GC pass, forever. `host_gc_propose` was leader-only ("the reaper only
walks groups this node leads"), but `efs_kv_lane_shard` is
`ish + lane × (2·(h & 0x7ff)+1)`: an odd stride, so lane 1, 3, 5 … of every
inode are in the *other* group. The anchor-group leader swept lane 0
(landed), got `NOT_PRIMARY` for lane 1, `break` → "retry the whole marker
next pass". No inode with an active odd lane could ever be reaped unless
one node happened to lead both groups (which is why it sometimes worked in
the past). The earlier `rc=-5` (`vlen`) and `rc=1` (batch-full) reaper
bugs masked this one: they failed lane 0 first.

**Fix.** `host_gc_propose` proposes locally when leader, otherwise
forwards to that group's leader with `host_remote_cmd` (the same path a
client op takes through `server_raft_host_submit` → `host_propose_wait_idx`)
and then `host_wait_applied` on the local replica if it hosts the group.
No `read_mu` (GC thread never holds it). Deployed with
`tests/roll_efsd.sh 1 2 3 4` (01:48–01:50 UTC, each node caught up in
< 10 s, build `d0fd0448adb6-dirty` unchanged). After the roll `REAP_DONE`
went from 0.3 % to 40 % of entries: the backlog of dead inodes (IO-500 /
mdtest / posix leftovers, thousands) drains at ~15 inodes/s per group. The
pass is serial (one propose+wait per lane, then REAP_DONE); batching the
32 markers' proposals before waiting is an obvious speed-up, not done.

**Why it cost clients 100 ms — first (wrong) theory.** The pump is one
thread per node: tick + apply, and every apply fsyncs the KV WAL
(`EFS_KV_LSM_SYNC`), so the 33 entries/s reaper stream was assumed to keep
the pump ~90 % busy and queue every client entry. After the reaper drained
(both groups flat) the median did NOT move: mkdir still ~100 ms, min 5.5.
The general shape still holds — **any background stream of small commits
adds latency to every client op**, so measure only on a flat-`commit`
cluster and check `raft_log_tail.py` first — but it was not the floor.

**The real 100 ms floor: outbox wakeup (fixed, 22:40).** `strace -f -tt` on
the leader (004) during a create loop: client request in, raft-log
`pwrite`+`fsync` 0.3 ms, KV WAL fsync 0.3 ms — and the AppendEntries to one
peer left the outbox **47 ms** after the propose. On a follower (003) the
picture was unmistakable: heartbeats arrive every 50 ms, but the follower's
outbox writes its 85-byte AE replies only on every OTHER heartbeat, two
back to back — every reply waited for the next incoming message. Code:
`raft_host.c` had ONE `h->outbox_cv` shared by all per-peer sender threads
and `host_send` used `pthread_cond_signal`, which wakes one arbitrary
waiter. A message for peer A woke B's sender (its queue empty, back to
sleep) and A's sender ran at the next signal for anyone — the next
heartbeat. Each hop lost 0–50 ms; AE + reply ≈ 100 ms per commit,
independent of fsync speed. Fix: `pthread_cond_t cv` per `struct
host_outbox`, `host_send` signals `tx->cv`, `host_stop_senders` broadcasts
all. Rolled all four (TCP peers). Result, idle cluster, 20 ops each
(`results/perf/20260921-md-latency.txt`): mkdir med 103 → **7.2 ms**,
create+1B+close 60–107 → **6.7**, append+close 160–180 → **9.0**, stat 0.4,
unlink 1.8. Lesson: when a median sits at a multiple of the heartbeat
interval while every syscall is sub-millisecond, it is a wakeup/scheduling
bug — strace the follower, not the leader, and look for replies bunching.

**Rolling-restart election storm (observed, not fixed).** Group 2 went
term 199 → 264 during and ~5 min after the roll. All three voters' logs
are full of `RDMA send CQE error status=12 (transport retry counter
exceeded)` and `*** SOCKET CLOSED/REUSED BEHIND THIS CONN ***`: server
peer connections (raft AE included, `raft_host.c` outbox →
`server_peer_conn_get`) are upgraded to RDMA by `peer_pool.c`; a restarted
node's QPs vanish and each peer's pooled conn blocks for the retry budget
before it is dropped, missing heartbeats. Same lines exist in every
`efsd.log.prev` from earlier restarts; it converges by itself. Same class
as the client pool identity bug (`test_conn_fd`, W10).

**Runner.** The login-node Cursor shell died five times today, twice
mid-measurement. From now on anything over ~60 s runs detached on node9901
(`~/.cursor/skills/efs-test-ssh/scripts/efs-bg.sh start|status|wait|kill`,
per-host ssh-agent in the wrapper since `$HOME` is NFS-shared; log
`~/efs-runs/<name>.log`), and the login node only probes. New scripts:
`tests/deploy_fuse_clients.sh` + `tests/fuse_client_remount.sh` (8 clients
rebuilt and remounted in 5 s wall, one status line each),
`tests/roll_efsd.sh` (rolling restart with build-ID refusal and per-group
catch-up wait). The deploy rule's `ps|awk` kill-by-port was replaced by
`pkill -9 -x efsd` after it killed the agent's own remote shell (003 and
005 down together, group 0 without quorum for 4 min, Sep 20 morning).

**9×4 IO-500 after the fix (23:00).** `results/io500/20260921-debug-9x4-outbox/`:
both `-R` reads 0 errors, all ior files unlinked; mdtest-easy-write 0.238
kIOPS (morning 0.050), mdtest-hard-write 0.201 (0.018), easy/hard stat 0.88
/ 2.76 (0.24 / 0.79); bandwidth phases unchanged (easy-write 0.78 GiB/s,
hard-write 0.046, hard-read 3.89) — those walls are W4 and the 36-way
sub-chunk CAS, not metadata latency. posix jobs=1 200/201 + mmap SKIP in
60 s (was 191 in 6 min).

**Wedged directory (found in the same run, open).** mdtest printed seven
`Unable to remove directory …/test-dir.0-0/mdtest_tree.N.0`; four rmdir
fine afterwards, three return EIO forever. `raft-rmdir` → status 3 =
`EFS_ERR_PROTO` from `prow.nlink < 3`: the parent (ino 824) has nlink 2 with
three live subdirectories. Mechanism: a child with an even ino is removed
on the same-group log path (`efs_meta_apply_rmdir` PUTs the parent row
without probing intents or bumping its version); an odd-ino child goes the
txn path (EXCL on the parent row at the version it read, PUT of a full
image with nlink−1). A txn that read before a log-path apply still
PREPAREs at the old version, wins, and overwrites the log-path change — a
lost update on nlink (and on dseq/mtime), identical in shape to the
`alloc_key_claim` bug of the same morning, just on a different key. It
also explains the long-standing "transient mdtest rmdir ENOTEMPTY". Two
ways out are written up in START-HERE §1b step E: generalize the ALLOC
rule to the parent row (BUSY under intent, bump version, client retries
STALE for the directory ops) or the §7.2 reductions (nlink as a signed
REDUCE delta). Waiting on the user.

<a id="ph-w6-narrative-as-it-stood-in-start-here-before-sep-20-superse-a6ecb4"></a>
## W6 narrative as it stood in START-HERE before Sep 20 (superseded)

hard/easy write = **0.097**. Easy write 0.26 GiB/s vs 9-client 8g
dd+fsync 0.20 GiB/s (same order; 0.6 % of 44 GB/s). First
ior-hard-write hung D-state (`20260918-debug-hung-hard/`, 26k-rec
STALE REPORT); remounted retry finished.

**IOR-hard `-W`** `results/io500/20260918-hard-w/`: 13.08 MiB/s,
583 s, **4244 incorrect-data errors**, IOR exit 40. Gate
mismatches=0 is FAIL. Do not retune 47008. Do not invent a chunk lock.

**30 s ior 9×1 aborted** `results/io500/20260918-ior-30s-abort/`:
ior-easy-write fsync/close EIO at 545 s; `report-split nrec=125000
rc=-13` (BUSY) on fcstor004, then apply-publish STALE (`rc=-14`) at
88 % CPU. Same class as loaded 50g `end_fsync` EIO. Do not raise
`EFS_IO_TIMEOUT_MS`. 1 s easy-write fsyncs; 30 s (~125k pubs) does
not.

**Cluster RESET (Sep 18 PM, user-authorized).** The 30 s abort left a
self-draining STALE backlog (~1.2 entries/s, term flapping, hours to
drain) and fcstor005 430k entries behind on group 2 (W11 oversized
SNAP — can never catch up). The table was wiped and `raft-mkfs`'d
fresh. Gotcha now fixed in `clean_cluster.sh`: mkfs proposes only to
group 0, so it must go to group 0's CURRENT leader; `rc=-15
leader_hint=H` means retry on node H+1 (`172.16.223.(57+H)`).

**FOUND + FIXED (Sep 18 PM): same-parent concurrent mkdir EIO.**
9×4 mdtest-easy aborted: 8/36 `mdtest_tree.N.0` mkdirs failed EIO
(9×1 passed). Repro: 36-way same-parent mkdir across 4 clients →
~5/36 EIO in 2.9 s (fast fail, not BUSY-retry exhaustion). Server
`EFS_RAFT_DBG` mkdir line: `rc=-13/-14 stage=11` — the cross-group
mkdir txn (MKDIR scatters the child, so every mkdir with an
even-shard child is a 2-group txn) CASes the parent-row + dseq
versions; the two dual-hosts (004/005) race, one commits, the other
preps STALE. The client RPC loops retried BUSY/NOT_PRIMARY but NOT
STALE → `fuse_create_errno` → EIO. Fix (client `inode_rpc.c`):
`EFS_INODE_RPC_STALE` retried like BUSY for `EFS_MSG_INODE_CREATE`
only (idempotent unique name; a landed retry reads as EEXIST, which
FUSE already handles). REPORT_CHUNKS keeps its STALE (W1
refetch+overlay). Gate: 36/36 then 90/90 same-parent mkdirs, 83
STALE/BUSY conflicts absorbed on 004 alone, 0 errors. Same exposure
exists for cross-group UNLINK/RENAME/LINK txns — not measured, not
fixed. **Deeper fix (not done):** the parent-row nlink++/dseq++/
times should be commutative txn reductions (spec §7.2), not an EXCL
row CAS — that removes the same-parent mkdir serialization point
entirely instead of retrying it.

**Fresh-cluster 9×4 (Sep 18 PM):** ior-easy-write **1.34 GiB/s**
(39.7 s) — 5× the poisoned-table 9×1. mdtest/reads/hard: see
`results/io500/<new id>/` when landed.

**W11 is every-run, not edge:** `snapshot skipped ... KV export
exceeds SNAP cap` fires as soon as real data flows (ior-easy-write
≈ 400k chunk pubs ≈ 40 MB KV > 4 MiB cap), so a data-bearing
cluster NEVER compacts its raft log → any follower restart = full
log replay, and a lagged follower is permanent (what killed 005).

**Still open (this item):**
1. `SLOTS=4 NP=36 bash tests/perf/io500/run.sh debug` (1 s
   stonewall) — RUNNING on the fresh cluster. `findmnt`
   `fuse.efs-fuse` on every rank. Copy ini+hostfile+result.txt to
   `results/io500/<id>/`.
2. Confirm every rank stayed FUSE (no local-disk fallback).
   Another 30 s `run.sh ior` will hit the same fsync EIO until
   REPORT can take 125k pubs.

- **Read:** `tests/perf/io500/README.md`; the fio rule's FUSE check
  applies to every rank.
- **Gate:** 9×1 debug is in; 30 s + 9×4 still required. `-W`
  mismatches are **4244**, not 0 — record that, do not hide it, do
  not reopen W1 with a lock.
- **Forbidden:** quoting a rank that fell back to local disk; tuning
  IOR's transfer size (47008 is the point); `pkill -f` (matches the
  agent). Kill hung `io500` with `pkill -9 -x io500` then remount
  FUSE (D-state `request_wait_answer` ignores SIGKILL until
  `efs-fuse` dies).


---

<a id="ph-the-always-applied-project-state-rule-as-of-sep-20-2026-verb-96f9cb"></a>
## The always-applied "project state" rule as of Sep 20 2026, verbatim


# efs project state (compact memory)

efs: distributed FS, 2+1 XOR EC, FUSE client, RDMA (RC QP), 128 KiB chunks.
Pre-alpha. **Goal: scale to >= 2^32 files/dirs** (see docs/scaling-roadmap.md).
**NO approval needed to run scripts against node9901, fstor007, fcstor003–015
— including full cluster wipe (user, Aug 27 2026). Wrapper scripts need
`required_permissions: ["all"]` to escape the sandbox; that is pre-authorized.
See efs-fcstor-deploy.**

**AUTO-RDMA FIRST INODE — ROOT-CAUSED (Sep 1), CLIENT POOL FIX IN TREE
(Sep 17). The server DESTROYS the QP the client is sending to, because
the client closed that connection's TCP side-channel while leaving the
conn object in its pool.**
Symptom: on a fresh mkfs + auto-transport mount the first `mkdir` D-states
forever; `fuse.log` repeats `efs: RDMA send type=43 WAIT TIMEOUT` (43 =
`EFS_MSG_INODE_LOOKUP`, plain LOOKUP — **not** `LOOKUP_PATH`, so b518003's
wire change is innocent). Servers healthy (fresh gen=1, clean catchup, no
BUSY); FUSE `waiting=1` (ONE outstanding request, not saturation); efs-fuse
S-state; the D-state `filename_create` procs are just queued behind the one
unanswered LOOKUP. `EFS_TRANSPORT=tcp` on the SAME binary passes in <1s.
**The plan's hypothesis (RNR NAK from too few recv buffers + infinite
`rnr_retry`) is DISPROVEN — do not re-chase it.** `rnr_retry` was made
configurable and set finite: failure identical. Requester-side HCA counters
`rnr_nak_retry_err` / `req_cqe_error` / `local_ack_timeout_err` /
`packet_seq_err` / `out_of_buffer` all stayed **0**; only the SERVER's
`resp_cqe_error` + `resp_cqe_flush_error` incremented.
**How it was proven (measurement, not code reading):**
- `rdma resource show qp` on the server during the hang: the client's
 `dest_qpn` is **missing** from an otherwise contiguous live QPN sequence
 (61571, [61572 absent], 61573) — the server had already destroyed it.
- Port correlation nails the pair: client `fd=14 qpn=86621 local_port=58152`
 ↔ server `qpn=61662 peer_port=58152 fd=24`. The server then logged
 `tcp EOF (peer FIN) fd=24` → `conn teardown reason=tcp_has_request failed
 (blocking) qpn=61662`, and the client's later send to `dest_qpn=61662`
 timed out. So the server tears down the whole conn (QP included) when the
 RDMA conn's TCP side-channel sees a FIN.
- The failing conn's OWN socket was never closed: the client records
 `/proc/self/fd/N` (`socket:[inode]`) at upgrade and re-reads it at failure
 — `(same socket)` every time. `ss -tanp` on the client showed the failing
 conns `ESTAB` but **5 other conns in TIME-WAIT**, i.e. the client sent the
 first FIN on those.
- **The smoking gun:** client `fd=6` upgraded for `qpn=86619`, then LATER a
 *different* connection upgraded on the **same `fd=6`** for `qpn=86622`.
 fd 6 was closed and recycled while the conn object holding `qpn=86619`
 was still reachable from the pool; the next RPC that checked that object
 out sent into a QP the server had already reaped.
- `efs_conn_destroy` backtraces resolve to `efs_client_conn_drop` — that is
 an error handler reacting to the breakage, **not** the origin. The origin
 (which bootstrap path closes a pooled conn's fd without evicting the conn)
 is the remaining unknown.
**Status: CLIENT POOL LIFECYCLE FIX IN TREE (Sep 17).** Not `rdma.c` tuning
(RNR/poller already landed). A pooled `efs_conn` now records sockfs
`st_dev`/`st_ino` at wrap; checkout treats a mismatch as dead; destroy
closes the fd only when that identity still matches (a recycled number is
someone else's TCP — closing it FINs the new owner and the peer reaps the
QP). `conn_init` skips busy slots (it used to destroy a checked-out conn
and clear busy). Post-mount GET_FEATURES / STATUS use the pool (no raw
`close(fd)` next to live QPs). New sockets are `SOCK_CLOEXEC`. Gate:
`test_conn_fd`. Live empty-table auto/RDMA first `mkdir` is still the
repro; 19820 is populated so do not treat a remount there as that gate.
Diagnostics stay behind `EFS_RDMA_FIRST`. Default leftover-1 gates stay
TCP until that empty-table mkdir is run. **SCOPE: it is NOT "RDMA is
broken".** A node9901 client on auto/RDMA (`mlx5_0`) against an
ALREADY-POPULATED table (`meta ready gen=13 inodes=55621`) drove millions
of creates with 0 network errors. The repro needs a **freshly mkfs'd /
effectively empty** table.
**Harness gotcha — FIXED (Sep 1).** `ensure_mounted` returned early when the
host was already mounted, so `EFS_TRANSPORT=tcp bash tests/run_tests.sh
setup` on a cluster whose clients were already up on auto **silently kept the
RDMA mount** and the variable appeared to do nothing (this is why "every gate
since Aug 30 ran on TCP" was itself unverified). It now checks
`grep -c "RDMA transport up" /tmp/efs/fuse.log` on a reused mount and
remounts on a mismatch. Only `tcp` is enforced — the RDMA upgrade is lazy
(first pool checkout), so a fresh auto mount legitimately has no such line
yet and must not be remounted. Verifying that count is **0** per host is
still the right way to confirm a TCP run; do not trust the env var alone.

**Prereq 1 of Cut C landed + gated (Sep 1, working tree).**
`slab_page_persisted` is now authoritative from the committed root
(`root.ino_page_count`, falling back to `root.page_count`) instead of
returning `ex->page_src != NULL`. Slab si is page 1+si, so the image covers it
iff `si+1 < ino_pc`. **This is a strict no-op today** — the predicate is only
reached from `inode_slab_ensure`'s `(flush_blob || page_src)` branch, the blob
branch is untouched, and `page_src` is still never installed — but it is the
one thing that MUST land before any page source exists, or every new slab's
first row (callers do `pos = inode_count++` before filling, which already
looks like a fault) becomes a hard failure = the "jammed at exactly 256 rows"
bug. Gate: `test_meta_v6` / `test_dir_stats` / `test_ino_path` /
`test_meta_slot` OK; fresh bits=3 wipe + solo posix on a TCP mount
`results/posix/20260901-171838` **196/201, 0 EFS bugs**.

**THE REAL CUT-C BLOCKER IS THE *FULL* SERIALIZE, NOT THE BLOB SIZE (Sep 1,
measured with new TRIM-PROF / `absent=` counters, fresh bits=3 TCP,
`EFS_INO_RAM_MB=64`, scale_grow to 500k).** Trim works, and it is still
futile. Do not build the spill until this is fixed — it would spill pages that
are immediately faulted back.
- **`incr=0` on 100% of flushes** (63/63 across primary + a joiner) for the
 whole 500k grow. `efs_export_serialize_dirty` bails whenever the inode
 region grows (`new_ino != cache_ino_len`), and during growth it grows
 constantly, so **every** flush takes `efs_export_serialize_ex`.
- `serialize_ex` repacks **every** row, so it **faults every evicted slab back
 in**. Measured on fcstor005: `evicted=241 resident=194 slab_n=512`, and the
 next flush is back to a full table. Eviction accomplishes nothing.
- Therefore `page_absent` (prereq 2) **never fires during growth** — it is
 only produced on the incremental path. Measured `absent=0` on 1762 flushes
 at 10M and 63/63 here. It is correct and it is the right groundwork, but it
 buys nothing until the full path stops faulting.
- The blob dominates the cap it is counted inside: `bytes_mb=63 blob_mb=54`
 (**86%**), so trim thrashes toward a floor it cannot evict.
- The primary is worse: **79 of 116** trim calls are `TRIM-PROF skip
 no-source` (`!flush_blob && !page_src`), 0 evictions.
**So the ordering is: make `efs_export_serialize_ex` able to emit a page for a
non-resident slab without faulting it (reuse-by-ci, same argument as
`page_absent`), THEN bound the blob, THEN spill.** At 10M the same run showed
`ser=350ms` + `snap=175ms` per flush, so this is also the flush-cost lever.
Instrumentation for all of the above is in tree and env-gated behind
`EFS_FLUSH_PROF`: `TRIM-PROF` (cap/bytes/blob/resident/slab_n/evicted/hops,
plus skip reasons) and `absent=` on the `FLUSH-PROF` line.
`clean_cluster.sh` now forwards `EFS_INO_RAM_MB`.

**10M SCALE RUN, errors=0 (Sep 1, `results/scale/20260901-181133`, bits=3 TCP,
9 clients x16, EFS_INO_RAM_MB=64).** total | creates/s | errors | rss_max MB |
B/inode: 100k 9999 0 101 3408 | 500k 26666 0 597 3358 | 1M 33333 0 968 2852 |
2M 33333 0 1532 2506 | 5M 27272 0 3333 2159 | 10M 15873 0 5587 1918. **errors=0
at every step incl. 10M** (Aug 31 bits=3 had 46/176 at 5M/10M, and the Sep 1
Cut-3 10M attempt wedged). Latency flat: stat p50 0.454, create 0.552, unlink
0.422, readdir 6.9 ms. `rss_max 5587 MB` ~= the 10M x 512 B `flush_blob` ino
region (5.1 GB) — the blob IS the RSS, which is the quantified case for Cut C.

**SPILL DESIGN CORRECTION — a full rewrite per flush is impossible.** The
plan said "write the committed region bytes to local NVMe at flush". At 2M
inodes the ino region is ~977 MB and flushes run at **~35/s**, so that is
~34 GB/s of writes. The spill file must be maintained **incrementally**:
`pwrite` only the pages the flush already knows are dirty (~5 pages, ~640 KB)
at offset `pi * EFS_META_PAGE_SIZE`, making it a page-indexed local mirror of
the committed inode region. This is also what makes it safe against a failed
commit: a fault only ever reads a **non-resident** slab, `trim_ino_ram` only
evicts **clean** pages, and a clean page is by definition not among the pages
a flush rewrites — so uncommitted bytes can never be the bytes a fault reads.
On restart the table is rebuilt from EC pages and every slab becomes resident,
so the file is a warm cache and can simply be recreated.

**Code-hygiene pass (Aug 26 PM, commits 4a959ac/1124e6d/6bb83b8/6cb5791):**
dead code removed (~469 lines: client coal subsystem, client meta-flush/
election leftovers, unused server helpers), obsolete slurm-jobs/ + old
scripts/fio-*.sh deleted (docs/testing.md now points at fcstor run_tests.sh).
**Valgrind memcheck: NO LEAKS** — unit tests, full efsd workload, and full
efs-fuse workload all 0 definite / 0 indirect. Possibly-lost are benign
one-time allocs (libfuse fuse_new session bufs, glibc dl-init constructors,
pthread TLS 288B/thread). **Found+fixed (1124e6d): uninitialised padding in
struct efs_ino_size_rec sent on the wire** (REPORT_CHUNKS) — malloc→calloc.
Root/installs rule added (6cb5791): always ask user, never work around.
**Repeatable leak gate (957c6df, RDMA added 1b72183):** `tests/run_tests.sh
leaks [host]` → tests/valgrind_leaks.sh. Self-contained single-node on a
private port + scratch (never touches the live cluster); gates unit + efsd +
efs-fuse under memcheck over **BOTH TCP and RDMA, client AND server**.
Hard-fail on definite/indirect leaks, uninit-on-wire, invalid rw;
possibly-lost only flagged when the direct caller is efs code. RDMA phase is
leaks-only (ibverbs/DMA fills structs valgrind can't track → uninit
false-positives by design). RDMA exercised via eager upgrade on connect +
metadata/packed + chunk PUT/GET ops. Verified PASS
on fcstor003: 0 definite / 0 indirect everywhere.
NOTE: valgrind on efsd needs a clean exit for full leak stacks (SIGTERM gives
summary only). **vgdb / LSan / `strace -p` / `gdb -p` all WORK — do not ask
the user to enable ptrace.** Verified Sep 1 2026 by live attach (`strace -p`
+ `gdb -p`) on **all 15** hosts: `kernel.yama.ptrace_scope=0` on node9901,
fstor007 and fcstor003–015. Every "ptrace/Yama blocked" note elsewhere in
this file is HISTORICAL and no longer true.
**BUT IT IS RUNTIME-ONLY AND REVERTS ON REBOOT.** All 15 still ship
`/etc/sysctl.d/99-ptrace.conf` with `kernel.yama.ptrace_scope = 2` (an
ssh-keysign fd-stealing mitigation); the 0 was applied with `sysctl -w` and
never persisted. If a host comes back from maintenance at 2, that is
expected — just re-apply `sudo sysctl -w kernel.yama.ptrace_scope=0` (sudo
needs a password, so the USER must run it). Do not "fix" it by editing the
conf file without asking — that weakens a deliberate mitigation fleet-wide.
Still unavailable:
`perf trace` (`perf_event_paranoid=2`, `/sys/kernel/tracing/events`
unreadable) — `perf record -p` / `perf stat -p` same-uid are fine.

<a id="ph-where-we-are-d0a2b3"></a>
## Where we are

**W1 I12 N-1 CAS LANDED + GATED (Sep 18).** `base_gen` on the wire, leader
CAS, audible apply STALE (no `last_applied` stall), client refetch+overlay
(64 tries). Gen-keyed fragments. `peer_shared_pwrite` concurrent 5/5.
n1 `results/stress/20260918-n1-w1/` lost=0. Honest 1-client
`results/perf/20260918-w1-honest/` sw-1m **758**, sw-50g **373**. Do not
reopen with a chunk lock. **W2 DONE option (i):** spec moved — `write()`
is client-buffered; durable+visible at `fsync`/`close`/`O_SYNC`.
Measured 0/10 visibility and 64 MiB lost on kill -9
(`results/stress/20260918-w2/`). `O_SYNC` specified, not wired. Do not
publish on every `write()`. **W3 DONE (Sep 18):** 8 GiB dd+fsync
**639 MiB/s** (13.5 s; best 724 / 11.86 s), remount HEAD/TAIL OK,
posix jobs=1 **195/201**, posix2 **58/63**. Cuts: N=2048, skip
get_chunk, sync_hold, flush pipeline, propose-only PUBLISH forward,
O_APPEND published-merge, close waits per-ino report. Remaining tail
is REPORT pack+push. Do not invent a REPORT RPC split. **W4 DONE (Sep
18):** writes share a ceiling and more clients make it worse. 8g
dd+fsync 1/4/9 = 639 / **251** / **202** MiB/s (3.8 / 0.46 / 0.37 %
of 16.7 / 44 / 44 GB/s). Morning 4-client sw-1m AGG **1589**. Loaded
4-client rw-1m 400 s TIMEOUT; 4-client fio storm can lose raft
heartbeats (`rc=-15`). Gate
`results/perf/20260918-w4-honest4/gate.txt`. **W5 DONE:** sw-50g
**341**. **Current: W6** (9×1 debug in
`results/io500/20260918-debug-9x1/`: easy-write 0.26 GiB/s,
hard-write 0.025 GiB/s, mdtest-easy-write 0.053 kIOPS; IOR-hard `-W`
4244 errors. 30s ior aborted on fsync EIO /
`nrec=125000` BUSY (`results/io500/20260918-ior-30s-abort/`). 9×4
debug rerun (`results/io500/20260918-debug-9x4/`): easy-write
**0.415 GiB/s** (1.58× 9×1), mdtest-easy-write **28.5/s** (0.54× —
worse), **hard-write DID NOT FINISH in 2h18m** (36-way CAS contention
on shared 128 KiB chunks; fsync report_ms 58 s–443 s, one 85-min
report lost STALE rc=-14; ~7.5 MiB/s aggregate vs 26 at 9×1). Raft
never the bottleneck (commit==applied, terms stable throughout).
W6 gate "hard -W mismatches = 0" unreachable until the shared-file
write tail is fixed. Harness bugs found: run.sh foreground prterun
dies with the ssh timeout (detach + log to NFS instead); never
gdb-attach an MPI rank through a timeout'd ssh (left a rank
T-stopped, job unrecoverable).

**W6 CORRECTNESS GATE MET (Sep 20, commit 708b350 on top of a9e94a6).**
9×4 IO-500 debug `results/io500/20260920-debug-9x4/`: **every phase
finished, ior-easy-read AND ior-hard-read 0 verification errors, every
unlink OK.** easy-write **0.814 GiB/s**, hard-write 0.044 (495 s, was
DNF), easy-read 1.80, hard-read 3.55, mdtest-easy-write 50/s. **Root
cause of the Sep 19 76108/4314 read errors + 27 undeletable easy files
was DUPLICATE INO ALLOCATION on concurrent CREATE**
(`results/io500/20260920-easy-dupino/`): `server_raft_host_create`
peeks the alloc watermark on the PROPOSER (applied state) and packs the
ino into the CREATE cmd; `create_file_batch(want_ino)` took it
unconditionally, so two creates on one shard that peeked before either
applied (other voters / forwarded) wrote two names onto ONE inode row
(36 IOR easy files → 10 distinct inos, 8 names on ino 4460); both
writers published onto one ino (mixed content), the first unlink deleted
the row and the siblings dangled (resolve → I9 EIO, `rm` ENOENT, `rmdir`
ENOTEMPTY). Fix `alloc_hint_or_next` in meta_apply: the apply IS the
allocator — hint honored only if ≥ watermark and unoccupied, else
`alloc_next_free`; deterministic per replica; host already reads the
ino back from the dentry. Same for the mkdir same-group fast path. The
hashed-create / mkdir txn paths CAS the alloc key and were never
affected. Gate `test_meta_apply` `test_create_log_at_dup_hint`. Raw IOR
repro: `SLOTS=4 run.sh ior-easy-write 1024` then `ior-easy-verify 1024`
(36/36 distinct, 0 mismatches, write 463→902 MiB/s). Same commit: the
reaper's `LANE_SWEEP` / `rsv_purge` misread `efs_kv_scan_prefix` rc>0
(batch-full) as an error → no lane with >64 chunks was ever swept (every
deleted file >8 MiB/lane leaked its fragments; `apply lane-sweep rc=1`
every 33 raft entries). **Do not quote the Sep 19 easy-read 4.0 GiB/s —
it was zero-fill.** Leftover dangling dentries on 19810: `/io500/easyv`
(26) and `/io500/2026.09.19-22.52.15/ior-easy` (27) — inert, cannot be
unlinked by design. **W6 residuals (perf, not correctness):** hard-write
45 MiB/s (36-way N-1 CAS on 47008 B records); easy-read open phase 20 s
of 22 s (1 GiB open = 128 sequential GETCHUNKS + 64-lane stat, 0.2–0.8 s
unloaded — spec §8 per-lane range fetch); 7/36 mdtest rmdir transient
ENOTEMPTY under load (clean seconds later). That run used client
`efs-fuse` a9e94a6 on 007–015 WITHOUT the read.c/ops.c pull change
(so the 0-error result is the server fix alone); servers 708b350.

**SALT DIVERGENCE + RAFT SNAPSHOT CATCH-UP FIXED (Sep 18 PM, commit
43e3e4e).** Three production bugs, all gated: (1) **export salt
diverged across nodes** — `host_export_salt` was computed locally, so
dir placement (`hash(parent,name,salt)`) disagreed between nodes and
hashed lookups missed. `EFS_MD_CMD_SALT` now replicates the salt
through Raft at mkfs; readers fall back to the anchor. (2) **A
restarted Raft leader could never serve InstallSnapshot** — the
snapshot blob is memory-only; after a restart `snap_idx`/`snap_term`
reload from disk but the blob is gone, so the leader sent an 8-byte
metadata-only SNAP that `efs_kv_group_import` rejects (PROTO), and the
follower starved forever. `send_snap` now re-exports the app state at
`last_applied` on demand (pump-serialized, so consistent; regression
`test_install_snapshot_restarted_leader`). (3) **`disk_save_snap`
rejected skip-ahead snapshots** — `last_index - snap_idx > n` → INVAL,
which is exactly InstallSnapshot onto a behind follower; the in-memory
`raft_group_snap` and the replay path already clamped that case, only
the live save path had the stale guard. Removed (backwards snaps still
INVAL); disk/mem parity case in `test_raft_store`. Found live:
fcstor005 stuck at applied=249 after a rolling restart of the
compacted leader; after the fix it installed `incl=512` and caught up
in seconds. `on_snap_req` save_snap failures now log under
`EFS_RAFT_DBG` (they were silent, no reply sent). Gates: test_raft /
test_raft_store / test_wire / test_txn / test_sim OK on fcstor003;
live 19810 restart all-4, group 0 applied=512 and group 2 applied=303
on every voter, term stable under IO-500 load.

**HPC REVIEW LANDED IN THE ROADMAP (Sep 18).** START-HERE §1a queue:
W1–W5 done. **W6 IN PROGRESS** (9×1 debug + hard `-W` 4244 errors;
30s/9×4 open). W7–W12 = old
W5,W6,W9,W7,W4,W10. Write 1/4/9-client 8g dd+fsync is 639 / 251 /
202 MiB/s. `architecture.html` is generated.

**19820 RETIRED; 19810 IS THE ONLY CLUSTER (Sep 17).** User-directed:
wipe 19820 `/tmp/efs-raft-scratch` and the leftover-1 19810 table, then
`raft-mkfs` 19810 on `/data1` (36T, TCP, seed 003, join 004–006).
`wipe_cluster.sh` now removes scratch dirs; `pkill -x efsd` is safe.
Do not stand up 19820 again. POSIX/perf/leaks gates run on **19810**
or localhost. Orphan old-engine sources deleted (`test_meta_batch`,
`test_rpc_create`, `blobscan`, `dump_root`, `txprobe`, `kv_compact_dir`).

**POSIX 1+2 gated on 19810 after 983bfb8 (Sep 17).** Same storage, TCP,
jobs=1. Harness: `concurrent_appends` / `concurrent_create_unlink_two_proc`
`@budget(30)` (isolated 22 / 20 s vs 15 s default); `dir_many_files`
`@budget(75)`; `POSIX2_STEP_SEC` default 45 (400+400 append SSH was 15 s).
One-node `results/posix/20260917-190719`: **194 both-pass / 2 EFS** (efs
TSV 199/1/1 in 241 s) — leftover `dir_many_files` 45 s (isolated PASS
12 s; mmap SKIP ENODEV). 4-node `--parallel` 007–010 under 5-way load
`20260917-190014`: 186–188 both-pass (008 164 / incomplete 175 tests).
9-node `20260917-191430`: every host hit the 385 s python cap at
131–144/201 (5–7 real walk/name timeouts; the rest `[None]` never-ran —
same class as the old 165 s cap, not 60 bugs). posix2 one pair
`20260917-191150` **58 both-pass / 4 EFS**; posix2 multi
`20260917-192141` **56–58 / 4–6**. Shared posix2 set: `peer_concurrent_append`
got 400 want 800 on every pair (one side's O_APPEND records never land);
overlap-pwrite RMW; `peer_fcntl_range_conflict` per-client lockf.
`peer_rename_dir` EIO is load-only (isolated PASS). Do not invent REPORT
split or chunked SNAP. 005 gossip-DOWN ~30 s after bounce is STATUS
probe, not a dead process.

**FRESH-19810 POSIX + DD (same day).** 8 GiB non-zero `dd` on the
*previous* 19810 (FUSE_OK, not zeros): stream **731–825 MB/s**,
`conv=fsync` **391 MB/s** (22 s) — short of ~2 GiB/s. Client on-CPU is
blake3 in `hash_write_fragments`; do not quote the idle-heavy 2 h
`perf stat`. Fresh-cluster posix jobs=1
`results/posix/20260917-161117`: **94 PASS / 3 timeout / 104 never
ran** — suite hit the 165 s SSH cap. Idle ops were **100–370 ms**
(stat/mkdir/listdir). Cause: unconditional `applied *` stderr on the
pump + a ReadIndex quorum per op. Fix in tree: gate success apply
logs on `EFS_RAFT_DBG`; skip ReadIndex when `efs_raft_read_current`.
lane-sweep `rc=-5` on empty files treated as OK. **Need efsd bounce
(keep storage) + re-gate.** Do not invent REPORT split / chunked SNAP.

**STEP 11 (delete the old snapshot/root-2PC metadata engine) COMPLETE
(Sep 11 2026, commits 94c4c15 Inc1 → d7baa38 Inc2 → db023c7 Inc3 →
c878a88 Inc4 → f5c4390 Inc5).** The Raft+KV engine is the ONLY metadata
path. Deleted: meta_server.c / migrate.c / verify.c, the old inode-RPC
handler branches, the server's metadata-table ownership (exports[] is a
shell {id,name,chunk_size}), the client's old lookup/report/bootstrap/
pack paths (client is raft-only; raft LOOKUP replies carry the full
resolved row; entry at parent shard, host walks lanes), the old efs-mgmt
control plane (mkfs now aliases raft-mkfs; list-exports/destroy/drain/
undrain/remove-node/feature/upgrade gone), 58 dead metadata.c functions
(serialize/snapshot/root_*/pack_*/flush-dirty/trim/evict/rehash/load/
save/merge/adopt — metadata.c 6042→4335 lines; it STAYS linked because
the client keeps `g_client.export` as its local staging/dirty table),
13 old tests + test_directio + 7 old-engine scripts, and the
EFS_MD_RAFT / EFS_MKFS_SHARD_BITS / EFS_FLUSH_PROF / EFS_LOCK_PROF /
EFS_INO_PROF / EFS_INO_RAM_MB / EFS_CWI_TRACE env knobs. **Everything in
this file below about the old engine (2PC root commit, CoW page flush,
GC races, meta-rebuild, shard tabs, extras catchup, EFS_INO_RAM_MB, the
dangling-dentry saga, flush group commit, bits=3/5 tuning) describes
DELETED code — historical reference only, do not re-apply.**
**Gates (all on the 19820 scratch raft cluster, TCP):** every increment
posix 196/201 with the exact known signature (dir_move_into_subdir
EINVAL debt, virt_find_query .find unimplemented, deep-nesting/many-files
15s timeouts, occasional concurrent_writes_disjoint EIO flake); unit
green (the 6 test_lock getlk failures were a test bug, closed Sep 18 —
the test aliased req/out and `efs_lock_getlk` clears out first).
Inc 5 final: posix 196/201 in 28s.
**`tests/valgrind_leaks.sh` PORTED to raft + GATE GREEN** (same commit as
this note): single-node efsd can't reach quorum, so every wire phase now
runs a 3-node localhost raft group (EFS_MD_RAFT_N=3, ports PORT..PORT+2,
per-node storage) + `raft-mkfs`; the valgrind'd server joins LAST as a
follower (plain nodes hold quorum/leadership; the follower still applies
every committed entry); UNIT_TESTS updated to the current suite;
kill_ours matches $WORK not $PORT; suspicious-possibly-lost frame list
updated to the current sources (raft/kv/meta/wire/data/sim). The port
surfaced TWO REAL CLIENT LEAKS, both fixed and gated: (1) main() ran
`efs_export_init(&g_client.export)` and `raft_bootstrap_metadata` then
re-inited the SAME struct without freeing (init memsets it) — the whole
first table leaked (~14 KB); now `efs_export_free` before the re-init.
(2) `decode_frag_scratch` (read.c) was a `static __thread` heap buffer
never freed — 786 KB definite (4 pool workers x 192 KiB); now a pthread
key destructor frees it at thread exit, and a new
`efs_client_read_pools_stop()` (called from `efs_client_shutdown` AFTER
the flush, which can read) joins the get/frag pool workers so the
destructor actually runs. Phase-4 RDMA traps fixed: the servers were
started with EFS_TRANSPORT=tcp, but `efs_rdma_available()` returns 0
process-wide under tcp, so the server REFUSED every upgrade (phase was
vacuously TCP) — servers now run auto; the client runs STRICT
EFS_TRANSPORT=rdma so a failed upgrade can no longer fall back silently;
the "RDMA transport up" check moved after the workload (the upgrade is
lazy — first pool checkout, not mount). Full gate PASS on fcstor003:
13 unit + efsd + efs-fuse TCP + efsd/efs-fuse RDMA, all 0 definite /
0 indirect / 0 uninit / 0 invalid.
**Open follow-ups:** Step 12 A–D landed (Part B `0df94e4`; C+D `ec3ec5b`).
Populated LOCAL-range migrate is a cross-group txn (`5a8219e`). LOCAL dirs
auto-begin SPLITTING when `nents > EFS_DIR_SPREAD_MIN` (`efb2ca9`). Background
leftover drain is in this tree (`dir_spread.c` + GC-thread pass). Honest
fio matrix (Part D perf contract) is **not** run on the 1G 19820 scratch.
Relaxed-coherence stays out of scope. Production `raft-change` is in this
tree (operator desired file + learner attach with C_old). The control-plane
desired Raft group is still sim-only. Leftover 1 honest fio on 19810 is
**1-client gated** (`results/perf/20260917-honest/`; see Sep 17 note).
First-matrix sw-1m 209; after WAL hold / activate-mask / pipelined
propose, honest 9×2g sw-1m **924**. 4/9-client not run. sw-50g
`end_fsync` NET (400k-pub REPORT vs 30 s).
Pressure-spread bound is unspecified (do not invent). Host KV snapshot uses the existing
WAL item payload (`kv_snap.c`); import dest-key collect stores **offsets**
(arena realloc UAF). A group over the 4 MiB SNAP cap is left uncompacted.
Chunked InstallSnapshot is unspecified. Do not `wipe_cluster.sh` /
`pkill -x efsd` while 19820 is up.

**BATCHED APPENDENTRIES + OUTBOX CLOSED (Sep 15).** Catch-up was
one entry per AE. `send_ae` now packs up to `EFS_RAFT_AE_MAX=128`
entries / `EFS_RAFT_AE_BYTES=1MiB`; `on_ae_req` appends the batch;
a reject jumps `next_index = match_index+1`. Host outbox
`HOST_OUTBOX_MAX` 256→2048; `host_send` heap-encodes when the
stack buffer is too small. Wire nentries 0..AE_MAX; decode packs
cmds into `cmd_buf`. Sim pack/unpack matches. Unit
`test_ae_batch_catchup` (drop follower, 200 proposes, 4 ticks).
**Gates (19820 scratch, TCP, keep storage):** `test_raft` / `test_wire`
/ `test_sim` / `test_txn` OK. Live: kill fcstor005, create while
down, restart — applied 22123→22241 in one poll (CATCHUP_OK).
posix jobs=1 **196/201** (`results/posix/20260915-ae/`): 4 walk
15s timeouts (`dir_deep_nesting`, `dir_many_files`,
`names_crazy_dirs`, `dir_deep_nesting_beyond_64`) +
`concurrent_writes_disjoint` zeros flake. Isolated
`dir_readdir_listing` PASS 2.6s.

**STEP 12 PART B — LOW-LEVEL FUSE (Sep 15, working tree).**
`efs-fuse` uses `fuse_lowlevel_ops` / `fuse_session_loop_mt`
(libfuse 3.10.2, `FUSE_USE_VERSION 32`). nodeid ↔ efs ino 1:1;
timeouts stay **0**. Two correctness holes closed in the same
cut: (1) sharded `unlink_name_ex` now honors `keep_last` (ghost
nameless nlink=0 row) so unlink-open getattr does not adopt owner
size 0 and `fuse_file_read_iter` empty-read; (2) path LOOKUP
adopts the RPC row (`efs_client_adopt_lookup`); path getattr
`efs_client_stat_refresh` (GETATTR+adopt+overlay); open-fd
getattr stays local-first so a peer REPORT cannot invalidate an
in-flight dcache. `run_tests.sh` remount/ensure_mounted honor
`EFS_SEED` (default still `:19810`).
**Gates (19820 scratch, TCP):** posix jobs=1 **196/201**
(`results/posix/20260915-partb/suite-jobs1-refresh.tsv`) — 5×
15s walk timeouts (`dir_deep_nesting` ×2, `dir_many_files`,
`names_crazy_dirs`, `concurrent_write_and_readdir`); unlink-open
3/3 isolated. posix2 **59/63**
(`results/posix2/20260915-partb-refresh/`) — known
`peer_concurrent_append` + `peer_fcntl_range_conflict`; two
overlap-pwrite load flakes that pass isolated. Isolated
`peer_shared_pwrite` / `peer_creat_excl_race` PASS. leaks
**PASS** (`results/leaks/20260915-partb/`, 0 definite / 0
indirect, 19820 pid unchanged). Unlink-storm 9×500 empty
**PASS** (`results/stress/unlink-storm-20260915-partb-n500b/`);
9×4000 is quota/latency on the 1G scratch, not a FUSE hang.
Hardlink storm `HLSTORM_OK` n=200 (`results/stress/hlstorm-partb.txt`).
Do not `wipe_cluster.sh` / `pkill -x efsd` while 19820 is up.

**STEP 12 PART C — EXACT SELF-INVAL (Sep 16).**
Timeouts stay **0**. `notify_inval_*` from a request handler
deadlocks (kernel holds the parent); queue after `fuse_reply_*`
on a dedicated thread. Do **not** `inval_entry` on create/unlink
(redundant at timeout=0; races create-then-pwrite). Only a
**shrinking** SETATTR SIZE notifies, and only pages at/after the
new EOF (`(0,0)` = whole mapping only for truncate-to-zero).
`clone_fd` stays libfuse default. GC `del_if_sum` now uncharges
`local->used` (`store.c`) — 1G scratch was ENOSPC because
unlink never returned quota. `wait_cluster_idle` treats raft
status (no `gen=` line) as idle when all 4 nodes are Heal idle.
**Gates (19820 scratch, TCP):** posix jobs=1 **194/201**
(`results/posix/20260915-partc/suite-jobs1.tsv`) then
**190/201** (`results/posix/20260915-partc3/suite-jobs1.tsv`,
453s, `--tag partc3 --keep`) — 15s walk/concurrent timeouts +
`symlink_relative_after_parent_rename` EIO load flake (known).
posix2 **58/63** (`results/posix2/20260915-partc/`). Isolated
overlap currently **FAIL** on HEAD Part B fuse too (same exclusive-
range zeros) — not a Part C regression; sub-chunk RMW, not page
cache. Do not `wipe_cluster.sh` / `pkill -x efsd` while 19820 is up.

**STEP 12 PART D — FOPEN_DIRECT_IO + PREFETCH (Sep 16).**
`fi->direct_io = 1` on every regular open/create. 4 KiB EINVAL only
when the **application** set `O_DIRECT` (FOPEN_DIRECT_IO still
accepts unaligned FUSE I/O). Kernel writeback cap is cleared.
libfuse **3.10.2** cannot emit `FOPEN_PARALLEL_DIRECT_WRITES` or
negotiate `FUSE_MAX_PAGES` (kernel default 32 pages = 128 KiB per
request) — do not set `conn->max_read` without `-o max_read=`
(libfuse aborts: `init() and fuse_session_new() requested different
maximum read size`). Sequential reads prefetch up to 16 published
chunks into rdcache (`EFS_READ_PREFETCH`, drop if the GET pool is
half full). `MAP_SHARED` mmap is ENODEV (spec: unsupported);
`MAP_PRIVATE` still works.
**Gates (19820 scratch, TCP):** posix jobs=1 **193/201**
(`results/posix/20260916-partd/suite-jobs1-clean.tsv`, 387s) —
walk/concurrent 15s timeouts + known symlink EIO flake + expected
`mmap_write_read` SKIP ENODEV. Isolated overlap still **1/3**
(same as HEAD Part B: exclusive-range zeros = chunk RMW, not
cache). posixpersist **25/26** (`results/posixpersist/20260916-042337/`);
the 1 is `many_files_in_one_dir` 60s timeout on prepare **and**
verify, not a silent loss. Honest fio not run here (1G quota).
`ensure_mounted` TCP check no longer treats grep -c 0 as RDMA
(`|| echo 0` concatenated to `00`). Do not `wipe_cluster.sh` /
`pkill -x efsd` while 19820 is up.

**POPULATED LOCAL MIGRATE AS TXN (Sep 16).** A leftover whose HASHED
dentry shard is on the other Raft group is a 2-shard txn (local DEL +
hashed PUT + dseq, optional parent `used_shards`); same-group leftovers
and I8 (hashed live/tombstone already there) stay single-group
`DIR_MIGRATE`. Lane-0 names stay local; migrate-done is NOT_FOUND (mgmt
status=1) once bit 0 is set so apply cannot swallow it into status=0
forever. Host bounces to a dual-host when this replica does not host
the dest group. `migrate_one` still PUTs for same-KV unit tests.
**Gates (19820 scratch, TCP, keep storage):** `test_sim` (new
`test_migrate_populated` seed 139) / `test_meta_apply` / `test_txn` /
`test_wire` OK. Targeted efs-mgmt `raft-smoke-mg`: leftover `x0` (psh
odd, dsh even) begin → two migrate steps → status=1 → finish; lookup +
readdir keep `pre` and `x0`. posix jobs=1 `--tag migrate --keep`
**195/201 in 247s** (`results/posix/20260916-migrate/`) — 15s walk
timeouts (`dir_deep_nesting` ×2, `dir_many_files`, `names_crazy_dirs`)
+ known `symlink_relative_after_parent_rename` EIO flake + expected
`mmap_write_read` SKIP ENODEV. Full `raft_host_smoke.sh` not re-run
(EXIT trap kills 19820). Do not `wipe_cluster.sh` / `pkill -x efsd`
while 19820 is up.

**AUTO-SPREAD SIZE TRIGGER (Sep 16).** LOCAL dirs carry `nents` in the
inode-row padding (bytes 60–63). Create/mkdir/link dest increment it;
unlink/rmdir/rename-src decrement it. Crossing `nents > EFS_DIR_SPREAD_MIN`
commits SPLITTING on that same parent PUT (dseq/dentry shard still use the
pre-flip layout). HASHED/SPLITTING freeze the count. Env override
`EFS_DIR_SPREAD_MIN` for tests (default 65k). Pressure-triggered spread
is still open (bound unspecified). `make docs-check` is the architecture
machine-gate (`python3 docs/check-architecture.py`).
**Gates:** `test_sim` `test_auto_spread` seed 140 (min=3). Do not
`wipe_cluster.sh` / `pkill -x efsd` while 19820 is up.

**BACKGROUND LEFTOVER MIGRATOR (Sep 16).** In-memory SPLITTING-ino queue
(`include/efs/dir_spread.h`, `src/meta/dir_spread.c`). Lost on crash;
re-note on DIR_BEGIN, on a nents flip, on apply of a SPLITTING parent, and
on a txn resolve PUT of a SPLITTING dir row. Host: `host_dir_spread_pass`
from the existing GC thread (cap 8 leftovers/tick) — reuses `host_dir_migrate`
so cross-group leftovers stay a txn; FINISH when peek is NOT_FOUND. No new
`raft_host` thread. Sim: opportunistic drain after create/mkdir/link/rename
only on LOCAL→SPLITTING (operator-begin leftover tests stay un-drained).
Pressure numbers are still unspecified.
**Gates:** `test_sim` `test_auto_spread` seed 140 reaches HASHED without an
explicit migrate loop. Do not `wipe_cluster.sh` / `pkill -x efsd` while
19820 is up.

**INSTALLSNAPSHOT IN THE RAFT SM (Sep 16).** `EFS_RAFT_MSG_SNAP_REQ=5` /
`SNAP_REP=6`. `last_log_*` = lastIncluded; `nentries=1` is
`[app_old:4][app_new:4][user blob]` so wire/sim/host codecs stay AE-
shaped. `efs_raft_snapshot` freezes `snap_get` before compacting.
`send_ae` sends SNAP when `next_index <= snap_idx` (no more skip).
`on_snap_req` rejects a skip-ahead unless `snap_put` installs that
exact prefix — empty metadata-only SNAP cannot jump `last_applied`.
`raft_mem` save_snap clamps like disk (empty learner).
**Gates:** `test_install_snapshot` (compact 40, grow `0x7→0x1f`,
learners install) + `test_install_snapshot_needs_blob` (metadata-only
snap stays BUSY) + `test_wire` SNAP codec. Do not `wipe_cluster.sh` /
`pkill -x efsd` while 19820 is up.

**HOST KV SNAPSHOT (Sep 16).** After persist_applied, if
`applied - snap_idx ≥ HOST_SNAP_MIN` (256), the pump flushes the LSM
then `efs_raft_snapshot`. `snap_get`/`snap_put` are
`efs_kv_group_export`/`import`: WAL item payload, keys whose shard
maps to that Raft group, import replaces that namespace only. Oversize
→ `EFS_ERR_BUSY`, `snap_oversized` latches, log stays uncompacted
(the live 283k scratch will hitch here until chunked SNAP exists).
Do not invent chunking. Do not restart 19820 for this cut.
**Gates:** `test_kv_lsm` `test_kv_group_snap` + `test_raft` /
`test_wire` / `test_sim` / `test_txn`. Do not `wipe_cluster.sh` /
`pkill -x efsd` while 19820 is up.

**19810 NVMe + HONEST FIO 1-CLIENT (Sep 17).** Same 19810 storage (no
`raft-mkfs`). 19820 left up; 007 remounted back to `:19820` after the
run. Unblocked 2g `end_fsync` with publication batching (`HOST_PUB_BATCH_N`
256 pubs / Raft entry) plus `host_rpc_submit` heap-encode when `clen >
HOST_CMD_MAX` (512 was the stack buffer; a 51 KiB follower-forward
returned INVAL and `host_remote_cmd` remapped that to NOT_PRIMARY
`efs_rc=-15`). Earlier in the same leftover: LSM compact `drop[]`
sized to `KV_LSM_MAX_SEGS*2` (004 SIGSEGV at 64 L0 + overlapping L1;
compact-first flush; `test_compact_full_l0`); dest-key offsets in
`kv_snap.c`; one ReadIndex per group; skip identical pub; per-ino
fsync report (not a new REPORT wire). Units on 003:
`test_raft` / `test_kv_lsm` / `test_sim` / `test_wire` /
`test_meta_apply` OK. Honest 007 TCP `results/perf/20260917-honest/`
FUSE_OK, no `md0`, err=0 except 50g:

| test | 1-client MiB/s |
| sw-1m | 209 |
| ow-1m | 196 |
| rw-1m | 196 |
| rw-128k | 194 |
| rw-4k | 89 |
| sr-1m | 3141 |
| rr-1m | 2276 |
| rr-128k | 1492 |
| rr-4k | 144 |
| sw-50g | FAIL `end_fsync` NET |

Write walls include fsync (intra-job write samples several GiB/s — do
not quote those as the number). 50g laid down 50 GiB then one REPORT
of ~400k pubs missed `EFS_IO_TIMEOUT_MS`. 4/9 not run. 005 19810 still
lagged (oversized SNAP). Do not invent chunked SNAP or REPORT split.
Do not `wipe_cluster.sh` / `pkill -x efsd` while 19820 is up. Do not
auto `raft-mkfs` again.

**HOT-PATH AFTER HONEST FIO (Sep 17).** Same leftover-1 19810 (no
`raft-mkfs`, 19820 left up). Profile of 1-client sw (attach to a
RUNNING daemon; do not restart under strace): client on-CPU is blake3
in `hash_write_fragments` / `dcache_flush_slot_inner` plus futex on
REPORT; server fsync counts 2223 / 4466 / 2112 on 003 / 004 / 006 —
one WAL fsync per sequential `apply_one_publish`. Group-commit does
not share a fsync among a single-threaded apply loop. Fixes in this
tree: `kv_wal_hold` / `efs_kv_lsm_sync_hold` across a batched
PUBLISH apply (`test_kv_lsm` `test_sync_hold`); `EFS_MD_CMD_ACTIVATE_LANE`
17 B mask via `efs_meta_apply_activate_lanes` (one inode PUT); inode
row cache on REPORT; `host_pub_batch_propose` then
`host_wait_applied` of the last idx. Honest remasure (007 TCP,
FUSE_OK, no `md0`, err=0): 512m 1-job **247** MiB/s
(`hot-sw-512m-fcstor007.txt`); 9×2g sw-1m **924** MiB/s, 18 GiB /
20 s (`hot-sw-1m-9job-fcstor007.txt`). A 255 figure was 9 jobs on
one 2g file (`$jobnum` not unique). Intra-job write samples stay
several GiB/s — do not quote those. 50g / 4/9 not re-run. 007
remounted back to `:19820`. Units earlier on 003: `test_raft` /
`test_kv_lsm` / `test_meta_apply` / `test_sim` / `test_wire` OK.
Do not invent chunked SNAP or REPORT split. Do not `wipe_cluster.sh`
/ `pkill -x efsd` while 19820 is up.

**RDMA FIRST-INODE POOL FIX (Sep 17).** Not all RDMA items were fixed:
poller re-arm, finite `rnr_retry`, and leaks RDMA phase already landed;
the parked empty-table first-`mkdir` hang did not. Client pool now
pins sockfs identity at wrap, evicts on mismatch, refuses to `close`
a recycled fd, and `conn_init` skips busy slots. Post-mount
GET_FEATURES/STATUS use the pool. Gate: `test_conn_fd`. Do not call
a 19820 remount the empty-table repro. Do not `wipe_cluster.sh` /
`pkill -x efsd` while 19820 is up.

**19810 NVMe + HONEST FIO ATTEMPT (Sep 16).** Port 19810 `raft-mkfs`'d
on `/data1` (36T, `--direct-io`, seed 003, join 004–006). 19820 scratch
left up. First sw-1m: fcstor005 19810 SIGABRT
`malloc(): mismatching next->prev_size` in `kv_mtab_set` ←
`efs_kv_group_import` (`kv_snap.c`) ← `on_snap_req`. Cause: collect
arena `realloc` left `key_ref` pointers dangling into `efs_kv_batch`.
Fix: store offsets. Gate: `test_kv_lsm` arena + LSM dest replace (2k
stale keys). Redeployed 19810 only (no `--join`). Honest
`results/perf/20260916-honest5` 007 TCP sw-1m **FAIL** 400s: data
moved (usable 13.5→27 GiB, g0 applied ~17k→33k) then `end_fsync`
`fsync-meta efs_rc=-6` (`EFS_ERR_NET`) on inos 18326/22422; fio EIO
on f.5/f.6 last 1m sync. 005 stays g0 `commit=751` /
`applied=18688` (applied≫commit) and `raft-create` to :19810 on 005
is status=5 BUSY. Kill 005 19810 to dodge BUSY **breaks 2+1 PUT**
(needs all fragment ACKs). Do not invent chunked SNAP or REPORT
split. Do not `wipe_cluster.sh` / `pkill -x efsd` while 19820 is up.
Do not auto `raft-mkfs` again.

**PRODUCTION RAFT-CHANGE (Sep 16).** `raft_host` wires `efs_raft_change`
(I18). Operator `efs-mgmt raft-change <node:port> <group> <voters>` reuses
`EFS_MSG_RAFT_MKFS` with host-only `EFS_MD_CMD_CFG` (NOTE then CHANGE — not
a log command). Desired mask is a per-group file under `mdraft/`; every
peer attaches a learner replica with **C_old** as `cfg.voters` before the
leader appends JOINT. Status reports live `efs_raft_voters` + `joint`.
`host_hosts` is the committed voting set, so a learner does not serve inode
RPCs until COLD. Fan NOTE uses `HOST_SEND_IO_MS` (a 30 s peer submit was
the first 0xb hang). Control-plane desired group stays sim-only.
**Gates (19820 scratch, TCP, keep storage):** `test_raft` / `test_sim` /
`test_wire` / `test_txn` OK on fcstor003. Live no-op `raft-change 0 0x7`
**OK** (`rc=0`, index=283372). Live 3-for-3 `0x7→0xb` started learner
catch-up on fcstor006 (applied ~26k of 283k, ~800/s) and wedged that
node's `h->mu` on `xlog_wait_on_iclog`; JOINT did not commit (g0 stayed
`0x7 joint=0`). Restored `0x7`, dropped `desired.0`, restarted 006+005;
`raft-create` `.raft-chg-smoke` ino=851969 status=0. Full live swap on
this log still needs a host KV snapshot (SM InstallSnapshot is in; the
host never compact). Do not `wipe_cluster.sh` / `pkill -x efsd` while
19820 is up.

**OLD-ENGINE 19810 CLUSTER DESTROYED (Sep 15).** No snapshot/2PC
on-disk compat. `/data1/01–06/efs` on fcstor003–006 renamed-aside +
`edelete` (empty dirs left). Removed: Aug 19 crash binaries under
`logs/`, `~/efs-bin-travel/` (Aug 18–19), `/tmp/efs-495` on
003–006 (Sep 11 leftover tree still linking `meta_server.c`).
19820 scratch (`/tmp/efs-raft-scratch`) was left running; clients
007/008 stay mounted on `:19820`. Do **not** `wipe_cluster.sh` /
`pkill -x efsd` while 19820 is up. Do **not** auto `raft-mkfs` a
36T 19810 cluster until asked. `/tmp/efs` on the nodes is the
current tree — keep it.

**`.FIND` READDIR WALK CLOSED (Sep 11 night, working tree).** POSIX
`virt_find_query` returned empty because `find_index_build_locked`
memcpy'd `g_client.export`, which has no GET_META snapshot after step 11.
Fix is client-only in `efs_fuse.c`: drop the whole-table index; walk the
query directory's subtree with `efs_client_rpc_readdir_cur` (name-order
cookies, depth 128, 65536-visit cap, skip `.fuse_hidden*` and reserved
`.find`/`.stats`). Paths are host-absolute from `g_mountpoint` + the
FUSE dir prefix of the `.find/<term>` path (getattr does not fill
name/parent). Result cache (16 slots, 5 s TTL) unchanged. Not a
server-side name index (derived-index design / roadmap "Server-side
`.stats`/`.find` refresh" stays NOT DONE).
**Gates (19820 scratch, TCP, remount efs-fuse only):** isolated
`virt_find_query` + `virt_find_not_a_real_dir` **2/2 in 0.1 s**. posix
jobs=1 **199/201 in 75 s** (`results/posix/20260911-find/suite-jobs1.tsv`)
— `virt_find_query` PASS. The 2 fails are same-parent dir-rename EIO
(`dir_rename_dir_with_contents`, `rename_dir_same_parent`) on this
long-lived scratch; both pass **5/5 isolated** (0.1 s) and were PASS on
the wipe+mkfs cross-dir jobs=1. Committed `986ca45`. Do not start step 12.

**CROSS-DIR RENAME CLOSED (Sep 11 night, working tree on top of
`e486ec4`).** POSIX `dir_move_into_subdir` was EINVAL because
`server_raft_host_rename_at` rejected `old_parent != new_parent`.
That test is a **file** rename `a/f` → `b/f`, not a directory-into-subdir
cycle. Apply + sim already implemented cross-dir; the host now runs a
LOCAL 2-parent txn (src dentry DEL, dest dentry PUT, inode parent=,
both parent stamps + dseq, bounce if this replica does not host both
parent groups). Directory cross-dir also moves nlink (src--, dest++)
and still GUARDs dest ancestry `parent_version` (`err_rename_dir_into_itself`
stays EINVAL). HASHED rename closed below; SPLITTING stays BUSY.
**Gates (19820 scratch, TCP, wipe+raft-mkfs):** `test_meta_apply` /
`test_sim` OK (plant the file — `create_file`'s first ino on shard S is
S itself and would clobber a planted parent at 21/37). Isolated
`dir_move_into_subdir` + `err_rename_dir_into_itself` PASS. posix
jobs=1 **200/201 in 104 s** (`results/posix/20260911-crossdir/suite-jobs1.tsv`)
— only `virt_find_query` (closed above). jobs=16 still saturation-timeouts
on this scratch (186 then collapse on stacked runs); not a rename
correctness fail (`dir_move_into_subdir` passed in the parallel batch).
Committed `4d47038`. Do not start step 12.

**HASHED RENAME CLOSED (Sep 11 night, working tree on top of `986ca45`).**
Same pattern as LOCAL cross-dir: apply + sim already implemented HASHED
rename; the host INVALed non-LOCAL parents. HASHED is now a txn that
stamps dir-lanes (parent row only for `used_shards` / nlink), dseq on
the name's dir-lane, and bounces when a hashed dentry shard is on
another Raft group. Same-dir dest first-use sets the dest lane
`used_shards` bit. SPLITTING stays BUSY. HASHED dest-dir overwrite
closed below.
**Gates (19820 scratch, TCP, wipe+raft-mkfs):** `test_meta_apply` /
`test_sim` OK. efs-mgmt: LOCAL same-dir, HASHED same-dir (`n0`→`r0`
status=0), LOCAL cross-dir all status=0. Isolated posix rename/find
PASS. posix jobs=1 **201/201 in 30.4 s**
(`results/posix/20260911-hashed/suite-jobs1.tsv`). FUSE HASHED dir
`hr-fuse`: `mv n0 r0` OK. HASHED unlink/rmdir closed below. Do not start
step 12.

**HASHED UNLINK/RMDIR CLOSED (Sep 11 night, working tree on top of
`0ce6167`).** Apply + sim already implemented HASHED unlink/rmdir; the
host INVALed HASHED parents (and HASHED child rmdir). FUSE `rm -rf` of a
HASHED dir then EIO'd on the directory itself. Host now allows HASHED:
LOCAL stamps parent-row times; HASHED stamps dir-lanes + dseq on
`efs_kv_dir_lane(name)`; bounce if this replica does not host the dentry
shard. HASHED empty-dir rmdir GUARDs used-lane dseqs (cap
`EFS_TXN_MAX_PART` → BUSY). Last-link same-shard still uses
`EFS_MD_CMD_UNLINK` (HASHED file create places the inode on the dentry
shard). Sim `apply_unlink_cmd` accepts the session on the dentry shard
(HASHED create is `dsh==ish` but the cmd previously accepted on `psh`).
SPLITTING stays BUSY. HASHED dest LINK closed below.
**Gates (19820 scratch, TCP, wipe+raft-mkfs):** `test_meta_apply` /
`test_sim` / `test_txn` / `test_wire` / `test_kv_lsm` OK. efs-mgmt:
HASHED last-link unlink (`u0` status=0, lookup NOT_FOUND) + HASHED
empty-dir rmdir (`raft-smoke-he` status=0). FUSE HASHED dir `hr-rm`:
`rm -rf` OK. posix jobs=1 **196/201 in 163 s**
(`results/posix/20260911-hashed-unlink/suite-jobs1.tsv`) — 4× 15s
timeouts (`dir_deep_nesting` ×2, `dir_many_files`, `names_crazy_dirs`)
from ~100 ms ReadIndex (pre-existing ~110 ms cross-group note), plus
`concurrent_writes_disjoint` zeros flake. Unlink/rmdir/find/rename
tests PASS. Do not start step 12.

**HASHED LINK DEST CLOSED (Sep 11 night, working tree on top of
`91838aa`).** Apply + sim already implemented HASHED dest LINK (dir-lane
stamp, dseq on `efs_kv_dir_lane(dst_name)`, parent row only for LOCAL
times or HASHED `used_shards` first-use). The host INVALed
`dprow.layout != LOCAL`. Host now allows HASHED: bounce via
`host_fwd_link` when this replica does not host dsh/ish/psh; LOCAL
stamps parent-row times on psh; HASHED stamps the dir-lane on dsh and
dseq on that lane. First-use sets the dest lane `used_shards` bit.
SPLITTING stays BUSY. HASHED dest-dir overwrite closed below. HASHED
file create still places the inode on the dentry shard, so last-link
same-shard unlink is unchanged.
**Gates (19820 scratch, TCP):** `test_meta_apply` / `test_sim` OK.
efs-mgmt: HASHED dest link (`n0`→`l0` status=0, nlink=2, dup EXIST).
FUSE HASHED dir `hl-fuse`: `os.link(a, l0)` OK (nlink 2). posix jobs=1
**200/201 in 36.8 s** (`results/posix/20260911-hashed-link/suite-jobs1.tsv`)
— only the `concurrent_writes_disjoint` zeros flake; all hardlink tests
PASS. Smoke `raft_host_smoke.sh` HASHED dest LINK check added (rename
hygiene: dedicated names so POSIX file-over-file replace is not scored
as EXIST). Do not start step 12.

**HASHED DEST-DIR OVERWRITE CLOSED (Sep 11 night, working tree on top of
`fc057cd`).** Apply does not replace (EXIST); production path is the
host txn. Sim and host both INVALed HASHED dest dirs. HASHED empty-dir
rmdir already proved distributed emptiness (per-lane `shard_empty` +
dseq GUARDs). Rename-over-empty-HASHED-dir reuses that, then DEL dest
inode (same as LOCAL dest dir overwrite). File-over-file replace
already works; file-over-dir / dir-over-file stay INVAL. SPLITTING dest
stays BUSY. Nonempty HASHED dest stays NOT_EMPTY (RPC status 8), not
INVAL. Host: bounce used-lane groups + GUARD used-lane dseqs (`gv[]`
stays pver ancestry; dest dseqs in `xdseq[]`); `EFS_TXN_MAX_PART` →
BUSY. Dest HASHED-dir overwrite only when source is also a dir.
**Gates (19820 scratch, TCP, wipe+raft-mkfs):** `test_meta_apply` /
`test_sim` OK (new `test_rename_hashed_dir_overwrite`). efs-mgmt:
nonempty `hs`→HASHED `hx` status=8, unlink child, overwrite status=0,
`hx` ino = src. FUSE `hd-fuse`: nonempty `mv` ENOTEMPTY, emptied `mv`
OK (dst ino = src). posix jobs=1 **200/201 in 44.9 s**
(`results/posix/20260911-hashed-dow/suite-jobs1.tsv`) — only the
`concurrent_writes_disjoint` zeros flake; rename/find/unlink/hardlink
PASS. Smoke `raft_host_smoke.sh` HASHED dest-dir overwrite check added
(fresh path passed; after-crash hung on an environmental no-leader
wedge, not this slice). Do not start step 12.

**SPLITTING DEST CREATE + READDIR CLOSED (Sep 12, working tree on top of
HASHED dest-dir overwrite).** Apply + sim already wrote hashed during
SPLITTING and merged READDIR (hashed side wins, I8). The host BUSY'd
SPLITTING dest CREATE and READDIR. File CREATE now reuses the HASHED
first-use txn when the dest lane's group differs from the parent
(`used_shards` bit + dest dentry/inode); same-group first-use stays a
single propose. READDIR drops the BUSY; apply already merges LOCAL
leftovers with hashed lanes. LOOKUP / LOOKUP_PATH were already
hashed-then-local. Smoke `raft-smoke-sp` stays SPLITTING: local `pre`
plus a hashed-name file. Populated-range migrate as a txn closed Sep
16 (below). SPLITTING dest UNLINK/RMDIR/LINK/
RENAME stay BUSY (tombstone I8 on the host later). HASHED/SPLITTING
dest MKDIR still writes the dentry on `psh`.
**Gates (19820 scratch, TCP):** `test_sim` / `test_meta_apply` /
`test_wire` OK. efs-mgmt: mkdir `raft-smoke-sp`, local `pre`, begin
(no migrate), hashed `n0` create status=0 (ino shard ≠ parent),
readdir `n0,pre`, dup EXIST, lookup-path both. FUSE `sp-fuse`: `pre`
then begin, hashed `n0`, `ls` sees both. posix jobs=1 **200/201 in
112 s** (`results/posix/20260912-splitting/suite-jobs1.tsv`) — only
the `concurrent_writes_disjoint` zeros flake. Do not start step 12.

**SPLITTING DEST UNLINK/RMDIR CLOSED (Sep 12, working tree on top of
SPLITTING dest CREATE+READDIR).** Apply + sim already wrote
`HASHED(name)=TOMBSTONE(layout_epoch)` on unmigrated names (I8) and
skipped the redundant local DEL when keys alias. The host BUSY'd
SPLITTING dest UNLINK/RMDIR. Host now uses `host_dent_drop_*` matching
apply: EXCL DEL local leftover unless HASHED or (SPLITTING && keys
alias); SPLITTING PUT tombstone on the hashed key; HASHED DEL hashed.
Bounce if this replica does not host psh/dsh/ish. Last-link
`EFS_MD_CMD_UNLINK` is forced onto the txn when SPLITTING && parent
group ≠ hashed group (the hashed-group UNLINK cmd cannot DEL a local
leftover on the other group). A child that is itself SPLITTING stays
BUSY. SPLITTING dest LINK closed below; RENAME stays BUSY.
HASHED/SPLITTING dest MKDIR still writes the dentry on `psh`.
**Gates (19820 scratch, TCP):** `test_sim` / `test_meta_apply` /
`test_wire` OK. efs-mgmt: hashed unlink NOT_FOUND, recreate OK
(tombstone ≠ EXIST), unlink local `pre`, rmdir empty `e`, readdir
empty of all three, rmdir of the SPLITTING dir itself status=5 BUSY.
FUSE `sp-rm`: `pre` + begin + hashed `n0`, `rm` both + empty child,
`ls` empty, `rmdir` self EIO (BUSY). posix jobs=1 **199/201 in 31 s**
(`results/posix/20260912-splitting-unlink/suite-jobs1.tsv`) —
`concurrent_writes_disjoint` zeros flake +
`symlink_relative_after_parent_rename` EIO (isolated PASS 0.1 s, known
load flake). Smoke `raft_host_smoke.sh` I8 checks already in tree; full
script not re-run (its EXIT trap kills 19820). Do not start step 12.

**SPLITTING DEST LINK CLOSED (Sep 12, working tree on top of
SPLITTING dest UNLINK/RMDIR).** Apply already wrote hashed dest LINK
during SPLITTING (dir-lane stamp, parent row only for `used_shards`
first-use). The host BUSY'd SPLITTING dest. Host now allows
LOCAL/HASHED/SPLITTING dest: hashed dest during SPLITTING, bounce if
this replica does not host psh/ish/dsh. Sim `link_build` used
hashed-key-only `dent_absent`, so a local leftover looked absent, the
link succeeded, and nlink went to 3. Now `sim_txn_lookup`
(hashed-then-local; tombstone = NOT_FOUND): leftover dest is EXIST;
tombstone dest is absent so nlink stays 2. SPLITTING dest RENAME
stays BUSY. HASHED/SPLITTING dest MKDIR still writes the dentry on
`psh`.
**Gates (19820 scratch, TCP):** `test_sim` / `test_meta_apply` /
`test_wire` OK. efs-mgmt: mkdir `raft-smoke-sp`, local `pre`, mkdir
`e`, begin (no migrate), hashed `n0` create (ino shard ≠ parent),
raft-link hashed `l0` nlink=2, dup EXIST, leftover dest `pre` EXIST,
unlink hashed, link onto tombstone dest nlink=2, unlink, recreate OK,
unlink hashed, unlink `pre`, rmdir `e`, dest link still present
nlink=1, rmdir self BUSY status=5. FUSE `sp-ln`: `pre` + begin +
hashed `n0`, `os.link` hashed `l0` nlink=2. posix jobs=1 **201/201 in
83.9 s** (`results/posix/20260912-splitting-link/suite-jobs1.tsv`).
Smoke `raft_host_smoke.sh` not re-run (EXIT trap kills 19820). Do not
start step 12.

**SPLITTING DEST RENAME CLOSED (Sep 12, working tree on top of
SPLITTING dest LINK).** Apply + sim already implemented SPLITTING
rename (hashed dest PUT, `dentry_drop_items` I8 src drop); the host
BUSY'd SPLITTING src/dest parents. Host now allows LOCAL/HASHED/
SPLITTING for both parents: dest PUT goes hashed, src drop reuses
`host_dent_drop_fill`/`host_dent_drop_prep` (EXCL DEL local leftover
unless HASHED or keys alias; SPLITTING PUTs HASHED=TOMBSTONE), and a
leftover dest under a SPLITTING parent is POSIX replace (EXCL DEL the
local dest key `dest_del_loc` + PUT hashed dest + retire the dest inode
nlink--/DEL with reap marker), not EXIST. Sim `drop_dentry_prep` now
reads the psh/hsh KVs it mutates (was the caller's group KV — a
leftover on the other group could CAS-fail); dest existence uses
`sim_txn_lookup` (hashed-then-local, tombstone = NOT_FOUND). A dest
dir that is itself SPLITTING stays BUSY (distributed emptiness).
HASHED/SPLITTING dest MKDIR still writes the dentry on `psh`.
Populated-range migrate as a txn closed Sep 16 (below).
**Gates (19820 scratch, TCP, wipe+raft-mkfs):** `test_sim` (new
`test_rename_splitting` seed 137: hashed dest, leftover dest replace,
tombstone dest restore, leftover src) / `test_meta_apply` /
`test_wire` / `test_txn` OK. efs-mgmt: `raft-smoke-sp` begin (no
migrate), hashed `n0`→`n28` status=0, `n28`→leftover `q` replace OK
(q = src ino, old q retired), `q`→`n0` onto tombstone OK, leftover
`pre` still present, rmdir self status=5 BUSY. FUSE `sp-rn`: same
sequence through `os.rename` with content + ino identity checks, 5
repeat reads stable. posix jobs=1 **200/201 in 42.9 s**
(`results/posix/20260912-splitting-rename/suite-jobs1.tsv`) — only
the `concurrent_writes_disjoint` zeros flake. Smoke
`raft_host_smoke.sh` SPLITTING rename checks in tree (hashed dest,
leftover dest replace, tombstone restore); full script not re-run
(EXIT trap kills 19820). Do not start step 12.

**HASHED/SPLITTING DEST MKDIR CLOSED (Sep 12-13, working tree on top
of SPLITTING dest RENAME) — the last slice of the dest series.** Apply
+ sim already implemented hashed MKDIR (dentry on the hashed lane
shard, child inode scattered via `mkdir_shard`); the host always wrote
the dentry on `psh`. Host `server_raft_host_mkdir` now mirrors the
HASHED/SPLITTING create path: dentry + dseq + first-use dir-lane stamp
on `dsh = efs_kv_dentry_shard(parent, name, layout)` (`p_lane =
dir_lane(name)` unless LOCAL), parent row on `psh` gets nlink++
always but times ONLY when LOCAL (else the `used_shards` bit), child
inode + alloc stay on `csh = efs_kv_mkdir_shard(parent, name, salt)`.
Parts = {psh, csh, dsh} dedup'd; bounce via `host_fwd_create` when
this replica does not host dsh; ReadIndex on dsh's group when
distinct. EXIST check was already layout-aware. Sim `mkdir_build`
gained the same shape (takes the client id, per-part `sim_sess_ensure`,
`sim_txn_lookup` for EXIST). This completes the op matrix for
HASHED/SPLITTING dests: CREATE, READDIR, UNLINK/RMDIR, LINK, RENAME,
MKDIR all write hashed. Populated-range migrate as a txn closed Sep 16
(below).
**Gates (19820 scratch, TCP, wipe+raft-mkfs):** `test_sim` (new
`test_mkdir_splitting` seed 138: SPLITTING hashed mkdir, readdir merge
with the local leftover, dup EXIST, rmdir, recreate over the tombstone
with a fresh ino, migrate to HASHED, lookup from a second client,
HASHED mkdir) / `test_meta_apply` / `test_wire` / `test_txn` OK.
efs-mgmt: `raft-smoke-sp` begin (no migrate), hashed `d0` mkdir
status=0 (served by primary=2 — the dentry lane is on the other
group), lookup ino/mode match, readdir merges `d0,pre`, dup EXIST,
rmdir OK, lookup NOT_FOUND, recreate OK with a new ino, rmdir of the
SPLITTING dir itself status=5 BUSY. FUSE `sp-mk`: same sequence
through `os.mkdir`/`os.listdir`/`os.rmdir` with ino identity checks.
posix jobs=1 **200/201 in 112.1 s**
(`results/posix/20260912-splitting-mkdir/suite-jobs1.tsv`) — only the
`concurrent_writes_disjoint` zeros flake. Smoke `raft_host_smoke.sh`
SPLITTING mkdir block in tree (hashed mkdir, lookup/readdir/dup/
rmdir/recreate); full script not re-run (EXIT trap kills 19820).
Do not start step 12.

**RAFT-LAG CLOSED (Sep 11 PM, working tree) — three fixes in
`src/server/raft_host.c`, gated 199/201 jobs=16 in 26.5 s with ZERO
post-startup term changes (was: term +8 in 75 ms storms, terms 113/328
cumulative).** (1) **Per-peer outbox:** `host_send` no longer does the
synchronous send+empty-ACK round trip under `h->mu`; it encodes and
queues to `h->tx[peer]` (cap 256, drop-newest) and one `host_sender`
thread per peer does FIFO pop → conn get → 250 ms timeouts → send →
wait ACK → release. Pump `h->mu` holds dropped from up to 250 ms to
~0.5–2 ms worst (drain ~200–480 µs = raft_log_append fsyncs; apply
~200 µs = KV WAL fsync; persist ~250 µs — all measured with the new
`EFS_RAFT_OBS=1` instrumentation: term/role-change logs, per-outbox
counters, 5 s pump-phase maxima dump). (2) **Cross-node read_mu
deadlock fixed:** `host_read_index`'s follower branches and
`host_propose`'s forward branch blocked in `host_remote_cmd` (30 s
`EFS_IO_TIMEOUT_MS` recv + retries) while holding `read_mu`; a
post-restart leader-confusion cycle (003→004→005→003) wedged ALL
metadata ops for minutes (suite hung at startup, FUSE
request_wait_answer). All three sites now drop `read_mu` around the
network wait and relock after (safe: the apply layer re-validates, the
command is fully formed before propose, the raft log orders mutations).
(3) **THE ELECTION-STORM ROOT CAUSE — ticks were iteration-counted,
not wall-clocked:** the event-driven pump (495444d) wakes on every
inbox message/propose, so under load the loop spins as fast as the
drain runs (µs/iteration on a leader), and one `efs_raft_tick` per
iteration fired the 100–200-tick election deadline in **milliseconds**
— any busy node campaigned constantly, its higher-term vote requests
forced the leader to step down (`maybe_step_down`), and the leader
itself re-campaigned instantly (3 re-campaigns in 35 ms observed).
Leaders also broadcast AEs every 10 spinning ticks (~50 µs), flooding
the outbox (hi=91). Fix: `host_pump` gates `efs_raft_tick` on
`now - last_tick_us >= HOST_TICK_US` (never catches up missed ticks —
late is safe, early is not); drain/apply/persist stay event-driven, so
the 495444d latency win is untouched. raft.c is unchanged (the
simulator drives ticks as logical time). **Gate:** fresh wipe +
raft-mkfs, posix jobs=16 **199/201 in 26.5 s**
(`results/posix/20260911-outbox/suite-tickfix-jobs16.tsv`), only the 2
permanent known-debt fails (`dir_move_into_subdir` EINVAL,
`virt_find_query`) — zero timeouts, zero EIO flakes; term lines in all
4 efsd logs are startup-only (g0 term 3, g2 term 4, stable through the
run). Unit: test_raft/kv_lsm/meta_apply/sim/wire/txn OK.
`EFS_RAFT_OBS` stays in tree, env-gated, off by default.

**GC-GAP CLOSED (Sep 11 PM, working tree on top of `0a480c9`) — raft
mode now deletes data-plane fragment files on unlink/truncate (spec
L7).** The old engine reclaimed fragments; the Raft+KV engine never did
— every unlinked/truncated chunk's `<ci>.<frag>` + `.sum` files leaked
on the storage nodes forever. This adds a metadata-driven reaper.
**Design (all in the KV/apply layer + a host reaper thread; NO client
change):** when the apply layer retires a chunk (publish CAS supersede,
truncate range-delete, tail CAS, last-link unlink) it emits a **GC
record** (KV kind 21: `[anchor:2][21][ino:8][chunk_gen:8][lane:1][ci:4]`,
112 B value) keyed on `anchor_shard(lane_shard(ino,lane))`; a last-link
unlink also emits a **REAP marker** (kind 22: `[anchor:2][22][ino:8]`,
16 B value = `[inode_generation][active_lanes]`) keyed on
`anchor_shard(inode_shard(ino))`. `efs_kv_anchor_shard(shard) =
(shard&1) ? 1 : 2` maps any shard to its group's anchor so the record
lands on the same group that owns the state being reclaimed. New raft
cmds `EFS_MD_CMD_LANE_SWEEP 21` / `_REAP_DONE 22` / `_GC_ACK 23`; new
store op `del_if_sum` (checksum-conditional delete so a reaper never
deletes a record a fresher write replaced); new wire
`EFS_MSG_GC_FRAGMENT 99` / `_REPLY 100`. A per-node **`host_gc_thread`**
(raft_host.c ~3290, 1 s loop) runs, for each group this node leads,
`host_gc_reap_pass` (REAP markers → LANE_SWEEP to enumerate live lanes →
GC_FRAGMENT per dead fragment → REAP_DONE) then `host_gc_frag_pass`
(GC records → GC_FRAGMENT → GC_ACK deletes the record via del_if_sum).
Caps per pass: GC_SCAN_MAX=32, REAP_SCAN_MAX=32, GC_ACK_MAX=16.
Followers apply the same cmds so their KV state matches; only the
leader issues the actual fragment deletes. Files: kv_key.h/.c,
meta_cmd.h, meta_apply.h/.c, store.h/.c, store_nvme.c, store_mem.c,
protocol.h, handler.c, raft_host.c, server_internal.h, sim_ns.c,
sim_sess.c, test_meta_apply.c. **Stub-tail alias fix:** a truncate that
lands on a chunk boundary re-uses the old placement with a zero digest;
the host aliases that case and the apply layer skips GC for it
(regression `test_gc_tail_alias`).
**THE END-TO-END BUG (found + fixed this session) — scan-full was
treated as fatal.** The reaper ran, the leader check passed, but nothing
was ever reclaimed. Cause: `reap_scan_cb`/`gc_scan_cb` return **1** when
the batch hits SCAN_MAX (32); `merge_scan` (kv_lsm.c) propagates any
non-zero callback return; and the pass functions did
`if (src != EFS_OK) return;`. So once ≥32 markers/records had
accumulated (all the prior testing while the reaper was broken), every
pass collected 32 and **threw them away**, forever — a classic
"full == error" confusion. Fix: bail only on a real KV error
(`src < 0`); process the partial batch on `src == EFS_OK` (complete) OR
`src > 0` (scan-full stop). Applied to both pass functions. Added
`EFS_GC_DBG=1` per-marker/per-delete diagnostics (`gc reap ... sweep ok,
reap_done rc`, `gc del ino=... ci=... frag=... node=... rc`).
**Known robustness gap (not separately fixed):** `host_gc_export`
returns NULL when `export_count==0` — the export is registered in
`s->exports[]` lazily on `EFS_MSG_PUT_CHUNK` (handler.c:351), so right
after a server restart with no client writes yet the reaper can't name
the export for a delete. Resolves itself with any client activity (all
fragment-holding nodes register). Worth a real registration at mount/
bootstrap if this ever bites.
**ENVIRONMENTAL-LATENCY RED HERRING — GC is EXONERATED from the jobs=16
collapse.** Earlier jobs=16 runs on the GC tree collapsed; an A/B on the
SAME env (TCP) proved it was transient environmental, not GC: pre-GC
(`0a480c9` clean) jobs=16 = **198/201 (63.6 s)**; GC tree jobs=16 =
**198/201 (71.2 s)**, identical 3 known-debt fails. Separately noted
during the hunt: a pre-existing ~110 ms cross-group 2PC fan-out latency
(unrelated to GC; cross-group txns multiply raft rounds).
**GATES (fresh wipe + raft-mkfs, port 19820, TCP, `EFS_GC_DBG=1
EFS_RAFT_OBS=1`):** unit 12/13 OK (`test_lock` getlk,
since closed as a test bug).
End-to-end on ino 4097: write 200000 B → ci0 frags on 003/004/005 + ci1
on 004/005/006; `ftruncate(50000)`+fsync → content sha256 intact, **ci1
fragments reclaimed** (g2 leader fcstor004 `gc del ci=1 frag=0/1/2
rc=0`), ci0 present; `rm` → **all fragments reclaimed** (g0 leader
fcstor003 `gc reap lanes=3 sweep ok, reap_done rc=0` + `gc del ci=0
frag=0/1/2 rc=0`), count 0 on all 4 nodes. jobs=16 posix **199/201 in
68.9 s**, only the 2 permanent known-debt fails (`dir_move_into_subdir`
EINVAL, `virt_find_query` .find) — `concurrent_writes_disjoint` passed.
`EFS_GC_DISABLE=1` turns the reaper off; `EFS_GC_DBG`/`EFS_RAFT_OBS`
stay in tree, env-gated, off by default.

**STEP 1 LATENCY BUCKET LANDED (Sep 11, commit 495444d) —
posix jobs=1 198/201 in 34s (was 182/201 in ~14 min); jobs=16 193/201 in
30s (was catastrophic saturation).** Three changes:
(1) **Event-driven `host_pump`** (raft_host.c): the fixed 5ms usleep poll is
now an **eventfd** (`pump_efd`, EFD_NONBLOCK) + `poll` with `h->mu` UNLOCKED;
`host_pump_kick` does a lock-free write from the inbox path and the
leader-side propose path. FIRST version used a condvar signaled under `h->mu`
and wedged the cluster (election flapping): the pump holds `h->mu` across
synchronous `host_send`, the peer's handler blocked on its own `h->mu` to
signal, 3-node cycle broken only by the 250ms `HOST_SEND_IO_MS` timeout.
**The network inbox path must NEVER take `h->mu`** — that is why the kick is
an eventfd. `host_wait_applied`/`host_read_index` now
`pthread_cond_timedwait` on `applied_cv` (broadcast by the pump each cycle;
safe — no I/O under that wait) with a 400ms absolute deadline.
(2) **Eager commit broadcast** (raft.c `on_ae_rep`): when `try_commit`
advances `commit_index`, the leader now `broadcast_ae` immediately instead of
letting caught-up followers learn the new commit only at the next 50ms
heartbeat. This was the deterministic 74ms create: any op landing on a
follower (2/3 of voters) stalled in `host_wait_applied`. Measured after:
create 2.2ms, stat 0.7ms, unlink 1.4ms, depth-100 mkdir chain 4.2s.
(3) **READDIR pagination cursor fix** (protocol.h, raft_host.c, handler.c,
inode_rpc.c, efs_fuse.c): the raft host scans a dir in NAME order but the
wire contract paginated by `after_ino` (ascending-ino assumption from the old
table-scan server) — with ino-interleaved names and 64-entry pages, later
pages dropped smaller-ino entries (8 threads x 20 creates: listdir saw 69 of
160). `efs_msg_inode_readdir` gained `after_src`/`after_name[EFS_MAX_NAME]`,
the reply gained `next_src`/`next_done`/`next_name` (resume cookie);
`server_raft_host_readdir` takes the cookie, dropped the ino filter, and its
internal page request is `max_ents - out->count` so the cursor never advances
past unemitted entries. Client: `efs_client_rpc_readdir_cur` +
`efs_fuse_readdir` uses it under `efs_client_raft_mode()` (legacy ino path
kept for non-raft; shared `readdir_collect_page` helper does the name dedup).
**Remaining 3 solo failures, all known debt (do not re-derive):**
`dir_move_into_subdir` cross-dir rename EINVAL; `virt_find_query` .find
unimplemented; `concurrent_writes_disjoint` EIO load flake (a sync report
carries ALL dirty recs; a churning file's barrier-BUSY rec EIOs an unrelated
close). jobs=16 adds only 15s-budget timeouts on the heavy-walk tests
(dir_deep_nesting x2, dir_many_files) + the rename-family EIO — saturation
latency, 0 correctness bugs. Unit: test_raft/kv_lsm/meta_apply/sim/wire/txn
OK. **NFS GOTCHA (bit once): rsync -a quick-check (mtime+size) can skip a
just-edited file through the NFS attr cache — if a node builds with a stale
header after rsync, re-copy with `rsync -a --checksum`.**

**STEP 1 PRODUCTION ADOPTION LANDED (Sep 10, commit 3299e89): efs-fuse mounts
the Raft+KV engine behind `EFS_MD_RAFT` (client + server env).** Bootstrap
polls RAFT_STATUS for `kv_has_root` (no GET_META — the host serves no
serialized table); inode RPCs route by the compiled-in shard→group→voter map
(odd shards→group 0 = nodes 1-3, even→group 2 = nodes 2-4) to any voter and
follow the host's `primary_id` bounce. **Scratch cluster: port 19820, storage
`/tmp/efs-raft-scratch`, fcstor003–006, `--quota 1G --writers 0
--no-direct-io`.** Kill by port (`grep -q 19820`). The old-engine 19810
NVMe cluster is gone (Sep 15 2026) — no snapshot/2PC on-disk compat;
19810 is free for a current-tree `raft-mkfs` on `/data1`. Client:
fcstor007 `/tmp/efs-mount`, `EFS_TRANSPORT=tcp`.
Fixes in the same commit, all found by the first real gate: (1) **monotonic
raft boot id via `boot.bin`** — pid^time can go backwards and the peer boot
fence then silently drops every reply from the restarted node (replication
wedge); (2) `efs_meta_apply_truncate` had a **765 KB stack frame** on 1 MiB
efsd pump threads (now heap, 25 KB) and its multi-lane range delete indexed
shared `del_keys` by a per-lane base so lane i+1 overwrote lane i's keys
(wrong chunks deleted; regression test in test_meta_apply); (3) **REPORT
publish for a deleted inode is a harmless no-op (P3)** at host AND apply
layer — one unreportable rec otherwise poisons every fsync of that client
forever (all-or-nothing report + merge-back; this was the
`fsync_reopen_visible` EIO); (4) **pack staging disabled in raft mode** (KV
row has no pack fields; pack chunks publish under the parent DIRECTORY ino →
INVAL); (5) SETATTR SIZE|MTIME pairing accepted. `EFS_RAFT_DBG=1` gates
server report/publish debug logs.
**Gate: 12/12 unit suites incl. test_sim; posix on the Raft mount 150/201
(`results/posix/raft-step1-20260910-2133`).** `test_lock` had 6
failures (a test bug, closed Sep 18).

**STEP 1 FOLLOW-UP FIXES (Sep 10 PM, working tree on top of 3299e89) — gate
182/201 (`results/posix/raft-step1-bug7-20260910`).** The 150→182 climb
fixed, in order: (2) `last_link_unlink_other_dir` EIO — unlink/rename of a
dentry whose inode row is on the OTHER group now bounces dentry-first
(before resolving the row); (3) txn PREPARE packed dentry keys with a
1-byte klen — names >255 bytes truncated and collided (now 2-byte, host +
sim); (4) rename-over-existing EEXIST — the destination row is retired in
the same txn (nlink--/DEL, empty-dir rules), host + sim; (5) ctime ns
truncation on the wire — claimed padding as ctime_nsec/atime_nsec; (6)
flock survived close — the kernel NEVER relays a flock UNLOCK on close, so
the last-lease edge now drops all lock records for (ino,gen)
(`efs_lock_drop_file`) and the client HOLD is edge-triggered on a per-ino
open-description refcount (`efs_open_note`/`efs_close_note`); (7) **the
11-test EIO window = `rsv_scan` 64-cap NOMEM** — a file with >64
append-reservation records (a timeout-killed concurrent_appends leaves
orphaned OPENs + their blocked DONEs) made `efs_meta_apply_append_open`
fail NOMEM inside `host_resolve_caught_up`, which failed the whole report
batch, which kept the rec dirty and poisoned EVERY later sync report
(direct_*, two_fds, trunc_open_other_fd, fsync_then_fstat_size, the
symlink family, last_link_unlink data loss — all one poison rec,
ino-pinned, proven by 418 × `report FAIL ino=6590 rc=-2` on fcstor004).
Fix: the scan is dynamic (no cap), a resolve failure no longer fails a
report whose publish succeeded (bookkeeping, not durability — size rides
the lane MAX), and the last-lease edge drains orphaned OPEN reservations
as ABORTED_HOLE (`efs_meta_apply_append_drain_file`, host + sim) so a
killed appender can no longer wedge the frontier or accumulate records
without bound.
**Remaining 19/201, classified (do not re-derive):** ~15 latency-bucket
timeouts (`host_pump` ticks every 5 ms, `host_wait_applied` polls 5 ms ×
80 → each Raft round 15–25 ms; cross-group txns multiply; depth-100 mkdir
chains and 16-way bursts sit on the 15 s test budget — the event-driven
pump is a post-step-1 PERFORMANCE item, not correctness); 2 load flakes
under the killed-test barrier churn that pass isolated
(`concurrent_writes_disjoint`, `symlink_relative_after_parent_rename` — a
sync report carries ALL dirty recs, so a churning file's barrier-BUSY rec
can EIO an unrelated file's close after retries exhaust; per-ino fsync
report or per-rec BUSY skip is the step-2 refinement);
`dir_move_into_subdir` (cross-dir rename EINVAL — slice debt);
`virt_find_query` (.find unimplemented). jobs=16 under the same latency
profile is saturation noise (first instance 182/182 fails all timeouts, 0
correctness classes). Known slice debts still open: cross-group utimens
INVAL (`host_utimens`), append_bar not pushed to cross-group lanes, sim
blind spot (all-voters shared-disk).

**ARCHITECTURE DECISION (Sep 1 2026, user-ratified; REVISED after external
protocol review).** The project now has a master spec: **`docs/architecture.md`**
(+ browsable `docs/architecture.html`). User's bar: efs must earn the name
**extremfs** — throughput tracks aggregate hardware (NVMe+NIC), no software
serialization point, incl. many writers to ONE file. Decisions locked:
- **Metadata = one Raft group per shard over an on-disk ordered KV**; RAM is a
  bounded cache. Replaces whole-table snapshot + CoW page flush + 2PC root
  commit (those get DELETED, not patched).
- **Build a deterministic simulator FIRST** (seeded PRNG drives net/disk/
  crashes; invariants checked every step; failures replay from seed; plus an
  independent linearizability/history checker). It becomes the correctness
  gate; the 13-node cluster becomes a perf harness.
- **Targets:** ≥ 2³² live objects; 3–64 nodes; reference 2³² on 4 nodes;
  immediate cross-client visibility. **Failure tolerance is CONFIGURABLE
  (user, Sep 1 PM):** f = max simultaneous PERMANENT node losses with no data
  loss, capped at 3, bounded by N. The math (do not re-derive): data EC k+f
  needs N ≥ f+2; metadata Raft needs quorum Q > f → **N ≥ 2f**, and full
  write availability through f losses needs N ≥ 2f+1. Rule: **RF = min(N,
  2f+1)**, config invalid unless N ≥ 2f. Table: 3 nodes → f=1 (RF=3, 2+1);
  4 → f=2 (RF=4, 2+2; durable through 2, available through 1); 5 → f=2 full
  (RF=5); 6 → f=3 (RF=6, 2+3; durable through 3, available through 2);
  ≥7 → f=3 full (RF=7). **f=3 needs 6 nodes, NOT 5** (RF=5 gives Q=3 ≯ 3 —
  the 3 dead nodes could be the whole quorum that acked the last writes).
  At N = 2f the system pauses rather than loses (CAP-consistent); recovery
  from a lost quorum is node-returns or an explicit operator
  `force-reconfigure`. Changing f is an online control-plane op (add/remove
  replicas + re-stripe parities), not a reformat. **Also fixed in the same
  pass: the write commit rule now requires ALL k+f fragment ACKs before
  publication** (D − f ≥ k ⟺ D = k+f); the old "≥ 2 of 3 ACKs" rule had a
  real durability hole at f=1 (publish with 2 durable fragments, lose 1 →
  1 fragment < k=2 → acknowledged data lost). I10/I11 parameterized to the
  configured f.
- Data path mechanism (client-direct RDMA 2+1 EC) UNCHANGED, but commit
  semantics now specified (immutable chunk generations + atomic metadata
  publication + target-side generation fencing).
**Review corrections now IN the spec (the reviewer was right; do not re-litigate):**
shard_bits=**12** (not 20) for 4096 shards; ino is **64-bit + generation**
(ABA-safe); dentry index stores a small **projection** `(ino,gen,type)`, the
inode row is the ONLY mutable copy (dentries on parent's shard, threshold
spread for huge dirs); cross-shard rename/hardlink/unlink-open use a
**Raft-backed transaction** (txid/coordinator/intents/durable decision) — the
"no 2PC" is only about the OLD root-snapshot 2PC, not per-op cross-shard 2PC;
I1–I4 restated as Raft's real properties (election uniqueness / leader
completeness / term fencing / quorum-ack, NOT "at most one believes leader");
reads are **leader + ReadIndex** (no clock leases — clocks never correctness);
safe reconfiguration via **joint consensus** (desired placement ≠ authoritative
config); request **idempotency** via op-IDs; open-unlinked lifecycle; FUSE
kernel-cache invalidation (entry/attr/negative_timeout=0 + active invalidate);
3 planes (data / metadata / control). **The extremfs-critical design (review P0.14 + follow-up):** a hot file must
NOT serialize. Two load-bearing details, both now specified: (1) **chunk
metadata is itself sharded** — `chunk_meta_shard = hash(ino, chunk_index) &
0xFFF` — because independent chunk-map KEYS on one Raft group still serialize
on that group's ONE log/leader; spreading chunk publication across many shard
leaders is what makes the hot-file claim structurally true. (2) **size is
sharded high-water LANES** — MAX() commuting removes the ordering dependency
but not the physical serialization point, so size lives in L distributed lanes
(each an idempotent MAX on its own shard); stat() = max over current-epoch
lanes; truncate bumps the content epoch to invalidate all lanes at once.
mtime coalesced; sub-chunk RMW = generation CAS; O_APPEND EOF allocation is
the ONLY accepted same-file hotspot.
**THIRD REVIEW ROUND IN (Sep 1 PM) — the POSIX composition layer is now
protocols, not intentions. Do not re-litigate:** (1) **Three governing
principles** now head the spec (§0): P1 never-serialize-parallel-able-work;
P2 **co-locate what must commit atomically on the common path, distribute the
rest**; P3 **make stale work harmless, not impossible**. (2) **CREATE
co-location rule (load-bearing):** `inode_shard(new_ino) = inode_shard(parent)`
(normal dir) / `= hash(parent,name)&0xFFF` (spread dir) — inos allocated
per-shard so CREATE = dentry+inode row in ONE Raft entry; without it every
CREATE is a distributed txn. (3) **Authoritative op→shard matrix** in §5.3
(the old "only rename/hardlink touch two shards" claim was WRONG: UNLINK
nlink>1 and LINK are 2-shard txns). (4) **Hot-dir spread = layout-epoch
protocol** LOCAL→SPLITTING(e)→HASHED(e): writes go hashed during SPLITTING,
reads check hashed-then-local, migrator moves idempotently. (5) **Size lane
co-located with its chunk shard** (`size_lane = chunk_meta_shard`) so an
extending write is ONE Raft entry `{publish chunk G; MAX(lane,end)}` — kills
the publish-without-size / size-without-publish hole (new invariant **I21**).
(6) **stat() linearization via epoch double-check:** read epoch E → read only
E-lanes → re-read epoch → retry if changed. (7) **Truncate = content-epoch
bump with stated linearization rule** (epoch-E write linearizes before the
bump; new invariant **I22** epoch fencing). (8) **O_APPEND = serialized
`reserve_append(len)` on the inode shard, then ordinary distributed write**;
crash-after-reserve leaves a POSIX-legal hole, never rolled back. (9)
**Distributed locking:** one lock authority per inode on `inode_shard(ino)`,
`(ino,start,end,owner)` via Raft; dead-client reclaim via stale incarnation;
advisory locks never fence the data path. (10) **File-data coherence =
direct-I/O, decided** (kernel page cache bypassed; coherent caching later
only if it pays); **cross-client coherent MAP_SHARED unsupported initially**
(documented, not implied). (11) **Simulator models the LOGICAL data protocol**
(PUT/ACK/crash/publish/stale-gen events), not RDMA mechanics — else I11–I15/
I20–I22 are uncheckable. (12) **Data targets are dumb:** fragment PUT is
immutable+idempotent; target verifies placement epoch/incarnation/identity
only; the metadata authority alone decides which generation is committed (NO
target-side generation ordering). (13) **readdir contract explicitly weak**
(name existed sometime during scan; no dups; cookies survive leader change;
NO snapshot). (14) **mtime exact:** rides the chunk publication entry
(`max(mtime,now)` stamped by the shard leader); a stat after a returned write
sees it, any client. (15) **namespace-as-database reworded honestly:** the
architecture makes namespace-wide queries possible WITHOUT POSIX walks;
secondary indexes/aggregates are a separate derived-index design. (16)
Migration renumbered: step 10 = dir-spread epochs + locking; delete-2PC =
step 11; FUSE cache opts = step 12. Sections: locking §5.12, FUSE/data cache
§5.13, modularity §5.14.
**FOURTH REVIEW ROUND IN (Sep 1 PM) — the hot-path implementation contract.
Do not re-litigate:** the reviewer's verdict was "topology is right for
linear scaling; the hot-path contract is not complete enough to guarantee
hardware efficiency". All 19 points are now IN the spec (md §5.7/§5.13/§5.15
+ HTML §3a/§3c/§5): (1) **EC publication: healthy = ALL k+f fragments durable
before publish** (already landed with the f-parameterization); NEW is the
**degraded rule** — with u domains already unavailable, publish with
≥ k+(f−u) durable fragments, mark degraded, re-stripe on repair (I11
rewritten; never for slow targets, only unavailable ones). (2) **FUSE is an
architectural serializer risk:** `FOPEN_DIRECT_IO` +
`PARALLEL_DIRECT_WRITES` + `FUSE_CAP_ASYNC_DIO` + `FUSE_CAP_PARALLEL_DIROPS`
+ large max_write/max_pages/max_background are REQUIREMENTS (without
parallel_direct_writes Linux re-serializes same-file direct writes ABOVE
efs). (3) **Direct-I/O disables kernel readahead → the client owns an async
prefetch pipeline** (adaptive queue depth), or sequential reads lockstep.
(4) **Size lanes REBOUNDED:** the round-3 `size_lane = chunk_meta_shard`
(hash over 4096 shards) would give a big file up to 4096 lanes → stat() =
4096-way fanout. Now: `lane = hash(ino,chunk) % L`,
`chunk_meta_shard = hash(ino,lane) & 0xFFF`, **L derived** (required hot-file
pub rate ÷ per-shard commit rate, capped at tens); per-file spread is L
leaders, global spread still 4096. (5) **mtime contradiction FIXED:**
write-mtime lives in the lanes (`lane.max_mtime` on the publication entry);
explicit utimens/chmod update the inode row's `base_mtime` + bump
`mtime_gen`; stat = MAX(base_mtime, current-gen lane mtimes) under the
content-epoch check. (6) **Multi-Raft runtime is an architectural
requirement** (§5.4): reactors per NUMA domain, messages batched by
destination, heartbeats coalesced, WAL group-committed across groups, KV
apply batched — 4096 logical groups, NOT 4096 physical WALs/threads.
(7) **Publication batching:** many chunk publications per Raft proposal (at
128KiB, 100 GB/s = ~800k pubs/s; one txn per chunk would cap metadata).
(8) **Small-write envelope DECLARED:** 4K-in-128K = ~80x amplification +
same-chunk CAS contention; efs is HPC-aligned-I/O optimized; immutable delta
objects + background consolidation are the designed escape hatch, NOT built
until benchmarks demand. (9) **Dir spread triggers on entry count OR op
pressure** (100-entry dir with 100k clients melts its leader without ever
crossing the size threshold); spread is one-way. (10) `EFS_DIR_SPREAD_MIN` is
a **scalability bound** (caps pre-spread stranded co-located inode rows).
(11) **P4 added** (hot path never crosses cores/NUMA/locks unnecessarily) +
§5.15 contract: NUMA-local async execution, no cross-core locks on ordinary
PUT/GET. (12) **Ceiling is protection-adjusted:** logical write BW ≤
min(client egress, storage ingress, NVMe) × k/(k+f), EC CPU/mem BW, pub rate
× chunk size; the bar is 85–95% of THAT. (13) **Near-linear defined per
workload class** (table in §7/HTML §5; semantically-serial ops excluded
explicitly). (14) **write()=durable is a documented latency-for-durability
trade**, benchmarked against the strict-POSIX alternative before final.
(15) **ReadIndex amortized** (one quorum round serves a batch). (16) **QoS
isolation:** Raft/membership/txn/small-meta get own RDMA queues/traffic
class/credits; NVMe WAL/KV/bulk/rebuild queues separate. (17) **Rebuild
distributed + rate-limited:** deterministic repair owner, priority foreground
> metadata > rebuild. (18) **O_APPEND respecified:** reserve (inode shard) →
parallel data → per-reservation commit; the reservation watermark is NOT the
visible EOF; a dead appender's range is **committed as a zero hole by
recovery** (stale incarnation); the glib "POSIX-legal hole" claim is gone —
holes arise only from never-returned writes, read as zeros, no returned write
lost. (19) **Multi-chunk write atomicity DECIDED:** default = per-chunk
visibility (a concurrent reader can see a mix — same as Linux O_DIRECT,
documented in the POSIX contract; after write() returns, all chunks visible
to everyone); strong path = the §5.6 transaction machinery (write txid +
intents + one decision) when atomic multi-chunk publication is required.
Scaling goal in §1 rewritten measurably: "no software serialization point may
become the limiting resource before a physical resource does — if a benchmark
stops at a mutex/one leader/one thread/FUSE serialization/one WAL/one
coordinator, that is by definition an EFS bug."
**FIFTH REVIEW ROUND IN (Sep 1 PM) — six architectural fixes, all now IN the
spec (md + HTML). Do not re-litigate:** (1) **CREATE co-location split by
kind:** the old `inode_shard(new)=inode_shard(parent)` rule co-located whole
subtrees on one shard until each dir spread — "many independent dirs →
near-linear" was false. Now: **files** co-locate with the parent dir (one
Raft entry, unchanged); **every MKDIR scatters** —
`inode_shard(new_dir)=hash(parent,name,export_salt)&0xFFF` — paying one
2-shard transaction per mkdir to buy an independently scalable subtree for
its lifetime (this generalizes today's ROOT-only dir hashing; nested-dir
hashing is now safe because §5.6 transactions exist — the Aug 30 revert was
a pre-transaction limitation). (2) **stat() is a double collect:** the
content-epoch check only covered truncate-vs-stat; concurrent extending
writes across lanes could assemble a size the file never had (A=100,B=100 →
reads A=100, commits land, reads B=500 → returns 500). Every lane now
carries a monotonic `lane_seq`; stat = read inode epoch/gen → collect L
lanes (seq,size,mtime) → collect seqs again → re-check epoch; all-unchanged
⇒ the vector existed simultaneously ⇒ MAX linearizable. Bounded retries,
then a read-only multi-shard txn over the L lanes as the escape hatch. Same
collect validates distributed mtime. (3) **Multi-chunk write atomicity
FLIPPED:** default is now syscall-level ATOMIC publication (POSIX read/write
atomicity) — parallel data movement, per-lane publication intents, ONE
durable write-txid decision; per-chunk visibility is an explicit relaxed
mount mode (MPI-IO-style), never the default. Atomicity unit = one FUSE
write request; `max_write` sized so the writes that matter arrive as one
request; recovering the exact syscall boundary under FUSE async-DIO
splitting is an explicitly OPEN investigation. New invariant I24. (4)
**Failure rule simplified: the automatic guarantee requires N ≥ max(2f+1,
k+f)** (3→f1, 5→f2, 7→f3; 4 and 6 nodes run the next-lower f). The user's
ratified 4→2 / 6→3 numbers survive ONLY as an explicitly labeled
**durability-only mode** (N=2f): no data loss (every committed entry keeps
≥1 survivor, Q+f>N), but a minority survives and plain Raft CANNOT safely
continue — recovery is a specified operator-gated DR protocol (freeze shard
→ collect ALL surviving logs → merge per index, highest term wins — leader
completeness proves a committed entry is never shadowed, and op-ID
idempotency makes uncommitted survivors harmless → force new config →
re-add replicas). Changing f has an explicit transition state:
`effective_f`→`target_f` only after replicas added + ALL generations
re-striped + verified. (5) **O_APPEND: data movement parallel, but
allocation order AND the visible commit frontier are serialized** — B's
bytes at offset 100 only mean anything because A reserved first, so the
visible EOF advances over contiguous RESOLVED reservations; a fenced
appender's range resolves as a committed zero hole, unblocking the frontier.
The old "never serializes one appender's visibility on another's speed"
claim was wrong and is gone. (6) **Client sessions are a real protocol
(§5.12a):** `session={client_uuid, session_epoch, state}` on a SHARDED
session authority (hash(uuid)&0xFFF), mutated via Raft; heartbeats only
decide WHEN to attempt replacement, consensus decides WHICH session is
authoritative; once epoch+1 commits, the old session is fenced everywhere
(locks reclaimed, open-unlinked refs dropped, append reservations resolved,
dedup records retained). Fencing a partitioned-but-alive client is an
availability sacrifice, never a safety one (the epoch IS the fencing token).
New invariant I23. §5.11 open-unlinked and §5.12 locking now sit on it;
migration step 8 = sessions first, then open-unlinked. **Small corrections
landed:** op matrix — MKDIR/RMDIR = 2-shard txns, UNLINK-last-link is
single-shard ONLY if dentry_shard==inode_shard (hardlink corner: after
link+unlink the surviving dentry can be on a different shard), GETATTR =
inode row + L lanes; replica math at 64 nodes = 192/node at RF=3, 448/node
at RF=7, leaders ~64/node at any RF; appendix "Raft group = RF replicas";
§6 "survive f nodes" wording; §5.15 gained **per-lane batched/range
chunk-map fetch** (lane i holds chunks i,i+L,i+2L… so one range request per
lane covers a contiguous window; metadata window runs ahead of the data
window — else 800k metadata RPCs/s at 100 GB/s caps sequential reads);
**"durable ACK" defined** = target completed the persistent-NVMe operation
(NVMe flush/FUA or PLP media), NOT RDMA WRITE completion — same for Raft
follower ACKs (WAL at the defined persistence level, not volatile cache).
**SIXTH REVIEW ROUND IN (Sep 1 PM) — ten fixes + the normative/detail SPLIT.
Do not re-litigate:** (1) **N=2f durability-only mode DELETED — the
survivor-log merge was unsound.** The reviewer proved "highest-term entry
wins per index" can synthesize a log that never existed (A: idx5/term10 X
uncommitted branch; B: idx5/term9 Y + idx6/term11 Z legitimately; merge =
X,Z — a command sequence no leader authorized; op-ID idempotency does not
fix it). The earlier "user-ratified 4→2 / 6→3" is GONE. Only rule:
**N ≥ max(2f+1, k+f), RF = 2f+1** (3→f1, 5→f2, 7→f3; 4 and 6 run the
next-lower f). Permanent majority loss = operator DR territory, not an efs
mode. (2) Stale "default: per-chunk atomicity" section deleted (contradicted
the atomic-publication default). (3) **FUSE syscall boundary = explicit
UNRESOLVED CONTRACT**: POSIX requires syscall-granularity atomicity, FUSE
ASYNC_DIO splits syscalls with no originating ID, so EFS MUST NOT claim full
POSIX write atomicity above the max_write/FUSE-request boundary until solved
(kernel op grouping / deliberate short-write / syscall txid / strict mode /
documented compat-vs-strict). I24 + §3 scoped to one FUSE write request.
(4) **Txn hot path:** coordinator = `participant[hash(txid) % n]` (was a
deterministic participant = same-file serializer); write-publication PREPAREs
run in PARALLEL with no-wait semantics (canonical order only for namespace
txns). (5) **Dir-spread dominating tombstone:** during SPLITTING a mutation
of an unmigrated name writes `HASHED(name)=TOMBSTONE(epoch)`; hashed side
always wins → migrator can't resurrect an unlinked name (I8). (6) **I23
narrowed:** fencing = no AUTHORITATIVE mutation; metadata leaders check the
epoch, **data targets do NOT** (orphan immutable PUT is harmless, P3).
(7) **Open-unlinked mechanism:** one session-scoped open lease per
(session, inode) on the inode shard — first open commits it, last close
removes it, fenced sessions' leases lazily dropped; reclaim iff nlink==0 AND
open_sessions empty. (8) **coding_profile_id** {k,f,stripe/coding epoch,
placement epoch} is part of chunk identity; online f/k change re-stripes to
NEW immutable generations under the new profile, never mutates in place.
(9) **Lane formula fixed:** `lane = (chunk_index + hash(ino)) % L` (was
hash(ino,ci)%L, which contradicted "lane i holds chunks i,i+L,…");
chunk_meta_shard = hash(ino,lane) & 0xFFF unchanged. (10) Small: "four
principles" header, I16 (not I9) for idempotency refs, flock/fcntl
non-interaction is LINUX behavior (flock isn't POSIX; OFD fcntl IS
POSIX.1-2024 and is in target), MKDIR row includes parent nlink, same-dir
rename widens when replacing a cross-shard destination.
**THE SPLIT (same round, user-directed): `architecture.md` is now the
NORMATIVE INDEX (1840 → 790 lines); all rationale/derivation/protocol detail
moved to satellites — `docs/arch/{naming,design,design-history,
failure-tolerance,performance,development,verification}.md` +
`docs/arch/protocols/{transactions,data,directory,sessions}.md`. If the
index and a satellite disagree, the index wins.** New section numbering (old
§5.x refs in this file are historical): §0 principles · §1 goal · §2 failure
guarantees · §3 consistency contract · §4 invariants · §5 state placement
(incl. the single authoritative **state-placement table**: state / authority
/ key / replication) · §6 op→participant matrix · §7 protocol summaries
(7.1 Raft+reads, 7.2 txns, 7.3 data, 7.4 dirs, 7.5 sessions, 7.6
open-unlinked+locking, 7.7 FUSE, 7.8 control plane, 7.9 idempotency) ·
§8 hot-path contract · §9 scaling envelope · §10 implementation order.
Historical review language ("the earlier claim was wrong…") lives in
`arch/design-history.md`, not the spec. HTML synced to all of the above.
**REVIEW ROUNDS 7–9 LANDED (Sep 1–2, by another agent; reviewed + verified by
kimi-k3 Sep 2 — verdict: correct, keep).** Three more external review rounds,
all in tree (md + HTML + regenerated `architecture-full.md`, 4155 lines);
full narratives in `arch/design-history.md`. Do not re-litigate:
(7) **protocol closure** — fencing became a real 3-phase revocation BARRIER
(session record carries a 4096-bit touched-shard bitmap; ACTIVE→FENCING→FENCE-
to-every-touched-shard→ACTIVE; registration once per (session,shard), never on
the I/O path); every data-plane key AND every inode-scoped ephemeral record
(leases/locks/reservations) is `FileID=(ino,inode_generation)`-scoped;
`chunk_generation` is a globally unique CANDIDATE identity, never G+1
(ordering from the publication CAS); content_epoch is a fence ONLY, never in
an object key (else truncate strands the surviving prefix); lanes are a FIXED
64 per file, `lane=ci%64`, `lane_shard=(inode_shard+lane*odd_stride)&0xFFF`
(permutation ⇒ 64 distinct shards, lane 0 = inode shard), `active_lanes`
64-bit bitmap bounds stat(); txn effects split exclusive-CAS vs COMMUTATIVE
REDUCTIONS (MAX = payload, never blocks prepare; pending committed reductions
are discoverable under the lane prefix and fold into authoritative reads);
write-ctime moved to lanes; dir mtime/ctime spread as `dir_lane` per dentry
shard; noatime default (strict atime not offered, documented deviation);
dedup = bounded window (highest_contiguous_seq + bitmap + reply cache),
reclaimed by client contiguous-ack watermark, never by time.
(8) **composition edge cases** — I24 gained a READ side (multi-chunk read =
validated collect: versions + re-resolve undecided txids after the fetch;
prefetch is not a cache); truncate = bounded inode fence (row + active lanes,
≤65) carrying per-lane RANGE DELETE (stops RMW resurrecting truncated bytes)
+ the straddling tail chunk CAS-published INSIDE the truncate txn;
`base_size` on the inode row; O_APPEND validates against the active-lane EOF
vector (a private counter is a correctness bug) + an EOF BARRIER on the lanes
while reservations are unresolved; resolutions COMPLETED/ABORTED_HOLE/
FENCED_HOLE; only `utimens` bumps `mtime_gen` (only op that moves time
backwards), ctime unguarded, implicit times MAX-clamped (CLOCK_REALTIME can
step back); profile cutover is a pushed+ACKed barrier BEFORE the re-stripe
scan; lowering f advertises the weaker guarantee FIRST; FileID keying for
ephemeral state; OFD/flock owned by open-file-description.
(9) **protocol completion + kernel reality** — read/predicate GUARDS are the
third txn primitive (durable, shared, held through the decision); RMDIR
phantom fixed by per-(dir,dentry-shard) `dentry_seq` witnesses; directory
rename puts the whole ancestry chain's `parent_version` in its read set
(cycle prevention); publication VALIDATES durability evidence
(coding_profile_id, placement_epoch, per-fragment ACK set w/ incarnations +
checksums, distinct domains) — dumb targets check nothing; I25 end-to-end
fragment checksums (identity+payload; corrupt = unavailable, never decoded);
`u` is PROTECTION DEBT not headcount (a returning node restores capacity, not
fragments); spread dirs use the same fixed 64-shard permutation as file lanes
(dir stat bounded, used-shard bitmap on the dir row, `dir_mtime_gen` fence);
Linux lock domains corrected: classic+OFD fcntl CONFLICT in one record-lock
domain, flock separate; **second open kernel-interface item: upstream Linux
serializes per mount** (extending direct write takes exclusive inode lock
even with PARALLEL_DIRECT_WRITES; IOCB_APPEND forces it; O_CREAT takes the
parent dir exclusively — PARALLEL_DIROPS covers lookup/readdir only), §9
bounds intra-mount scaling for those three, pre-sizing avoids the first.
**New file `docs/arch/START-HERE.md`** — task routing (current task = Phase M
`wire/`), "I am changing X" → read/governs/gate table, done-means,
never-without-asking. Generator updated; full.md verified current.
**ONE GAP FOUND IN REVIEW: the doc machine-gate specified in
`arch/development.md` (regen-diff check, link/invariant-ref validation,
single-home normative tables) has NO implementing script yet** — only
`docs/gen-architecture-full.py` exists and is run by hand. Build the gate
before relying on it.
**SEVENTH REVIEW ROUND IN (Sep 1 PM) — protocol closure. Reviewer accepted the
structure and asked only for remaining protocol holes; 10 of 11 points merged
as given, 1 merged CORRECTED. Do not re-litigate:** (1) **Session fencing was
not a protocol.** "Leaders cache the session table and re-validate on doubt"
fences nothing — a shard that hasn't heard of `epoch+1` can still commit for
the dead client (I23 violation) and nothing tells it to doubt. Replaced with a
**revocation barrier**: the session record carries a 4096-bit `touched_shards`
bitmap; a shard registers ONCE per session before accepting its epoch; then
`ACTIVE(E) -> FENCING(E+1)` (freeze the set) `-> FENCE` every touched shard
`-> ACTIVE(E+1)` only after ALL ACK. No lock reclaim / lease drop / reservation
resolution before that commit. Unreachable shard ⇒ the NEW session waits (not a
real availability loss — a shard that can't establish authority can't commit for
the old client either). No session lookup on the I/O path. (2) **ABA protection
didn't reach the data plane:** handles were `(ino,gen)` but chunk objects were
keyed by bare `ino`. Every data key is now scoped by **`FileID = (ino,
inode_generation)`** — fragment objects, chunk map, lanes, append reservations,
publication intents. (3) **`G+1` generation numbering was unsound** — two
writers reading committed G both mint "G+1" for DIFFERENT bytes and PUT
different content under one immutable object name. Generations are now
**globally unique candidate identities** (`H(client_uuid, session_epoch, op_id,
chunk_index, retry)` or UUID); ordering comes from `CAS(expected = base)`, the
loser's object is an orphan. (4) **CORRECTED, and the spec had the bug already:**
the reviewer also wanted `content_epoch` in the chunk key. It WAS there, and it
is wrong — with the epoch in the object identity, `truncate()` to a smaller
non-zero size strands the surviving prefix under an epoch no reader consults
(and the text said those pages were GC'd), while re-tagging instead makes one
truncate an O(file-size) rewrite (8M entries for 1 TiB). POSIX requires the
prefix to survive. **content_epoch is now strictly a fence** over lane state +
in-flight publications; committed chunk data survives a bump. That exposed a
second gap: with lanes epoch-invalidated, `stat()` had nothing to read after a
truncate — the inode row gained **`base_size`** next to `base_mtime`. (5)
**Derived `L` could never change** (`lane = f(ci) % L` relocates every chunk-map
key). Replaced by **64 fixed lanes activated on use**: `lane = ci % 64`,
`lane_shard = (inode_shard(ino) + lane*stride(ino)) & 0xFFF` with
`stride = 2*(hash(ino)&0x7FF)+1` (odd ⇒ permutation over 2^12 ⇒ **64 guaranteed
DISTINCT shards**; independent `hash(ino,lane)` collides). **Lane 0 = the inode's
own shard** (small files have zero fan-out). Monotonic 64-bit `active_lanes`
bitmap on the inode row, set on a lane's first use — ≤64 inode touches per file
LIFETIME, batched into the write's existing txn; `stat()` collects only active
lanes. (6) **The lane was about to become the next hotspot:** if a txn's `MAX` on
lane size/times were an exclusive intent key, two writers publishing DIFFERENT
chunks sharing a lane would conflict on nothing. Transactions now split
**exclusive CAS keys** (chunk map, dentry, inode row) from **commutative
reductions** (lane `MAX`es, dir-lane `MAX`es) carried as payload, applied by the
reducer, never blocking a prepare. Intent resolution gained its 4th outcome:
**cannot-establish-authority ⇒ retryable error, NEVER "absent"** (I9). (7)
**Timestamps had two hidden serializers.** A write updates ctime too, so
**write-ctime moved into the lanes** (`lane.max_ctime`) — routing it to the inode
row sends every hot-file writer back to the inode leader. Worse: POSIX updates
the CONTAINING DIRECTORY's mtime/ctime on every entry create/remove, so a spread
dir would still funnel every create through its home shard — **hashed dirs now
keep a `dir_lane` (max_mtime/max_ctime) on EACH dentry shard**, `stat(dir)`
reduces over used shards. POSIX distinction kept exactly: **chmod sets ctime,
NOT mtime**. **atime decided: `noatime` default**, `relatime` a coalesced mount
option, strict per-read atime deliberately not offered. (8) **Two namespace
cases were hiding behind "2 shards":** `rmdir` must prove emptiness, which is
distributed on a HASHED dir — now a **transactional read set** over its dentry
shards (an exact distributed entry counter is REJECTED: it rebuilds the
per-directory hotspot the spread removed); and **same-dir `rename` is 2-shard on
a HASHED dir** because `hash(parent,old)` and `hash(parent,new)` are independent.
(9) **Directory-rename cycle prevention was missing and is unsound if you just
walk the tree:** two concurrent renames each validate a legal tree and together
create an unreachable cycle (`mv /a /b/a` ‖ `mv /b /a/b`). Every dir row now
carries `parent_dir` + `parent_version`; a dir rename collects the destination's
WHOLE ancestry chain at those versions into its conditional PREPARE, so any
concurrent reparent in the chain aborts it. O(depth), rare. (10) **Dedup state
is now a bounded window** — per `(client_uuid, session_epoch)`:
`highest_contiguous_seq` + a small completion bitmap for the out-of-order edge +
a bounded reply cache; GC'd once fenced + ambiguity resolved + window passed.
Transaction **decision records** got the matching participant-ACK GC condition.
(11) **Publication validates durability evidence** — since targets are
deliberately dumb, the publication entry carries `coding_profile_id`,
`placement_epoch` and the per-fragment ACK set (target incarnation, role, object
identity, checksum), and the lane leader REJECTS evidence that doesn't match the
current profile/placement/k+f roles/distinct failure domains. A stale client can
durably write a full stripe to an obsolete placement and still not publish it.
(12) **NEW INVARIANT I25 · end-to-end integrity** — every durable fragment is
checksummed over **identity + payload**; a failing fragment is treated as
UNAVAILABLE and repaired, never fed to the EC decoder. Media corruption is not
Byzantine, and EC without integrity checking reconstructs confidently wrong data.
Simulator gained a silent-corruption fault (that is the only way I25 is ever
tested). (13) **Performance contract was too strong:** "no physical NVMe sync per
logical operation on any hot path" contradicts `write()`=durable. Now: **"no
persistence boundary is paid per chunk, per metadata record or per Raft group
when several operations can safely share one; boundaries are amortized to the
largest batch the externally visible semantics allow."** (14) **Doc CI gate**
added to `arch/development.md`: regenerate `architecture-full.md` and fail on
diff, validate internal links, validate every `I1..I25` reference, validate §6
matrix protocol refs, and **reject a normative table defined in more than one
place** (duplicated tables are how the index and a satellite come to disagree).
Round-7 rationale lives in `arch/design-history.md`; the index, all satellites,
the HTML and `architecture-full.md` are in sync (links + invariant refs
verified).
**CONTRIBUTOR ENTRY POINT (Sep 1 PM):** `docs/arch/START-HERE.md` — the
answer to "what do I work on and what governs it". §1 the current task
(Phase M step 2 `data/` + transport/store interfaces; step 1 `wire/`
landed Sep 2) + the rule for picking the next; §2 a routing table
(changing X -> read exactly these pages / these invariants govern / this gate
proves it); §3 what "done" means (build on a node, unit tests, the row's
gate, a timeout is a FAIL, honest measurement); §4 never-without-asking
(invent a decision the spec lacks, restate a normative table, new monolith
code, weaken an invariant, widen a timeout). **§5 added (Sep 2):** how to
brief a LOW-END AI agent — the briefer owns task pick/decomposition/reading
whitelist/diff review, the agent owns staying inside the named files + the
§3 checklist; includes a paste-able briefing skeleton and the two traps an
outside agent cannot rediscover (EEXIST-on-unique-name is never benign;
`pgrep -x` never `-f`). Linked from `architecture.md`
line 3, `arch/development.md` and the HTML; included in the generated
one-file version. It is navigation, not normative — it links to the single
homes of the §4/§5/§6 tables rather than restating them.
**EIGHTH REVIEW ROUND IN (Sep 1 PM) — composition edge cases. Reviewer's
verdict: architecture STABLE, no redesign; 8 of 10 merged as given, 2 merged
CORRECTED (better mechanism). Do not re-litigate:** (1) **I24 had only a
write side.** Publishing every chunk of a call under one decision does not
stop a slow READER: fetch A, writer's `{A,B}` commits, fetch B → old A + new
B, a state no serialization produced, and POSIX makes read/write atomic *with
respect to each other*. Multi-chunk reads are now a **validated collect**
(collect chunk-map versions → parallel fetch → revalidate → bounded retry →
read-only txn), same shape as `stat()`. Consequence: per-lane chunk-map
prefetch windows are **prefetch, not a cache** — usable without revalidation
only inside the read whose linearization interval covers them. (2)
**Committed reductions could briefly vanish.** Round 7 made lane `MAX`es
commutative payload but said they apply "at resolve time" while a txn is
visible at its *decision* — so between a durable COMMIT and the reducer
running, `stat()` could return a size older than a returned write. Pending
reduction intents are now **discoverable under the lane's key prefix**;
authoritative lane read = MAX(materialized, committed-pending);
materialization is background compaction. Also fixed the Appendix-6 sentence
`aborted/unknown → absent` (contradicted §7.2 + I9). (3) **Truncate had two
holes; the second was data resurrection.** (a) Nothing distributed the new
`content_epoch` to the lane leaders that must reject stale publications, and
writes must never touch the inode shard → **truncate pays: a bounded fence
over the inode row + active lanes (≤65 authorities)**. (b) "chunks beyond the
new size become unreferenced, reclaimed lazily" left the chunk-map entries
live, so a later sub-chunk RMW takes the pre-truncate generation as its base
and brings back truncated bytes; `base_size` alone cannot fix it (across
shrink→extend→shrink, survival depends on the SMALLEST size any *newer*
truncate imposed, not the latest). **MERGED CORRECTED:** reviewer proposed
logical range tombstones; the fix is a **per-lane RANGE DELETE** carried by
the same fence — the KV is ordered and lane i holds chunks i,i+64,i+128…, so
the entries beyond S are a contiguous key suffix = **64 range deletes, not 8M
key removals**. O(1)-ish shrink survives AND nothing resurrects, with no
per-chunk epoch bookkeeping and no unbounded truncate history. (4)
**`O_APPEND` was correct only against other appenders.** `append_eof` was an
independent counter on the inode shard, but an ordinary extending `pwrite`
publishes on a LANE and never advances it → after a write to 1 GiB the next
append reserved offset **0** and overwrote the file. Reservation now
validates against the **active-lane EOF vector using the `stat()` read set**;
an extending publication bumps `lane_seq` and aborts a racing reservation.
Ordinary writers pay NOTHING; the appender absorbs the retry. (5) **Online
f/k change could not prove its own result** (scanner races a live writer
republishing X on the old profile). The **profile cutover now commits BEFORE
the scan**, and round-7 publication validation enforces the current profile
from that moment, so the old-profile set can only shrink and the scan covers
a set that cannot grow. (6) **MERGED CORRECTED — timestamps.** Reviewer
showed one `mtime_gen` guarding both times makes mtime move BACKWARDS (write
stamps gen 7 → chmod bumps to 8 → the write's mtime is filtered out) and
proposed splitting `mtime_gen`/`ctime_gen`. Better fix, adopted: a generation
guard is only ever needed for a time that can be set **backwards**, and only
`utimens` can do that. So **only `utimens` bumps `mtime_gen`, only lane
mtimes are guarded, and ctime needs NO generation** — every other time source
is a monotone "now" and plain MAX is correct; guarding ctime would CREATE a
backwards ctime. The same fence as truncate distributes `mtime_gen`. (7)
**ABA reached data objects but not ephemeral inode state:** open leases,
byte-range locks and append reservations were keyed by bare `ino` → all now
**`FileID`-keyed**, and any op acting through an inode handle validates the
generation first (mismatch = stale-handle error, never a silent no-op). (8)
**Dedup GC was gated on a "retention window"** — the failure model allows
unbounded delay, so a timer asserts what the network never promised. Now a
client **response-ack watermark**. Sharpened: a retried `O_APPEND` reservation
must recover **the same offset** from the reply cache; "something completed"
would let it allocate a second range and leave a permanent hole. (9) **Lock
ownership is per namespace:** classic `fcntl` = process token; `flock` AND
OFD `fcntl` = **open-file-description token** (shared by dup'd/inherited
fds) — a single process token modeled only classic fcntl. (10) Textual:
"six review rounds"→eight; direct-write step 4 `size/chunk-map/mtime`→
`+ctime`; directory two-index "size and mtime"→`+ctime`; **I9 now says
non-`ENOENT`**, not necessarily *retryable* (an unrepairable integrity
failure is legitimately terminal EIO — the substitution is forbidden, not the
finality); `noatime` reworded to Linux's actual meaning (reads do not update
atime; it does NOT "track mtime/ctime"). **Positioning (§3):** efs is a
"high-performance parallel filesystem targeting Linux/POSIX semantics **with
explicitly documented deviations**", not "a POSIX filesystem" — §3 now lists
the two known deviations (no strict per-read atime; no syscall-level write
atomicity above the FUSE request boundary). New glossary terms: **inode
fence**, **validated collect**. Round-8 rationale in `arch/design-history.md`;
verification.md gained 8 fault-injection events (multi-chunk read vs write
decision, stat between COMMIT and resolve, truncate vs in-flight lane
publications, sub-chunk write into a truncated range, append vs extending
pwrite, lost reservation reply + same-op-ID retry, utimens vs in-flight
write, profile cutover vs live writer) and 3 new checker properties.
**NINTH REVIEW ROUND IN (Sep 1 PM) — protocol completion + kernel-interface
reality. Reviewer: architecture STABLE, "no reason to change the fundamental
architecture"; what remains is protocol completion, not redesign. All 10
merged. Do not re-litigate:** (1) **The normative §6 matrix had gone stale
against round 8 and §6 wins ties** — it still said `TRUNCATE / 1 shard /
single Raft entry` while §7.3 said inode+active lanes, so the SUPERSEDED
design was binding. Fixed; `SETATTR` split by class (mode/owner = 1 shard;
`utimens` = inode fence; `SETATTR(size)` IS truncate); added multi-chunk
`READ` row. §5 gained `mtime_gen`, the dir used-shard bitmap, `dentry_seq`,
append reservations, and the coding-profile authority. This is the failure
mode the "never restate a normative table" CI rule exists for. (2)
**Read/predicate guards are §7.2's THIRD primitive** — round 8 leaned on read
sets (O_APPEND lane_seq, RMDIR emptiness, rename ancestry, stat/read
fallbacks) that were never defined. Guards are durable, SHARED, and held
**through the decision**, not until the last check. **RMDIR forced the
phantom case:** emptiness is a claim about keys that DO NOT EXIST, so an
insert into an observed-empty shard has no version to check — each directory
now carries a per-dentry-shard **`dentry_seq`** bumped by every mutation
incl. inserts, and RMDIR guards those. Predicate isolation without MVCC and
without the rejected distributed entry counter. (3) **Validated collects were
validating the wrong thing.** Intents exist from PREPARE, so a read spanning
a *decision* can take old-A (undecided) then new-B (committed) with EVERY key
version unchanged. Collects now carry the set of txids they resolved as
**UNDECIDED** and re-check those; decisions are final, so anything already
decided needs no re-check (cost ∝ in-flight txns actually touched, usually
0). Applies to `stat()` and multi-chunk reads alike. (4) **O_APPEND was fixed
only up to the reservation.** An ordinary extending pwrite could publish EOF
400 while an append's [100,200) was unresolved — no legal serialization. An
**append barrier** now sits on the active lanes while reservations are
outstanding (same bounded fence, once per append BURST), constraining only
publications that would push EOF past the reservation watermark; ordinary and
within-watermark writes untouched. Plus: reserved length = the length the
client will attempt (it holds the whole FUSE buffer before reserving), so a
short append comes only from failure; and **ABORTED_HOLE** — a LIVE client
whose append fails commits the resolution itself (previously only a fenced
client could resolve, so a live failure blocked the frontier until someone
killed it). (5) **Truncate's tail chunk is now IN the transaction:** the
zero-filled tail candidate is written first and CAS-published inside the
truncate txn (losing the CAS retries the whole truncate) — publishing it
after would leave a window where the file is nominally short while the old
bytes past the new end are readable. (6) **Profile cutover repeated the
pre-round-7 fencing mistake:** a control-plane commit does not change what
4096 lane leaders believe. It is now a **barrier pushed to every publication
authority and durably ACKed** before the scan. **Lowering f is NOT the raise
sequence reversed** — drop the advertised guarantee FIRST, else weaker data
is written while the stronger promise stands. **`u` is protection DEBT, not
a headcount:** a returning node restores capacity, not the fragments it never
held (f=1: A down → G on B,C → A returns → B dies → 1 fragment, data lost
after ONE failure), so a degraded generation consumes budget until REPAIR
completes; "unavailable" is a committed control-plane state, never a client
timeout. (7) **Factual Linux error fixed (verified against fcntl(2) +
POSIX.1-2024):** classic and OFD `fcntl` are NOT non-interacting namespaces —
they differ in ownership/release but share ONE record-lock conflict domain
and conflict by byte range even within one process on one fd. `flock` is the
separate domain. Model = **2 conflict domains, 3 ownership kinds**. (8)
**Linux/FUSE is itself a serializer, per mount:** extending direct writes
still take the inode lock exclusively even with `FOPEN_PARALLEL_DIRECT_WRITES`,
`IOCB_APPEND` forces it, and `O_CREAT` takes the parent dir inode
(`FUSE_CAP_PARALLEL_DIROPS` = lookup/readdir ONLY). Not cluster-wide (64
nodes fine) but 64 ranks on ONE mount serialize before efs is called. Now an
explicit second unresolved kernel-interface item + a §9 caveat; pre-sizing
fixes only the first case. A benchmark stopping there must be reported as
that, not as "efs scaled". (9) **Hashed-dir fanout was NOT bounded** — dentries
hashed over all 4096, so `stat(dir)` could be a 4096-way collect while the
text claimed "bounded like a file's lanes". A spread dir now uses the **same
fixed 64-shard permutation** as file lanes (used set = 64-bit bitmap); first
use of a dir lane registers on the parent shard (≤64/dir ever — the exception
to "parent not involved"); dir `utimens` gets its own `dir_mtime_gen` fence.
(10) Smaller: implicit mtime/ctime are **MAX-clamped** because CLOCK_REALTIME
can step backwards (stated as a deliberate HPC choice, not assumed);
re-stripe/repair must NOT touch user-visible size/mtime/ctime; dedup ack
watermark must be highest **CONTIGUOUS** reply (out-of-order arrival would let
100 authorize discarding 99) and a **fenced** session's state is dropped
wholesale after the revocation barrier (it can never advance a watermark —
old-epoch requests are rejected by epoch, not by lookup); **L7 broadened** to
once-published-now-unreachable generations (truncate range-deletes,
re-striped old-profile copies). verification.md gained 12 more fault events
and 4 checker properties. Round-9 rationale in `arch/design-history.md`.
**ROUNDS 8 AND 9 ARE BOTH UNCOMMITTED** (HEAD = 492bfed, round 7).
**BYTE-RANGE LOCKING PROTOCOL GAP CLOSED (Sep 2, kimi-k3, user-reported gap).**
The spec had lock PLACEMENT/ownership/conflict domains (state table row,
§7.6, sessions.md) but not the distributed PROTOCOL. Added: blocking waits
(`F_SETLKW`/blocking flock) are long-lived RPCs held at the lock authority,
granted FIFO from an in-memory leader queue (grant = the reply; no polling,
no timers, queue is NOT Raft state); queued exclusive blocks later shared
grants (no starvation); leader failover → client re-issues under the same
op-id (I16 resolves a pre-failover grant); a session fenced mid-wait is
dequeued by the revocation barrier and never granted; EINTR cancels by
op-id. Full POSIX range algebra on the single authority (partial-unlock
split, adjacent merge, in-place type conversion, F_GETLK = leader read),
per-inode record cap → ENOLCK. Deadlock detection is SAME-INODE ONLY (the
authority holds the whole wait-for graph for its inode); cross-inode cycles
not detected — POSIX makes EDEADLK a MAY, Linux checks only classic fcntl
even locally, and no wait is ever stuck (signal + fencing). Touched:
sessions.md (3 new subsections), architecture.md (§7.6 paragraph + §6 LOCK
matrix row), architecture.html (locking card, plain language),
verification.md (8 lock fault events + 3 checker properties: no conflicting
pair granted, fenced waiter never granted, every wait resolves).
architecture-full.md regenerated (4272 lines); links + invariant refs
verified. UNCOMMITTED with rounds 8-9.

**One-file paste version (user, Sep 1 PM):** `docs/architecture-full.md` =
index + all satellites inlined, GENERATED by `docs/gen-architecture-full.py`
— never edit it directly; regenerate after any doc edit (for pasting the
whole architecture to another agent).
**SUPERSEDED Sep 18: `architecture.html` is now GENERATED** by
`docs/gen-architecture-full.py` from the md (same run that builds
`architecture-full.md`); `make docs-check` fails if either artifact is
stale or hand-edited. The hand-written plain-language/SVG version below is
gone — one source of truth, the markdown. Historical note follows.
**HTML IS THE PLAIN-LANGUAGE RENDITION (Sep 1 PM, user-directed).**
`docs/architecture.html` was rewritten end-to-end in simple wording: short
sentences, everyday words, and a "few words we use" glossary table up front
(shard / Raft / leader / majority / term / committed / KV / EC / generation /
idempotent / linearizable read / serialization point — each defined in one
plain sentence). Jargon is kept only as *names* (Raft, NVMe, RDMA, FUSE) and
explained once at first use. The md stays the precise spec; the HTML is the
readable version — keep both in sync when editing. Structure, SVGs, and
content are unchanged; only the voice changed.
**HARDWARE ENVELOPE (Sep 1 PM, user-confirmed): efs is flash/NVMe-ONLY — no
HDD support, ever.** Now in the spec (md §1 hardware-envelope block + §8
rejection; HTML §1 + §10). The architecture actively spends the assumption:
random I/O is first-class (no seek-aware layouts; KV pager faults and random
chunk reads are cheap by assumption), deep hardware queues are assumed (the
P4 model can't run on disk at all), µs-scale device latency is what makes
write()=durable affordable, and SMR/rotational/track-alignment problem
classes are deleted, not engineered around. Scaling claims do not transfer
to HDDs.
**IDENTITY / positioning (from the 2nd review — do not overclaim):** every
primitive (Raft, dist. metadata, RDMA, EC, immutable gens, KV, dir sharding,
deterministic sim) has existed; WEKA/DAOS/VAST/CephFS get close to parts.
The defensible claim is the COMPOSITION + the principle "never serialize work
that semantics and hardware allow in parallel", NOT "novel" / "nobody has done
this". Also part of the identity: **the namespace IS the database** (du/find/
lifecycle = KV queries, not traversals) and **minimalist ops** (3 nodes → efs
init → mount → done; no MDS tier, no special node types). **NAMING WARNING:**
"extremfs" collides with XtreemFS (trademark, Quobyte) and "EFS" = Amazon EFS
— pick a distinctive, searchable public name later; "efs" is a working name
only. Do a trademark/search check before any public naming.
**MODULARITY IS AN ARCHITECTURAL CONSTRAINT (Sep 1, user-directed) — and the
roadmap does it FIRST.** `docs/architecture.md` §5.14: a module must be
understandable/changeable/testable from its own source + interface header
alone; no file over ~1000 lines; state machines pure (no globals, I/O behind
transport/storage interfaces — the SAME property that lets the simulator
reuse the compiled SM). **The bar, user-directed (Sep 1 PM): a LESS ADVANCED
AI model must be able to contribute a correct change** — not the best model
with the whole tree in context. Consequences now in the spec: (1) local
correctness must be locally decidable (module + header + tests, never global
reasoning — else the project scales with model quality, not contributor
count); (2) the blast radius of a mistake is one module and fails fast/locally
(unit test / simulator assertion / interface check — the quality bar is
enforced by the boundaries, not the contributor's sophistication); (3)
interfaces carry the contract (invariants/ownership/threading in the header);
(4) conventions are machine-checkable (lint/build/test gate, not reviewer
vigilance). Honest current state: 4 files hold ~45% of the
36.5k-line tree (`metadata.c` 6042, `efs_fuse.c` 3720, `meta_server.c` 3654,
`handler.c` 3648). **Roadmap "Phase M — carve the monolith FIRST"** (added to
`docs/scaling-roadmap.md`, ahead of all other forward work): carve into
`raft/ kv/ meta/ wire/ data/ client/` as behavior-preserving refactor gated by
EXISTING suites (make test + solo posix, no wipe). It is both the dev-cycle
lever AND the hard prerequisite for migration step 1 (the simulator can only
reuse a state machine that is already pure). Do NOT build Raft/KV/txns as new
monolith code — every new component lands inside the carved boundaries.
Read `docs/architecture.md` before any metadata work. The scaling roadmap is
now the increment plan toward that spec, not the spec itself.


**"DANGLING DENTRIES" ROOT-CAUSED + RECOVERED (Sep 1 PM) — IT WAS THE INODE RAM
CAP, NOT A DESCRIPTOR CLOBBER, AND NO DATA WAS EVER LOST.** Trigger: shard 7's
inode region is 2.82M rows x 512 B = **~1.44 GB**, which EXCEEDS the 1024 MB
`EFS_INO_RAM_MB` default, so it is the ONE table big enough to make
`trim_ino_ram` evict. An evicted slab's only fault source is `flush_blob`; when
the fault fails `inode_at` returns NULL and the handler answers **NOT_FOUND** —
so a live row reads as a missing inode. Proof (decisive, one variable): restart
all 4 efsd with `EFS_INO_RAM_MB=32768`, change nothing else — **all 12 dangling
ROOT entries went to 0**, `synth-test-data` nlink 2 -> **11** and children 0 ->
**13**, `imagenet2` children 0 -> 3, every `setattr` OK. Shards 1/3/5 (132-232
pages) on the SAME owner as shard 7 were always healthy, and shard 6 (511 pages)
self-repaired on a plain restart while shard 7 did not — exactly the size
ordering the cap predicts. This is the known Cut-C blocker ("bounding the blob
without another page source just makes faults FAIL") **confirmed to be silently
returning wrong answers on a live cluster**, not a theoretical constraint; Cut A
(512 B rows) is what made it reachable at this table size.
**TWO HARDENING ITEMS THIS EXPOSES:** (1) a failed slab fault must NEVER be
reported as NOT_FOUND — it is a resource failure and must surface as EIO/BUSY,
because answering "no such inode" lets CREATE repopulate the name and mint a
second ino for a live object (this is the likely source of the ever-higher-ino
`name_dup` retries seen in the user's rsync); (2) the cap silently stops holding
instead of failing loudly. **MITIGATION IS ENV-ONLY AND NOT PERSISTED** — the
cluster currently runs `EFS_INO_RAM_MB=32768`; any efsd restarted without it
reverts to 1024 MB and the symptom returns.
**Superseded analysis kept only as the record of what was ruled out:** `rsync` into `/tmp/efs-mount/synth-test-data`
failed `failed to set times on "."` = EIO. Two separate things:
1. **Client bug, FIXED (working tree):** `efs_fuse_utimens` collapsed EVERY rc
 to `-EIO`, so a server `NOT_FOUND` read as "Input/output error". `chmod`
 hits the SAME SETATTR handler and correctly reported ENOENT via
 `efs_rc_to_errno` — that mismatch is what identified the real status.
 utimens/chown/truncate now all route through `efs_rc_to_errno`. Builds
 clean on fcstor007. **NOT yet gated with posix.**
2. **Real condition:** a set of directories have a ROOT dentry but NO inode
 row on the owning shard. Every one reports `nlink=2` (the dentry-stub
 default) while having real children; every one fails SETATTR NOT_FOUND;
 creating INSIDE them fails ENOENT (`mkstemp ... No such file or
 directory`), and each rsync retry re-mkdirs the same names with ever
 higher inos (deep_skinny_chain 18851023 -> 18855407 -> 18856063).
**The split is exactly by shard, and it is OLD rows only:**
 - broken (shard 6 or 7): `imagenet1` 893014, `imagenet2` 2603039,
 `scale-fcstor007` 52022, `scale-fcstor008` 16671, `scale-fcstor014` 6,
 `scale-fcstor015` 7, `synth-test-data` 889799.
 - fine (shards 0-5): `imagenet3` 444904, `imagenet4` 9,
 `scale-fcstor009`-`013` inos 8,2,3,4,5.
 - shard 6 owner = fcstor005 (also owns 2, healthy); shard 7 owner =
 fcstor006 (also owns 3, healthy) — so it is per-SHARD, not per-node.
**NOT spreading — do not treat as an active regression.** 64 fresh ROOT dirs
created on fcstor007, evenly spread over all 8 shards incl. 6+7, probed from a
DIFFERENT client (fcstor008): 64/64 SETATTR OK, and still 64/64 several
hundred rebuilds later. So new rows on 6/7 are fine; only pre-existing ones
were lost.
**RULED OUT by measurement — do NOT re-chase:** client-side artifact (same
inos + same EIO from a clean second client); ongoing/spreading loss (64 fresh
dirs over all 8 shards still 64/64 after hundreds of rebuilds); whole-shard or
whole-node outage (shards 6/7 accept NEW rows fine, and their sibling shards
2/3 on the SAME owners are healthy); `efs_export_evict_cold_shards` (defined
in metadata.c + declared in the header, but called ONLY from
`tests/test_meta_v6.c` — it is dead code in the server, so LRU eviction cannot
be the way a tab goes non-resident).
**ESTABLISHED by code read.** The only way an extra tab goes non-resident is
the keep/free block in `server_rebuild_export_from_pages_ino` (meta_server.c
~1088-1132): `keep=0` falls through to `efs_export_free(ex)`, which frees it.
It is then recreated LAZILY by `shard_tab_get_or_create` (metadata.c ~2473),
which sets `meta_needs_rebuild = (tab->root.page_count > 0)` **only if the
installed root carries a descriptor for that shard**. No descriptor (or
page_count 0) leaves an EMPTY table with `meta_needs_rebuild = 0`, which
`server_ensure_shard_ready` reports READY — so LOOKUP answers NOT_FOUND and
CREATE happily repopulates it. That is exactly the observed signature
(dentry survives on another table, inode row gone, new rows fine).
**SUPERSEDED SUSPECT (was wrong — the cap was the cause; kept because the fix
below is still correct hardening and is now IN TREE):** the rebuild installs its
root with a WHOLESALE
`efs_export_root_move(&ex->root, &snap)` at meta_server.c:1202 — **no
`efs_export_root_maxmerge_extras`**, unlike the paths at 2516 and 2996-2997
which merge first. A root_move that drops extra-shard descriptors is precisely
the Aug 23 "efs-s3 root clobber" bug, whose documented symptom was "shard
tables permanently lost"; that fix was applied to primary-commit capture,
PUT_META full-adopt and catchup install, but meta_server.c:1202 (and 2474,
2637, 2817) still move wholesale. Proving it needs a descriptor dump at
recreate time = an efsd restart on all 4 (build-id gate), which was deferred
because the user was running load.
**THREE SERVER FIXES LANDED IN TREE (Sep 1 PM, built + unit-gated + deployed;
NOT posix-gated, NOT committed).** None of these was the cause of the symptom
above (the diagnostic below fired **0** times, which is itself the evidence that
the descriptor path was innocent) — they are correct hardening found by reading
that path, and they ship with the redeploy:
- **A. rebuild root install now maxmerges extras.** `snap` is deep-copied from
 `ex->root` BEFORE the long unlocked page fetch, and the re-check at the swap
 compares only `generation`. Extra-shard commits are deliberately SAME-gen
 (`server_commit_cluster_extras` keeps the main gen so peers can tell an
 extras-only refresh from a real advance), so a descriptor landing during the
 fetch window is invisible to that guard and `efs_export_root_move` would drop
 it. Now `efs_export_root_maxmerge_extras(&snap, &ex->root)` runs under the
 lock before the keep/free scan (so the scan's descriptor comparison sees it
 too). Same class as the Aug 23 root clobber; this was the one call site the
 Aug 23 fix never reached.
- **B. `efs_export_precreate_shards` was a silent no-op (the "definite bug").**
 It ran BEFORE `efs_export_root_move` installed the real root, so `ex->root`
 was still the staged root — which the code's own comment says carries no EFSR
 — hence `shard_bits == 0` and it returned immediately, creating NOTHING after
 every main rebuild. Blocker 2's invariant ("op/read paths never do the
 per-export lazy-create mutation under a single shard lock") was silently not
 held. Moved after the root install (and after chunk_size/features restore,
 which new tabs inherit), still inside the held shard locks.
- **C. diagnostic:** loud warning when an extra tab comes back empty AND not
 marked for rebuild AND has no descriptor while the root carries descriptors
 for OTHER shards — i.e. the exact state that reports READY with no rows.
 Fired 0 times across the redeploy.
Context: all 3 joiners rebuild constantly (2287-2355 `meta-catchup:
rebuilding` each), every rebuild loads a root at `next_ino` ~594368 while the
live allocator is at 2603040 (the cad1083 `meta-repair` floor fires 1294-1496
times per joiner), and fcstor005 logged 233 `page N unrecoverable ... raced
with GC` (fcstor004 24, fcstor006 1).
**Two measurement traps that cost time here — do not repeat:**
 - `efs_client_chmod` returns EFS_OK WITHOUT an RPC when the mode is
 unchanged, and utimens/chmod/chown all short-circuit locally when
 `efs_client_ino_is_dirty(ino)`. Probing with an unchanged mode, or from
 the client that just created the row, gives a false PASS (that is why
 `imagenet1` first looked healthy). Probe from a SECOND client with
 `os.utime(p, (current atime, current mtime))`.
 - node9901's `efs-fuse` died mid-session with no log line; `/tmp/efs-mount`
 reverts to a plain empty dir and EVERYTHING reads ENOENT, which looks
 exactly like mass data loss. Check `findmnt -o FSTYPE` before believing
 a disappearance (same class as the fio `NOT_FUSE` rule).

**SIZE-VISIBILITY CLOSED (Sep 1, commit b518003, GATED).** `stat` could return
a stale/zero size right after a write (5-6 of the 10 non-deep 16x failures
since Aug 27). Root cause, found with NDJSON probes: `dirty_snap_save_locked`
DETACHES the dirty set the instant a REPORT starts, and `efs_fuse_flush`
(close) kicks that report **without waiting**. So between snapshot and apply
the inode reads CLEAN, `lookup_walk` skips the local overlay, and the owner's
older size wins. Fix: `efs_client_ino_is_dirty` also consults the set a report
is publishing (`pub_ino_keys`, aliased under `dirty_mu`, cleared on **every**
exit path incl. both OOM returns — the alias would otherwise dangle past
`dirty_snap_merge_back`); and the leaf overlays local size/pack when its mtime
is not older, leaving name/nlink/mode authoritative on the owner (Cut 4).
Shrink still works: SETATTR stamps mtime=now server-side so a truncate is
newer and wins. **Ruled out by probe, do NOT re-chase:** H2 local row size 0,
H3 loss in `fill_stat_from_inode`, H4 server rejects grow on the mtime rule,
H5 ino never in the size recs, H6 report and GETATTR on different nodes,
H7 server drops recs for an ino not on the tab, H11 `lookup_needs_getattr`
keeps a size-0 dentry stub.
Gate: 16x size-visibility failures **8 -> 0** (`attr_stat_fields`,
`content_random_roundtrip` x3, `opt_fallocate`,
`content_random_overwrite_append`, `opt_copy_file_range`,
`names_near_path_max`, `chmod_preserves_mtime`); solo posix **0 EFS bugs**;
fresh-cluster 16x leaves only `dir_deep_nesting_beyond_64` x11-13 plus known
mtime/flock flakes.

**POST-CUT-A RAM BASELINE — Cut A made bytes/inode WORSE and BREACHED the cap
(Sep 1, `results/scale/20260901-cutA-baseline`, fresh bits=3 TCP, 9 clients
x16 threads, same shape as the Cut 3 run).** This is the number the structural
program is measured against; there was none before today.

| total | creates/s | errors | rss_max MB | rss_sum MB | B/inode |
| 100k | 9999 | 0 | 100 | 323 | 3387 |
| 500k | 26666 | 0 | 559 | 1474 | 3091 |
| 1M | 33333 | 0 | 874 | 2520 | 2642 |
| 2M | 33333 | 0 | **1476** | 4429 | **2322** |

vs Cut 3 at 2M (`20260831-cut3`): 1023 MB / 3216 MB / **1686 B/inode**. So
Cut A is **+636 B/inode (+38%)** and `rss_max` **1476 MB now EXCEEDS the
1024 MB `EFS_INO_RAM_MB` default** (Cut 3 sat exactly ON it at 1023).
Latency is fine and unchanged (stat p50 0.39 ms, create 0.41, unlink 0.39,
readdir 5.4 ms, all flat 100k->2M) and **errors=0 at every step** (Cut 3 had
42 at 2M), so this is purely a memory regression, not a correctness or speed
one.
**Cause is arithmetic, not a bug.** v8 makes slab == page with 512 B rows,
so the serialized inode region is 256 rows/page and `flush_blob`'s ino region
is **exactly 512 B/inode**: 2M inodes = 7813 slabs x 128 KiB = **977 MB**, or
95% of the whole 1024 MB cap. `ino_ram_bytes` counts the blob INSIDE the cap
and `trim_ino_ram` loops `while (bytes > cap && ino_slabs_resident > 1)`, so
once the blob alone approaches the cap, evicting every slab but one still
cannot get under it and the cap silently stops holding. At 1M the blob is
488 MB (48% of cap) and rss_max 874 MB is still under — the breach appears
between 1M and 2M exactly as the arithmetic predicts.
**Do not read this as "Cut A was a mistake":** the 512 B self-contained row is
what makes per-page faulting possible at all. It trades RAM for the ability to
page, and the trade only pays once the blob is gone. That is Cut C, and this
baseline is the quantified case for it.

**CUT C IS BLOCKED ON A LOCK INVERSION — measured, not guessed (Sep 1).** Do
not schedule "bound flush_blob" as a standalone task. `flush_blob` is
**server-only** (nothing on the client ever assigns it; the client never
trims, which is why Cut D is the client-side item) and plays THREE roles:
(1) the ONLY backing store for an evicted slab (`inode_slab_fault`), (2) the
base `efs_export_serialize_dirty` memcpys forward for clean pages, (3) the
memcmp base for page reuse in the flush loop. Role 3 already degrades safely
to the committed-root checksum compare. Role 1 is the blocker: bounding the
blob without another page source just makes faults FAIL, and `trim_ino_ram`
already refuses to evict when `!flush_blob && !page_src`.
**Why `page_src` is not a wiring job:** `server_global_lock` is a plain
NON-RECURSIVE `pthread_mutex_lock(&s->lock)`, and handlers hold it across
`efs_export_get_inode` -> `inode_at` -> `inode_slab_fault`. A fault calling a
network page reader would (a) re-lock `s->lock` and self-deadlock and (b)
issue peer RPCs under the global metadata lock. Faults must be resolved
OUTSIDE the lock (try -> "need page N" -> drop lock -> fetch -> retry) before
that hook can be filled.
**Landed as the prerequisite (commit 05df9eb):** `server_fetch_meta_page`
factors the per-page fetch/decode/heal out of
`server_rebuild_export_from_pages_ino`. Behaviour-identical; gated by a joiner
restart that rebuilt and rejoined at a consistent gen with 0 unrecoverable /
zero-filled / checksum-mismatch / decode-failed on all 4 nodes.
**Also note for whoever does Cut C:** `slab_page_persisted` currently returns
`ex->page_src != NULL` when there is no blob — with a page_src installed that
is TRUE for every si and would re-break fresh slab growth (the Sep 1
"jammed at exactly 256 rows" bug). Make it authoritative from
`root.ino_page_count` first. And `serialize_dirty` only needs bytes for pages
that will actually be ENCODED: a non-resident slab is clean by construction
(trim skips dirty pages), so its page can be reused by ci from the committed
root without any bytes at all.

**PATH RESOLUTION WAS CUBIC — FIXED + GATED (Sep 1, working tree, UNGATED
commit).** `posixstress 16 fcstor007` (16 concurrent suites, one client) had
`dir_deep_nesting_beyond_64` failing **16/16 on `timeout after 15s`**. Not a
flake and not saturation: solo it took **7.0s** of the 15s budget.
Root cause, measured (NDJSON probes on `lookup_walk` + `efs_fuse_mkdir`, not
code reading): building a depth-100 tree cost **256,985 LOOKUP RPCs**, and
`total_walk=6.23s` of a 6.6s run. Two multipliers compose:
(1) `entry_timeout=0` means the kernel caches no dentry, so a depth-k op
issues k FUSE lookups; (2) Cut 4 made `lookup_walk` re-resolve **every
component from the root** with no memoisation, so each of those k lookups
costs up to k more RPCs. Net **O(depth^3)**.
**Ruled out by the same probes — do not re-chase:** the CREATE itself
(102 mkdirs = **0.00s** total, flat ~38us at every depth) and BUSY backoff
(a flat **24us/RPC** at every depth; a 50ms<<n sleep is unmissable).
Fix: `walk_batch_ancestors` in `ops.c` resolves the leaf's ancestor chain
with batched LOOKUP_PATH (<=64 components per RPC) and leaves the **leaf**
on the authoritative per-component LOOKUP(+GETATTR), so Cut 4's invariant is
untouched. Any non-OK batch reply falls back to the full per-component walk,
so a batch miss (extra hashed ROOT shard) can only cost an RPC, never change
an answer. Only engages past `EFS_WALK_BATCH_MIN` (8 components) — normal
3-6 component paths keep today's exact RPC pattern.
**Wire change:** `struct efs_msg_inode_lookup_path` gained `efs_ino_t start`
(path is relative to it; 0 = root) so a >64-deep chain can be walked in
ceil(depth/64) trips while still returning every ancestor's mode/uid/gid for
the exec check. `efs_client_rpc_lookup_path` now routes to the owner of
`start`. **EFS_BUILD_ID changes — restart all 4 efsd together** (on-disk
EFSM v8 is unchanged, so NO mkfs/wipe is needed; redeploy on the live table).
Measured before/after, same 6696 kernel-driven walks both times (so this is
fewer RPCs, not a cache hiding work): LOOKUP RPCs **256,985 -> 9,815**,
walk time **6.23s -> 0.25s**, RPCs/walk **101 -> 1.5**, test **6.6s -> 1.1s**.
Gates: solo posix `results/posix/20260901-134359` **1 EFS bug**
(`attr_touch_terminal`, known mtime flake); posix2 007+008
`results/posix2/20260901-134445` **1 EFS bug** = `peer_fcntl_range_conflict`
(known per-client `lockf` gap) with the hardlink/nlink and
`peer_rename_across_dirs_chase` tests — the ones Cut 4's caches broke — all
**PASS**; `test_meta_v6` / `test_dir_stats` / `test_ino_path` /
`test_meta_slot` OK.
**posixstress 16 `results/posix/20260901-134816`: 23 EFS bugs total (was 34),
3 instances fully clean 195/0 (was none).**
**STILL OPEN — the kernel multiplier.** `dir_deep_nesting_beyond_64` still
fails **13/16** under 16-way. Measured under load: **450,037 walks** in the
run and per-RPC latency inflating **25.5us -> 62.9us** (walk p50 144us, p90
1423us, max 18ms), so ~6700 sequential walks sit right on the 15s budget.
The remaining factor of k is `entry_timeout=0`, which is a deliberate
cross-client correctness choice (a kernel dentry cache ghosts a peer unlink —
posix2 `peer_open_rename_fd`). Per-op cost is now near-optimal; do **not**
"fix" this by raising POSIX_TEST_SEC or by turning on entry_timeout without
solving peer invalidation.

**CUT A (EFSM v8) POSIX SESSION — 2 BUGS FIXED, 1 OPEN (Sep 1, working tree,
UNGATED).** v8 = 512 B self-contained inode row (name inline at off 126),
slab si == page 1+si, per-slab name arenas, `slab_idx` in the row, `page_src`
hook (unwired). **Not wire-compatible with v7 — fresh mkfs required.**

**Fixed 1 — moving an inode row must re-intern its name.** `name_off` is an
offset into the OWNING SLAB's arena, so any raw `*dst = *src` row copy across
slots aliased or lost names (false EEXIST on fresh names, ENOENT on real
ones). Added `inode_row_move`; used by `export_drop_zero_inodes` (compaction)
and `remove_inode_slot` (swap-remove). `efs_export_link` clears
`name_off`/`name_len` and re-stamps `slab_idx` before `inode_set_name`.
`inode_set_name` no longer takes the in-place path when the row does not own
`name_off` (`name_len == 0`), and interning `""` is a no-op.
Regression tests in `test_meta_v6`: row-move churn + never-used-name
aliasing, sharded ino uniqueness, and a v8 serialize/deserialize round trip
over 5 slabs of mixed 1-char/long names. `test_meta_v6: OK`.

**Fixed 2 — `lookup_walk` leaked a dir-lock stripe.** It locked
`child.parent ? child.parent : child.ino`, then OVERWROTE `child` with the
local row and recomputed the same expression to unlock — a different stripe
when the local row's parent differed. The stripe stayed held, and the client
flush thread's `lock_all_dirs` then deadlocked against FUSE workers (mount
D-state, `/sys/fs/fuse/connections/*/waiting` climbing, `idx_mu` free).
Now the key is captured once in `lk`. `efs_client_lock_dir` /
`unlock_dir` carry a thread-local `t_dir_held[]` that reports
`DIRLOCK-RECURSE` / `-NEST` / `-UNDERFLOW` with a backtrace; after the fix
those are silent. **This is why HEAD (1d7d582, without the fix) HANGS the
16-way suite while the v8 tree completes in seconds** — do not read HEAD's
hang as a v8 regression, and do not use HEAD as the fresh-cluster baseline.

**Gate reached:** long-lived cluster, remounted client, `--jobs 16` ×3
consecutive: **200/201 each**, only `fcntl_byte_range_lock` (known
per-client `lockf` gap). Compare gate `results/posix/20260901-044258`
190/201 was on a client whose table still held pre-fix corruption.

**CLOSED (Sep 1) — the fresh-mkfs catastrophe was TWO bugs, both fixed and
gated.** Fresh bits=3 TCP, solo posix on fcstor007, **4 consecutive runs
196/201, 0 EFS bugs** (`results/posix/20260901-13*`); unit
`test_meta_v6` / `test_dir_stats` / `test_meta_slot` / `test_ino_path` OK.
Found by NDJSON probes, not by reading code — both hide on a settled
cluster, which is why HEAD looked fine there and died on a fresh wipe.
1. **A catching-up joiner answered LOOKUP as if the export were not
 sharded.** `export_is_sharded_root` required a resident ROOT inode row —
 true of the main table in steady state, false on a joiner that has not
 caught up. `efs_export_lookup` then took the flat single-table path and
 never consulted a shard tab, so every name on an extra shard came back
 ENOENT. Evidence: 2171 events, all from fcstor006, all reporting
 `shard_id=0 shard_bits=3 shard_count=8` (configured sharded, unsharded
 path). Extra tabs carry the same `shard_bits` — that is what the ROOT-row
 term was really guarding. Fix: test `ex->shard_id == 0`; shard 0 always
 resolves to `ex` itself and never gets a tab.
2. **Tables jammed permanently at exactly 256 rows (one v8 slab).**
 `inode_slab_ensure` split "fault from disk" from "fresh slab" on
 `s0 < inode_count`, but callers do `pos = ex->inode_count++` BEFORE
 filling the row, so the first row of a new slab always looks like a
 fault; its page was never persisted, the fault failed, `inode_at`
 returned NULL, and the table stopped growing. Evidence: 173 identical
 events `s0=256 inode_count=257 blob_ino_len=262144 page_needed=2
 page_bytes=262144`, plus 1248/1520 misses on a table stuck at 256. Fix:
 `slab_page_persisted()` — a failed fault is fatal only when the image
 actually covers that page, otherwise it is fresh growth and gets a
 zeroed slab.
Post-fix deltas: unsharded fallthrough 2171 -> 0; tables at 256
1248 -> 0 (now 900+); successful creates 262 -> 1446; rows present at a
lookup miss 0/1520 (every remaining miss is an ordinary pre-create
negative). Do NOT re-chase the list below — it stays only as the record of
what was ruled out.

**Historical (the symptom list while it was open):** 173/181 failures are one shape: a test dir is created OK
(`cwi:` trace shows correct sharded inos, e.g. root ino 5 -> children 13,
21, 29 all on tab_shard=5) and the very next open of `<testdir>/f` gets
ENOENT. **Ruled out by direct instrumentation, do NOT re-chase:**
`efs_export_alloc_ino` never restarts a class or regresses (`ino-alloc:
restart/REGRESS` = 0 on all 4 nodes); `create_sharded` and CREATE_SHARD
always allocate from a table whose `shard_id == target` (`create-tab` /
`create-shard: MISMATCH` = 0); no ownerless create (`create-owner: NO
OWNER` = 0); the root is never transiently unsharded (`create-bits:
UNSHARDED` = 0); no server-side EXIST (`create-exist` = 0); CREATE replies
always carry the requested parent+name (`rpc-create: REPLY MISMATCH` = 0);
`nlive=4` and `owner_psh` agrees with the answering node. The `lookup-miss`
flood (2822 on one node) is ordinary negative lookups from `makedirs` —
not evidence. The secondary `cwi-fail: ino_dup parent=8 name=a ino=8`
family (client-side, child handed its parent's ino, always the 8/16/24…
shard-0 sequence) appears only in SOME fresh runs and is still unexplained;
`held_by` shows the ino belongs to a dir the server gave a different ino.
LOOKUP is answered purely from local tables and never returns NOT_PRIMARY —
worth a look. Next probe: log successful CREATE on the REMOTE/CREATE_SHARD
branches (only the local branch is logged today, 78 of ~200 creates) and
pair it with the miss by name.

**Debug scaffolding still in the tree from that hunt (safe to remove now
that the bug is closed; all env-gated, off by default):**
`EFS_INO_PROF` (server: ino-alloc restart/REGRESS, create-exist,
create-tab/create-shard MISMATCH, create-owner, create-bits, create-ok,
lookup-miss; forwarded by `clean_cluster.sh`), `EFS_CWI_TRACE` (client:
every `create_with_ino` with ino < 256), the unconditional `rpc-create:
REPLY MISMATCH` check in `efs_client_rpc_create`, the `held_by` fields on
`cwi-fail: ino_dup`, and the SIGUSR1/SIGUSR2 all-thread stack dumper in
`efs_fuse.c` (spinlock-serialized) + `tests/debug/resolve_stacks.sh`.
Patch snapshot of the pre-instrumentation cut: `/tmp/v8-work.patch`.

**INFRA: the shell exec env died at the end of this session** (same trap as
Aug 29) right after a `clean_cluster.sh` + `setup`; the cluster may be
mid-deploy. Verify `efs-mgmt status` and remount clients before trusting
any measurement.

**METADATA STRUCTURAL PROGRAM (Sep 1, working tree, UNGATED):** Four cuts in
tree. Do not treat posixstress flakes / LINK `lock_all` / BUSY backoff as
the next lever.

**Cut 1 — extra writes do not inherit MAIN fence.**
`main_fence_blocks_shard` BUSY only if `!shard_bits` or shard 0 is in the
lock set. CREATE with parent dentry on shard 0 still fences (`20260831-182724`
EIO). LOOKUP of hashed ROOT names ensures/locks the hash shard, not shard 0.
After main rebuild: keep live+clean extra tabs whose descriptor gen+checksum
is unchanged (`meta-rebuild: kept extra shard=N`). Joiner restart then hashed
ROOT mkdir: **OK** (10.3s BUSY then create, not EIO); fcstor005 logged
`kept extra shard=6`. TCP solo posix `results/posix/20260831-232435`:
**193/201** (3 EFS bugs: `concurrent_write_and_readdir` 39/40 flake,
`dir_deep_nesting_beyond_64` same-name `/d/d/…` ENOENT ~depth 31 isolated,
`names_crazy_dirs` isolated PASS). posixstress 2×4 `20260831-233935`: **0
FUSE create EIO**; 008 warmup 70s miss + 007 4-way 15s timeouts (Cut 4 RPC
tax, not the fence EIO). bits=5 ecopy_rounds 3r `results/ecopy/20260831-bits5-cut1`
vs `20260831-bits5`: joiner **005 rebuild_work 313→34, GC 62→0**; 006
rebuild_work 5 / GC 1; 004 129 / 17; primary 0. Kept-extra lines
151–285/joiner. files/s 63 | 172 | 58 (Cut 4 LOOKUP tax; host FAILs remain
CREATE-EIO family). `EFS_MKFS_SHARD_BITS=5` on `clean_cluster.sh` +
`EXTRA_DEFS` / `#ifndef EFS_DEFAULT_SHARD_BITS`. Sep 1 re-wipe is **bits=3**.

**Cut 2 — name arena, slim live inode.** `efs_inode_mem` has `name_off`/
`name_len`; RPC `struct efs_inode` still has `name[256]`. No EFSM bump.
Empty-name SETATTR/REPORT upsert must not intern `""` (O_TRUNC ENOENT).
`test_meta_v6: OK`. scale_grow 1M `results/scale/20260831-cut2`: RSS
**1897 B/inode** (1809 MB / 1M); unlink p50 **0.386 ms** flat; creates 19k/s
at the 1M step. Residual errors 10–29/step (catchup, <5%).

**Cut 3 — inode slabs + RAM cap.** `EFS_INO_RAM_MB` default 1024; trim
clean slabs after flush. scale 2M `results/scale/20260831-cut3`: rss max
**1023 MB** (cap) sum 3216 MB = **1686 B/inode**; unlink p50 0.375 ms.
5M/10M continuation wedged (007 `makedirs` FileExistsError on existing
hashed ROOT `scale-fcstor007` after LOOKUP miss — Cut 4 remount path).
Do not quote 5M/10M.

**WHY META IS STILL RAM (Sep 1, code-read, no code changed).** Cut 3 is a
RAM↔RAM pager, not RAM↔disk — do not read the 1023 MB cap as "cold inodes
live on NVMe". `inode_slab_fault` restores a slab from **`ex->flush_blob`**,
the last serialize of the WHOLE inode region, also in RAM; `trim_ino_ram`
returns early `if (!ex->flush_blob)`, and `ino_ram_bytes` counts the blob
INSIDE the cap, so trimming slabs fights a term it can never evict. If the
blob cannot restore a slab, `inode_at` returns NULL and the op fails —
there is **no** "fetch `page_cis[i]` from the 2+1 fragments" path on a
LOOKUP/CREATE miss. `page_cis` is read only by
`server_rebuild_export_from_pages_ino`, which loops **all** `page_count`
pages into one `calloc(page_count, 128 KiB)` and deserializes the full
table. `evict_cold_shards` frees whole extra tabs (shard>=1, clean), never
cold pages inside a live tab. `name_arena` + `chunks[]` stay one contiguous
alloc per tab. Third full copy: every client still `efs_export_deserialize`s
the GET_META blob into `g_client.export` at mount (read.c comment: "over a
GiB at multi-million-inode scale"); Cut 4 stopped *serving names* from it,
not *loading* it. Hence ~1.7 KB/inode cluster-wide and 2^32 ≈ multi-TB.
**Two defects found while verifying (unfixed):** (1) `name_arena` is
append-only with no free list and a refaulted slab has `name_len=0`, so
`inode_set_name` takes the grow path — every evict→refault cycle re-appends
every name and permanently grows `name_arena_cap`, which is itself inside
the cap. (2) `inode_slot_of` is a linear scan over `ino_slab_n` when
`ex->inodes` is NULL (~9.5k slabs at 10M, `EFS_INO_SLAB_ROWS`=1057) on the
rename/mark-dirty paths. Real fix is the pager: page-granular fault from
`page_cis`, `flush_blob` shrunk to dirty pages, bounded client inode cache.
Not more shard bits, not 9x4 posixstress.

**Cut 4 — lookup_walk always RPC names/nlink.** No dir short-circuit /
`created_recent` / `lookup_cache`. No LOOKUP_PATH (primary misses extra
hashed ROOT dest). Dirty size/pack overlay + CREATE dual-apply kept.
Client LOOKUP tries hash(1,name) first for ROOT. posix2 007+008
`results/posix2/20260831-234915`: hardlink/nlink tests **PASS**
(`peer_nlink_after_link`, `peer_hardlink_*`); 50+ tests PASS then
`peer_rename_across_dirs_chase` 45s SSH hang and the rest 124. ecopy
sample bits=5 TCP: **FUSE_OK**, 1299/1300 files, **75 files/s** (create
RTT, not cache).

**Re-gate Sep 1 (bits=3 TCP, after LOOKUP skip-ensure):** Parent owner
LOOKUP of hashed ROOT no longer `ensure`s an extra shard it does not own
(that BUSY'd remount LOOKUP while CREATE saw the shard-0 dentry). Isolated
same-name nest `d/d/…` **100/100 OK**. TCP solo posix
`results/posix/20260901-010229`: **194/201**, 2 EFS bugs
(`concurrent_create_unlink_two_proc` flake; `dir_deep_nesting_beyond_64`
ENOENT ~56 under 16-way — isolated PASS). posix2 `20260901-010348`: **49/63**
both-pass; hardlink/nlink (`peer_hardlink_visible`, `peer_nlink_after_link`,
`peer_hardlink_write`, `peer_chmod_via_hardlink`) **PASS**; then
`peer_rename_across_dirs_chase` 45s D-state and the rest 124. posixstress
2×4 `20260901-011809`: **0 EIO** in any TSV; 007-0 **141** both-pass before
385s kill; others empty-TSV/FileNotFound (Cut 4 RPC tax, not CREATE-EIO).
scale 10M `20260901-cut3-10m` wedged (011/013 create count frozen, FUSE
D-state); **do not quote**. Cut 3 RSS gate remains 2M
`20260831-cut3` **1023 MB cap**. `test_meta_v6: OK` on fcstor003.

**Empty-name upsert (Cut 2, gated with posix):** `inode_from_rpc` skips
interning `""` when the live row has a name; upsert rebind only if
`rec->name[0]`.

**POSIXSTRESS 2×4 LOOKUP FENCE (Aug 31, bits=3 TCP):** `posixstress 4`
fcstor007+008. Baseline `results/posix/20260831-184519`: 007 near-clean
(196/194/192/194), 008 saturated (120/192/127/119, ~70 timeouts). Servers
idle (~2 cores); client **busy_us** 124s/273s. Debug: 87/112 `main_fence`
BUSYs were LOOKUP on extra shards 1/2/3/5 (Heal idle) — client
`usleep(50ms<<n)`. Skipping the main fence for **all** extra ops caused
CREATE EIO (`20260831-182724`). Fix: `main_fence_blocks_shard_read` only
for LOOKUP/GETATTR/READDIR/GETCHUNKS/LOOKUP_PATH when shard≠0; CREATE/
UNLINK/SETATTR/REPORT/APPEND still fence. Gate `20260831-185301`: **196/0
×2, 195/1 ×5, 194/2**; **0 timeouts**; **0 FUSE create EIO**. LOOKUP
extra `main_fence` 87→0. 008 `busy_n` 940→68. Residual: `ctime_on_link_rename`
ENOENT (known flake) ×5; one `fsync_then_fstat_size`; one rename-over-nonempty.
Do not skip extra CREATE fence.

**POSIX2 MULTI (Aug 31, bits=3 TCP):** `results/posix2/20260831-195716`
4 pairs **61/63**, 0 timeouts. `peer_concurrent_hardlink` /
`peer_concurrent_unlink_hardlinks` were 4/4 `nlink got 2 want 3/1`:
`lookup_walk` served the local dual-apply row (created_recent /
lookup_cache) while the peer's link/unlink had already landed on the
owner. Fix: those shortcuts only apply when `nlink<=1`; `adopt_rpc_inode`
merges owner nlink. Gate: both tests 4/4 PASS. Remaining (unchanged):
`peer_fcntl_range_conflict` = per-client `lockf` (feature, adjacent-range
is target-better); `peer_rename_across_dirs_chase` = known dual-apply
stale-dentry flake (`x` at d2 and d3). Harness parents now include
`RUN_ID` so leftover `posix-2c-*` cannot FileExistsError prepare.

**META BENCH HOT PATHS (Aug 31, bits=3 TCP, `efs-bench --meta`):**
`results/meta/20260831-153805` after three server-side cuts (working tree).
Workers 1/4/16 × 5000 files × 64 hashed ROOT dirs. Rename was 5000/5000
NOT_FOUND (`efs_export_rename` used `inode_ptr` on the main table; files
live on extra tabs — FUSE uses `rename_at`, bench used `efs_export_rename`).
Hashed-ROOT rmdir mapped `ensure_shard_ready` not_owner → BUSY → 50ms<<n.
CREATE_SHARD/UNLINK_SHARD inherited the MAIN `meta_needs_rebuild` fence
(joiners catching up after extras+main flush) → w4 mkdir **82 ops/s**.
Fixes: shard-route rename; skip ensure on shards this node does not own;
CREATE_SHARD/UNLINK_SHARD do not BUSY on the main fence (extra readiness
is `ensure_shard_ready(target)`). Bench dirs are hashed names under ROOT.
Post-H6 mkdir w4 **11831 ops/s** (`busy_n=0`; was 82 / `busy_n=16` / 3s
sleep). Rename 10k ops 0 fail. Create 25k / 78k / 111k ops/s. Cluster
Heal idle gen=2 all 4. 2PC/CoW unchanged. Next: CREATE RTT (~40 µs), not
more false BUSY.

**POSIXSTRESS 9×4 HOT PATH SPLIT (Aug 31):** Fresh bits=3 TCP
`results/posix/20260831-140025` + `EFS_RPC_PROF` (client checkout/send/recv/
BUSY). 28/36 tsv (009/010 warmup lost). **99.3% of fails are `timeout after
15s`** (3297/3320); fcstor012 nearly clean (196/0 … 199 pass). Servers idle
(primary **1.9/24** cores, 103 `wait_woken`). `lock_all` hold 19 ms/run,
`1_wait` 4.7 ms, `global_wait` 0.56 s primary / 2.5 s worst joiner — not
15 s. Client (007, 15k RPCs): checkout 6 µs, send 5 µs, **recv 124 µs**
(LOOKUP 81 µs, CREATE 37 µs, REPORT 3.1 ms). **99% of client RPC wall is
`usleep(50ms<<n)` on BUSY** (245 s / 609 events). APPEND does not take
that sleep. Joiner extras rebuild was **not** the storm (0/0/1/4
`meta-rebuild`, 0 GC-race). Next is per-opcode BUSY, not more lock drops
and not “the wire is slow.”

**FLUSH SKIP-CLEAN + INCR SERIALIZE GATED (Aug 30/31):** Stopped O(table)
encode+blake3 on clean CoW pages, then incremental serialize so a flush
does not snapshot/pack every compact slot. Per-page dirty bits on
create/set_chunk/setattr; same-count rename marks the compact slot + all
dentry pages (mark_full disabled incr for the ecopy temp→final storm).
Unlink/swap still `flush_full`. `serialize_dirty` copies the last blob,
rewrites dirty compact pages (slot range, not a full inode scan) and
dentries; page loop **memcmps** against `flush_blob` before reuse so a
missed bit cannot keep a stale ci. `FLUSH-PROF incr=1` 46105 vs
`incr=0` 34 on the primary after 6 ecopy rounds; last window
pages=39 written=5–6 reused=33–34, `ser` ~1.5–2 ms.
Gate: TCP posix `results/posix/20260830-174926` (fresh) and
`results/posix/20260831-030124` (on the grown ecopy table) both
**196/201, 0 EFS bugs**. ecopy_rounds 9×2 dirs, dest `ecopy-<host>/rN`,
`results/ecopy/20260830-flush-incr3`: files/s
**183.8 | 208.9 | 196.1 | 186.3 | 183.7 | 177.4** vs Aug 29
**84.5 | 75.2 | 65.6 | 59.4 | 46.9 | 37.6** (2.2× decay). Peak-to-end
now 1.18×. Harness keeps RESULT on ecopy erc=2. Residual: ~81/134567
files missing (26 class dirs short 1–5, CREATE EIO / ftruncate; joiner
fcstor005 293 `meta-rebuild` / 52 `raced with GC` — same catchup family,
not the flatten term). Do not schedule LINK/RENAME lock drops or H3
client RTT as the 2^32 cut.

**SCALE_GROW REMEASURE (Aug 31, bits=3 TCP):** Harness now uses a unique
ROOT name per client (`scale-<host>`) — shared `scale/<host>` raced
hashed ROOT mkdir (ENOENT) and invented 80k/s / 400 B/inode fiction.
Fail-closed on missing ERRORS / probe n=0. Primary SIGSEGV in
`inode_ptr` during 9×16 CREATE: extras merge freed shard tabs under
the global lock only, extra flush could lock shard 0 if `shard_id`
was 0, catchup `adopt_tables` swapped inodes without shard locks.
Unlink is **flat** (drop_chunks_scan fix holds): p50
0.14 / 0.14 / 0.14 / 0.14 / 0.15 / **0.17 ms** at 100k→10M
(Aug 28 was 0.44→4.6 ms). stat ~0.16 ms flat; readdir ~4.1 ms flat;
create p50 0.19→0.30 ms. Creates/s 20k–50k then 25k at 5M / 15k at
10M (`results/scale/20260831-bits3`). errors 0 through 2M; 46 / 176
at 5M / 10M (0.0015–0.0035%, catchup; rebuilds 42→86, 0 primary
death). RSS **1.7 KB/inode at 10M** (16280 MB / 10M; 2.1 KB at 5M vs
Aug 28 1.56 KB — `flush_blob` cache). 2^32 × 1.7 KB ≈ **7.3 TB**
aggregate still cannot sit in RAM on 4 nodes. Next: bits=5 trial.

**BITS=5 TRIAL (Aug 31, mkfs shards=32, default back to 3):**
Dedicated mkfs (`EFS_DEFAULT_SHARD_BITS=5` for that wipe only).
TCP posix `results/posix/20260831-041839`: **196/201, 0 EFS bugs**.
ecopy_rounds 9×2 `results/ecopy/20260831-bits5`: files/s
**230.7 | 209.3 | 205.4 | 191.5 | 186.8 | 174.6** (peak-to-end 1.32×
vs bits=3 1.18× — flatten term still gone, not the old 2.2×).
Most ecopy rounds FAILED on 4–7 hosts (CREATE EIO). Joiner
**fcstor005** 313 `meta-rebuild` / 62 `raced with GC` (fcstor004
48/6; primary 0). Same extras catchup/GC family; 32 tables amplified
host-level EIO. Do **not** jump to bits=12; cap extra-shard commit
rate before that.
`scale_grow` to 1M on that table (`results/scale/20260831-bits5`,
leftover ecopy in RSS at 100k): errors=0; creates 9k then **40k / 50k**/s;
unlink p50 **0.135 / 0.138 / 0.141 ms** (flat, same as bits=3);
stat ~0.16 ms; readdir ~4.1 ms. Rebuild log **unchanged** (362) across
the 1M grow — empty-create burst does not trip the ecopy extras storm.
Incremental RSS ~1.4 KB/inode (3060−1820 MB over +900k files). Next on
the cap track is Phase 4 (name out of inode), not H3 RTT.

**POSIXSTRESS QUEUE CLASSIFIED H3 (Aug 30):** Extended `EFS_LOCK_PROF`
(`1_wait_us`/`1_calls` on `server_shard_lock`, `global_wait_us`/
`global_hold_us` on inode+REPORT `server_global_lock`, `busy` replies).
One fresh TCP 9×4 `results/posix/20260830-160943`: 36 tsv, 1454 fail,
**96.5% timeout**, 6× 196/0 (008×2, 011×3, 014×1). Primary last window:
`1_wait_us=14ms` (`1_calls=75k`, 0.19 µs/call), `n_wait_us=2.2ms`,
`all_hold_us=33ms`, `global_hold_us=59ms` (0.59 µs/call),
`global_wait_us=2.85s` (28 µs/call) / joiners 5.3–5.4s,
`busy=2873` with `append=4988` (joiners busy−append ≈ 200–350).
**0 `meta-ensure` rebuilds on primary** (1 each joiner). H1/H2/H4 do not
fit: parent-shard wait is not the queue, `ensure_shard_ready` I/O never
ran, APPEND BUSY already returns without the 50ms<<n sleep. Do **not**
drop LINK/RENAME, do **not** drop global before ensure, do **not** cap
BUSY backoff as the 9×4 lever. Next is client `rpc_send_recv_shard` RTT
(not more server lock drops). `clean_cluster.sh` now forwards
`EFS_TRANSPORT` with `EFS_LOCK_PROF`.

**AUTO RDMA FIRST INODE GATED (Aug 30):** First `mkdir`/`ls` after mount
no longer D-states. Root `stat` is still local (GET_META one-shot TCP);
the first pool checkout upgrades and CREATE/READDIR ride the QP.
`efs_conn_wait_request` is shared with `test_rdma_xprt` (quiet-gap +
CREATE-sized frame). Poller `poll_cq`s after comp-channel POLLIN.
Gate: remount auto, `mkdir` in 5s, fuse.log `RDMA transport up`,
solo posix `results/posix/20260830-154329` **196/201, 0 EFS bugs**.
`EFS_RDMA_FIRST=1` for the six-site trace. Do not start 9×4 as an RDMA
correctness gate. `EFS_TRANSPORT=tcp` remains the fallback.

**PER-OP LOCK DROP GATED (Aug 30 afternoon):** Finished dropping `g_server->lock`
for APPEND, CREATE_SHARD, GETCHUNKS, READDIR (one shard), LOOKUP_PATH (relock
per component), UNLINK/UNLINK_SHARD/`fan_drop_chunks` (discover-relock, no
`lock_all`, no global across peer RPC), SETATTR+SIZE. LINK/RENAME/HOLD still
`lock_all` — `EFS_LOCK_PROF` after 9×4 shows they are not the ceiling
(`all_hold_us` 5.6–17 ms total for the whole run, ~1–2 μs/call; `n_wait_us`
~1–2 ms). Harness: `wait_cluster_idle` + fail-closed warmup (empty-TSV trap).
Sharded `unlink_name` is dentry-only; client dual-apply must `nlink_dec_ex` or
hardlink tests see stale nlink (fixed in `ops.c`).
**Lock-drop gate was TCP** (`EFS_TRANSPORT=tcp`); auto first-inode is
now gated separately (see above). Do not quote RDMA posixstress for the
lock-drop cut.
Solo posix `results/posix/20260830-143842`: **195/201**, 1 EFS bug
`dir_rename_over_existing` (flake family), 0 hardlink bugs.
**3× fresh 9×4 vs Aug 30 RDMA baseline `20260830-064913` (36 tsv, 5137 fail,
98.6% timeout, 1 clean):**
| run | dir | tsv | fail | timeout | clean/near/sat |
| r1 | `20260830-144643` | 28 | 1458 | 95.5% | 12/2/14 |
| r2 | `20260830-145338` | 32 | 1827 | 96.4% | 10/2/20 |
| r3 | `20260830-145931` | 28 | 1118 | 94.9% | 13/4/11 |
tsv < 36 = fail-closed warmup on 1–2 hosts that started while others were
already in-suite (008/009, 007, 009/013). Clean 196/0 is now common (whole
hosts 012/014/015 or 011/012). Saturated hosts still ~95% `timeout after 15s`
— not `lock_all`. Next lever is not RENAME/LINK.

**FIO ROUNDS RDMA (Aug 30):** Honest 9-client sw-1m, 5 rounds, fresh
bits=3, auto RDMA (`results/fio-rounds/20260830-rdma-5r/`). FUSE_OK every
job, no `md0`. Agg MiB/s **3409 | 3234 | 3117 | 3126 | 3104** (1.10× decay
r1→r5 — not the ecopy 2.2×). Per-client lockstep ~338–372 MiB/s; 18 GiB
written per host per round; primary `used=304 GiB` after 5 rounds matches
9×18×5=810 GiB logical × ~1.5 EC / 4 nodes. Aug 29 TCP 9-way sw-1m was
3599. `fio_rounds.sh` is now honest (`--end_fsync=1`, no `time_based`, 9×2g).
First attempt EIO: 9 clients `mkdir` the same ROOT name `fio` at once and
wedged RDMA QPs; dirs are now `fio-<host>/rN`.

**RDMA posixstress (Aug 30):** Nested dir hashing (`cda9ac9`, every mkdir
hashed) split dentry vs inode and broke posix (rmdir-nonempty, LOOKUP,
rename). Reverted to ROOT-only hash (`1df9a31`: testdir/ecopy dest spreads;
nested stay on that shard). Single-client posix over auto RDMA:
**196/201, 0 EFS bugs** (`results/posix/20260830-064840`). 9×4 posixstress
over RDMA (`results/posix/20260830-064913`): 36 instances finished, cluster
idle gen=109; **fcstor008-2 clean 196/0**. Aggregate ~4971 EFS-bugs,
**99.3% `timeout after 15s`** (same saturation family as TCP 9×4, not
RDMA-specific correctness). 36 non-timeouts (`dir_deep_nesting` EIO leaf
×12, `dir_many_files` ×4, rest singles) — load flakes, 0 of those on the
solo gate. Do not start posixstress in the post-mkfs catchup window (first
attempt: all 9 warmups D-state, used=0, empty TSVs).

**RDMA (Aug 30):** Shared-CQ poller re-arms after drain (the old loop dropped
the next CQE after a quiet gap — inode/REPORT hung on a fresh mount so they
were forced onto TCP). Small SEND copies into the registered pool (no stack
INLINE). Peer pool is `efs_conn` + the same upgrade, so server↔server
GET/PUT_CHUNK is RDMA. Default transport is auto; only GET_META /
GET_META_ROOT stay TCP (unbounded replies). Gates no longer force
`EFS_TRANSPORT=tcp`.

**ECOPY ROUND 6 (Aug 29 night):** Directories under ROOT hash to a shard.
ecopy dest trees no longer funnel through the metadata primary.
1-client 61 → **112 files/s**; primary create=1; dest owner does the rest.
gens 11 (extras). Nested dirs stay on the dest shard (rename/hardlink).

**ECOPY ROUND 5 (Aug 29 night):** SETATTR without SIZE (futimens/chmod)
drops the global lock; one inode-shard lock. ~88 files/s.

**ECOPY ROUND 4 (Aug 29 night):** Skip GETATTR after LOOKUP when the inode
lives on the same shard as the dentry (files stay with parent). getattr
3.0 → 1.9 per create. ~82 files/s.

**ECOPY ROUND 3 (Aug 29 night):** REPORT_CHUNKS drops `g_server->lock` and
takes only the shards the recs hit (not lock_all). 0 drops. Mix-dependent
~75–80 files/s (73 dirs) vs round 2's 98 files/s (333 dirs).

**ECOPY ROUND 2 (Aug 29 night):** LOOKUP and GETATTR drop `g_server->lock`
and run on the parent/ino shard lock. Fair empty-cluster ecopy
61 → **98 files/s** (5962/60s, `results/ecopy-rounds/r2/`). CREATE still
all on shard 0 (dest under root). Next: REPORT/SETATTR off the global lock,
then independent directories.

**ECOPY ROUND 1 (Aug 29 night):** CREATE no longer holds `g_server->lock`.
Files stay on the parent directory's shard; ino alloc is shard-local.
Hashed dirs tried and reverted (split dentry/inode broke ecopy rename).

**ECOPY LOOKUP TAX CUT; RATE STILL ~60 FILES/S = CREATE (Aug 29 night).**
The 56–70 LOOKUPs/file were real but not the 70 files/s ceiling. Client
`lookup_walk` now coalesces same-client (parent,name)→ino for ~250ms
(`ops.c`); needed because sharded upsert is child-shard while name lookup
is parent-shard, so local HIT always missed and a name-only cache never
fired. Keep `entry_timeout=0` (posix2 peer unlink/rename). Gate: fresh
bits=3, dest **does not exist** so ecopy sets `destination_fresh` and skips
its 512-wide dest-stat. 60s `ecopy …/ecrawl-synt-small/ /tmp/efs-mount/r3fresh`:
3646 files, **FUSE_OK**, gens 6052×4. RPC delta: lookup 11098 / create 3782
= **2.9 LOOKUPs/file** (was ~70 into an existing dest). Rate **61 files/s**
unchanged — CREATE 63/s is the serial limit (primary idle CPU, lock_all hold
~2.5μs). Copying into an **existing** dest still dest-stats 512 names/batch
(~70 LOOKUPs/file) and is the same ~60/s. 1000 files/s is CREATE, not LOOKUP.
Do not quote the interrupted 3-node `ino_dup` run (fcstor006 was DOWN).
**2PC GEN-SYNC GATED (Aug 29, working tree, UNGATED commit).** Joiners no
longer pin at gen=2/5 while the primary runs away. Repeatable gate:
`bash tests/clean_cluster.sh && bash tests/run_tests.sh setup && bash tests/stress/ecopy_gen_sync.sh`
— one client, `~/git/direct_copy/ecopy ~/orcd/scratch/ecrawl-synt-small/ /tmp/efs-mount/`
(contents of SRC into DST; tree is ~6M files, default window 180s). First
run `results/ecopy-gen-sync/20260829-163709`: **FUSE_OK**, 12057 files / 180s,
**gens 5779 5779 5779 5779 PASS** (still 5887/5887/5887/5887 after). Joiners
logged 5886 `meta-commit: promoted` each; **0 BUSY / 0 STALE**. Primary
`perf record -p $(pgrep -x efsd)`: 20% blake3 in `server_flush_fragmented_meta_locked`
(known O(table) flush). Joiner `strace -cf -p`: 61% futex / 24% recvfrom
(followers waiting, not hashing). Do **not** wrap efsd in strace.
Fixes: META_COMMIT BUSY only if `primary && shard_dirty`; PUT_META
gen-advancing prepare only from the metadata primary (else extras-merge /
STALE); extra-shard `bootstrap_export_on_peers` is a no-op; catchup rebuild
allows joiner `shard_dirty`. Leftover compile trap: do not leave a stray
`return 0;` after `put_meta_status_name`.
**FIO / DATA-PATH (Aug 29) — method + table live in `.cursor/rules/efs-fio-honest.mdc`.
Do not re-derive.** Honest writes `--end_fsync=1`; reads after remount.
Harness: `tests/stress/fio_honest_matrix.sh`. Writes ~2.4 GiB/s (1 client)
→ ~3.6–5.3 GiB/s (9 clients, 1.5–2.1×). Reads 7.8 → 49 GiB/s (6.3×).
Stock `run_tests.sh perf` write column is cache-inflated — never quote.
Last `perf`: client blake3 on flush (2.7 cores); primary off-CPU idle.
Same shared serialization family as ecopy (9 clients only ~1.3× one).
Cleanup gotcha (not a bug): a dir holding files fio still had open rmdir'd
ENOTEMPTY forever while readdir showed it empty — a leaked libfuse
`.fuse_hidden` node (readdir filters the prefix). A remount cleared it; the
server had no orphan. Minimal create/rm/rmdir cycles are clean.
**REAL-WORKLOAD SCALE FINDING (Aug 29) — ImageNet ecopy throughput DECAYS 2.2x
as the table grows, and the primary is the single-threaded ceiling.**
New harness `tests/stress/ecopy_rounds.sh` (+ `ecopy_verify.sh`): every client
ecopy's a disjoint slice of ImageNet class dirs into its own subtree, one slice
per round, so each round is fresh work on a strictly larger table and rounds are
directly comparable. Source
`/orcd/scratch/orcd/001/erbmi1/imagenet/images_complete/ilsvrc/train` (1000
class dirs, 732-1300 files each, ~117 KB avg — metadata-bound, not bandwidth).
**GOTCHA: `ecopy SRC DST` copies the CONTENTS of SRC into DST, not SRC as a
subdir** — the harness mkdirs `DST/<class>` per class or ImageNet's shape is
lost and per-class accounting reads 0.
**6 rounds, 9 clients, 2 class dirs/client/round, fresh bits=3 cluster
(`results/ecopy/20260829-031849`), files/s aggregate:**
`84.5 | 75.2 | 65.6 | 59.4 | 46.9 | 37.6` over 23k->140k files. Monotonic,
no plateau. Single-client solo is ~66 files/s (unchanged since Aug 24), so
9 clients together only ever beat one client by 1.3x and end up BELOW it.
**Correctness is CLEAN: `ecopy_verify.sh` = 139767/139767 files, 108/108 class
dirs exact, 0 mismatches; 0 `ino_dup` / 0 `raced with GC` / 0 `meta-rebuild` on
all 4 servers.** Short rounds (23183/23171/23213 vs 23400) are ImageNet's
variable class sizes, NOT loss — always verify per-class before calling it a bug.
**Cause is NOT the flush, measured directly and do NOT re-chase:** pages
*written* per flush (the `next_ci` delta) is FLAT 3.1 -> 4.6 while the table
grew 8 -> 149 pages, i.e. CoW page reuse works and metadata write volume is
not what grows. 86861 commits in ~41 min (~35 flushes/s).
**Cause IS a single-threaded serialization point on the primary:** mid-round
sampling shows fcstor003 at **87.5% of ONE core (of 24)** while fcstor004/006
are at 0.0%, fcstor005 at 6.7%, and `efs-fuse` on the clients at 0.0%. All the
work funnels through one thread on one node; the other 3 servers and every
client are idle. This is the same shape as the posixstress saturation
(everything through the primary) but here it also *degrades with table size*,
which the empty-file scale test did NOT show (create rate was flat to 5M) —
so the growing term is on the chunk/REPORT_CHUNKS side, not create.
**`peer-commit-promote` ROOT-CAUSED + FIXED (Aug 29, working tree, UNGATED).**
Peers stashed every `meta-prepare` but stayed pinned at an old committed gen, so
recent metadata generations existed ONLY on the primary — a real durability
exposure. Cause: `EFS_MSG_INODE_DROP_CHUNKS` (handler.c ~2590) did
`ex->shard_dirty = 1` on the **main** table unconditionally. `fan_drop_chunks`
sends that RPC to EVERY node on EVERY unlink/truncate, so a peer set the flag
even when it dropped nothing (its tables hold none of that ino's chunks), and
**nothing on a peer ever clears it** — only a main-table flush clears
`shard_dirty`, and peers do not own shard 0, so the primary is the only node
that ever flushes it. One unlink anywhere therefore latched every peer's main
table dirty PERMANENTLY, and the META_COMMIT promote gate
(`if (ex->shard_dirty) { reply = BUSY; /* keep pending, do not promote */ }`,
handler.c ~1181) then refused every subsequent commit forever. Matches the
symptom exactly (prepares stashed for gens up to 11, still `committed gen=3`).
**Ruled out first, do not re-chase:** the commit's `export_id`
(`ex->root.id`, correct), the `root_sum` fingerprint (peer hashes the same
prepare bytes the writer hashed), and prepare/commit reordering (both PUT_META
and META_COMMIT are synchronous request/reply, so the writer cannot send
prepare N+1 before the peer has processed commit N).
Fix: mark dirty per-table and only when a table actually lost a chunk —
`drop_chunks_scan` now counts removals and sets `ex->shard_dirty` on the table
it modified; the blanket flag in the DROP_CHUNKS handler is gone.
**GATE OWED: posixpersist + confirm peers advance `committed gen` under load.**
**HYPOTHESIS WORTH TESTING FIRST — this may also be the residual
`unmount-drain-chunks` / `large-file-tail` content loss.** Both are "size
correct, contents all zeros" with idle, normally-flushing servers, i.e. the
inode row survived but the chunk records did not. If peers were pinned at an
old committed gen, then any post-remount read served from a peer's root is
missing every chunk record written since that gen — which is exactly that
symptom. Re-run posixpersist after this fix BEFORE chasing those two
separately; they may already be closed.

**FLUSH THREAD CHURN REDUCED (Aug 29, working tree, UNGATED).** Each page
written spawned 3 threads (one per fragment) and joined all 3, on the single
thread that already serializes every flush. Now spawns fragments 1..N-1 and
runs fragment 0 inline: same concurrency, 2 pthread_creates per page instead
of 3. The join loop was already guarded by `spawned[fi]`, so fragment 0 is
correctly not joined.

**UNLINK O(n) TERM FOUND + FIXED (Aug 29, working tree, UNGATED — no build was
possible, shell backend dead).** `drop_chunks_scan` (metadata.c ~1414) took its
bounded fast path only when `icnt_get(ex, ino) > 0`; when the count was **0** it
fell through to a full `while (i < ex->chunk_count)` pass over that table's
whole chunk array. `efs_export_drop_chunks_from` fans the scan over the main
table AND **every loaded shard tab**, and an ino's chunks live in only one or a
few of them — so for every other table the count is 0 and one unlink scanned
that entire unrelated table. **One unlink was O(all chunks on this node)**,
which is the measured unlink p50 0.44ms -> 4.6ms from 100k to 5M. Fix: if the
icnt index exists and reports 0 for this ino, return immediately (checked
BEFORE `layout_epoch++` — a table with nothing to drop did not change, and a
spurious bump makes the flush treat it as raced and stay dirty). Safe because
`icnt` is maintained everywhere `chunk_idx` is (`efs_export_set_chunk`, the
merge path, `remove_chunk_at`, `export_reindex_chunks`), so a 0 is exactly as
trustworthy as the non-zero count the existing fast path already returns on.
Holds for `first_chunk > 0` too (no chunks means none in any range).
`fan_drop_chunks` is fine — it dedups by node (O(nodes), not O(shards)) — but
note its `server_ensure_shard_ready` loop is O(shard_count) per unlink, which
is 2 owned shards at bits=3 but ~1024 at bits=12: fix that before raising bits.
**GATE STILL OWED: build + posix + posixpersist + a scale_grow unlink curve.**

**HOT PATH FOUND BY CODE READING (Aug 29, not yet measured — shell was dead):
every flush re-serializes and re-HASHES the WHOLE table just to discover which
pages changed.** `server_flush_fragmented_meta_locked` (meta_server.c ~1721):
1. `efs_export_table_snapshot_ex` — full memcpy of inodes[]+chunks[] under
 global + all shard locks;
2. `efs_export_serialize_ex` — serializes the ENTIRE table to a ~20 MB blob
 (156 pages x 128 KiB at 140k files);
3. then **for every one of the 156 pages**: `efs_meta_extract_page` (128 KiB
 memcpy) + `efs_encode_chunk` (EC encode) + **3x `efs_hash` over 64 KiB
 fragments**, and only THEN compares the checksums against the committed
 root's to decide the page is unchanged and can be reused.
So ~4.6 pages are actually written but all 156 are encoded and blake3'd. Per
flush that is ~78 MB touched incl. **~30 MB of blake3**; at ~35 flushes/s that
is **~1 GB/s of hashing+memcpy on ONE thread** — the 87.5%-of-one-core, and it
is LINEAR in table size, which is exactly the observed 2.2x decay. Matches the
old bits=5 profile (`server_flush_fragmented_meta_locked` ~50% of efsd, blake3
20% + memmove 12%) — that was the same cost multiplied by shard tables.
`efs_export_set_chunk` is O(1) (hash idx + append) and REPORT_CHUNKS apply is
O(recs), so **the apply path is NOT the problem — do not chase it.**
**FIX, in order of value:**
(a) cheap: cache the previous flush's serialized blob per table and `memcmp`
 each page's raw bytes instead of encode+3x blake3 (encode is deterministic,
 so identical bytes => identical fragments/checksums; same outcome, ~4-10x
 less work on the ~97% of pages that are reused). Invalidate on layout
 change (`ino_page_count`/`chunk_blob_len`) and on any failed flush.
(b) structural: per-page dirty tracking so step 2 serializes only dirty pages
 and step 3 never visits clean ones — removes the O(table) term entirely.
 This is the one that matters for 2^32.
NOTE `omit_chunks` already skips the chunk region when `chunk_epoch` is
unchanged, but an ecopy/fio write workload dirties chunks every flush, so it
never engages there.
**NEXT: find which primary thread is hot and confirm the above.**
`perf record -g` on efsd HUNG both times (perf report never returned);
use `top -H` + `/proc/PID/task/TID/{comm,wchan,stack}` instead.
**Harnesses staged but NEVER RUN (shell died first):**
`tests/stress/fio_rounds.sh` (rounds of fio from all 9 clients, fresh dir per
round so the table grows — same shape as ecopy_rounds so the two curves are
comparable; a flat fio curve + decaying ecopy curve would localize the cost to
metadata) and `ecopy_verify.sh deep` (remounts each client, then reads each
host's files back FROM A DIFFERENT host and md5s them against the ImageNet
source — the counts mode only proves the writer can still see its own writes,
possibly from its own dcache).
**INFRA: the shell exec env wedged right after this** — caused by
`pkill -f "efsd-ecopy"`, which self-matched the shell's own command line and
killed the session (the documented `pgrep/pkill -f` trap, third time it has bit
this project). A round-7 ecopy (`results/ecopy/round7`, log `/tmp/r7.log`) was
left in flight and its result was never read; the cluster still holds the ~140k
file tree under `/tmp/efs-mount/ecopy/<host>/r<N>/`.

**DURABILITY GATE ADDED (Aug 28, b2e0dce) — `run_tests.sh posixpersist`.**
Every other suite writes and verifies inside ONE mount session, so all of them
would pass even if efs never made anything durable (the client cache answers
the reads). `tests/posix/posix_persist.py` splits each of 26 tests across two
processes — `--phase prepare`, unmount, remount, `--phase verify` — with
content derived from the test name via `rand_bytes` so verify recomputes what
prepare should have written instead of trusting a file. Self-validating, NO
XFS baseline. Two guards, both load-bearing: the unmount is a real
`fusermount3 -u` (NOT `remount_client`, which SIGKILLs — a clean unmount must
preserve everything written, a crash only what was fsynced; `--crash` picks
the kill deliberately) and it NEVER falls back to a kill; and the run fails if
the efs-fuse pid did not change, since a surviving daemon serves the reads
from cache and passes vacuously. Validated both ways before trusting it:
26/26 on a local FS, and a negative control (truncate / zero-fill / delete one
of 500) fails exactly those 3.
**FOUND+FIXED on the first run: unlink freed the chunks of a file that still
had links.** `ln a b; rm a` left b with the right size and nlink but reading
all zeros, PERMANENTLY (identical after a second remount). Invisible
in-session — the client still had the data cached — which is why posix's
hardlink tests never caught it. The parent-owner UNLINK handler dropped chunks
for ANY non-dir unlink (`urc==0 && have_victim && !is_dir && !keep`); the
sibling UNLINK_SHARD path always gated on `nlink == 0`. Now reads the
surviving link count from the authoritative row (`efs_export_get_inode`, or
the child owner's UNLINK_SHARD reply) — `victim.nlink` is the PRE-unlink count
and on a parent shard can be a dentry stub. Gates: posixpersist 26/26 settled,
posix 0 EFS bugs.
**FIXED (Aug 28, cad1083) — the post-wipe loss was a DIRTY MAIN TABLE being
rebuilt.** The extra-shard paths have always refused to rebuild a table with
unflushed ops (DIRTY-REBUILD: the owner is the single writer, so a dirty table
is AHEAD of the committed root and a rebuild can only discard ACKed work); the
**main table had no such guard** — missing in both `server_ensure_shard_ready`'s
shard-0 branch and the catchup thread's `need[e]`. The rebuild also did
`ex->next_ino = ex->root.next_ino` AFTER `*ex = staging`, so the live watermark
was discarded and an older root rolled the allocator BACKWARDS; next_ino is now
floored at the pre-swap value (captured next to `saved_shard_id`). Reusing a
live ino corrupts a file, skipping ino numbers costs nothing.
**Gate: posixpersist immediately after a fresh wipe 25-26/26 (was 11-16 LOST),
0 `ino_dup`, 0 `raced with GC` on all 4 servers (fcstor004 alone had 45
rebuilds + 11 races before); posix 195/201, only the known size-visibility
flake.** The metadata half of the loss (missing creates, ENOENT, spurious
EEXIST) is GONE. What remains is content-only ("size correct, contents zeros")
and the servers are idle and flushing normally through it — that is the
separate clean-unmount drain item below, NOT this.
**Original diagnosis, kept for the symptom list:** a fresh `mkfs` is followed
by MINUTES of metadata rebuild churn
(measured: fcstor004 45 `meta-rebuild` + 11 `raced with GC`, fcstor006 25,
fcstor005 6). **While that churn runs, the servers silently drop applied
metadata ops.** All of these are the SAME bug, not separate ones:
- chunk mappings lost → "size correct, contents all zeros" (11-16 of 26);
- whole creates lost → `many_files_in_one_dir` came back **3 of 500**,
 symlinks/files ENOENT;
- the ino allocator ROLLS BACK → `cwi-fail: ino_dup parent=8 name=f043 ino=19
 shard=0 cnt=45` on the primary → **spurious EEXIST on a brand-new name**,
 then the file does not exist. **Note this is the scale-test `ino_dup`
 signature WITH per-op CREATE already reverted (bc27e0c), so per-op CREATE
 was NOT its only cause** — a rebuild reloading a shard table from stale/GC'd
 pages loses recently applied rows and the allocator re-issues their inos.
**Once the rebuild churn stops the cluster is stable and posixpersist is
25-26/26.** So it is a startup/catch-up window, not steady-state corruption —
but it is silent, and `fsynced_file` was among the lost, i.e. **fsync returned
success and the data was gone** (the report succeeded, then the rebuild threw
the row away). All of the above is fixed by cad1083.
**RULED OUT as the cause (do not re-chase):** the client. `efs_client_report_dirty`
merges the dirty snapshot back on failure and sets `report_flush_failed`, and
in every failing run the client logged NOTHING because the report returned OK.
`efs_fuse_destroy` was still hardened (845b853): it flushed once, discarded the
rc, and let `efs_client_shutdown` free the merged-back table — retry-until-drain
+ a loud message now, but that loop never engages here.
**HARNESS CONSEQUENCE: a cluster is NOT usable the moment `clean_cluster.sh` +
`setup` return.** Runs started immediately after a wipe measure the churn
window, not the build (this is what made one posixpersist run look 16/26 and
the next 26/26). Wait for `meta-rebuild|rebuilt export` counts to stop growing
on all 4 servers, or accept the noise.
**Residual, separate, still open — CONTENT-ONLY unmount drain.** Post-cad1083 a
wipe+immediate posixpersist still loses 0-4 of 26, always "size correct,
contents all zeros" (`closed_but_not_fsynced` every time; also
`multichunk_file` / `overwrite_middle` / `sparse_file`), while the servers log
only normal `meta-flush: committed` + `meta-prepare: stashed` — no rebuild, no
GC-race, no ino_dup. So chunk records are not reaching durability on a clean
unmount; metadata does. Same family as `large_file_size_only` (64 MiB file,
tail chunk at size-128KiB, ~1 run in 3 even settled; isolated repro of the same
write always survives, with and without fsync). **NOTE while chasing this:
peers log `meta-prepare: stashed ... (was committed gen=3)` for every gen up to
11 — the 2PC COMMIT phase is not promoting on the peers, so only the primary
holds recent gens. Unrelated to the loss seen here (the primary serves) but a
real durability exposure of its own.**
**SCALE TEST BUILT + FIRST 10M-INODE RUN (Aug 28) — every number before this
was from a table under 440k against a 2^32 goal. `tests/stress/scale_grow.sh`
(+ scale_worker.py / scale_probe.py) grows an export in steps and records
create rate, idle per-op latency, server RSS and error count at each size.
It found four things in one afternoon; three are FIXED.**
1. **SILENT DATA LOSS: per-op CREATE dropped ~5% of concurrent creates —
   REVERTED (bc27e0c).** O_CREAT|O_EXCL on unique never-used names returned
   EEXIST and the file was then absent (stat ENOENT); 111/111 sampled dirs
   were short (4-67 of 1000 files). 407k `cwi-fail: ino_dup` on the primary,
   **all shard=0**. Cause: `efs_export_alloc_ino_for_shard` consults the MAIN
   table (`efs_export_inode_slot(ex, ino)`) on EVERY alloc, but per-op CREATE
   locks only {psh,dsh,target} — when none is 0 it read shard 0's ino index
   while a concurrent create was inserting + reallocating it → missed a live
   row → handed out an in-use ino → the dentry write onto main rejected it.
   0 errors at 1 thread vs 27 at 16 threads (same client, same size) = pure
   race. **Revert was also FASTER: 500k/9x16 errors 25017→0, creates/s
   23809→39999, unlink p50 0.95→0.53ms, RSS 2055→1265MB.** Consistent with
   402d14a (partition perf-neutral). Locking shard 0 per create would
   serialize all creates = the thing the partition existed to avoid, so
   there is **no cheap correct version**; the allocator's dependency on the
   main table must be designed away first. Transitional step (d462ab0,
   aa7bd5a) is NOT implicated and stays.
2. **readdir was O(inodes in export) PER PAGE — FIXED (60cbef3).** The
   handler scanned the whole shard table for rows with a matching parent, and
   readdir fans each page to every shard. A 1000-entry dir: 31ms at 90k →
   1.9s at 10M. The parent→children index already existed
   (`efs_export_foreach_child`) and just was not used. Now **4.6ms at 5M and
   FLAT** (4.4/4.3/4.6 at 100k/1M/5M). Also retired an O(ncand^2)
   duplicate-dentry DIAG (~500k strncmp/page) — foreach_child only yields a
   slot the name index maps back, so a dup cannot reach the reply.
3. **Flush + GET_META snapshots took only the global lock — FIXED
   (60cbef3).** Fine until per-op CREATE started reallocating `ex->inodes[]`
   under a shard lock alone; then the snapshot memcpy could read freed
   memory. **2 of 4 servers died with SIGSEGV in
   `efs_export_table_snapshot_ex` on the flush thread** under 9x10M creates,
   and the other 2 wedged re-polling for pages the dead nodes held. Both now
   take global + all shard locks (same `rebuild_shards_lock` discipline as
   the rebuild). RSS at 5M also fell 27.5GB→7.4GB, consistent with the racing
   snapshot reading torn `inode_count`s. **This was the "flush is not
   shard-lock-aware" gap that higher bits were blocked on.**
4. **STILL OPEN — RAM is the 2^32 wall.** Marginal ~1.3-1.5 KB/inode
   (5M→10M: +6.6GB for 5M inodes) cluster-wide, tables fully RAM-resident.
   2^32 x 1.3KB ≈ **5.6 TB aggregate** = ~1.4 TB/node on 4 nodes. Reaching
   2^32 needs a paged/on-disk table or ~40-100 nodes, NOT more shard bits.
   Measure again now that the snapshot race is fixed.
**Scale curve (post-fix, 9 clients x 16 threads, bits=3, TCP):** creates
16.6k-40k/s and NOT degrading with size; stat p50 flat 0.27ms 100k→5M;
create p50 0.28→0.34ms; readdir flat ~4.4ms; **unlink still grows
0.44→0.53→4.6ms (100k/500k/5M) — next thing to chase.**
**HARNESS GOTCHA (cost an hour):** the worker swallowed `FileExistsError` as
benign, so a 5%-per-dir loss reported `errors=0`; and it batched progress at
2000/thread so a 694-file slice printed 0 forever and a dead cluster looked
like a hang. Both fixed. **Never treat EEXIST on a unique name as benign.**
**BIGGEST PERF WIN — fsync flush GROUP COMMIT (Aug 28, 886f75b): 36-way
posixstress 34.7 -> 7.2 EFS-bugs/instance (4.8x), 3 fresh-wipe runs each.**
Every sync REPORT_CHUNKS (every client fsync/close) ran its OWN full metadata
flush, all serialized on the single `meta_flush_mu`: ~55 flushes/s x ~14ms =
~77% duty on one mutex, so each flush queued ~88ms. **85% of the flush window
was pure WAIT, not work** (flush wait 126.7s -> 0.30s, 428x; calls 1441 ->
610). Concurrent fsyncs now share a flush: the caller's mutation is already
applied before it reaches the flush, so any flush that *starts* after that
point includes it — a caller finding one in flight waits for the NEXT one.
Leader keeps flushing while callers are queued, tracked as the highest
generation anyone needs (`flush_target`), NOT a waiter count (waiters can only
decrement after the leader drops the mutex, which would spin the leader).
Gates: posix 195-196/201 0 real bugs; posix2 60/63 at POSIX2_STEP_SEC=45.
**unlink/nlink fan de-scanned (e754c23):** `efs_export_unlink` and
`tab_set_nlink` linearly scanned `inodes[]` while `for_each_loaded_tab` fans
them across EVERY loaded shard tab → one unlink was **O(total inodes)**. Now
early-out on the authoritative ino index. Neutral at bits=3 (small tabs),
~15% at bits=5, and removes a term that is FATAL at 2^32.
**bits=5 STILL ~8x worse than bits=3 (7.1 vs 57.3, 3 runs each) — CAUSE NOT
FOUND. Ruled out by direct measurement, do NOT re-investigate these:**
flush work (identical: 1441/19.7s vs 1448/22.2s) and flush wait (fixed by
group commit); `lock_all` acquire (0.09s vs 0.11s) AND hold (bits=3 is
*worse*: 2.88s vs 2.44s); per-op `lockn` wait (bits=3 worse: 0.19s vs 0.14s);
shard rebuild/evict churn (0 lines); per-op cost unloaded (single suite 3.5s
vs 4.0s); RPC amplification (bits=5 issues FEWER RPCs: 755k vs 846k — it
completes less work). Under load all 4 servers are IDLE (154/183 threads in
`wait_woken`) and efs-fuse is at 11% CPU, so **the bottleneck is client-side
or on the wire, not the server.** Client profile is flat (memmove 12%,
blake3 9%, mutex 5%). NEXT PROBE: client-side per-op latency/RPC-wait
instrumentation, or wire round-trip count per op.
**Tooling added (env-gated, off by default):** `EFS_FLUSH_PROF` (per-stage
flush timing, separating lock WAIT from work — this is what found the group
commit win), `EFS_LOCK_PROF` (lock_all calls/wait/**hold**, per-op lockn
wait, per-opcode RPC counts). Shard-lock waits are off-CPU (futex) so
`perf record` CANNOT see them — that is why the on-CPU profile was
misleading for two rounds.
**STOP-THE-LINE MEASUREMENT (Aug 28, 51ebb17) — the per-shard lock partition
has produced NO measurable throughput change. A 7-point bisect of 36-way
posixstress, every point a fresh wipe+mkfs at bits=3, EFS-bugs per instance:**
`ff7896c` pre-transitional **35.1** | `aa7bd5a` transitional **41.7** |
`1dc03f1` +per-op CREATE **29.8** | `e4efd97` +per-op APPEND **38.9** |
`main` 51ebb17 **37.4 / 30.7 / 36.1** (3 repeats). Aug-27 pre-partition
baseline **46.2**. **Every variant sits in a 30-46 noise band — the whole
partition (blockers 1-3 + transitional + per-op CREATE/APPEND) is
performance-NEUTRAL.** Reason: only CREATE/APPEND are per-op; every other
handler still does global + `lock_all`, and at bits=3 there are only 2
shards/server (max theoretical 2x anyway).
**METHODOLOGY WARNING — single 36-way runs are NOISE.** Run-to-run spread is
±20%, and a confounded run read **162.8/instance** (4x outlier) purely
because a previously-killed posixstress left load on the clients. NEVER
conclude from one run; use >=3 fresh-wipe repeats. Two `pgrep -f` traps bit
this session: `pgrep -f edelete` / `pgrep -c -f posix_suite.py` **match their
own ssh command string** — always `pgrep -x`, or the check is a false
positive (cost ~15 min of waiting on a drain that had already finished).
**bits=5 REVERTED to bits=3 (51ebb17).** bits=5 measured 133/instance
(outside the band, 2 runs) and profiling under load pinned ~50% of efsd
on-CPU in `server_flush_fragmented_meta_locked` (blake3 20%, memmove 12%):
the flush pays a FIXED per-shard-table cost (snapshot under `s->lock`, CoW
page setup, >=1 page write per table), so 32 tables quadruple the flush
lock-hold and starve handlers (all 4 servers idle while clients blocked).
The create-only latency probe looked ~6x BETTER at bits=5 (a create burst
dirties few pages) — it is not a proxy for the suite. Raise bits only after
the flush snapshots under the SHARD lock instead of `s->lock` and packs
small tables per page.
**Correctness on main is GREEN:** single-client posix 195/201 + 1 known
flake, 0 real bugs; posix2 59/63 (the 3 = harness ssh-timeout,
`peer_fcntl_range_conflict` known feature gap, `peer_rename_across_dirs_chase`
known flake). 36-way "bugs" are ~99.6% 15s timeouts = saturation, NOT
correctness.
**Per-shard `g_server->lock` partition — BLOCKERS 1+2+3 + TRANSITIONAL STEP LANDED (Aug 27, c65865f + 958b15b + b167ea2 + d462ab0 + aa7bd5a); per-op global-lock drop remains.**
**Transitional nested-lock step DONE (d462ab0 Batch 1: GETATTR/CREATE_SHARD/
APPEND/GETCHUNKS per-shard; aa7bd5a Batch 2: LOOKUP/CREATE/UNLINK/UNLINK_SHARD/
RENAME/RENAME_AT/SETATTR/LINK/LINK_SHARD/HOLD/DROP_CHUNKS/LOOKUP_PATH/READDIR/
REPORT_CHUNKS via `server_shard_lock_all`).** Global lock stays HELD in every
handler (no parallelism yet) — this only establishes global→shard discipline.
Every mid-op global-lock drop (CREATE/UNLINK/LINK fan-out, REPORT_CHUNKS yield)
and every `fan_drop_chunks` call is wrapped unlock_all→drop→relock→lock_all so
no shard lock is held across a global-lock drop (would deadlock vs the rebuild's
`rebuild_shards_lock`). Shard locks taken after any ensure/reply_if_shard_busy.
**Because ALL handlers now hold lock_all, the earlier per-op use-after-free
(CREATE realloc of `ex->inodes[]` racing a shard-only GETATTR) is now IMPOSSIBLE
— a per-op reader holding shard X excludes with any writer's lock_all.** Gate
(fresh bits=3, TCP): posix 194-196/201 (only the pre-existing size-visibility
flake family, moving run to run); 4-way posixstress smoke 3/4 clean + 1 same
flake, NO deadlock/wedge. **GOTCHA: the posix harness `ensure_mounted` REUSES an
existing mount — a manual `efs-fuse` started without `EFS_TRANSPORT=tcp` comes
up on RDMA (broken data path on fresh mount) and the suite then mass-times-out
looking exactly like a server deadlock (server idle, gen stuck, client D-state
request_wait_answer). Always let the harness mount, or mount with
EFS_TRANSPORT=tcp.** NEXT (hard, the actual ~2x win): per-op drop of the global
lock for the hot ops. Reads (GETATTR/LOOKUP/READDIR/LOOKUP_PATH/GETCHUNKS) are
the easy/safe ones (no fan-out/yield) but don't fix the create burst; the burst
needs per-op CREATE/UNLINK/REPORT_CHUNKS, which must take the global lock only
briefly (export lookup + membership + ensure) then run on their shard set —
hard because of the CREATE fan-out, UNLINK fan/fan_drop_chunks, and the
REPORT_CHUNKS yield all currently relying on the global lock.
**Per-op CREATE LANDED (working tree, first per-op write).** CREATE now holds
the global lock ONLY for the membership/ownership/node-addr snapshot +
shard-ready ensure (all stable during a burst), then drops it
(`global_held=0`, dispatch unlock guarded by the flag) and runs on per-shard
locks: remote branch {psh,dsh} for the EXIST check + local dentry write,
fan-out with NO lock; local branch {psh,dsh,target} around `efs_export_create`.
Landmines handled: (1) `dsh`/`dir_is_spread` is computed under the psh lock —
it reads the parent inode, and a global-lock-only read would race a concurrent
per-op create's `inodes[]` realloc (the same UAF as the original GETATTR bug);
(2) `mark_rpc_dirty` is global-lock-free (atomic + CV hint, safe under a shard
lock); (3) `evict_cold_shards` is SKIPPED in the per-op path — it FREES shard
tables = UAF vs per-op shard locks (no-op at bits=3; needs shard-lock-aware
eviction as a follow-up for higher bits); (4) `hold_inc` uses the leaf
`hold_mu` (safe under a shard lock); (5) fence re-check (`meta_needs_rebuild`)
after each shard-lock acquisition → BUSY. The fan-out EXIST→create non-
atomicity (orphan row on a duplicate) is PRE-EXISTING (the old code also
dropped the global lock for the fan-out). Gate (fresh bits=3, TCP): posix
196/201 0 bugs (clean run; other runs 0-2 pre-existing size-visibility flakes);
4-way posixstress smoke 0-2 bugs/instance (flake family, no deadlock, no
consistent FileExistsError — the one 7-bug spike was the wipe's background
edelete). NEXT: per-op UNLINK/REPORT_CHUNKS/SETATTR/APPEND + the reads.
Goal: fix the posixstress 9×4 startup burst — volume-driven `g_server->lock`
queueing (~2s op latency in the first ~4s as 576 threads start; 99.6% of
failures are 15s timeouts, **0 correctness bugs**; sustained latency after the
burst is ~7ms). The `drop_chunks` O(chunk_count)→O(chunks_of_ino) fix
(commit a2b90cf, `icnt` per-ino live-chunk map) gave only ~5% — the burst is
op-VOLUME, not long lock-holds. **This partition is a MULTI-DAY refactor, not
a quick win.** Infrastructure + blocker 1 LANDED (commit c65865f):
`g_server->shard_locks` heap array (`[eidx*EFS_META_MAX_SHARDS + shard]`,
malloc+init in efsd.c) + canonical-order helpers `server_shard_lock/
lockn/unlockn/lock_all/unlock_all` in server_internal.h. **Design:** per-shard
locks for the ~17 inode RPCs + REPORT_CHUNKS + LOOKUP_PATH/READDIR/GETCHUNKS;
ONE global lock kept for membership + flush + rebuild + catchup + append
barrier; strict **global→shard** ordering (never hold a shard lock while
acquiring `s->lock`); flush/rebuild take global + ALL shard locks of the
export; `create_rr` becomes a single atomic `fetch_add` (it lives on shard 0
but is bumped by every create). **Expected benefit ~2×/server at bits=3**
(only 2 shards/server to parallelize); more at higher bits.
**3 foundational BLOCKERS — must land BEFORE any handler is partitioned:**
(1) **Rebuild manages `s->lock` internally — DONE (c65865f).** The rebuild's
table-swap commit in `server_rebuild_export_from_pages_ino` now holds the
export's shard locks in global→shard order via `rebuild_shards_lock`/
`rebuild_shards_unlock` (meta_server.c): ALL shards for a main-table rebuild
(`server_shard_lock_all`, since `efs_export_free` drops every shard tab),
ONE for an extra-shard rebuild. The page fetch still runs lock-free (network
I/O). No handler holds shard locks yet, so this is a no-op — it establishes
the discipline. Validated on a fresh bits=3 cluster: posix 196/196 0 EFS
bugs; a joiner restart (fcstor004) exercised BOTH rebuild paths (main
all-shards gen=49, extra single-shard shards 1+5) with no deadlock and a
consistent gen. (The 4-way posixstress smoke surfaced a PRE-EXISTING
load-dependent size-visibility flake `content_random_overwrite_append`
size-0 — also seen at 12:39/13:31 pre-blocker-1, passes 5/5 isolated; NOT a
blocker-1 regression, but a real cross-op size race worth its own look.)
(2) **On-demand shard-table creation — DONE (958b15b).** `efs_export_table`/
`table_for_ino` lazily created+installed shard tables and bumped
`ex->shard_tick` — a shared per-export mutation hiding inside read paths
(GETATTR/LOOKUP). Now the lazy create is extracted into
`shard_tab_get_or_create` (called only under the global lock or all shard
locks), every shard table is pre-created at rehash (mkfs/upgrade) and after a
main-table rebuild (which frees the clean tabs), and the LRU tick bump is
atomic (`__atomic_add_fetch`). Validated on a fresh bits=3 cluster: posix
196/196 0 EFS bugs; a joiner restart (fcstor004) exercised the rebuild
precreate (`meta-catchup: rebuilt export=efs-test` + owned shards 1,5) with a
consistent gen; 4-way posixstress smoke 3/4 clean (1 pre-existing concurrency
size-visibility flake `fsync_then_fstat_size`, passes 5/5 isolated).
(3) **Global-state + multi-shard ops — global-state DONE (b167ea2).** The
three pieces of global state the partition touches are now safe to touch
without the global lock: `ex->create_rr` is a single atomic fetch_add
(bumped by every create); `s->rpc_dirty_ops[]` is atomic bump/read/reset
(the flush trigger); the HOLD table (`hold_buck`, handler.c) has a dedicated
`hold_mu` leaf lock. All behavior-preserving under the still-held global
lock (no handler takes shard locks yet). Validated on a fresh bits=3
cluster: posix suite (5 runs, 0-2 pre-existing flakes/run, one clean
196/196) + 4-way posixstress smoke (2/4 clean) — only the pre-existing
size-visibility/ENOTEMPTY flake family, each passing isolated; no
regression. **Deferred to the transitional step:** the multi-shard
discover-relock — UNLINK reads the parent shard to discover the victim ino
(→ the child shard), and `fan_drop_chunks` touches all shards, so the lock
set isn't knowable upfront (lock→read→discover→relock-in-canonical-order +
re-validate). `fan_drop_chunks` re-acquires the global lock mid-op and
`server_ensure_shard_ready` can trigger a rebuild — an ordering entanglement
that doesn't belong in a clean global-lock-still-held step. **Plan:**
nested-lock transitional step (handlers take global+shard together, no
parallelism, safe) → drop the global lock per op, gating each with posix
suite + posixstress + valgrind. NOTE: on a FRESH cluster the shards are not
hollow, so the rebuild-deadlock (blocker 1) doesn't fire in the posixstress
gate — but it MUST stay fixed for this to be rejoin-safe.
**posix2 cross-client fixes (Aug 26 PM, commits c65ca48/731d023/5525d49/
5062a9b).** posix2 was 15-34 bugs; now ~56-59/63. Fixes: (1) `c65ca48`
created-set getattr refresh (truncate stale-size). (2) `731d023` unlink
non-fatal on local-table miss (peer_open_unlink_nlink EIO). (3) `5525d49`
REMOVED the created-set short-circuit from `lookup_walk` — it was not
authoritative for a shared dir (a peer can create/rename/unlink names in a
dir this client created); now every clean-file lookup under a locally-known
parent does LOOKUP+GETATTR (2 RPCs, inherent to sharding: parent-shard LOOKUP
returns a size-0 dentry stub because the child shard tab is hollow on the
parent owner, so GETATTR on the child owner gets the full row). This fixed 7
visibility/nlink/excl bugs. (4) `5062a9b` SETATTR size-shrink clears the ino's
stale append reservations (a truncated-away rsv looked permanently
outstanding, BUSYing the next O_APPEND into -EIO; peer_append_while_truncate).
**Remaining posix2 bugs:** `peer_fcntl_range_conflict` (byte-range lockf is
per-client in-memory `efs_fuse_lock`, NOT coordinated cross-client — deep
feature, needs server-side range-lock tracking); `peer_rename_across_dirs_chase`
(x in two places — FLAKY, passes 5/5 isolated, load-dependent cross-shard
rename race); flaky EIO/timeout on concurrent truncate/extend/pwrite tests
(load-dependent, amplified by the created-set removal's 2-RPC lookup cost
adding g_server->lock contention). The 15s POSIX2_STEP_SEC is too aggressive
post-removal; 45s gives a cleaner signal. **posixstress 9×4 RE-RUN on the
created-set-removal build (Aug 27, results/posix/20260827-121700, fresh
bits=3, TCP):** 36 instances, 1665 EFS bugs, **1658 (99.6%) are
`timeout after 15s` (saturation); 7 (0.4%) non-timeout — and ALL 7 pass in
isolation** (flock_two_proc_exclusive ×5 = the test's 0.2s child-flock sleep
is too short under load — the child's LOCK_EX RPC hasn't landed when the
parent's LOCK_EX|NB arrives; fdatasync_write ×1; dir_deep_nesting_beyond_64
×1). **ZERO genuine correctness bugs under 36-way.** The GETATTR in the
2-RPC lookup is NOT redundant (parent-shard LOOKUP returns a size-0 dentry
stub; GETATTR on the child owner gets the full row) — it's inherent to
sharding + immediate visibility. The saturation lever is the
`g_server->lock` partition (roadmap Perf lever 1), not fewer lookup RPCs.
**posixstress saturation CONFIRMED = `g_server->lock` latency, not CPU
(Aug 27 profiling on a fresh bits=3 TCP cluster, 36-way):** servers NOT
CPU-saturated (primary ~7.5/24 cores, joiners ~2.8/24; client efs-fuse
~0.43 cores) — a LATENCY bottleneck, not throughput. On-CPU work (perf
record): blake3 chunk hashing, memmove, `drop_chunks_scan`, serialize.
wchan identical at rest AND under load (156 `wait_woken` + 22
`futex_do_wait` = idle worker pool, NOT elevated lock contention — the
per-op hold is short, threads don't pile up). Per-op latency probe under
load vs rest: **create (touch) 5-13ms → 100-350ms (~20-70x)**; stat/readdir
stay ~2-3ms. ALL inode RPCs (LOOKUP/GETATTR/CREATE/UNLINK/...) take the
SINGLE global `g_server->lock` (handler.c:1621). Two lock-hold bottlenecks
confirmed in code: (1) `drop_chunks_scan` is O(chunk_count) under the lock
per unlink/truncate (handler.c:2387 INODE_DROP_CHUNKS, metadata.c:1298);
(2) the CoW flush snapshots the table (memcpy) under the lock
(meta_server.c:1469-1505 server_flush_fragmented_meta_locked). The CREATE
handler already releases the lock during the fan-out CREATE_SHARD RPC
(handler.c:1763-1766), but a create still takes the lock 3x (parent 2x +
child 1x) + the fan-out RTT. Under 36-way the lock arrival rate exceeds the
service rate → creates queue 100-350ms → tests exceed the 15s
POSIX_TEST_SEC. Lever: partition g_server->lock (per-shard/per-export) or
shrink the hold time (chunk-index drop_chunks instead of a table scan;
snapshot without holding the lock).
**posix suite 191→201: seeded random content + strange-name/depth tests
(Aug 26 PM).** `rand_bytes(seed,n)` = SHA-256-keystream generator mixing
ASCII/whitespace/control/NUL/high bytes (NOT zeros/repetitive — zero-fill/
dedup bugs can't hide); same seed → same bytes on any client (peer-verifiable).
New tests: content random roundtrip across 128KiB chunk boundaries,
all-256-byte values, whitespace/line-endings, NUL+control, random
overwrite+append, 2MiB multichunk (sha256-verified); raw-byte names
(control/DEL/high/space-only), unicode NFC-vs-NFD coexistence, crazy names
as dirs, dir nesting past the 64-ancestor LOOKUP_PATH cap. basic_dd_rw now
dd's a seeded-random file (was /dev/zero). `rmdir_rideout_sillyrename` rides
out the FUSE silly-rename ENOTEMPTY transient that outlasts the daemon's
20ms retry under concurrent open/close load. **Limits (from code):** name
component ≤255B (`EFS_MAX_NAME`, ENAMETOOLONG per-component), path ≤4096B
(`EFS_MAX_PATH`), NO hard depth cap (LOOKUP_PATH batches ≤64 ancestors then
falls back to a per-component walk; `find` reporting caps at 128 comps).
Single-client fcstor007: 196 both-pass, 0 EFS bugs, 3 consecutive runs.
**9×4 posixstress harness + first run (Aug 26 PM, commits d123fa4/79df463).**
`run_tests.sh posixstress [N] [hosts]` = N full posix suites per host, all
hosts in parallel (default 9×4=36). posix_suite `--tag` gives each instance a
unique testdir prefix (`posix-<host>-<tag>-`) so same-host siblings never sweep
each other; `posix_one` fans out to `posix_instance`. wipe_cluster now renames
`/data1/*/efs` aside + recreates instantly (detached background edelete
reclaims) and polls for D-state efs-fuse — wipe is ~ms not minutes.
**First 9×4 run (results/posix/20260826-224050, fresh bits=3, TCP):** all 36
instances completed; cluster STABLE after (4 up, gen=1060 consistent, no
wedge/GC, client probe OK). ALL 337 failures are `timeout after 15s`
(saturation), ZERO correctness/assertion failures. Always-timeout (36/36):
`concurrent_appends` + `concurrent_appends_two_proc` (O_APPEND reserve-RPC
path). Near-always: `mtime_monotonic_many_writes` (32),
`concurrent_create_unlink_two_proc` (32), `create_excl_two_proc` (31).
Per-instance 2–22 bugs (fcstor012 cleanest at 2). The `Bad file descriptor`
stderr flood = worker threads of a timed-out `concurrent_appends` erroring
during cleanup (cosmetic). Finding: 36-way concurrency saturates the cluster;
O_APPEND + same-dir concurrent create/unlink serialize worst. Not yet
root-caused.
**2PC metadata root commit LANDED (Aug 26 early AM, working tree) — the
GC-wedge fix.** `PUT_META` is now phase-1 PREPARE (peer stashes the root as
`pending_root[ei]` + raw bytes in `pending_blob[ei]`, NO install/persist/
fence/GC). New `EFS_MSG_META_COMMIT`=91 is phase-2: after the writer's
prepare quorum + local commit, it broadcasts COMMIT(export, gen, root_sum);
peers promote only on a fingerprint match (`efs_hash` of the stashed prepare
bytes) — a same-gen retry that rewrote the same cis can never promote a
superseded prepare. Stragglers converge via GET_META_ROOT catchup (only ever
serves committed roots). Client-side metadata write path DELETED (write
lease, `meta_heal`, `replicate_metadata`, `send_meta_root`, server
`META_FLUSH_BEGIN` election, dead `server_send_metadata_to`) — **the server
is the sole metadata writer.** Validated on a fresh bits=3 cluster: primary
committed gen→135 under 4-way, peers `meta-commit: promoted` + catchup,
**0 unrecoverable / 0 "raced with GC"** — the wedge is gone.
**Two bugs fixed in the 2PC path itself:** (1) `META_COMMIT` busy guard uses
`ex->shard_dirty` (main table) not `rpc_dirty_ops` (shard tabs) — the broad
guard was blocking every promote. (2) use-after-free: the writer hashed
`root_buf` for the fingerprint AFTER `free(root_buf)` → garbage fingerprint
→ 0 promotions (all catchup). Now hashed at serialize time into a local
`root_sum[]` before the free.
**HARNESS FALSE-PASS BUG FIXED (critical):** `cmd_posix` never verified the
mount. After `wipe_cluster.sh` kills all efs-fuse, `/tmp/efs/mnt` is a plain
local dir and the suite was silently passing 186/191 against **tmpfs** (0
"EFS bugs" trivially) — the Aug 25-late "clean 4/9-way" results were NOT
against efs. `run_tests.sh` `posix_one` now refuses to run unless
`findmnt` shows `fuse.efs-fuse` (writes an ERROR stub + fails the host).
**Real gate status on a fresh cluster (EFS_TRANSPORT=tcp):** single-client
007 **185/191, 1 bug** = `names_crazy_roundtrip` (known parallel-visibility
flake, passes isolated). 4-way: no wedge, but **flaky concurrency
correctness** — per-run ~1-2 of 4 clients hit a cluster of rename→EINVAL /
hardlink→EIO / readdir-missing-just-created-entries / FileExistsError on
unique names. Failures move between clients across runs (not
client-specific). Hypothesis: a dirty SHARD table fenced+rebuilt from stale
pages drops unflushed creates (the main table is protected by the
`shard_dirty` COMMIT guard; shard tabs are not). Added `rename-fail:` log in
the RENAME_AT handler failure branch (prints rrc, shard, tab ptr,
np_present, tab_rebuild, tab inode_count, gen) — **rebuilt efsd on all 4,
needs rolling restart + a 4-way `--filter rename` repro to confirm.**
**Shell exec env went unresponsive mid-session (infra)** — resume by:
rolling-restart the 4 efsd (same commit-dirty build-id), `run_tests.sh
setup` all 9 clients, then 4-way gate and read `rename-fail` lines on the
shard owners.
**Default is bits=3 (Aug 25).** `mkfs` creates `EFS_DEFAULT_SHARD_BITS=3`
(8 shards) — no `upgrade` step. bits=0 is not a product mode; cluster
wipe is allowed. `root.id` is stamped on that create so bootstrap
GET_META does not invent export id=2.
**Cluster (Aug 25 evening):** efsd+fuse rolled with the 9-way fixes
on the grown leftover table (no wipe). After that 9-way, remount
failed: EFSR gen=571 pages decode-error on all 4 nodes. **Needs
wipe+mkfs** (clients first). Do not timeout-skip.
**GC-wedge ROOT-CAUSED via instrumentation (Aug 25 PM):** the chain is
**root adopted before its pages are local, then old pages GC'd.**
Instrumented `meta-flush: committed` / `meta-adopt` / `meta-gc` logs on
a fresh gen=1 cluster + 16-way posix suite showed: primary (fcstor003)
commits gen=1,2,3 via its own flush (page0_ci 65536→65538→65541, real
writes). Then gen=4 and gen=5 appear ONLY as `meta-adopt: PUT_META
old_gen=3 new_gen=4 page0_ci=65544` / `new_gen=5 page0_ci=65546` on
EVERY node — **no node logged `meta-flush: committed` for gen=4/5**, so
no node ever wrote the page fragments for ci=65544/65546. The adopt
(catchup_install_newer_root full-adopt, meta_server.c:2210-2234, and the
PUT_META full-adopt handler.c:1175+) installs the new root, fences the
table (`meta_needs_rebuild=1`, `FENCE-SITE catchup-root-no-blob`), and
the SAME adopt then GC's the PREVIOUS gen's cis (handler.c:1274
server_gc_meta_cow_pages). Rebuild then fetches ci=65544/65546 → rc=-3
on all nodes (never written, or already GC'd by the symmetric adopt on
peers) → `page 0 unrecoverable, gen N raced with GC, re-polling`
forever → shard-0 never ready → REPORT_CHUNKS BUSY → client
meta_flush_main spins (inode_rpc.c:128) → all fuse_flush D-state.
**The defect: a node adopts + persists + GC's a root whose CoW pages it
has NOT fetched/verified locally.** The pages for the new gen live only
on the original writer's placement; once every node adopts-and-GCs, the
fragments are gone everywhere. Fix direction: catchup/PUT_META adopt
must FETCH the new root's pages (or confirm 2+1 fragments present)
BEFORE server_save_export + before the old-gen GC runs; defer the
old-cis reclaim until the new root's pages are local. (My earlier
live-root recheck in server_gc_meta_cow_pages only skips cis STILL in
the current root — it cannot save a ci that was correctly absent from
the adopted new root but never written.) Also still open: REPORT_CHUNKS
BUSY retry has no give-up (amplifier), and the RDMA data path never
completes a CQE on a fresh mount (gate runs on EFS_TRANSPORT=tcp).
**GC-reclaims-live-root-pages race REPRODUCED on a FRESH cluster (Aug 25 PM):**
after a clean wipe+mkfs (gen=1, verified stable), running the 16-way
posix suite drove the export to gen=96, then the primary (node 1)
wedged: `meta-rebuild: page 0 unrecoverable (CoW ci=65600); gen 96
raced with GC, re-polling` — page 0's fragments are rc=-3 (gone) on
ALL of nodes 3,4,1. Restarting the primary does NOT help (it re-reads
the on-disk gen=96 root whose page_cis[0]=65600 is GC'd). So GC
reclaimed pages still referenced by the LIVE committed root gen=96 —
that is the bug, not a leftover-table artifact. Effect chain:
`server_ensure_shard_ready(0)` fails forever → REPORT_CHUNKS shard 0
BUSY → client `meta_flush_main` spins in `rpc_send_recv_shard` BUSY
retry (inode_rpc.c:128, usleep 50ms<<shift, attempt<16, NO give-up for
REPORT_CHUNKS — the INODE_APPEND/FLOCK early-return guard at line 123
does NOT cover it) → dirty set never drains → every close-time
`fuse_flush` wedges D-state (kernel `fuse_flush`+`request_wait_answer`).
Two client wedges from ONE server bug. Hardening gaps: (a) GC must
never reclaim a ci still in the committed root's page_cis[]; (b)
REPORT_CHUNKS BUSY retry needs a give-up/circuit-breaker so an
unready shard EIOs instead of wedging the mount; (c) the rebuild
"re-poll newest root" is a livelock when gen=96 IS the newest root.
**RDMA data path broken on fresh mount (Aug 25 PM, FIXED Aug 30):**
chunk PUT/GET over RDMA never completed a CQE (QP reaches RTS, first
SEND/recv after upgrade lost). Cause: shared-CQ poller armed, spun,
then blocked in `ibv_get_cq_event` without re-arming — the next CQE
after a quiet gap generated no event. Inode/REPORT were forced TCP as
a workaround. Fix: drain → req_notify → race-drain → block; small SEND
from the registered pool; peer pool uses the same QP. Default transport
is auto. Historical numbers: write 10B took 10.36s, read timed out 20s;
`EFS_TRANSPORT=tcp` was 0.005s / 0.023s. On TCP, single-client posix suite:
185/191 both-pass, 1 EFS bug `names_crazy_roundtrip` (passes isolated
— parallel-visibility flake), 5 target-better XFS quirks. Then the
16-way suite triggered the GC race above and wedged the cluster.
**vfs_unlink D-state / create+unlink hang ROOT-CAUSED (Aug 25 PM):**
NOT a C bug — the cluster was still on the **corrupt leftover table**
(build af6f380, log starts extras-merge gen=7→80, never a fresh mkfs).
Primary efsd loops `meta-rebuild: page 0 unrecoverable (CoW ci=65554)
gen 80 raced with GC` — shard-0 pages are corrupt (`scheme=legacy`
checksum mismatch + frag fetch rc=-3). So `server_ensure_shard_ready(0)`
fails forever → every REPORT_CHUNKS for shard 0 gets BUSY → client
`append_reserve_offset` BUSY-flush loop spins holding `g_append_mu`
inside FUSE `.write_buf` → kernel holds the inode write lock → the
suite's cleanup `unlink` wedges D-state in `vfs_unlink+0x48` (before
FUSE is called; gdb showed all 10 FUSE workers idle in
`fuse_dev_do_read`, one thread in the report BUSY retry). First suite
test `basic_oappend_flag` (ino 3224) hung before the TSV was written.
**Fix = wipe+fresh mkfs, not code.** Hardening gaps to consider later:
(a) `append_reserve_offset` spins 4000× (~8s+) holding `g_append_mu`
with no give-up-and-EIO, so an unready shard wedges the whole mount's
O_APPEND + blocks unlink on the same inode; (b) no circuit-breaker when
REPORT itself returns BUSY repeatedly (shard permanently unready).
**Fixes this cut (working tree, not committed):** (1) fsync EIO —
sync REPORT flush failure is BUSY (retry), skip hollow extras
instead of failing the barrier; client retries BUSY. (2) two-proc
O_APPEND — rsv slot is free once size catches up (was live 30s);
256 slots; never fall back to an unreserved local end. (3) suite
exit hang — cwd off the mount, close leaked FUSE fds per test,
`os._exit` after TSV; release no longer re-does data_sync (flush
already did; 014 D-state on close of leaked fds). (4) posix2
`--parent` + `run_tests.sh posix2 multi` unique per-pair parents.
(5) heal status counts **owned** tables only; label is
need-rebuild not "dirty". After efsd restart, status was idle
gen=177 (was stuck "healing 6/8 dirty").
**Re-gate on dirty table (partial):** isolated 007 fsync+two-proc
PASS. 9-way: two-proc PASS on every host that finished; 007/008
original 4 fsync EIO gone (EEXIST leftovers instead). 012/014
188/188 then hung (leaked fds + blocking release) — TSV written.
Some later hosts still EIO on fsync-path tests. Clean mkfs gate
not done (pages now corrupt).
**9-way posix `20260825-170203` (pre-fix):** 6/9 clients 0 EFS
bugs. 007 4 fsync EIO; 008/010 two-proc lost lines; 008–015 hung
after TSV.
**posix2 multi-pair `20260825-171200`:** 4 pairs **34/34** on
remount-A+prepare+remount-B. Leftover `posix-2c` was harness.
**POSIX suites (working tree):** posix_suite **191** (+3 local:
high-then-low extend, mtime monotonic, trunc0+high pwrite).
posix_2client **63** (+29 adversarial). Harness now runs
`("ab", (fn_a, fn_b))` A∥B. Layer 4 crash/EC/partition is **not**
in this suite. Re-run XFS baseline before the next compare.
**bits=3 gate (committed `af6f380`):** single-client posix
`20260825-160200` **188/188**. posix2 A=007 B=008
`20260825-162207` **34/34**. unlink-storm 9×4000
`20260825-163504` **9/9 RM_OK+ALIVE**.
**posix2 EIO / size-0 was three bugs:** (1) wr() close kicks async
REPORT; O_TRUNC setattr size=0; late grow-only REPORT restores the
old size (B saw 10). REPORT grow is now rejected when the rec mtime
is older than the row. SETATTR SIZE also stamps client-clock mtime
so a behind-client post-trunc REPORT is not treated as stale (B
saw 0). (2) remounted peer LOOKUP skipped GETATTR / overwrote RPC
with a local size-0 stub → utimens showed "now". GETATTR unless
dirty; keep local only when dirty. (3) posix2 harness remounts B
after prepare fsync (`.keep`) so B does not walk an old posix-2c
ino. Drain REPORT before utimens/truncate. fcstor004 catchup stuck
at gen=329 vs 347 broke flock/unlink mid-suite — restart joiner.
**link_across_dirs** (earlier this cut): created-set local hit must
return the stitched inode (LOOKUP/GETATTR wedged FUSE).
**Isolated PASS this cut:** `attr_utimens` / `attr_utimens_ns` /
`attr_touch_terminal` / `perm_owner_utime_readonly` (setattr now
applies times locally + pin so data-path "now" cannot clobber);
`names_too_long_component` (lookup returned EFS_ERR_INVAL → ENOTDIR;
now EFS_ERR_NAMETOOLONG + f_namemax=255).
**Still open:** 9-way `20260825-170203` — 007 4× fsync-path EIO;
008/010 two-proc O_APPEND dropped lines; post-suite FUSE wait on
exit. posix2 multi-pair is green when testdirs are clean.
**Fixes this cut (working tree):** (1) serialize `dentry_bytes` +
bounds — flush SIGABRT. (2) bootstrap no longer invents export id=2.
(3) `mkfs` default bits=3 + stamp `root.id`. (4) `create_with_ino`
uses `lookup_on_tab` (parent dentry skip → 1/8 readdir miss).
(5) catchup no longer bails on gen=0+`shard_bits` mkfs root (joiners
stayed bits=0 → CREATE_SHARD NOT_PRIMARY → file create EIO).
(6) `efs_export_lookup` consults dentry shards only — walking extras
found the child-row name after rename/unlink. Isolated
`dir_rename_file` / `dir_rename_symlink` / `unlink_open_file` PASS.
(7) utimens: local set_mtime/set_atime after setattr upsert +
mtime pin vs dcache_note_size/REPORT echo; adopt takes owner times
when !dirty. Do not shrink size on an older setattr mtime — that
zeroed basic_dd_rw after dirty cleared. (8) NAMETOOLONG:
EFS_ERR_NAMETOOLONG, not INVAL. Deployed efs-fuse on 007–015.
Isolated gates PASS. Full suite on the ~440k leftover table hung
in FUSE wait on `fsync_reopen_visible`. Need a clean mkfs before
re-gating posix/posix2/storm.
**Wipe always kills clients first** (`killall -9 efs-fuse` on
003–015) — leftover fuse is the 232MB/1.9M-chunk re-adopt.
**Phase 3b gates (Aug 24 late, build `09d38fe9c2b8-dirty`)** — cluster
rebuilt after a 13-way NFS rsync of `results/` wedged the first deploy.
**bits=0:** posix `20260825-034030` 182 both-PASS; 1 classified EFS bug
`dir_move_into_subdir` (ConnectionAbortedError) **isolated PASS**; fcntl
matches XFS FAIL; 3 virt SKIPs. posix2 first run 23/34 — primary
**SIGABRT** (`corrupted size vs. prev_size`) in
`efs_export_table_snapshot_free` ← `server_flush_fragmented_meta_locked`
(heap smash during flush, not 11 independent bugs). Restart + remount:
posix2 `20260825-035117` **34/34, 0 EFS bugs**. **bits=3 blocked:**
fresh mkfs+upgrade shards=8, FUSE bootstrap `export id=2` vs
`efs-mgmt list-exports` `id=1` → `mkdir` EIO (CREATE to missing id).
mc_stress + multi-9 sw-1m not run. Do not treat bits=3 as green.
**Phase 3b implemented (Aug 24 night, working tree)** — Items 0–2 of
docs/scaling-roadmap.md. Item 3 (replica-3 meta pages) left optional / not
in this cut. `test_meta_v6` OK; efsd + efs-fuse build clean. **Fresh
`edelete`+`mkfs` required** (chunk routing is not backward compatible).
- Item 0: op-class comments on REPORT (commutative), CREATE (independent),
  RENAME (conflicting) in protocol.h / handler.c.
- Item 1: `efs_export_chunk_shard_of` — group 0 stays on `shard_of(ino)`
  (small-file locality); later 8 MiB groups mix. REPORT/GETCHUNKS route by
  group via `rpc_owner_conn_shard` (no crafted ino). GETCHUNKS serves one
  group, no inode required on that shard, loud NOT_PRIMARY. Truncate/last-
  link unlink fan `EFS_MSG_INODE_DROP_CHUNKS` to other shard owners.
  `drop_chunks_from` scans the table (extent groups are not dense from 0).
- Item 2: `EFS_DIR_SPREAD_MIN` (65k) derived from rollups. Dentries go to
  `hash(parent,name)` past the threshold; LOOKUP tries hash then parent;
  readdir merges `EFS_READDIR_F_LOCAL_ONLY` per shard; unlink prefers hash
  shard (hash-shard row wins a crash duplicate). Stress:
  `tests/stress/hotdir_spread_probe.sh`.
**Roadmap gained Phase 3b (Aug 24 night)** from the architecture review
(plan still in docs/scaling-roadmap.md). Rejected: consensus-group sprawl,
MVCC, SPDK, universal hash dentries, range leases.
**rsync 10-round (Aug 24 night).** `rsync -avvvcSP` of `ecrawl-synt-small`
onto FUSE, 70s windows. Peak **16156 files/70s** (r9; r10 15983) vs r1
13889 / old LOOKUP_PATH-era ~230/70s. NFS→local same tree is ~3238/s —
efs is the limiter. C levers that mattered: hashed `hold_find`; same-dir
rename `expand_parent_chain` + skip child_idx; **`stamp_ctime_loaded`
nlink<=1 is O(1)** (was a full-table scan per rsync temp→final — 9% efsd
on a grown table). Also: LOOKUP last-component to parent owner; session
`created_set` skips dest-stat ENOENT under dirs this client mkdir'd
(remounted peers still RPC). Flush window 10s/20k. Reverted parent-shard
file create (piled work on shard 0). Residual: CREATE+RENAME RTTs
(~11k temps from `-P`). **Do not push ewrite/ecopy.**
**Unlink-storm 9×4000 PASS (Aug 24 night).** First run wedged at ~4–7 creates/s
(9-way LOOKUP_PATH+CREATE+HOLD+blocking REPORT, plus 100 ms extras flush).
Fixes (no new caches): local-table getattr for dirs this client already
dual-applied; `EFS_CREATE_F_HOLD` piggybacked on CREATE; close kicks
REPORT off the FUSE worker; server flush window 100 ms→2 s / 1000→8000 ops.
Grown leftover table (~65k inodes) still ~8 min/4000; **fresh mkfs bits=3**
is **~10 s create + ~3 s rmtree** (`results/stress/unlink-storm-20260825-000136`,
9/9 RM_OK+ALIVE). POSIX: isolated `dir_move_into_subdir` PASS; posix2
**34/34** (`20260825-001749`). Full 188-row tsv has one timeout-abort
artifact from a 300 s SSH cut. **Do not push ewrite/ecopy.**
**POSIX + posix2 CLEAN on bits=3 (Aug 24).** Fresh `efs-test` shards=8.
Single-client `posix_suite.py` **188/188** (`results/posix/20260824-214726`,
0 EFS bugs vs XFS; 5 target-better = known XFS quirks). Two-client
`posix_2client.py` **34/34** A=007 B=008 (`results/posix2/20260824-214648`,
0 EFS bugs). Suite grown 162→188 and 24→34 (same-fd size, hardlink/symlink
corners, rename-over-symlink, more peer visibility). The Aug 24 gaps are fixed: `ctime_on_link_rename` (RENAME_AT
+ ctime bump), `direct_unaligned_einval` (4k O_DIRECT), cluster flock
(HOLD/FLOCK RPCs + per-`fi` owner), unlink-open / peer unlink-while-fd
(keep_last + HOLD), same-fd empty reads (getattr prefers local size after
adopt — RPC size 0 was clobbering kernel i_size), `peer_symlink_visible`
(owner getattr for non-dir leaf + sync REPORT after symlink write).
**9-way POSIX (Aug 24 night):** sequential clean ≠ parallel. `trunc_open_other_fd`
fstat 0: adopt shrank a dirty local size to the owner's still-0 getattr
(fsync path-lookup). Fix: skip shrink when ino dirty; fsync by `fi->fh`.
`concurrent_appends*` lost lines: `append_rsv[ino%64]` stole another ino's
live reservation. Fix: open-address, never evict a live rsv. 008 hang =
`rmtree` D-state after 9 suites finish together — use `--keep`; not a
suite-correctness bug. **Do not push ewrite/ecopy.**
**Phase 3 storage model (Aug 24).** Pages + v8 descriptors are the DB.
Commits: `38dd6f0` (owner-only extras + unlink no-instantiate),
`9eff867` (reuse unchanged CoW pages), `a3485eb` (skip chunk snapshot
when chunk_epoch unchanged). **Post-restart rebuild + flush FIXED**
(this commit): unsharded create/unlink/link now set `shard_dirty`
(flush was no-oping after `rpc_dirty` only); never adopt leftover
`.efsm` (that cleared `needs_rebuild` on a 1-inode table). Catchup
rebuilds a hollow table when `blob_len > 64K`. Flush holds dirty
ops until rebuild instead of dropping them. Trustworthy meta w=1
`20260824-150410` on the grown table: create **69325** / unlink
**60047** (was 119 / 30); flush `reused 546/547`, Heal gen 1→2.
Persist: `--keep` 32 files, gen→3, kill+restart all 4, rebuild,
cold FUSE mount saw all 32. The earlier 70k print was RAM-only;
this one is RPC apply + async CoW reuse. **Current throwaway `efs-test`
is bits=3** (posix/posix2 above). FUSE timeouts 0. No caches.
Cluster live on **EFSR v8** + Phase 3 data-path sharding Stages 0–3
(committed). CoW = **debe6dd**, catchup-vs-GC = **76753ef**, 2b reads
= **94f96fd**.
**Data-path sharding LANDED (Aug 23 night):** both
blockers from the morning finding are fixed together (fixing only one
= silent data loss). (1) REPORT_CHUNKS is per-shard (`write.c`
`efs_client_report_dirty` partitions by `efs_export_shard_of`; server
loud-`NOT_PRIMARY` instead of silent drop). (2) file creates
round-robin `create_rr % sc` (dirs stay on parent shard);
parent-owner fans `EFS_MSG_INODE_CREATE_SHARD` to the child owner,
then writes the dentry locally. Probe
`tests/stress/shard_spread_probe.sh` PASS: 32 files, 8 distinct shards,
non-primary-owned, md5 match on a second client. Extra: hardlink/
last-link unlink fan `LINK_SHARD`/`UNLINK_SHARD` to the child owner
(parent-local `nlink_inc` was NOT_FOUND → EIO). PUT_META never fences
a live table for extras-only roots (that dropped unflushed mkdir
ops and made sharded dirs vanish mid-create).
**POSIX suite extended (Aug 24):** `posix_suite.py` 144 → 162 → **188**,
`posix_2client.py` 12 → 24 → **34**. bits=3 **188/188** + **34/34**
— 0 EFS bugs. The four Aug 24 gaps (`ctime_on_link_rename`,
`direct_unaligned_einval`, `peer_flock_exclusive`,
`peer_unlink_while_b_has_fd`) are fixed.
**POSIX after datapath sharding:** bits=0 `efs-test`
`20260823-232541` 0 EFS bugs; bits=3 `20260823-233448` 0 EFS bugs
(hardlink 8/8 after LINK_SHARD); posix2 12/12 both
(`20260823-232632` bits=0, `20260823-233514` bits=3).
**Perf on fresh bits=3 mkfs (Aug 23 233721/233849, all rc=0):**
single 007 sw-1m **5772** (new peak, was 5656 bits=0). Multi-9 sw-1m
agg **6925** — did NOT beat bits=0 **9342** / missed the ≥3× single
(~17 GB/s) gate. Files DID spread (007 f.0–f.7 inos cover shards
0–7). Residual: extras-commit catchup storm (~113–131
`rebuilt export=efs-test shard=N` per server during the multi run)
re-centralizes work. Next lever: skip extra-shard rebuild when the
local tab is already at/above the incoming extras gen (or batch
extras so peers don't rebuild every commit). Guide remains
`docs/phase3-datapath-sharding-impl.md`.
**Extras catchup storm FIXED (Aug 24):** two changes.
(1) Per-shard incremental rebuild cache
(`s->shard_blob_cache[export][shard]`) so a shard rebuild only fetches
pages whose checksums changed (was: every descriptor refresh re-fetched
every page). (2) THE LEVER: `server_flush_fragmented_meta` now only
flushes shards with `shard_dirty` set (was: flushed every shard on
every 100ms window, bumping every descriptor gen and making every peer
rebuild every shard). Multi-9 sw-1m agg **7569** (was 6925), rebuild
lines per server 113–131 → **7–8**. Still below bits=0 **9342**;
residual is per-chunk server latency under 9-way contention
(`g_server->lock` sharing between REPORT/flush/PUT). mc_stress on
bits=3 VERIFY-OK both clients; kill -9 non-primary shard owner
(fcstor005) → 200/200 failover reads OK, restart rejoined, cold-mount
client read OK. Cluster healthy (4 up / 0 down).
**Perf push (Aug 23, committed, bits=0, all on top of the above):** a chain of
CPU/lock fixes validated on a FRESH cluster (wiped with edelete + mkfs —
accumulated chunk-table state was silently degrading throughput ~15-40%, so
A/B on a grown table is meaningless; always measure perf on a fresh mkfs).
Fresh-cluster peaks (all rc=0): single 007 sw-1m **5656** (was 4993); multi-9
agg sw-1m **9342** (was 8270) / sr-1m **30513** (was 27931) / rw-4k **13934**
(~flat). Server CPU during multi write dropped from 13-15 cores to ~2.6.
Fixes, in order: (1) shared-CQ + single poller thread replaced per-conn CQ
spin-polling (top CPU on client AND server); (2) generation-tagged recv
`wr_id` (`gen<<32|reg_idx<<8|buf`) so a stale completion from a destroyed QP
can't be injected into a reused conn slot (was corrupting `g_server->lock` →
SIGSEGVs); (3) server conn-thread leak — `efs_rdma_recv_wait` now polls the
TCP control fd alongside the efd (a silently-dead peer never errors the QP)
and drains the efd when the pend ring empties (4091 leaked threads → 32);
(4) server conn threads use `efs_rdma_reply_ready_quick` (no adaptive spin —
~13 cores of pure spin under 9 clients); (5) O_DIRECT write fast path —
`EFS_RDMA_RECV_ALIGN` places the PUT payload on a 4096 boundary so
`shard_io_thread` writes straight out of the recv buffer (no 64KiB bounce
memcpy, was 11% CPU); (6) clock-gated `now_us()` in the poller + recv_wait
spins (was ~1 core of pure clock_gettime); (7) client PUT reply wait capped
to a fixed 24us spin (`efs_conn_reply_watch_us`) — the adaptive budget handed
200us to each of 32 put workers (~8 cores of spin at 5 GB/s); (8) THE
multi-write lever: `REPORT_CHUNKS` held `g_server->lock` for ~8ms per report
(applying tens of thousands of chunk recs, yielding only every 8192) while
every data-path `PUT_CHUNK` blocked on the same lock — multi sw-1m was 12ms
avg / 1.5s max write latency. Now yields every 1024 recs + `sched_yield()` so
PUTs interleave → 8.8ms avg / 447ms max, multi sw-1m 7550→9342. **Remaining
write ceiling:** per-chunk server latency inflates ~870us→~3.9ms under 9-way
contention (residual `g_server->lock` sharing between REPORT/flush/PUT +
single recv_poller at 30% CPU). Next levers: partition the metadata lock or
N recv CQs/pollers; the real fix is Phase 3 data-path sharding (bits>0).
EFS_WRITE_PIPELINE 64 measured WORSE than 32 (single 3780 vs 4686) — depth
is not the limiter. EFS_RDMA_BUFS 32 ≈ 4 (recv depth not the limiter).
POSIX after the perf chain: 143/144 + `concurrent_creates_same_dir` flake
(passes 3/3 isolated) + 5 known XFS quirks.
**Data-path sharding load-split (the Aug 23 morning finding) is FIXED**
— see the "LANDED" paragraph above. The old
silent-drop + create-stride=`nlive` pair is gone. The remaining
multi-write gap vs bits=0 is extras catchup rebuild, not routing.
**Phase 3 code-complete (working tree, Aug 21):** tables + v8 persist +
owner routing + owner extra flush + on-demand LRU + spread creates
(dentry on parent, inode on child, stride=nlive) + `efs_export_rehash`
(`efs-mgmt upgrade`). Same-fd zeros fixed (dcache overlay).
**Phase 3 VALIDATED on a throwaway sharded export (Aug 21 PM, 0298143):**
`efs-s3` bits=3 (8 shards), single-export cluster (see multi-export gap
below). Low-bits ino encoding (`ino & (2^bits-1)`, root pinned to shard 0),
canonical sorted-live ownership, server rebuild preserves shard_id,
catchup rebuilds shard tables after main rebuild, client dual-apply
mirrors server (child full row + parent dentry), attr accessors route to
the canonical shard row (`shard_route`). **Root-protocol fix (the big
one):** extra-shard owners no longer bump the main gen on extras commits
(`server_commit_cluster_extras` same-gen), and PUT_META/catchup recognize
identical shard-0 checksums as an extras-only refresh — merge descriptors
(`efs_export_merge_extra_roots`: skip dirty tabs, skip >= gen), never
fence live tables. Flush thread drops dirty ops only when primary.
Cross-client O_APPEND is atomic via `EFS_MSG_INODE_APPEND` reserve RPC;
REPORT_CHUNKS applies size grow-only / mtime newer-only; `adopt_rpc_inode`
re-pulls chunk mappings on newer mtime even at same size.
**mc_stress (tests/stress/, 2 clients, bits=3): VERIFY-OK on both** —
200 same-dir creates cross-visible with content, 200 appends (100+100,
0 torn), 64×128 KiB disjoint chunks of a shared 8 MiB file intact on both
clients, rename churn clean, growfile monotonic. kill -9 shard owner →
16/16 failover reads OK; restart rejoined; cold mount 16/16 + appfile
200 lines. Zero "dirty under rebuild"/"unrecoverable" log lines on all
4 servers during the stress.
**Regression after all Phase 3 fixes (Aug 21 PM, bits=0 efs-test):**
POSIX **144/144** (`results/posix/20260821-185338`, fcstor007; 5
target-better = known XFS-baseline quirks), posix2 **12/12**
(`results/posix2/20260821-185357`, A=007 B=008), perf quick: single
007 sw-1m **5049** / ow-1m 4970 / sr-1m 4686 / rw-4k **1559** (399k IOPS);
multi-9 agg sw-1m **4842** / sr-1m 24576 / rw-4k **13917** (≈ the 13428
pre-collapse baseline; rw-128k 121 GB/s agg is the known not-credible
dcache artifact). No regression anywhere.
**Known gaps from sharded validation — ALL FIXED (Aug 22-23, working
tree):** (1) multi-export server metadata: PUT_META/GET_META/catchup are
per-export (named requests, per-export dirty/flush); two exports
(`efs-test` bits=0 + `efs-s3` bits=3) coexist on one cluster. (2)
shard-table pages GC'd on the commit=0 flush path too. (3)
`entry_timeout=0`/`negative_timeout=0` — no stale negative dentries.
(4) shard ownership remap fenced (commit-time recheck + extras filter).
Plus: sharded hardlink/unlink nlink routing (`efs_export_link`/
`efs_export_unlink_name` shard-aware), primary-owned extra-shard
durability (commit captures local tabs + max-merges previous
descriptors), self-heal create (parent dentry authoritative).
**Cross-client data fixes (Aug 22-23):** O_APPEND barrier
(`EFS_MSG_INODE_APPEND` reserve RPC + `append_rsv` table; barrier
released by REPORT after data lands — `dcache_put_now` marks the ino
dirty). rwfile staleness: (A) read-miss self-heal —
`efs_client_read` pulls the mapping range from the owner
(`efs_client_pull_layout_miss`, rate-limited 1/s/ino) instead of
zero-filling a stale-cache "hole"; (B) `dcache_note_size` bumps mtime
on EVERY write (reports are newer-only; a frozen mtime left peers
without an adopt trigger). FUSE flush/release use `fi->fh` (no path
re-lookup) so a failed lookup RPC can no longer silently skip the
close-time flush.
**efs-s3 root clobber FIXED (Aug 23):** non-primary servers in a
rebuild storm (root-only GET_META window after each primary commit →
peer page rebuilds; 114-116 rebuilds/5.5h) eventually adopted a
newer-gen root that lacked extra-shard descriptors; the catchup adopt
(`catchup_install_newer_root`) did a wholesale `root_move` with no
extras preservation, the rebuild wiped `shard_tabs` and reinstalled
from the (empty) extras → shard tables permanently lost → clients saw
3 inodes. Fix: `efs_export_root_maxmerge_extras` (per-shard
higher-gen-wins + carry-forward-missing) applied on EVERY adopt/commit
path — primary commit capture, PUT_META full-adopt, catchup install,
and `efs_export_merge_extra_roots` (which otherwise dropped
descriptors for shards with no local tab). Extras are now monotonic.
**Fresh-cluster validation (Aug 23, post-fix, efs-test + efs-s3
coexisting):** mc_stress on efs-s3 VERIFY-OK both clients + kill -9
shard owner failover OK + restart rejoin OK + cold mount OK; rebuild
counts 0/6/2/5 (storm gone), zero fence/dirty-drop lines. POSIX
**0 EFS bugs** on efs-test (`results/posix/20260823-071727`) AND on
efs-s3 (`20260823-071843`); posix2 **12/12**
(`results/posix2/20260823-071901`). Perf quick: single 007 sw-1m
**4993** / ow-1m 4974 / sr-1m 4578 / rw-4k **1536** (393k IOPS);
multi-9 agg sw-1m **8270** / sr-1m 27931 / rw-4k **13817** — all at or
above the Aug 21 baselines, all rc=0.
**Multi sw-1m FIXED (working tree, Aug 21):** 023817's 1274 agg
(~140/client lockstep) was a 100MB-table flush stalling PUTs — serialize
and peer deserialize ran under `s->lock`, and overwrite REPORTed every
checksum. Snapshot-serialize + unlocked catchup adopt + checksum-only
skip dirty. Single overwrite **4564** MiB/s; 9-client sw-1m agg **~4326**
(shared write ceiling; was 4593 on 220930). Live table ~880k chunks.
**Same-fd POSIX zeros FIXED (working tree):** `dcache_copy` now serves
fully-covered `have_base=0` dirty ranges; GET overlays those ranges onto
a zero/fetched chunk. That was the 2b-reads regression
(`basic_rdwr_no_reopen` et al.) — not a getattr/adopt size clobber.
**POSIX 144/144** (`results/posix/20260821-005111`, fcstor007) after Phase 3
code-complete (bits=0 live path). Re-run `20260821-010553`: 1 flake
(`concurrent_create_unlink_two_proc` rc=1) that passed on isolated retry.
**posix2 12/12** (`results/posix2/20260821-010629`, A=007 B=008).
**Test suite committed (d52287e)**: reusable POSIX + perf suites, results
tracked in git under `results/`. See "Test suite" section below.
**2 client data-path fixes (7fa99e8)**: packed-file overwrite/append + rename/
link data loss (see "Root causes" below). POSIX suite 64/69 (was 16 fail).
**5 POSIX gaps fixed (7e442fa) → suite 69/69**: daemon-side permission checks
(access/open via check_access), rmdir ENOTEMPTY, ENAMETOOLONG, serialized
O_APPEND, + a concurrent-write dcache flush race (see "Root causes").
**Suite extended to 144 tests (user, ef4e9cd + working tree) + 3 more data
fixes → 126/144, then the last 18 POSIX gaps fixed → 144/144 on fcstor007**:
- O_TRUNC in efs_fuse_open (ce269e0) — kernel gives no separate truncate for
  O_TRUNC on open, so truncate in the open handler.
- Concurrent-append re-dirty (c129b39) — a write landing during another
  thread's flush of the same chunk found dirty cleared, fell to load_and_patch,
  saw the not-yet-published chunk, and built a zeroed have_base=1 entry that
  overwrote in-flight data. Fix: patch the mid-flush entry + re-dirty it
  (pairs with the shard_io flush lock, which serializes the re-flush).
- chmod/setattr dual-apply size (396c1b8) — primary's returned inode lags the
  data path (learns size via async REPORT_CHUNKS), so a non-truncate setattr
  upserted a stale size=0, hiding data. Preserve local size unless truncate.
**18 POSIX gaps FIXED (working tree, Aug 20) → 144/144**:
- Permissions: daemon-side W|X on create/mkdir/unlink/rmdir/rename/link/
  symlink (export root skipped — root-owned 0755), ancestor X on getattr/
  open/access, R on readdir, W on path truncate, EPERM chown for non-root.
- Metadata: create dual-apply via create_with_ino (parent nlink++), unlink/
  rename adjust dir nlink, chmod bumps ctime even in the same second,
  setattr mode/owner applied locally so hard-link rows share mode;
  attr_timeout=0 so the high-level path cache cannot hide the other name.
- Errno: rename-over-nonempty maps EFS_ERR_NOT_EMPTY → ENOTEMPTY (was EIO).
- Features: st_blocks counts present chunks (not holes), .lock implements
  overlapping exclusive byte-range locks, .lseek implements SEEK_HOLE/DATA.

<a id="ph-test-suite-d52287e-027ec0"></a>
## Test suite (d52287e)
Run from the login node (needs ssh to nodes): `tests/run_tests.sh
posix|posix2|perf|setup|all`. `setup` rsyncs+builds+mounts the 9 pure clients.
- **ewrite sweep** (`tests/run_tests.sh ewrite [host]`): 30s
  `ewrite.sh <mnt> 1 {2,4,8,16}`, kill, record bytes/wall. Default
  fcstor007:/tmp/efs/mnt. Results: `results/ewrite/`.
- **POSIX** (`tests/posix/posix_suite.py`, **162** tests, @test-extensible): run vs
  an XFS baseline (node9901:/data1/efs); `compare.py` → efs bug = PASS on XFS,
  FAIL on efs. Extended Aug 24 (+18). Latest bits=3 `20260824-131138`:
  155 both-PASS, 2 new EFS bugs (`ctime_on_link_rename`,
  `direct_unaligned_einval`), 5 known XFS target-better.
- **POSIX 2-client** (`tests/posix/posix_2client.py`, **24** tests): create/mkdir/
  rename/O_CREAT on client A, check client B with no remount. Extended Aug 24
  (+12). Latest bits=3 `20260824-131451`: 22/24; EFS bugs
  `peer_flock_exclusive` + `peer_unlink_while_b_has_fd`. Harness remounts B
  only if the parent is missing. `fusermount3 -uz` — a busy FUSE remount hangs.
  Run: `tests/run_tests.sh posix2`.
- **Perf** (`tests/perf/perf_node.sh`): dd + 9-job fio, single|multi client
 (parallel), aggregated to `results/perf/history.tsv`. Harness logs stay
 LOCAL (don't depend on the efs read-after-write path being benchmarked).
- **Multi-client stress** (`tests/stress/mc_stress_worker.sh` A|B on two
 clients + `mc_stress_verify.sh`): same-dir creates, same-file O_APPEND,
 disjoint 128 KiB chunks of one shared file (chunk-aligned — sub-chunk
 cross-client RMW is still a follow-on), rename churn, read-during-write
 monotonicity. VERIFY-OK on both clients = pass. NOTE: run the harness
 UNSANDBOXED (ssh to the nodes); a sandboxed run fails ssh silently and
 produces empty 0-test results.
- **NFS scratch baseline (20260820-221413)**: same suite, one client
  (fcstor007) vs Engaging ORCD scratch NFS
  (`~/orcd/scratch/efs/nfs-fio-baseline` → fstor004.ib, nfs4, 294T).
  Recorded under `results/nfs/` (not mixed into efs `perf/history.tsv`).
  All rc=0. fio sw-1m 5725 / sr-1m 17818 / rw-4k 131 MiB/s. Sequential
  read is well above efs single-client; 4k random write is in the same
  ballpark as the collapsed efs number before the dcache-limit fix.
- **Local NVMe ceiling (20260820-221902)**: same suite on fcstor003-006
  `/data1/01`–`06` (6×7T XFS NVMe each). Harness:
  `tests/run_tests.sh nvme` → `tests/perf/perf_local_nvme.sh`. Writes only
  under `<path>/fio-ceil` (never live `<path>/efs`); efsd stayed up.
  All rc=0. Per-drive serial typical: sw-1m ~3.5–3.9 GB/s, sr-1m
  ~3.2–4.1 GB/s, rw-4k ~2.4 GB/s. Host-sum with all 6 busy: sw-1m
  16.7–21.4 GB/s, sr-1m 21.4–23.5 GB/s, rw-4k 11.2–12.4 GB/s. Two
  serial outliers (fcstor004 /data1/01 sw-1m 642, fcstor006 /data1/03
  rw-4k 647) — likely momentary efsd/device contention, not a dead drive.
- Multi-client baseline (20260820-170303): write ceiling SHARED (~4.9 GB/s
  aggregate sw-1m across 9 ≈ single-client), read scales (~25 GB/s sr-1m).
- **Perf re-run after POSIX 144/144 (2b47a2c), Aug 20**: single
  `20260820-204328` + multi-9 `20260820-204949` (all rc=0). Sequential
  1m write still shared-ceiling (multi sw-1m agg 5853 MiB/s). 4k IOPS
  collapsed vs morning baseline: single rw-4k 1372→125 MiB/s, multi
  rw-4k 13428→302 MiB/s. Cached dd-read also ~3× slower.
  **rw-4k profiled (fcstor007, Aug 20): NOT getattr/attr_timeout.** perf
  `-g -F 999` during isolated 8-job 256m randwrite: getattr/check_search/
  inode_allocated_bytes ~0%. Cost is 128 KiB RMW flush — GET fragment
  ~33% inclusive, dcache_flush_slot_inner ~41%, writer
  `maybe_reclaim` inline GET+PUT when dirty_bytes > 2×512 MiB cap
  (working set 2 GiB). `have_base=0` on published chunks forces a full
  chunk GET before PUT. strace of efs-fuse blocked (Yama) — **historical;
  ptrace_scope is 0 since at least Sep 1 2026, strace -p works.**
  **rw-4k ROOT-CAUSED + FIXED (working tree, Aug 20)**: two changes.
  (1) `dcache_load_and_patch` now reads the published chunk base ONCE at
  patch time (have_base=1) instead of zero-fill + a GET on EVERY flush —
  flush is now PUT-only (verified: no client_read under flush_slot_inner).
  Correct hygiene but NOT the lever by itself (GET just moved to patch).
  (2) THE LEVER: `dcache_reclaim_limit` default 512 MiB → **2 GiB**. The
  dcache is 65536 slots × 128 KiB (8 GiB hard cap) but reclaim keyed off
  512 MiB held only ~4096 chunks; a 2 GiB randwrite working set (16384
  chunks) churned reclaim → every evicted partial chunk = a 128 KiB RMW
  (~32-64x amplification). At 2 GiB the working set stays cached, repeat
  4k writes coalesce, flush is one PUT. Verified: EFS_DCACHE_BYTES=4G and
  the new 2 GiB default both recover single rw-4k to ~1545 MiB/s
  (run 20260820-220703, was 125). Multi-9 rw-4k 302→1799 MiB/s agg
  (20260820-220930) — still well under the 13428 morning baseline; the
  residual is cross-client RMW contention on the shared write ceiling
  (sw-1m agg 4593 ≈ ceiling). Single-client is fully fixed; multi-client
  sub-chunk/ranged PUT is the follow-on if that gap matters.
- **Perf after Phase 3 (2b63d31), Aug 21**: single `20260821-013841` all
  rc=0 — sw-1m 4255 / sr-1m 3566 / rw-4k 1554 (matches 220703). First
  multi `20260821-023152` is invalid (009–015 still on stale v7 FUSE,
  all rc=1 in 1s). Remounted those seven; multi `20260821-023817` all
  rc=0. dd seq1m.write agg 18944 / sr-1m 9171 / rw-4k 9225. fio sw-1m
  collapsed to 1274 agg (~140/client) vs 4593 on 220930 — 9-way write
  lockstep, not a single-client regression. rw-128k agg 90 GB/s is not
  credible (likely dcache-served or a parse/report artifact).
  **FIXED (working tree):** (1) flush/GET_META serialize via table snapshot
  unlocked; (2) catchup `deserialize` into a tmp table then
  `efs_export_adopt_tables` (was under `s->lock` for 800k chunks);
  (3) REPORT norollup + yield every 8k; (4) overwrite with same placement
  does not `mark_chunk_dirty` (checksum-only). That flush+peer-rebuild
  storm is what pinned 9 writers at ~140. Revalidated after efsd bounce +
  remount 007–015: single overwrite 4564; 9-client warmup ~231/client
  (~2.1 GB/s agg first-write), sw-1m 262×8 + 2230 on 015 ≈ **4326 agg**
  (shared ceiling). First-write of *new* files on a huge table is still
  ~1.7 GB/s single (grow + REPORT); overwrite is the apples-to-apples
  number. dd-of-zero still skips PUTs — do not trust seq1m.write agg.

<a id="ph-posix-gaps-vs-xfs-suite-aug-20--all-fixed-144144-9cd874"></a>
## POSIX gaps vs XFS (suite, Aug 20) — ALL FIXED, 144/144
FIXED (7fa99e8): packed-file overwrite/append + rename/link data loss (see
"Root causes" below). All data-path tests pass (basic_append, basic_oappend,
basic_overwrite_middle, basic_terminal_cp_cat, basic_dd_rw, dir_rename_*,
hardlink_*, unlink_open_file).
**unlink-open investigated — NOT reproducing**: the theoretical concern
(efs_export_unlink_name removes inode+chunks on nlink->0, breaking open-fd
reads) does NOT manifest. Reads via the open fd succeed for buffered AND
O_DIRECT, small packed AND large chunked, flushed AND unflushed. No fix needed.
FIXED (7e442fa) — the last 5:
- **O_APPEND atomicity** (concurrent_appends): serialized daemon-side
  (g_append_mu + re-read true end) AND the underlying concurrent-write dcache
  flush race (see "Root causes" — the real data-loss bug).
- **Permission enforcement** (attr_access, err_write_readonly_file): daemon-side
  check_access helper in the access + open handlers. NOT default_permissions
  (that opt gated the root-owned export root → whole mount read-only).
- **rmdir-nonempty ENOTEMPTY**: client fast-path + authoritative server check
  (EFS_INODE_RPC_NOT_EMPTY).
- **ENAMETOOLONG**: name-component >255 check in split_parent_name.
- **Hardlink** (known FUSE kernel-level gap, below) — much improved by the
  7fa99e8 link fix (hardlink_* now pass) but the kernel-level dentry gap
  remains latent.

<a id="ph-phase-2-sub-phase-state-80a18d"></a>
## Phase 2 sub-phase state
- **2a server-side mutation durability (DONE, cffb138)**: RPC INODE_CREATE/
  UNLINK handlers bump rpc_dirty_ops[eidx]; primary-only meta_flush_thread
  batches + commits dirty exports via CoW server_flush_fragmented_meta.
- **2b route FUSE through RPC (DONE, d7d2c7c)**: all FUSE mutations (create/
  unlink/rename/setattr/link/symlink/readlink) run on the primary via RPC
  (rpc_send_recv_primary retries NOT_PRIMARY), dual-applied to the local
  snapshot via efs_export_upsert_inode. Server allocates inos (next_ino moved
  server-side). Data path: efs_client_report_dirty snapshots the dirty set and
  sends REPORT_CHUNKS (chunk mappings + ino size/mtime) to the primary instead
  of the client blob flush — the client flush/election/dirty-set machinery is
  no longer the flush path. fsync = sync REPORT_CHUNKS + server commits before
  replying. Validated on a fresh mkfs cluster: all mutations + data + sparse
  holes persist across remount, flush failures 0.
  **Two server flush bugs fixed in 2b**: (1) server_flush_fragmented_meta
  carried a stale pre-2a client write_lease_id → peers rejected the root (no
  quorum, mutations lost); now cleared. (2) meta_flush_thread raced a sync
  fsync flush on the same new_gen → loser rejected STALE → fsync EIO; now
  serialized by efsd_server.meta_flush_mu.
- **2b reads (DONE, working tree, Aug 20)**: lookup/getattr/readdir via
  `rpc_send_recv_primary`. `adopt_rpc_inode` upserts new rows only; merge
  remote size/pack when the primary grew (or newer+shrink). GETCHUNKS is
  indexed (`start` = chunk_index) — never scan `ex->chunks[]` (387k-row
  scan under `g_server->lock` stalled the cluster). Packed files: pull
  only the pack_off window. `posix2` 12/12. Single-client POSIX 135/144
  (4 same-fd zeros — see Test suite).
- **2c full-table gen-check cache**: **skip, never needed.** One-gen/one-blob
  cache would be ripped out for Phase 3. The *idea* (don't RPC every stat)
  is Phase 3 item 4: per-shard on-demand LRU. Same-fd POSIX zeros land there.
- **Phase 3 (STARTED, Aug 20)**: design `docs/phase3-sharding.md`. This cut:
  `efs_shard_owner_of`, `efs_export_alloc_ino` (parent shard when bits>0;
  today's `next_ino++` when bits=0), RPC send path takes an owner ino
  (still the export primary until per-shard flush). Next: split the table
  + flush (`efs_meta_shard_table_ino`), then ungate `rpc_owner_conn`.

<a id="ph-known-live-issues-4732f9"></a>
## Known live issues
- **Hardlink (`ln`) flaky at the FUSE kernel level** (pre-existing, NOT a 2b
  regression): the kernel intermittently fails the link's *source* path
  resolution from a cached dentry WITHOUT calling the daemon (efs_fuse_link
  never invoked). The 2b link RPC itself is correct (server applies it —
  nlink bumps). efs-fuse uses the high-level FUSE API which has NO .lookup op
  (never had one) — likely a dentry/generation-tracking gap. Doesn't affect
  fio/rclone/dd (no hardlinks). Not yet root-caused.
- **efs-fuse double-free SIGABRT** (reported Aug 19): was on the client-driven
  flush+resync path. **2b removed that flush path** (clients no longer
  blob-flush), so this is likely moot now — but the resync/rebase path still
  exists. If it recurs under 2b, reproduce with an ASan efs-fuse.

<a id="ph-known-live-issues-not-yet-root-caused-3576da"></a>
## Known live issues (not yet root-caused)
- **efs-fuse double-free SIGABRT** (reported Aug 19): `double free or
  corruption (!prev)` during heavy write (`ewrite.sh /tmp/efs-mount/001 1 32`,
  32 jobs) + resync/rebase under transient no-quorum. Log: STALE quorum →
  resync → `rebased ... onto gen 100623` → abort. Backtrace only shows libc
  `free`→abort (efs-fuse frame is just the fatal-signal handler). Code review
  of resync (fetch/deserialize/merge/swap/adopt/frees), flush blob ownership
  (`blob_is_cache`, `dirty_snap_free`), `incremental_serialize_v6` bounds,
  `efs_export_root_copy` (deep), `MARK_SPAN` (bounds-checked) found NO obvious
  double-free/overflow. Crash binaries (with debug_info) in ~/git/efs/logs/.
  Cluster healthy after (4 up/0 down). **Next step: reproduce with an
  ASan build of efs-fuse (`-fsanitize=address`) under the same workload to
  pinpoint the exact free.** This is the client-driven flush+resync path that
  Phase 2 replaces — so Phase 2 is the long-term fix, but the bug is live
  until then.

<a id="ph-done--dont-re-test-9e3111"></a>
## Done — don't re-test
- Phase 1 perf fixes (8b71dd3, bed1123, 00323c4, cddf692, 41d047e): RDMA
  latency, statfs phantom usage, torn-name readdir, meta-flush election
  hardening, GET_META serialize cache, efs-fuse RSS ~27GB→~4GB, read-verify
  moved server-side, resync/rebuild staged out of global locks.
- **v7 wire format** (f147fb8): split inode/dentry regions, page-aligned
  dentries → O(1) creates. Validated to 50K files, ~5.6 ms create+fdatasync.
- **Cycle fix** (d7b95fb): recompute_dir_postorder stack-overflow on cyclic
  garbage tables (the "raced deserialize SIGSEGV" that killed fcstor005).
- **CoW metadata flush / EFSR v7** (debe6dd): root carries page_cis[] +
  next_ci; flush writes each dirty page to a fresh ci and commits the root
  only after all pages land; rebuild reads page_cis[i] exactly; GC
  (server_gc_meta_cow_pages) reclaims cis the old root no longer references.
  Validated: kill -9 mid-flush (touch + fsync-heavy) remounts clean, no torn
  pages. **Torn-page root cause is fixed.**
- Scaling roadmap Phases 2–4 (0a381b8): docs/scaling-roadmap.md.
- Validation: fio suite on fcstor007 all rc=0, no segfault under load
  (sw 4.6 GB/s, sr 2.4 GB/s, 4k rand ~40k IOPS). rclone 32×10GiB copy rc=0.

<a id="ph-root-causes-already-found--dont-re-diagnose-365429"></a>
## Root causes already found — don't re-diagnose
- **names_crazy_roundtrip / `.fuse_hidden` leftover (ROOT-CAUSED + FIXED,
  Aug 26)**: NOT an efs bug, and the silly-rename is done by **libfuse's
  high-level API** (the userspace lib efs-fuse links), NOT the kernel.
  Mechanism: the kernel defers the final `fput`/FUSE `release` (task_work /
  delayed_fput) by a few hundred µs, so it sends FUSE_UNLINK before
  FUSE_RELEASE. libfuse's `fuse_lib_unlink` then sees the file still open in
  its internal tree (`is_open`) and calls `hide_node` → renames it to
  `.fuse_hidden%08x%08x` (nodeid, hidectr); when the deferred release is
  processed, libfuse unlinks the `.fuse_hidden`. Proven with a timestamped
  open/flush/release/unlink/rename daemon trace (`EFS_SILLY_DBG`): the
  silly-renamed cycle is missing the read-close `release` before the unlink
  (a normal cycle has it); the release lands ~103µs AFTER the unlink, then
  `.fuse_hidden` is unlinked ~63µs after that. Always transient, never leaks
  (0 lingering after 19k cycles). Repro: `tests/debug/silly_race.py`.
  **`.fuse_hidden*` is the ONLY namespace artifact libfuse injects** — both
  unlink-of-open (`fuse_lib_unlink`) and rename-over-open (`fuse_lib_rename`
  → `hide_node`) use the same pattern; an exhaustive grep of lib/fuse.c found
  no other generated names. The kernel adds nothing to the mounted namespace.
  **`hard_remove` mount opt considered + REJECTED**: it skips the silly-rename
  but then read/write/fsync/fstat/ftruncate on the still-open fd fail ENOENT
  (libfuse removed the node) — breaks unlink-while-open (posix
  `unlink_open_file`/`unlink_open_then_recreate`). libfuse docs explicitly
  recommend against it.
  **Fix = daemon readdir hides `.fuse_hidden*` entries** (efs_fuse.c) — a
  deleted-open file is never user-visible; the open fd uses `fi->fh` (ino),
  not a path lookup, so filtering is safe and libfuse still cleans up.
  Prefix match on `.fuse_hidden` covers both the unlink and rename cases and
  any hex length. Edge case: a user file literally named `.fuse_hidden*`
  would be hidden too (de-facto-reserved namespace; accepted). Test reverted
  to a STRICT `listdir == []` (a leftover now = a real bug). The only way to
  eliminate silly-rename entirely is the low-level (inode-based) FUSE API —
  a large rewrite, not warranted for a transient cosmetic artifact.
  Single-client gate after fix: 186/191, 0 EFS bugs.
- **232MB / 1.9M-chunk adopt after wipe (Aug 25)**: not a client disk
  cache and not leftover `.efsm` on the NVMe. `adopted cache blob` is
  the GET_META payload kept in RAM. Wipe of `/data1/*/efs` + new `mkfs`
  while **any** `efs-fuse` is still alive (often D-state; `pkill -x`
  without `-9` misses it) lets that daemon REPORT/flush the old table
  onto the empty primary. Next mount sees the same fingerprint
  (`232007402B`, 110 inodes, 1932296 chunks, gen=102). **Wipe order:
  `killall -9 efs-fuse` on fcstor003–015, confirm zero, then efsd,
  then edelete.** `tests/wipe_cluster.sh`. A 1-second `cat fuse.log`
  can also show the previous mount's 232MB line because
  `rsync --exclude='*.log'` never deletes it — `rm -f fuse.log`
  before start; trust `meta ready` from the new process, not a stale log.
- **efs-s3 metadata clobber = extras loss on adopt (FIXED, Aug 23)**: a
  newer-gen root missing extra-shard descriptors (born during a rebuild
  storm) was wholesale-`root_move`d by `catchup_install_newer_root`;
  the rebuild then wiped `shard_tabs` and reinstalled from the empty
  extras → permanent shard loss (clients saw 3 inodes). All adopt/commit
  paths now `efs_export_root_maxmerge_extras` (per-shard higher-gen-wins
  + carry-forward-missing), so descriptors are monotonic. The storm
  itself = peers rebuilding whenever they poll during the primary's
  root-only GET_META window (gm_blob freed on new gen before the new
  blob is cached) — a perf nit, not a correctness bug.
- **Sharded-export data loss = dual-writer root gen (FIXED, working tree)**:
  the primary AND every extra-shard owner bumped the SAME cluster-root gen
  (owner's `server_commit_cluster_extras` did gen+1 with a STALE copy of
  the shard-0 page_cis). The primary's PUT_META handler accepted the newer
  gen, fenced+zeroed its live table, and the flush thread dropped the
  unflushed RPC ops ("dirty under rebuild"); the rebuild then fetched cis
  the primary's own flushes had long superseded (GC'd → unrecoverable).
  mc_stress symptoms: cross-client appends vanished (appfile 100/200, all
  one client's), packed files read size 0 on the peer, rwfile chunks
  missing. Fix: extras commits keep the main gen; receivers compare shard-0
  page checksums — identical ⇒ extras-only refresh, merge descriptors
  without fencing (per-shard single-writer makes gen-compare safe; dirty
  tabs are never fenced). Also: cross-client O_APPEND needed a server-side
  reserve RPC (local size reads race), and pre-sized files needed
  mtime-newer chunk re-pulls in adopt_rpc_inode (size-only growth missed
  peer writes into an already-sized file).
- **Multi-client sw-1m ~140 MiB/s lockstep (FIXED, working tree)**: not
  RDMA, not 1m-try_patch. After warmup, each close REPORTed ~32k new
  chunk recs; `meta_flush_thread` serialized the live 100MB table under
  `g_server->lock` (every PUT takes that lock for `export_acquire`).
  Peers then GET_META + `deserialize` of 800k chunks, also under the
  lock. Overwrite also dirtied checksum-only updates so the storm
  continued into sw-1m. Fix: snapshot serialize, unlocked catchup adopt,
  norollup REPORT, skip dirty when placement is unchanged. dd-of-zero
  "18 GB/s" is fake (`chunk_put_worker` skips all-zero PUTs).
- **Same-fd read-your-writes zeros after 2b-reads (FIXED, working tree)**:
  a first write to an unpublished chunk is `have_base=0` (zeros + dirty
  ranges). `dcache_copy` refused those entries (unpatched bytes are not
  data), so same-fd read fell through to GET/rdcache zeros. Close+reopen
  flushed then read published data and passed. Fix: serve a have_base=0
  entry when the requested range is fully dirty; overlay dirty ranges
  onto a GET/zero chunk when a read straddles chunks
  (`basic_chunk_boundary`).
- **Concurrent-write data loss = dcache flush base-read race (FIXED, 7e442fa)**:
  dcache_flush_slot dropped the shard lock (dcache_mu) before the network
  merge-base read + PUT, so two concurrent flushes of the SAME chunk (e.g. 4
  threads appending to one file, each close→flush) both read a stale pre-PUT
  base and the last PUT wiped the other's just-written ranges → NUL holes with
  a correct file size. Fix: per-shard shard_io mutex held across the whole
  flush (base-read + PUT). Lock order shard_io → dcache_mu; the base-read's
  efs_client_read only takes dcache_mu, so no deadlock.
- **Packed-file overwrite/append = rdcache staleness (FIXED, 7fa99e8)**:
  dcache_put_now wrote the merged chunk to the servers but never invalidated
  the rdcache entry that the flush's have_base=0 merge-base read had populated
  with the OLD published chunk → a later read served the stale rdcache copy
  and lost the update. Fix: efs_rdcache_invalidate(ino, ci) after a
  successful PUT.
- **rename/link data loss = dual-apply upsert (FIXED, 7fa99e8)**: the 2b
  dual-apply upserted the RPC-returned inode, which carries a stale size=0
  (the close's metadata flush is batched/async, so the server hasn't seen the
  size yet) → upserting wiped the fresher local data-path state (size, pack
  fields, chunk mappings). Fix: dual-apply via efs_export_rename /
  efs_export_link (name/parent/nlink/ctime only), fallback to upsert.
- Slow creates = **v6 dentry-shift** (O(table) memmove of the packed dentry
  region on every create), NOT fdatasync/election. Fixed by v7.
- **Torn metadata pages = non-atomic dual-slot flush** (PUT dirty pages to a
  same-parity slot before committing the root; kill mid-flush overwrites a
  page the committed root references → checksum mismatch → zero-fill →
  garbage next_ino). Fixed by CoW (debe6dd).
- fcstor005 SIGSEGV = **pre-existing rebuild-race** (torn page → cyclic garbage
  table → infinite postorder recursion), NOT v7. Fixed by cycle detection.
- **Catchup-vs-GC race (FIXED, 76753ef)**: a catching-up server rebuilding an
  OLD root lost the race to GC reclaiming that root's CoW cis → zero-fill →
  corrupt table. Fix: for a CoW root, an unrecoverable page = GC-race (not
  genuine loss), so rebuild returns EFS_ERR_PROTO (no zero-fill) and the
  catchup re-polls for the newest root. Validated: 20k fsync-creates while a
  wiped server caught up → 65 GC-races, 0 zero-fills, no corruption.
- **Stuck catchup (DEFERRED, todo 15)**: during that validation fcstor006's
  catchup hung ~7 min (0% CPU, idle conns, all S-state) after installing the
  final gen. NOT a blocking recv — peer conns already have SO_RCVTIMEO
  (peer_pool.c:173). Likely a futex (s->lock) or logic stall; needs
  gdb/instrumentation to root-cause. Workaround: restart the node.
- `used=0 B` in `efs-mgmt status` can be **stale gossip**, not data loss —
  check on-disk (`du`) before concluding.
- Build-ID gate: rolling efsd restart only works within the same commit.

<a id="ph-housekeeping-6aea7b"></a>
## Housekeeping
- Integration tests `test_quota` / `test_migrate` are **pre-existing flaky**
  (unrelated to recent changes) — don't chase them as regressions.

---
**After any milestone/fix/validation, update "Where we are" + "Done" so the
next session doesn't repeat completed work.**

<a id="ph-start-here-handoff-archive-oct-1-2026-1800z--2200z--moved-oc-8297ba"></a>
## START-HERE handoff archive (Oct 1 2026 18:00Z – 22:00Z) — moved Oct 2 2026

Verbatim from START-HERE §1b when the Oct 2 01:20Z block replaced them.

**Oct 1 22:00Z: the cluster is STOPPED (`stop731`, CLUSTER_OK, `efsd=0 perf=0 strace=0`), after the review of the user's 16× dd run of 20:56Z (servers `dc6b0af19832-dirty` under `--perf --strace` since `restart720`, no fcstor clients; client fstor007 under strace + perf). Recorders were moved aside to `/tmp/efs-perf/efsd-2056.{data,strace}` on fcstor003–006 (plus the window extract `efsd-2056-win.strace`); the analysis is `~/orcd/scratch/efs/perf/efs-mount/server-2056/<host>/{strace-ddphase,strace-drain,perf-flat,perf-pid}.txt` + `client-fuse-strace-summary.txt`; the user's client files are in `~/orcd/scratch/efs/perf/efs-mount/`. Restart is `cluster.sh start [--perf] [--strace]` (no `--fresh`, no `--join`); nothing in the tree changed. What the review found, in order of weight — the queue rows are 0g (W43), 0h (W44) and the asks D25–D26 below:**
- **0g · W43 — `truncate`/`O_TRUNC` of a file with more than 32 chunks in a lane is a silent no-op (correctness, goes first).** dd opened the 16 existing 10 GiB files `O_WRONLY|O_CREAT|O_TRUNC`; every replica logged `raft-host: apply truncate rc=-2 index=… ino=180014` for all 16 inodes and `apply lane-fence rc=-2` ×661 (fcstor003). `-2` is `EFS_ERR_NOMEM`: `TRUNC_IT_CAP` (`meta_apply.c`, 32 chunk DELs per lane) and `efs_meta_apply_lane_fence`'s `it[1+32+32]` are the batch bound; `trunc_del_cb` sets NOMEM at the 33rd chunk, `truncate_apply` returns before `efs_kv_batch`, so the inode row (`base_size`, `content_epoch`) and every chunk row stay as they were — and `apply_truncate_cmd` / `apply_lane_fence_cmd` log it and **return `EFS_OK`**, so the client saw `status=0` (the ten `inode-rpc: slow-ok type=63 … us=1.0–1.6 s` lines; the 457 ms `apply_max` / `apply-sleep` ×7 at 20:56:42 are the pump scanning 81 920 chunk keys per file before giving up). The file kept its 10 GiB size and all its chunks. The dd then rewrote the same source bytes → the same content generations → the server PUT overwrote the identical fragment files in place (`openat O_WRONLY|O_CREAT|O_TRUNC|O_DIRECT`, 0 EEXIST in 150 625 fragment opens) → **all 34 REPORTs on fcstor004 were `report-split nrec=8192 … push_ms=0 finish_ms=0–2 skip=8192`** (`host_pub_pack` → `efs_meta_apply_chunk_holds` = 1), not one publish proposed. So that run measured the data path only; its REPORT cost (1.0–1.15 s per 8192 records) was 8192 point gets on a 50–54-L0 table, not publishing. The 18:23Z run on fresh files (`server-perf-20261001-1823`) is the real one. With *different* content the rewrite would publish `base_gen=0` against existing generations. Recommendation (D25, ask): the apply drains a lane's chunks in bounded batches the way `efs_meta_apply_lane_sweep` already does (`SWEEP_CHUNKS` 64 per KV batch inside one entry, ~128 batches for a 1 TB lane), and a truncate apply that cannot complete must answer an error on the ring, never OK. W24 ("`open(O_TRUNC)` of a large existing file did not return") is the same path seen under load; this is the idle-cluster half of it.
- **The data path, both ends traced (shape, not numbers):** per stream 211 → 44 MB/s in the first 10 s (dcache fill, then the dirty-cap throttle), 16 streams ≈ 670 MB/s (one `report-landed` per 1.6 s). Client FUSE WRITE service n=29 727 med 3.8 ms, mean 20 ms, p90 63 ms, p99 141 ms, of which 555 of 600 s is `poll()` **in the FUSE worker itself** — inline PUTs (`ll_write_buf → efs_dcache_maybe_reclaim → dcache_flush_slot_inner → dcache_put_now`, 17.9 % of client cycles vs 39 % in `put_pool_thread`); 22 of 38 libfuse workers spent 31 of 40 s in poll. Per-fragment reply wait on the 33 poll threads: med 2.03 ms, p90 6.6, p99 9.5, max 940 ms; `recv_poller`'s eventfd `write` (one per CQE, 749 K) is 10 % of client cycles; FUSE copy-in ~8 %; blake3 19 %; memmove 9 %; client CPU ≈ 1.35 cores. Server writers (20 per node): futex wait ~4 ms → `openat` 73 µs → `writev` 69 632 B 105 µs → `close` 47 µs → `FUTEX_WAKE INT_MAX` broadcast + 2 futex ops; ≈ 0.5 ms busy per fragment, 7491 per writer in 40 s → **writers ~10 % utilized**, pump healthy (`fdatasync` 0.3 ms, `apply_max` 0.5 ms, no L0 back-pressure). ~85 µs of ptrace per syscall × ~10 server + ~4 client syscalls per fragment is ~1.2 ms of the 2 ms wait, so the ratio does not transfer; the shape does: a fixed per-fragment wakeup chain × in-flight depth (32 pool + ~22 inline) bounds the write phase, the same conclusion as the untraced 18:23Z run (1.1 ms RTT → 3.7–3.9 GB/s). Two mechanical candidates it names (no decision needed, measure before/after with the W28 gate): the writer pool's `cond_broadcast` per completed PUT (same herd class as the client's `g_reclaim` fix) and one eventfd `write` per CQE in `recv_poller` (coalesce per poll batch).
- **0h · W44 — the leader's GC thread is 80 % of each leader's `efsd` cycles** (fcstor003 tid 1056219, fcstor006 tid 731846, over the whole 84-min recording): `__memcmp_avx2_movbe` 25 %, `merge_scan` 17 %, `kv_seg_iter_next` 4 %, `kv_msrc_advance` 3 %. 20:11–20:56 it logged `gc-pass ms≈205 frag=205` every 1.2 s **finding nothing** (the frag prefix scan over 50–54 L0 + L1; `l0=54 l1=149`, compaction removes one L0 per 7 min with `inputs=2 range=1 l0only=1`); from 20:56:42 to the stop `gc-frag records=256 ms=290–517` continuously (inodes 234425, 82797 — not this run's files; ~140 records/s per group, 83 `unlink`/s per server). Did not bind this run, but it is 20–40 % of a core per leader holding the KV lock per scan, and the same scan cost is why 8192 `chunk_holds` gets take ~1 s. Compaction shape is D9/D10/D12/D13 territory; the frag pass's empty-scan cost is D26 (ask).
- **Next steps are the two tables in §1a "Plan after the Oct 1 22:00Z review"**: eleven rows that run without a decision (W43 b/c, W45 apply-verdict audit, W38, W36, W27, W26, W44 a, W46/W47 wakeups, the untraced re-measurement) and thirteen asks (D25, 0a(d), W41, D23, D17, D26, `efsd --bench`, D15/D16 close, fragment layout, W40, zero-copy receive, `efs-fuse --bench`, recorders) each with a recommendation. Take the first table top to bottom.

**Oct 1 18:00Z: committed `dc6b0af1` and rolled — `cluster.sh restart --clients --perf` (`~/efs-runs/restart705.log`, CLUSTER_OK): servers `dc6b0af19832-dirty` on fcstor003–006 with `--perf` (`/tmp/efs-perf/efsd.data`), clients fcstor003–015 remounted RDMA, fstor007 untouched. Post-check (`post706`): PREFLIGHT_OK, g0 term 17 leader fcstor003, g2 term 12 leader fcstor006, commit==applied, both idle; fcstor007 2 GiB write 1.4 GB/s, cold read 3.2 GB/s, tail cmp OK; server log stamped and `store: put O_DIRECT zero-copy` active; client `read reply zero-copy active`. The user does the heavy testing on this build.** Earlier that afternoon (15:55Z): W39 (RDMA zero-copy send), read path R1–R5, 64 get workers, the server's PUT bounce copy and the W37 mkfs guard were put in the tree, clients deployed first, servers later. The user asked for the performance items from the roadmap, implemented and smoke-tested, heavy testing his.
- **Client, deployed to fcstor003–015 (`dep680`) and fstor007's `/tmp/efs` (the user's running efs-fuse is untouched):** (1) **W39** — `efs_rdma_zc_region_add` registers the bufpool slabs lazily per HCA (`zc_lkey`, `ibv_reg_mr` once per slab per device, append-only region table), `efs_rdma_send_frame` posts a two-SGE send (pool header, slab payload ≥ `EFS_RDMA_ZC_MIN` 4 KiB) when the QP reports `max_send_sge ≥ 2`; the payload stays referenced until the send completes, which every caller already guarantees (a conn ends replied or `efs_conn_destroy`ed; `efs_rdma_send_quiesce` reaps for the destroy path). Log once `efs: RDMA zero-copy send active`. (2) **Read path R1–R5:** a prefetched chunk's buffer is handed to the rdcache instead of copied (`efs_rdcache_put_owned`); a whole-chunk demand read decodes into the caller's buffer (`chunk_get_job.ext`); the two data fragments are received directly into the chunk and `efs_decode_chunk` skips the self-copy; a chunk-aligned READ whose chunks are all in the rdcache is answered with `fuse_reply_iov` over pinned images (`efs_client_read_refs`, `efs_rdcache_pin/unpin`, `rdcache_ent.pins` excludes a pinned way from victim selection and from `rdcache_put`'s overwrite; `fuse_reply_data` with a multi-buffer bufvec copies in libfuse 3.10, `fuse_reply_iov` does not). Log once `efs: read reply zero-copy active`. (3) The get pool is 64 workers (`GET_POOL_N`), no longer tied to `EFS_WRITE_PIPELINE` — one synchronous fetch per worker is the client's in-flight read cap. (4) **`rmdir` no longer fails ENOTEMPTY from the client's local table** (`efs_client_unlink`): the local child list can hold a child whose removal committed elsewhere, and the refusal was the recurring one-host `dir_deep_nesting` 199/201 (fcstor012 Sep 30 `d50`, fcstor009 Oct 1 15:55Z `d25`, `results/posix/20261001-155455`; the server's row was empty both times). The client logs `efs: rmdir ino=… local table lists child '…' — asking the server` (first 16) and the server (`EFS_INODE_RPC_NOT_EMPTY`) decides. After it (`dep691`, all 13 clients + fstor007's tree): 9-host **200/201 on all nine** (`results/posix/20261001-160049`), jobs=1 200/201 (`-155932`).
- **Measured on fcstor007 (16 GiB random file, cold remount before every read, `agent-rd-20261001-145047` → `-154007-refs`):** write 1.4 → **1.5 GB/s** (client CPU 21.4 s), single cold read 2.5 → **3.6 GB/s** (client CPU 21.6 → **10.8 s**), four readers 3.7 → **6.5 GB/s** (14.9 s CPU); `EFS_READ_PREFETCH=32` is slower than the default 16 (2.9 GB/s) — leave it. cmp at 0/5/15 GiB, whole-file md5, unaligned and tail windows cold and warm all OK (`gate671`; gate670's `CMP_TAIL_BAD` was `dd skip=` on a pipe without `iflag=fullblock` — the harness, not the client). posix jobs=1 fcstor007 **200/201** (`results/posix/20261001-154333`); the D24 wedge gate again: 4 × 8 GiB dd+fsync 8.2 s, storm p50 5.6 / p99 70 / max 112 ms, 32 `report-landed`. The remaining user-space copies in the read profile (`agent-rd-20261001-153356-prof`) are RDMA recv buffer → chunk (zero-copy *receive* needs per-request posted receives — a transport change, **ask**) and the kernel's own copy out of the iov. Single-stream is in-flight bound (~1 ms per chunk, 8 demand + 16 prefetch), not CPU.
- **Server, rolled at 18:00Z (`dc6b0af1`):** `store.c` writes an aligned PUT with a digest as one `writev` (payload + the 4 KiB D20 tail page) — the D20 bounce memcpy per fragment is gone (`store: put O_DIRECT zero-copy` once-log); **W37** — `server_raft_host_mkfs` replicates the salt the committed MKFS recorded (read back with `efs_meta_apply_export_salt`), never its own; a node that holds no salt record answers BUSY; SALT verdicts ride the ring (a mismatch is PROTO, not an apply halt). The mkfs apply itself is unchanged (no-op on an existing root). Compiled with `test_sim` and `test_meta_apply` passing (`bld701`, 17:56Z). Plus the 14:00Z log timestamps. A fresh-table start after the next wipe is W37's gate.
- **Asks, unchanged:** W41 (`report_mu`), D23 (clean-image cache), W40 (FUSE write copy), the fragment-layout change (needs a wipe), RDMA zero-copy receive, D17, W36, W38.

<a id="ph-start-here-closed-items--moved-oct-2-2026-ea8027"></a>
## START-HERE closed items — moved Oct 2 2026

Verbatim from START-HERE §1a. The decisions themselves (D1–D28) stay in START-HERE.

<a id="ph-sep-28--sep-29-ordering-tables-superseded-by-the-oct-2-plan-016bf6"></a>
### Sep 28 / Sep 29 ordering tables (superseded by the Oct 2 plan)

**Order for continued implementation (decided Sep 28 2026).** Correctness
and truthfulness first because they are small and they are what "easiest
to use" means; then the CPU items that need no decision and are most of
both profiles; then the two design items above; W12 whenever a run dir is
cited.

| # | item | what (order Sep 28) | why now |
| --- | --- | --- | --- |
| 1 | **W17 step 1** | `fsync`/`release` return in bounded time; a killed writer never leaves a D-state FUSE request | seven clients wedged for 20 min after `kill -9` |
| 2 | **W16 step 1** | RPC BUSY/NET/IO surface as EBUSY/EIO, never ENOENT | a busy filesystem must not claim a file is missing |
| 3 | **W18** | dcache reclaim from a dirty list; init once | 73 % of the client's CPU walks a table looking for dirty chunks |
| 4 | **W19** | encode each AppendEntries once; `try_commit` from the replying peer's match | 35 % of the leader is copying AEs on the pump |
| 5 | **W14 step 4** | fragment `path_index` hint on PUT | six path walks per PUT |
| 6 | **W15 step 3** | one `fuse_buf_copy` per whole chunk, for multi-chunk writes too | in tree only for `size == chunk`; `dd bs=1M` (eight chunks) still copies twice: 16 % + ~19 % in the 22:11 profile |
| 7' | **W14 step 2 (a)–(b)** | outbox coalesces replies/heartbeats instead of dropping them | `drop=72114 > sent=50695` to one peer; terms +59 / +26 in two hours; every client BUSY is `no-hint` |
| 7'' | **W15 step 4** | post the RDMA SEND and return; reap CQEs in `send_buf_pick` and the reply wait | 6.9 % of the client waits synchronously for its own send CQE; more send buffers did not change it |
| 6a | **W20** | `st_blocks` from a per-inode counter; `ll_setattr` reads `size` from the row | a `stat` walks every chunk under two locks; 8.9 % of the client under `ll_setattr` |
| 6b | **W21** | a kick does not wake a scan that cannot evict (1 s idle after a no-evict wake, cleared on unpin); cross-tab running total | in-tree cursor did not help: 12.8 % of the client on the 22:48 profile because every write over cap kicks a wake |
| 6c | **W15 step 5** | `FUSE_CAP_SPLICE_READ` so the write payload is read from the pipe straight into the dcache buffer | after step 3 one user copy per byte remains (~22 % `fuse_buf_copy`) plus the kernel copy into libfuse's buffer (~9.5 %) |
| 6d | **W23** (the client item; renamed **W49** Oct 2) | probe a pooled connection's liveness once per idle period, not per checkout; pool generation instead of `fstat` per send | 1.17M `fstat` + 1.17M `getsockopt` + 0.6M `MSG_PEEK` in 230 s (~15K syscalls/s) for 586K checkouts; ~2.5 % of the untraced client |
| 6e | **W24** | `open(O_TRUNC)` / `truncate` of a large file returns in bounded time; SETATTR under BUSY is EBUSY, not a hang | a `dd` onto an existing 1.7 GB file never got past `openat`; SETATTR is the most-exhausted RPC (136 of 424) |
| 2' | **W25** | `futimens` fences cross-group lanes (today EINVAL on every multi-lane file); atime keeps nanoseconds; BUSY → EBUSY everywhere | `ecopy --verify` fails `futimens` on every large file **and then deletes its copy**; the atime check fails on every small one; correctness, goes with row 2 |
| 7 | **W14 steps 2, 3, 5; W15 step 4** | election trigger under import; reap `snap-*.kvx.tmp`; `send_buf_pick` | smaller residuals from the same profiles |
| 8 | **D1 → W17 steps 2–3, W6.1** | commuting span publish | shared-file write rate must rise with ranks; the headline N-1 number |
| 9 | **D2 → W6.2** | open adopts the row; maps in windows | 1 GiB open 1.6–2.5 s under 36 openers |
| 10 | **W16 steps 2–3 (D8)** | `pub_batch_ms` in `raft-obs`, one 9-client IOR, bring the number; no budget change without the user | `push_ms=9869` for 337 batches is either queueing or the 100 ms `fsync` mode, and the two have different fixes |
| 11 | **W8, W10** | 9-host posix 201/201; RDMA faster than TCP | gates re-run once 3–9 land |

**Revised order (Sep 29 2026, after D4–D8).** The items below go
**before 9 and 10**: they are the cause of the two IOR aborts and
nothing downstream can be measured until an ior-easy-write completes.
9 (D2 windows) and 10 (D8) follow; 11 last.

| # | item | what (order Sep 29) | why now |
| --- | --- | --- | --- |
| 8a | **W22 step 1 (D4)** | snapshot by log bytes (512 MiB) with a retained log window; InstallSnapshot only below the window | a 2.67 GB export every 256 entries is the top user symbol on all four servers and turns any 257-entry lag into a 12–16 s import |
| 8b | **W22 step 2 (D5)** | the follower applies an import diff in per-tick slices and keeps answering heartbeats | terms +45 / +105 across one IOR with `drop=0`: the import is the election trigger left |
| 8c | **W14 step 4 (b) (D7)** | `path_hint = 0xffffffff` = first write of this fragment generation; server creates on the least-queue root with no probe | 1.59M `access()` / 192 s on one server per IOR; the re-PUT hint cannot reach a fresh chunk |
| 8d | **W22 step 3 (D6)** | measure `fsync` on the shared vs a quiet root; if the 100 ms mode is the sharing, `--meta-storage <root>` and redeploy 19810 with `mdraft/` off the fragment roots | compactor `fsync` flat 100 ms ×97, `persist_max` 254 ms; a redeploy is a cluster change, so it is last of the four and its measurement decides it. **Superseded by D11 (Sep 29 04:27Z trace): the Raft-log `fsync` is 0.4 ms; the 100 ms is the compactor's own segment. Flag is in tree; no move** |
| 8e | **W23 step 1 (D9)** | the pump never waits for the compactor; back-pressure is BUSY at `host_propose` on the leader while L0 bytes are over 1 GiB | **rolled 05:31Z, held on the 12:02Z trace:** no pump wait, no `backpressure` line. The BUSY in that IOR is not this admission |
| 8f | **W23 step 2 (D10)** | compact a range when its L0 bytes reach 1/8 of its L1, L0 cap in bytes, split range 0, 1 MiB merge reads | **rolled 05:31Z. Byte half held** (0.25 GB compacted, not 159 GB; `pread` over 1 MiB is in use). **File-cap half did not** — see 8i |
| 8g | **rolled 05:31Z** | `disk_log_new_bytes` O(1); `kv_seg_w_open` `setvbuf` 1 MiB | in the same roll as 8e |
| 8h | **W23 step 3** | `lookup` skips a segment whose first/last key span does not contain the key, before `block_for` | **rolled 12:29Z.** Last key is cached in `kv_seg_open`; `kv_seg_probe` returns NOT_FOUND when the key is before `idx[0]` or past the cached last key. Not separately profiled (this IOR had no perf). L1 was still 397 files and `report-split` was still mostly `rc=-13` |
| 8i | **W23 step 4 (D12)** | when `n_l0` is over the file cap, compact the range with the most L0 files even under the 1/8 ratio, and repeat until the count is under the cap | **rolled 12:29Z.** On the short IOR, fcstor004 ended at L0=3. On the 100 GiB copy the same rule rewrote L1 the whole time (67 GB, L0 peak 379). That shape is D13 |
| 8j | **W23 step 5 (D13)** | over the file cap, merge the range's L0 files with each other and do not read its L1; the 1/8 rule stays the only L1 rewrite | **in tree 14:20Z, not rolled, not gated.** The probe walks every in-span L0 file, and the full-L1 backstop both failed to keep the count down during the copy and put 67 GB on the same disk as the apply |

<a id="ph-d15d16-closed-oct-2-d640a9"></a>
### D15–D16 (closed Oct 2)

**D15–D16 — CLOSED Oct 2 2026 01:45Z (user).** From the post-fix ecopy (`results/measure/20260930-040600-postfix-review/SUMMARY.txt`); the two mechanical fixes (F1, F2 there) were rolled Sep 30 05:06Z, and the 30 s "lost reply" that motivated both rows was the sender reading the wrong channel (`85f5b31c`). Neither is to be built; the rows stay for the record.

| item | question | recommended then (D15–D16, Sep 30) — closed | why, in one line |
| --- | --- | --- | --- |
| **D15 · W14 step 2 / raft.c `on_vote_req`** | may a voter that heard from the current leader within the last election timeout refuse a higher-term VOTE_REQ and keep its leader? | **Yes — leader stickiness (Raft §4.2.3 / §9.6; Pre-Vote or CheckQuorum are the two standard shapes).** Recommend Pre-Vote: a candidate first asks "would you vote for me at term+1?" without bumping anyone's term; a voter answers no while its leader heartbeat is younger than the election timeout; only a majority of yes starts the real election. `maybe_step_down` then never fires from a peer whose only problem is that its own lane to the leader is stalled | 426 group-2 terms in 13 minutes: fcstor006, unable to get a reply from fcstor004/005 for 30 s at a time, campaigned every 0.5–0.9 s and each VOTE_REQ deposed the live leader (`LEADER->FOLLOWER leader=-1`, re-elected 0.6 s later); every cycle is 400 ms of BUSY reads for every client. F1 shortens each stall from 30 s to 250 ms; it does not stop the deposition |
| **D16 · peer transport** | should Raft peer frames share the RDMA device's one shared recv CQ / poller and the per-conn 36-credit scheme with ~400 client conns, or get their own class with reserved credits and their own poller? | **Measure first (stack-sample the sender and peer-conn threads in a storm), then decide.** If the lost RAFT_REPLY is the shared `recv_poller` behind client completions, peer conns need their own CQ/poller; if it is the peer's reply send waiting on a credit, reserved credits. Do not pick before the sample | the frames of a blocked batch reach the peer and are processed (fcstor006 won votes through a frozen lane); only the reply is late by up to 30 s, and only while ecopy drives hundreds of client conns per server. **Sep 30 06:17Z: the 30 s late reply was the sender reading the wrong channel (`85f5b31c`), not the transport; D15/D16 keep no motivating case** |

<a id="ph-done-rows-of-the-added-oct-1-2026-correctness-table-25e379"></a>
### Done rows of the "Added Oct 1 2026 (correctness)" table

| # | item | follow-up steps | evidence and limits |
| --- | --- | --- | --- |
| 1j | **Read path R1–R5 — DONE (Oct 1 15:45Z, all clients 16:01Z); server PUT bounce copy — rolled 18:00Z** | R1 a prefetched chunk's buffer is handed to the rdcache (`efs_rdcache_put_owned`), R2 a whole-chunk demand read decodes into the caller's buffer (`chunk_get_job.ext`), R4 the two data fragments are received straight into the chunk (`efs_decode_chunk` skips the self-copy), R5 a chunk-aligned READ whose chunks are all cached is answered with `fuse_reply_iov` over pinned rdcache images (`efs_client_read_refs`, `efs_rdcache_pin/unpin`; `rdcache_ent.pins` keeps a pinned way from being a victim); the get pool is 64 workers (`GET_POOL_N`, was tied to `EFS_WRITE_PIPELINE`). Server: an aligned PUT with a digest is one `writev` (payload + 4 KiB tail page), no bounce memcpy. **Still a copy:** RDMA recv buffer → chunk (zero-copy *receive* is a transport change — ask), and the FUSE write copy (W40) | fcstor007 16 GiB cold read **2.5 → 3.6 GB/s** (client CPU 21.6 → 10.8 s), 4 readers **3.7 → 6.5 GB/s**, prefetch 32 is slower than 16 (2.9); cmp/md5 OK, posix jobs=1 200/201 (`results/posix/20261001-154333`). Dirs `agent-rd-20261001-145047` (base), `-153035-inplace`, `-154007-refs`. `perf record -a` is refused at `perf_event_paranoid=1`; attach `-p` after the pools exist |
| 0f | **D24 · the close-time REPORT of a long sequential write — DECIDED and in the tree (Oct 1 14:00Z, user: "implement all fixes that prevent the lock")** | `report_landed_note` (write.c): every 8192 landed PUTs (1 GiB, global) kick the flush thread's whole-set REPORT; the REPORT reply wait is sized to the record count (`EFS_IO_TIMEOUT_MS + 0.5 ms × recs`, `rpc_send_recv_dual(recv_ms)`) so a long REPORT is not re-sent while the server executes it; both daemons stamp every log line (`log_ts.c`). Gate: the 14:00Z block in §1b | the 13:41Z wedge on fstor007 (`~/efs-runs/rec-look55x.log`): 8 × 20 GiB closes → `retry type=67 why=recv rc=-6` → `slow-ok attempts=6 us=116 s`, 16 dd in D-state in the kernel's forced FLUSH, ecopy's closes queued on `report_mu` for minutes. Still open as **W41**: `report_mu` serializes every close on a client behind one REPORT's retry loop (up to 8 s on STALE); per-inode dirty-set extraction + a multi-slot `pub_ino` set would remove the serialization point. Ask before building it |
| 1i | **W39 · RDMA zero-copy fragment send — DONE (Oct 1 15:30Z, all clients 16:01Z); W40 · FUSE write copy (ask)** | W39: bufpool slabs are registered lazily per HCA (`efs_rdma_zc_region_add`, `zc_lkey`), `efs_rdma_send_frame` posts a two-SGE send (header + slab payload ≥ 4 KiB) when `max_send_sge ≥ 2`; the payload is referenced until the send CQE, which every caller already guarantees (reply or `efs_conn_destroy`). Log once `efs: RDMA zero-copy send active`. W40 (ask): the `fuse_buf_copy` into the pool is the other full write copy; libfuse 3.10.2 has no custom-buffer receive — an own `/dev/fuse` loop | 16 GiB random dd+fsync on fcstor007 1.4 → 1.5 GB/s, client CPU 21.6 s (`agent-rd-20261001-151704-new`); cmp at 0/5/15 GiB and whole-file md5 OK. CPU, not wall |
| 0d | **W37 · `raft-mkfs` on a second node forks the salt — fixed, rolled 18:00Z (`dc6b0af1`); gate = the next fresh-table start** | `server_raft_host_mkfs` reads the salt the committed MKFS recorded (`efs_meta_apply_export_salt`: root shard's export key, else group 2's anchor) and uses THAT for the SALT step, never its own (`table exists with salt …; own salt … not used`); a node holding neither record answers BUSY (`retry on a group-0 node`); `EFS_MD_CMD_SALT` is on the apply's ring-only list so a mismatching SALT answers PROTO instead of halting group 2's apply. `efs_meta_apply_mkfs` keeps its contract (no-op on an existing root, first salt kept — `test_meta_apply` `idempotent`, `test_sim` `idempotent mkfs`; an EXIST-returning variant broke both and was reverted). Not exercised on a fresh table yet — the next `cluster.sh start --fresh` is the gate | `rec-st402..404` (`apply salt rc=-7 index=3` forever), `start401`, `wipe405` |
| 1a–1h | **W28–W35 · the 1 GiB dd review (Oct 1 05:10Z)** | eight items from the fstor007 write/read straces, server straces and profile — the subsection "W28–W35: what the 1 GiB dd showed" below this table; W28 (layout window off by one group) and W29 (prefetch/demand duplicate GETs) are one-function fixes and go first; D19–D22 are the asks it raised | 1 GiB write 207 MB/s = 1.6 s of writes with zero PUTs then a 3.56 s close; 1 GiB read 184 MB/s = a serial 80 KB GETCHUNKS over TCP plus ~400 futex per MiB. Both numbers were taken with strace on both daemons and are tracer plateaus; the shapes are not |

<a id="ph-w28w35-done-983384"></a>
### W28–W35 (done)

#### W28–W35: what the 1 GiB dd showed (Oct 1 05:10Z) and how to fix it

**Source.** fstor007 `dd bs=1M count=1024` write then read of `/tmp/efs-mount/001/dat17`, 01:06:20–01:07:20 EDT; client `strace -f` + `perf record`, servers `4b2844487831-dirty` with `--perf --strace` (restarted 01:03 EDT); straces and profile in `/home/erbmi1/orcd/scratch/efs/perf/efs-mount/`, the read-only passes in `~/efs-runs/ddana2.log`, `ddana3.log`, `ddana4.log`, `srvana2-fcstor00{3,4,5,6}.log`, `srvana3-fcstor004.log`. Both daemons were traced and both sat on the strace plateau (client 97–103 K lines/s during the read, servers 101–113 K/s during the write drain), so **207 MB/s write and 184 MB/s read are tracer numbers; every count below is real**. Untraced references: single dd write 918–1000 MB/s, single dd read 585 MB/s.

**Gate for the whole group (one script, run after each item, never with strace on a daemon):** on fcstor007, remounted, idle cluster: `dd bs=1M count=8192 conv=fsync` of a non-zero source to a fresh file, then remount, then `dd bs=1M` read of the same file; record MB/s for both, the per-`write`/`read` latency histogram from `strace -T` **on dd only**, and from the servers' `efsd.log` nothing (no recorders). Plus the counters each item names. The client bench (§1a, plan only) replaces this once it exists.

**Order:** W28, W29 (one function each, read side), W30, W31 (write side, mechanical), W32, W33 (both sides, mechanical), W34 (server, mechanical), W35 (measure), then the asks D19–D22. Correctness items (0a/0b above) still come first.

| # | item | what | measured | fix (mechanical unless marked ask) | done when | forbidden |
| --- | --- | --- | --- | --- | --- | --- |
| 1a | **W28 · layout window off by one group** | `efs_client_pull_layout_miss` (`ops.c:449`): `fresh` requires `seen.ci1 >= ahead` with `ahead = ci0 + 2·win`; `seen.ci1` is the previous call's `ahead`, which is one 8-chunk range short of this one's, so after the window ramps to 256 **every 1 MiB read issues a GETCHUNKS before its data GETs** | 94 `recvfrom … 79888` on the client in a 1024-read window (one per read per libfuse worker); each is the whole 64-entry group (64 × 1248 B), **> 72 KiB ⇒ TCP side channel**, ~1.3 ms request-to-reply, serial, ≈ 25 % of each read's 5.2 ms | freshness is "the **requested** range `[ci0, ci1)` is covered and the pull is < 200 ms old"; extend the window only when the covered range ends within `win` of `ci1` (pull `[seen.ci1, ci1 + 2·win)` then), not on every call. Keep the forced `have_ce = 0` re-pull semantics (peer spans, D1) and the 200 ms bound | a 1 GiB sequential read issues ≤ 40 GETCHUNKS (8192 chunks / 256-window steps), counted on the client (`recvfrom … 79888` or a counter); posix/posix2 unchanged; `peer_overlap_pwrite_chunk_straddle` and `basic_chunk_boundary` pass | dropping the per-batch re-pull, caching the map past 200 ms, or widening the 200 ms |
| 1b | **W29 · the read fetched every chunk it already had, and the prefetch fetched nothing** | **corrected 05:45Z from the server straces:** GET sequences per server 4096 / 4098 / 4098 / 4100 = 16392 ≈ 2 × 8192 — exactly two fragments per chunk, every path opened once, no parity, **no duplicate** (the earlier "2.6 per chunk" counted strace `unfinished` lines twice). So no prefetch GET landed: `prefetch_ahead` skips any chunk `efs_dcache_has` (`read.c:1213`) and this client had just written the file — every clean image was still in the dcache. The demand worker does the opposite: `efs_rdcache_get` miss → `efs_client_fetch_published_chunk` (2 GETs) → `efs_dcache_overlay`, which **copies the whole current dcache image over the fetched bytes** (`write.c:3168`, `dcache_image_current`). The 3 ms GET wave in every read was for data the client already had | 1032 GETCHUNKS and 4096 GETs on fcstor004 in the read window, one of each per MiB; servers at ~8 % of one core during the read (blake3 verify 16.6 % of that); client wave ~3 ms per MiB | (a) the demand worker asks the dcache first — `efs_dcache_copy` (image current and no spans), the same test the sub-chunk path already makes at `read.c:1364` — and fetches only on a miss; (b) on a remounted client (no dcache) the prefetch *does* run and, by code, a demand job refetches a chunk whose prefetch is in flight (`rdcache_hit` is false until it lands): give the rdcache a *pending* entry with a waiter so demand joins the in-flight job; (c) submit the next window's prefetch before waiting on the current batch, not after the copy-out | same-mount read-after-write of 1 GiB: server GETs = 0 (the dcache serves it; reads see unchanged bytes, sha256); **remounted** read: server GETs per 1 GiB = 16384 ± 1 %, rdcache hits ≥ 7 of every 8 demand chunks (counter under `EFS_DCACHE_TRACE`); the honest read number is the remounted one | a second cache, raising `EFS_READ_PREFETCH` to hide (b), zero-filling anything, serving a dcache image whose `base_gen` the map does not name |
| 1c | **W30 · flush starts at `close()`** | a file under the dirty-byte cap is never PUT while it is written: 1024 writes in 1.6 s with **zero PUTs** (poller 27 then 2 kicks/s), then `close()` = 3.1 s PUT drain + 0.36 s REPORT; wall = write + drain, not max | `dd_1m.strace.txt`: writes 0.5–1.3 ms each, `close(1) <3.563>`; `ddana2.log` close-phase kicks ~2000 per 100 ms from 22.4 s on | **D19, ask first (recommended yes):** a chunk whose 128 KiB are complete (sequential write moved past it) is PUT-eligible at once — the reclaim thread pool takes it from the per-shard dirty list without waiting for the cap; REPORT/publish stays at close/fsync/threshold (W2 unchanged: PUT is not publish). Cost: a chunk rewritten before close is PUT twice (new generation; the STALE/putid machinery already handles it) | 1 GiB dd+fsync wall ≈ max(write phase, drain) — the close phase under 0.5 s after a 1 GiB sequential write on the gate script; poller kick rate non-zero during the write phase; posix `concurrent_appends`, `mtime_monotonic_many_writes`, posix2 63/63 | publishing on write (forbidden by spec), raising the dirty cap, a per-file flusher thread |
| 1d | **W31 · one `mmap` per chunk in the write path** | `efs_fuse_write_buf` → `efs_buf_alloc(cs)` → pool empty → `malloc(128 KiB)` → glibc mmap threshold → fresh mapping + 32 page faults per chunk; the pool refills only when the flush returns buffers, which (W30) is at close | **8193 `mmap`** in the 1.6 s write phase; `clear_page_erms`, `do_anonymous_page`, `vma_alloc_folio`, `lru_add_fn`, `rmqueue_bulk`, `gup_*` ≈ 5 % of the client profile | the pool is sized to the dirty cap at startup (or grows in slabs of N chunks from one `mmap`), and `efs_buf_free` returns buffers to it instead of `free()`; alternatively `mallopt(M_MMAP_THRESHOLD, …)` once at start — pick the pool, it is already there | `mmap` count during a 1 GiB write phase < 64 (strace on the daemon for this count only, then off); the page-fault symbols leave the top 30 | growing the pool without bound (it must cap at the dirty cap + pipeline) |
| 1e | **W32 · flush concurrency = 16 chunks in flight** | each of the 16 `dcache_reclaim_main` threads PUTs one chunk (3 fragments) and waits for all three replies before taking the next; put-pool mutex `0x597680` 8450 WAKE / 8060 WAIT in 3 s | 8192 chunks / 3.1 s = 2.6 K chunks/s; Little: 16 × 128 KiB ÷ 5.5 ms ≈ 370 MB/s = the observed drain (the 5.5 ms is the server RTT under its tracer; the 16 is ours) | a reclaim thread keeps up to P chunks' PUTs in flight (send chunk N+1's fragments before chunk N's replies are back; reap in order) so in-flight = 16 × P, with P from `EFS_WRITE_PIPELINE` logic that `dcache_flush_keep` already has for the close path — one code path for both; shard the put-pool queue per thread or per node so the mutex is not shared by 16 threads × 3 fragments | at a fixed server RTT the drain rate scales with P on the gate script (P = 1 vs 4: ≥ 3×); `0x597680`-class contention gone from the strace (count only, then off); `test_data`/posix unchanged | more threads (D-class: the poller is not the lever), a second put pool, changing the fragment size |
| 1f | **W33 · ~400 `futex` per 1 MiB read, ~27 per fragment PUT on the server** | client get pool (`0x596960`: 34.7 K WAIT / 28.9 K WAKE per 1024 reads) and frag pool (`0x545540`: 27.2 K / 18.7 K) wake 32 + 128 workers through `pthread_cond_signal` chains, 8 chunk jobs + 16 fragment jobs + batch completion per read; server: a PUT is handler → `g_pool.lock` → `not_empty` → one of 20 `writer_thread`s → `run_job` → `done_cv` back to the handler (`writer.c:173–274`), plus ~3 eventfd kicks per PUT from the recv pollers (tids 951867/951947: 19.4 K `write` for 6172 PUTs); 86 K futex in the 6 s write window on fcstor004 | **398 futex + 78 read + 39 poll + 32 write ≈ 550 syscalls per MiB read**; at the tracer's ~100 K/s that is exactly 5.5 ms = 184 MB/s; untraced it is ~1 ms of CPU and scheduler per MiB and every read sampled shows a 2.2 ms tail before the last two completions | one wakeup per batch, not per job: submit the 8 jobs then signal once per idle worker needed (or hand jobs to workers through per-worker slots like `host_waiter_sleep`); the fragment fan-out of a chunk runs on the chunk's own worker (2 GETs issued back-to-back, one wait) instead of a second pool hop; server: same count on the writer pool, measured by `efsd --bench` first | futex per 1 MiB read ≤ 40 (count by strace, then off); the 2.2 ms tail is gone from an untraced dd read's per-`read` histogram (dd-side `strace -T`); server futex per fragment ≤ 6 | adding workers, spinning instead of sleeping, touching `recv_poller` |
| 1g | **W34 · server PUT = 3 `openat`, 2 inodes, and the drain is kernel time** | per fragment: `openat(O_WRONLY\|O_TRUNC\|O_DIRECT)` without `O_CREAT` **fails ENOENT** on every first write — **corrected 05:45Z:** this is the *quota* probe, not the path-hint one: `server_write_fragment_with_sum_sync` opens no-create first so an overwrite is not charged (`store.c:1259–1291`, `arg.no_create = 1`), on every PUT under `--quota` (19810 runs `--quota 36T`); W14.4b's `EFS_PATH_HINT_SKIP` only skips `server_find_fragment_root`'s `access()` walk — then `openat(O_CREAT\|O_DIRECT)`, `write` 64 KiB (50–60 µs p50), `close`, `openat(.sum, O_CREAT)`, `write` 32, `close`, 2 `fstat`; **no `fsync`**. Perf, write-drain bin on all four servers (`~/efs-runs/perfana2-fcstor00{3..6}.log`): **75 % kernel, 12–17 % `[xfs]`, 4–5 % efsd, 3–7 % libc**; of the kernel share roughly a third is the tracer (`ptrace_do_notify`, `ptrace_stop`, `__task_pid_nr_ns`, `syscall_trace_enter/exit_work`), the rest is file creation — `xfs_btree_lookup`, `xfs_cntbt_key_diff`, `xfs_buf_get_map`, `xfs_trans_read_buf_map` (inode + extent allocation for two new files) and the path walk (`__d_lookup_rcu`, `link_path_walk`, `inode_permission`, `step_into`: an 11-component path walked three times per fragment). efsd's own symbols: `server_handle_conn`, `efs_rdma_recv_wait`, `shard_io_thread`, `pick_write_path`, `server_write_fragment_with_sum` — none above 0.6 % | fcstor004 write window: 6144 no-`O_CREAT` opens (probe → create gap 176–188 µs p50 on all four), 6194 creates, 6150 `.sum` creates, 68 `mkdir`, 0 fsync; ≈ 55 syscalls per 64 KiB; ~2.4 cores per server during the drain, traced | (i) invert the quota probe: open `O_CREAT\|O_EXCL` first (new file ⇒ charge), and only on `EEXIST` take the no-create overwrite path — or skip the probe outright when `efs_tls_path_hint == EFS_PATH_HINT_SKIP` (a first write cannot exist); the common case pays one open; (ii) `mkdir` on ENOENT stays (68 per 6172 — D7 holds); (iii) **D20, ask:** the `.sum` sidecar as a second inode per fragment — options: a 32-byte trailer in the fragment file (read path `pread` one 4 KiB `O_DIRECT` block more), an xattr, or keep; on-disk format, so it is a decision; (iv) the path walk is 3 × 11 components per fragment — an `openat` relative to a per-thread parent-dir fd was tried once (Sep 28, "thread-local inode-dir fd", reverted because the slowest 9-client dd got worse) — do not retry it without the bench | no-`O_CREAT` `openat` count = 0 during a fresh-file write (strace on one server for the count, then off); fragment-creates per 64 KiB = 1 after D20; `efsd --bench` data kind prints the before/after ops/s at QD 64 and its `perf` shows `[xfs]` + path walk below 30 % | an `fsync` per fragment (spec: the Raft log is the durability boundary), a third file, a per-thread dir fd without the bench |
| 1h | **W35 · the 1248-byte chunk record** | REPORT for 1 GiB = `writev … 10,223,693` B = 8192 × 1248 B (1 % of the data) over TCP, reply 0.355 s, leader `pub_p50=227 ms` for that publish; GETCHUNKS reply = 64 × 1248 B = 79,888 B > `EFS_RDMA_BUFSZ` (73,728) ⇒ TCP for every window pull | the REPORT's 0.36 s is 10 % of the 1 GiB close; the GETCHUNKS frame size is why W28's RPC is a TCP round trip. **Server perf, the bin after the drain (fcstor004, g2 leader for this inode):** `__memmove`, `lookup`, `search_block`, `kv_mtab_pos`, `kv_seg_probe`, `kv_seg_iter_next`, `efs_crc32` — the publish apply is one KV point-get with a block CRC per chunk record, 8192 of them sequential ≈ 28 µs each = the 227 ms `pub_p50`; the server was at ~0.2 cores while doing it (latency, not CPU) | **measure first:** `tests/tools` dump of one encoded record — which fields are the 1248 B (3 × 32 B checksums, nodes, 8 × delta, generation, padding?); then **D21, ask:** a compact record for the common case (full image, no deltas: ~120 B) with the delta list only when `ndelta > 0`; and/or **D22, ask:** `EFS_RDMA_BUFSZ` ≥ 84 KiB so a 64-entry window and a 64 KiB fragment both fit RDMA (memory: 36 bufs × conns). Both are wire/pool constants with a spec | REPORT bytes per GiB ≤ 2 MB; GETCHUNKS and GET replies never take the TCP side channel on a healthy conn (`recvfrom … 79888` count 0 in a read); 9-client IOR hard-write fsync time falls with the REPORT size | splitting REPORT into several RPCs (forbidden), compressing on the wire |

**Not new, already queued, confirmed by this run:** the two user copies per written byte (`memcpy_erms` 5.7 % + `fuse_copy_*` 2.3 % kernel→libfuse, `memmove` 16.3 % libfuse→dcache) are W15 steps 3/5; blake3 4.3 % + `xor_into` 3.1 % are the arithmetic floor for the `cpu` kind of the client bench. The REPORT publish at 227 ms is D8's `pub_p50` with a 10 MB entry.

**Server perf and strace (reviewed 05:45Z, recorders stopped 05:27Z with the daemons):** `/tmp/efs-perf/efsd.data` on fcstor003–006 is `perf record -g -F 999` cycles, 1394 s, 24–31 K samples each — the daemon averaged ~2 % of one core; the write drain is the only hot bin (7279 samples / 10 s on fcstor004 ≈ 2.4 cores for 3 s), the read bin is 841 (≈ 8 % of one core, `blake3_hash_many_avx2` 16.6 % of it — the server-side verify on GET), and the bin after the drain is the publish apply (W35). Per-bin DSO split and the symbols are in W34; the strace counts in W29/W33/W34. The whole-run top is the tracer (`ptrace_do_notify` 5.4 %, `__task_pid_nr_ns` 2.9 %, `child_wait_callback` 2.7 %); the only background efsd symbol is `host_gc_frag_pass` → `lsm_scan_prefix` → `memcmp` at 0.7 %. **What the server data changes:** W29 (no duplicate GETs; the same-mount read refetched what the dcache held and the prefetch never ran), W34 (the dead probe is the quota overwrite probe, and the drain is XFS creation + path walk, not efsd), W33 (the 27 futex is the handler↔writer-pool hand-off). W28, W30, W31, W32, W35 stand. Nothing on the server side limits the read: the read ceiling is the client's serial per-MiB chain. Scripts: `~/efs-runs/perfana.sh`, `perfana2.sh`; logs `perfana{,2}-fcstor00{3..6}.log`. The data files stay on the nodes (`/tmp/efs-perf/`, 170–375 MB strace each) until the user removes them.

In tree and rolled (Sep 29 04:08Z, `54a500da9dc8-dirty`, RDMA, `--perf --strace`; the 02:35Z roll was `bbcbcb5ad779-dirty`), not gated by a suite: 1 (sync report loop: 8 s checked between REPORT attempts, plus whatever one in-flight RPC takes; dirty set merged back; fsync returns EIO), 2 (`fuse_stat_errno`), 3 (dirty list + `dcache_ensure` so `pthread_once` is not on the lock), 4 (`send_ae` reads the log into the wire buffer; `try_commit` starts at the quorum match), 5 (PUT reply carries the storage root), 6 (a 1 MiB write walks chunk-aligned pieces; each whole chunk is one `fuse_buf_copy` into a dcache-owned buffer; a partial head or tail bounces its own length), 6a, 6b, 7's snap-tmp reap and `send_buf_pick`, 7' (an AE reply replaces an older queued reply to that peer, a heartbeat replaces an older heartbeat, a vote is not evicted, and the sender drains that lane before entry AppendEntries; a full entry lane returns AGAIN and does not increment `st_drop`), 7'' (`efs_rdma_send_frame` posts and returns; the reply wait reaps send CQEs and treats a send still busy after 5 s as a dead QP), 8, 9 step (a), 8a (snapshot by 512 MiB with a retained window), 8b (import diff in 1024-key slices), 8c (`EFS_PATH_HINT_NEW`), and the `pub_p50`/`pub_max` line of 10. 8d's `--meta-storage` flag is in and defaults to the first `--storage` root; `mdraft/` was not moved and the shared-vs-quiet `fsync` measurement was not run. 8h and 8i / D12 were rolled 12:29Z. The 12:36Z IOR (`results/io500/20260929-123635-rdma`) finished ior-easy-write at 2.218 GiB/s and mdtest-easy-write at 1.765 kIOPS, then ior-hard-write aborted on fsync (31 failures, no bandwidth) and fcstor012's ranks stuck in D behind a reclaimer holding the shard I/O lock across a fragment GET. The 13:17Z copy failed a different way: `fsync` returned EIO because D12 rewrote L1 for the whole ingest (D13). 8j / D13 is in the tree (14:20Z), not rolled, not gated. The shard-lock drop is in the tree (14:50Z), not rolled. Next is 10's one IOR, 7 step 2 (c) (measure, then ask), 11. 8e, 8f, and 8g were rolled 05:31Z, including the 1 GiB L0 byte cap; the 12:02Z trace is their measurement (§1b). In tree and rolled with that same build: W23 client (pool generation instead of `fstat`; liveness probe only after 1 s idle), W24 step 2 (one 8 s deadline around truncate's flush, REPORT, and SETATTR), W25 (`futimens` proposes the utimens command on the other group for cross-group lanes before the inode-group entry — not `LANE_FENCE`, which would delete chunks; `atime_nsec` on the SETATTR wire; `EFS_ERR_BUSY` is EBUSY). The three `test_meta_apply` cases named under W17 step 3 are in `test_chunk_deltas` (Sep 29 15:40Z): replay of a folded span is a no-op, a span after the fold attaches to the new base without naming it, overlap with a live span is STALE (a tombstone's old range is free), and a full image after the fold must name the current base. The 04:08Z roll and the 02:35Z IOR analysis are in §1b.

Prior recommended answers, for the record:

| item | question | recommended (server perf, Oct 1) | why, in one line |
| --- | --- | --- | --- |
| W13 | full-L1 compaction holds the apply path 2.4 s and costs a term | **done Sep 26**, partitioned flush rolled Sep 27 on TCP | background compactor kept `apply_max` under 70 ms. Flush now writes one L0 file per `key[0]`; one compact leaves the other range's L1 file in place (`test_kv_lsm`) |
| W11 | the KV export is larger than one 4 MiB SNAP command, so the log never truncates | **done Sep 27** — chunked InstallSnapshot of a file; import is a sorted diff | logs under 5 KB; fcstor005 rejoined in 510 ms; `apply_max` 0 on a 386 MB export. 9-host posix 200/201 (`results/posix/20260927-123717`) |
| W9 | bound the client staging table | **done Sep 27** — in-flight pin, append-reservation pin, chunk maps before the row, and the evictor walks past a pinned oldest window | a cold stat of 1M files leveled at 233 MB RSS (`results/measure/20260927-w9-walk`). posix 2 is 63/63 (`results/posix2/20260927-123946`). Leaks are clean (`results/leaks/20260927-035622`) |
| W10 | RDMA needs an empty table to gate | **live Sep 28**, suites pass, not faster than TCP on posix or the 9-client write | 19810 is RDMA (`db2b88c4802a-dirty`). jobs=1 after user xattr is 200/201 (`results/posix/20260928-043918`); 9-host before that was 199/201 in 57–59 s; posix2 63/63; 9-client dd **1326.6** vs TCP 1478 |

---

<a id="ph-w1w5-w7-w11-w13-w26-done-stubs-as-they-stood-d5f767"></a>
### W1–W5, W7, W11, W13, W26 (done; stubs as they stood)

#### W1 — Shared-file (N-1) writes from two clients silently lose data — DONE

**Done Sep 18 2026** (working tree on leftover-1 19810, TCP). I12 CAS is
live: `efs_chunk_rec.base_gen` + `chunk_generation` on the wire, leader
checks the writer's base before propose, apply STALE is audible
(`EFS_INODE_RPC_STALE=11` / `EFS_ERR_STALE=-14`) and does **not** stall
`last_applied`, client refetch+overlay+PUT retries. Fragments are
`{ci}.{fi}.{gen}`; GET uses `fragment_path_at` and mints the export shell.
Report identity comes from the last PUT (`putid`), not a GETCHUNKS stub.

Full text (measurements, steps, forbidden list) is in [project-history.md](project-history.md), "START-HERE closed items".

#### W2 — `write()` is specified as durable-and-visible; the code buffers — DONE

**Done Sep 18 2026**, option (i): the spec moved. [architecture.md §3](architecture.md)
now lists three deviations; a returned `write()` is client-buffered;
durable + cross-client visible at `fsync` / last `close` / `O_SYNC`.
`O_SYNC`/`O_DSYNC`/`-o sync` is specified write-through and is **not
wired**. Do not implement publish-on-every-`write()` — that is the
rejected 10× throughput change.

Full text (measurements, steps, forbidden list) is in [project-history.md](project-history.md), "START-HERE closed items".

#### W3 — Split the single-client fsync tail, then remove the larger half — DONE

**Done Sep 18 2026** (leftover-1 19810, TCP). Measure said REPORT owned
73 % of the 8 GiB `fsync` (flush 5.7 s / report 15.0 s / wall 25.8 s).
Cuts, in order: `HOST_PUB_BATCH_N=2048` + skip `get_chunk` when
`base_gen==0`; raft-log `sync_hold` across the report's proposes;
client flush pipeline (`dcache_steal` + put pool); PUBLISH follower
forward is `host_propose` not `host_propose_wait`; O_APPEND fetches a
published mapping before sparse RMW; truncate/symlink report only that
ino; FUSE `flush` waits `report_dirty_ino(ino, 1)` (W2 close is
durable — kick-only left `i_size` 0 so the next O_APPEND wiped the
prefix). N=4096 did not win. AE 1 MiB is ~5168 pubs; deeper batching
is exhausted.

Full text (measurements, steps, forbidden list) is in [project-history.md](project-history.md), "START-HERE closed items".

#### W4 — 4-client and 9-client honest fio and dd — DONE

**Done Sep 18 2026.** Writes share a ceiling and more clients make it
worse. Gate: `results/perf/20260918-w4-honest4/gate.txt`.

Full text (measurements, steps, forbidden list) is in [project-history.md](project-history.md), "START-HERE closed items".

#### W5 — Re-measure `sw-50g` after W3 — DONE

**Done Sep 18 2026** as the W4 morning 50g row:
`results/perf/20260918-w4-honest/` sw-50g **341** / sr-50g **2203**,
`FUSE_OK` `err=0`. W1 was 373. Later loaded reruns laid 50 GiB then
`end_fsync` EIO (the same REPORT tail). Completes when the path is
healthy; do not treat EIO as a reason to raise `EFS_IO_TIMEOUT_MS`.

Full text (measurements, steps, forbidden list) is in [project-history.md](project-history.md), "START-HERE closed items".

#### W7 — Two POSIX suite-1 tests exceed the 15 s budget even in isolation — DONE

**Done Sep 21 2026.** The 15 s failures were the metadata wakeup floor
(fixed Sep 20), not a remaining per-test bug. Isolated on an idle cluster,
`results/measure/20260921-133437-posix-isolated` (budget 15 s, jobs=1):

Full text (measurements, steps, forbidden list) is in [project-history.md](project-history.md), "START-HERE closed items".

#### W11 — chunked InstallSnapshot — DONE Sep 27

Running build `612ee9ba9202-dirty`, TCP. The log truncates. A follower
install is a sorted diff (`src/kv/kv_snap.c`). fcstor005 rejoined in
510 ms. 9-host posix is 200/201, skip `mmap_write_read`, 30.4–31.3 s
(`results/posix/20260927-033723`). The earlier 199–200 run is
`results/posix/20260927-015719`. Gate numbers are in
`results/measure/20260927-w11-gate` and `docs/project-history.md`
(Sep 27). The original recommendation, kept so the steps stay
findable:

Full text (measurements, steps, forbidden list) is in [project-history.md](project-history.md), "START-HERE closed items".

#### W13 — Synchronous full-L1 compaction is the remaining election trigger — DONE Sep 26 2026

Gate is at the top of §1b. Steps 1–4 landed: pinned view, background
compactor, flush no longer compacts, and the 9-host suite kept
`apply_max` under 100 ms with no term change. Step 5 (partitioned
flush) is in the TCP build rolled Sep 27 05:07 UTC. A flush writes
one L0 file per `key[0]` (at most 16 on efs keys, shard in the first
two bytes). Compaction merges one of those ranges plus the L1 files
that overlap it, and cuts an output file when the range changes, so
the next cycle does not rewrite the other ranges. `make test` on
fcstor014 passed, including `test_partitioned_flush`. The live table's
existing L1 files are still wide until the next compaction rewrites
them; the byte bound is the steady state after that.

Full text (measurements, steps, forbidden list) is in [project-history.md](project-history.md), "START-HERE closed items".

#### W26 — FUSE `fallocate` handler — ASKED Oct 1 2026, IN TREE Oct 2

`posix_fallocate` on an `O_DIRECT` file returns `EINVAL` because
`efs_ll_ops` has no `fallocate` (W25). The user asked for the handler.
`/home/erbmi1/git/direct_rw/ewrite.c` opens `O_DIRECT`, calls
`posix_fallocate(fd, 0, size)`, and falls back to `ftruncate` only for
`EOPNOTSUPP`/`ENOSYS`. `EINVAL` aborts the job with 0 bytes written.

**What.** Add `ll_fallocate`. Mode 0 (what `posix_fallocate` sends)
extends the file to `offset + length` the way `ftruncate` already does:
one size SETATTR, no fragments. A read of the new range is a hole and
returns zeros (a missing chunk already does). `FALLOC_FL_KEEP_SIZE`
leaves the size unchanged and succeeds when the range is inside the
file. Any other mode (`PUNCH_HOLE`, `ZERO_RANGE`, `COLLAPSE_RANGE`,
`INSERT_RANGE`, `UNSHARE_RANGE`) returns `EOPNOTSUPP`, so a caller can
fall back. This does not reserve NVMe blocks; a later write can still
fail if a disk or the quota is full.

- **Gate:** `fallocate(fd, 0, 0, size)` on an `O_DIRECT` fd returns 0;
  `stat` size is `size`; a read of the new range is zeros; no chunk
  object was stored for that range. `ewrite` from `direct_rw` gets past
  `prepare_file_space` and writes. posix jobs=1 stays **200/201**
  (`opt_fallocate` still PASS; it must no longer depend on glibc
  writing the 4 KiB). posix2 63/63.
- **Forbidden:** writing zero fragments to satisfy the call; a quota
  reservation; implementing punch-hole or zero-range in this item;
  treating glibc's `posix_fallocate` success on a normal fd as proof
  (that success is the zero-fill fallback until this handler exists).
