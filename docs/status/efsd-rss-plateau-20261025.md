# efsd RSS plateau under the fixed-working-set D4 load (Oct 25–26)

**Purpose.** Release check F1 (memory and concurrency gates, [alpha 1.0
checklist](alpha-1.0-2plus1-checklist.md)) needs every daemon's RSS to
plateau. The [F1 follow-ups](f1-followups-20261025.md) left item 2 open
(Open 4 there): under the 60-minute fixed-working-set D4 load efsd grew
8–13 % over minutes 40–60, with throughput, and its connection-thread count
stepped up at the same time. This page tests that hypothesis, finds what
grows, and bounds it. Criterion: the `scripts/rss_plateau.py` growth (the
least-squares slope over the last third of the load times its span, as a
share of the peak) at most 5 % for every efsd and efs-fuse.

**Verdict: PASS for the RSS plateau (one run), at `66a3a4024`.** Three
growth sources were found and bounded. On a 60-minute run on xorinox, alone,
with free page cache, every efsd and efs-fuse plateaus: efsd +0.6 / −0.8 /
−0.1 %, efs-fuse +0.0 / +3.6 % (run F). That run did 128,803 rewrites, three
times nuc's rate. The same D4 run fails on one truncate EIO, a client
`flush-meta` protocol error that is not memory and not investigated ([What
remains](#what-remains)). On nuc, efsd failed the criterion on every run,
the last of them (E, same code) after a host page-cache shortage at minute
48 with elections. Every bound held: efsd at most 77.3 MiB of 2048,
efs-fuse at most 234.3 MiB of 1024.

| Item | Result |
| --- | --- |
| Connection-thread hypothesis | **Refuted as the main cause; the reap half confirmed.** Per thread: 50–90 KiB of stack and about 100 KiB of request buffers, threads flat from minute 15 on a quiet run. No pool ever closed an idle connection; now both do (`4e1d2a55d`) |
| Reclaim journal ring | **Found and fixed** (`cbfbadbff`): half the efsd growth over minutes 40–60 on the baseline |
| Arena fragmentation from packer compaction | **Found and fixed** (`66a3a4024`): the leader's arena step late in the load. With it, run F's leader was flat (+0.6 %) |
| Raft index windows | Bounded (2 MiB + 256 KiB per group), reached by minute 15–20; not growth |
| 60-minute plateau, efsd | **PASS on run F** (xorinox, `66a3a4024`: +0.6 / −0.8 / −0.1 %); FAIL on every nuc run (A–C, E; table below) |
| 60-minute plateau, efs-fuse | **PASS** on 4 of 5 runs (+0.0 to +4.5 %); the run with two EIO retry storms: load mount +5.4 % |
| D4 run F as a whole | **FAIL: 1 truncate EIO** (client `flush-meta` `EFS_ERR_PROTO`), not an RSS finding; every other D4 check passed |
| `make test-all` (`a6720c088`, intel2, 8 jobs) | **PASS with host limits:** 481 commands, 0 FAIL, 16 NOTRUN (RDMA device, `~/git/cluster` scripts, valgrind, sanitizer self-tests, an `EFS_FAULTS=1`-only case), 557 s; exit 3 (INCOMPLETE) for the NOTRUN lines. At `4e1d2a55d` it had 3 FAIL: client-pool harnesses that compile `efs_client_conn_get` from its source text, fixed in `a6720c088` |
| `make tsan-check`, `make asan-check` (`a6720c088`, intel2) | **PASS** (exit 0 each). At `4e1d2a55d`: TSan 18 run, 0 failed; ASan 47 run, 1 failed (`resolver_deadline`, the same harness compile error) |
| `make test-all` (`66a3a4024`, xorinox, 8 jobs) | **2 FAIL, both a read EIO under the 8-job pool:** 481 commands, 14 NOTRUN host limits, 660 s. `test_k33_payload.py` and `test_k33_growth.py` each failed one read with EIO with a member down or during growth. Reruns alone, three each: `test_k33_payload.py` failed 1 of 3 on this tree **and 1 of 3 on the base `1ebdb2557`**, with the same EIO; `test_k33_growth.py` passed 3 of 3 on both (its gate failure did not reproduce, so it is not attributed). The K33 degraded-read EIO is owned elsewhere (k33-payload-read-eio session) |
| `make tsan-check`, `make asan-check` (`66a3a4024`, xorinox) | **PASS** 18/18 (631 s), 47/47 (553 s) |

## Method

Branch `claude/wizardly-mcclintock-0e2efd`, from `claude/devel-f1-followups-ae2deb`
(`1ebdb2557`, code `758dd144c`). Each run is `tests/test_d4_plateau.py` with
`EFS_D4_LOAD_S=3600 EFS_D4_SET=256 EFS_D4_DRAIN_S=900 EFS_D4_QUOTA=8G
EFS_D4_KEEP=1`. That is three efsd of the `EFS_FAULTS` build and two
efs-fuse on loopback TCP with buffered I/O, 8 writers rewriting 2048 files,
and a 32-file foreground ring. Each ran alone on nuc. Beside it,
`scripts/rss_mappings_sample.py` read `/proc` every 30 s (no ptrace, no gdb).
`scripts/rss_mappings_split.py` splits the growth of a window by
component.

Two diagnostics, on the runs marked *diag*, are not in the tree; their
sources are in the raw evidence:

- a patch to `src/server/efsd.c` and `src/server/handler.c`
  (`diag-efsd-mallinfo.patch`). It logs every 60 s the live connections,
  the connections ever accepted, the handler buffers' total, and
  `mallinfo2` before and after the 15 s trim. Every 5 minutes it writes
  `malloc_info` to `EFS_DIAG_MALLOC_DIR`;
- the heap-site shim of the F1 follow-ups (`heapsite.c`, `heapdiff.py`;
  `LD_PRELOAD`, efsd only, a dump every 120 s).

## Findings

**Connection threads.** From minute 22 to minute 54 of the baseline, efsd's
thread stacks held 3.1–6.5 MiB of RSS for 65–83 threads. All handler payload and
reply buffers together held 3.1–4.9 MiB. The thread count reached its
high-water by minute 15 and stayed there on a quiet run. So the threads
cost about 0.15 MiB each, and 15 extra threads cost about 2 MiB, too little
for the growth. Connections do turn over (2,200–3,800 accepted per node per
hour while 35–49 are live), but only through errors and short-lived tools.
No pool ever closed an idle connection. The client pool (64 slots per server
by default, `EFS_CLIENT_CONNS_PER_NODE` caps it at 256) and the peer pool
(34 per peer) kept what their busiest moment opened. efsd waits forever on
an idle connection (no `SO_RCVTIMEO`).

That matters once a stall occurs. In run B, two rewrite EIO retry storms (at
minutes 50 and 56) raised the live connections from 46 to 65 on n3 and from
35 to 59 on n1. With them, in-use heap rose by 3 MiB, the arenas by 8 MiB,
and none of it came back. **Fix `4e1d2a55d`:** both pools close a
connection whose last completed send or receive (`last_ok_ms`) is over 30 s
old and that no caller holds. They scan at most once a second per node, on
checkout and release. `tests/test_conn_idle_reap.py` (`make test-long`,
about 60 s): a 48-writer burst raises efsd's open sockets from 50 to 152,
and 45 s of light traffic later 44 are left. Without the fix all 63 of its
burst's sockets stay open. In runs C and E, within two minutes after the
load each efsd fell back to 36–64 threads.

**Reclaim journal ring.** In run A, the only mapping that grew steadily was
a 30,732 KiB VMA. It is three adjacent `calloc` blocks merged: the ring of
unretired reclaim records (`RJ_CAP` = 262,144 × 96 B = 24 MiB) and the two
fence tables (6 MiB, resident from the start). The ring's RSS rose linearly,
by about 140 KiB a minute (n2: 6.6 → 13.5 MiB over 50 minutes), because
appends walk the ring forward and no page was ever released. RSS followed
the records ever accepted, not the unretired window, and would only stop at
24 MiB. Over minutes 40–60 the ring was 2.8–3.3 of the 5.5–7.2 MiB growth
per node. **Fix `cbfbadbff`:** the ring is its own mapping without huge
pages. Under THP `always`, one append after a release faulted a whole 2 MiB
back in; the unit test showed it. `retire_some` drops, under `rj.mu`, the
whole pages that hold only retired or never-reused slots. In runs B, C and E
the ring held 8 KiB. A `test_reclaim` case checks residency with `mincore`,
for a prefix and for a window across the wrap; it fails with the `madvise`
disabled.

**Raft index windows.** Per-file RSS (run B): each group's `.index-*`
mapping reaches its 2 MiB window plus the 256 KiB trim hysteresis by minute
15–20 and saws below it from then on. File-backed RSS stays at 6–8 MiB.

**Arena fragmentation from packer compaction.** Run C (ring fix and reap):
the group leader's efsd:1 was the only one past the criterion (+16.3 %). Its
threads and sockets were flat. One glibc arena heap stepped from 9.8 to 22
MiB at minute 54, and to 27.6 MiB in the drain. Run D (diag, profiled)
showed the leader's live heap flat over minutes 36–60 (38.9 → 32.9 MiB). Its
arenas held 37 MiB with 14 MiB in use, and the free part rose from 21.7 to
23.3 MiB. The live-heap spikes (up to 51 MiB) were compaction in flight. The
pass held one 128 KiB image per source segment for its whole length (39
live, 5 MiB), plus a 192 KiB fragment buffer per segment read. All of them
were below the fixed 256 KiB mmap threshold, so they were carved from the
arenas and left holes. Compaction work grows late in a long rewrite load.
**Fix `66a3a4024`:** a segment read takes one 320 KiB buffer (the image,
then its three fragments), which is its own mapping. Compaction reads its
sources one at a time into one such buffer, and copies the live records into
a block after it; the output segment bounds that block.

## Runs

Runs A–E on nuc, alone (another session's runs moved off it at 18:50 on Oct
25); run F on xorinox, alone, on Oct 26. Growth and verdict from
`scripts/rss_plateau.py` as the test prints them. Run F's split by component
over minutes 40–60 (`scripts/rss_mappings_split.py`): every component within
±0.9 MiB per node, the ring at 0.

| Run | Tree | Rewrites (ow / trunc / rewrite) | efs-fuse fg / load | efsd 1 / 2 / 3 | Peak efsd / efs-fuse | Packer put errors | Other failures |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A | `1ebdb2557` + diag | 15,528 / 15,527 / 15,520 | +0.8 / +4.1 % | **+9.6 / +7.4 / +9.8 %** | 77.3 / 191.5 MiB | 99 | none; drain 409 s |
| B | + `cbfbadbff` + diag | 14,496 / 14,496 / 14,493 | +0.0 / **+5.4 %** | **+12.8 / +19.2 / +21.7 %** | 71.4 / 232.0 MiB | 200 | 2 rewrite EIO; 28 pack candidates left after 900 s |
| C | `4e1d2a55d` | 14,225 / 14,220 / 14,219 | +0.0 / +3.6 % | **+16.3 / +5.4** / +3.6 % | 69.6 / 194.6 MiB | 272 | pack candidates left after 900 s |
| D | `a6720c088` + diag + heap shim | diagnostic; stopped in the drain | — | — | — | — | — |
| E | `66a3a4024` | 14,305 / 14,304 / 14,299 | +3.0 / +4.5 % | **+7.6 / +23.7 / +7.0 %** | 65.0 / 234.3 MiB | 302 | 1 rewrite EIO; pack candidates left after 900 s; group 0 and 2 elections at minute 48 |
| **F (xorinox)** | `66a3a4024` | 42,937 / 42,935 / 42,931 | +0.0 / +3.6 % | **+0.6 / −0.8 / −0.1 %, PLATEAU** | 63.1 / 196.9 MiB | 0 | 1 truncate EIO; drain 396 s; no election during the load |

Every run kept all 2048 files (no net creation) and every file exact,
except those whose last rewrite failed (unspecified state, as the driver
defines it). Every foreground operation finished within 10 s. The put errors
are the follower `NOT_PRIMARY` refusals of F1 follow-ups Open 2: this branch
does not carry the flake-sweep resend (`fc79fe086`); run F counted none.
Load rate on nuc was about 43,000 rewrites an hour, against 128,803 in run F
and 119,827 on xorinox in the F1 follow-ups' run 6.

## What remains

1. **The plateau rests on one clean run (F).** On nuc, run E (same code)
   stepped up at minute 48. At that moment every efsd lost file-backed pages
   (its binary text and its index pages), groups 0 and 2 held elections (n2
   took group 0), and every member's live connections rose from about 43 to
   66–75, threads from about 61 to 78–86. n2, the new leader, then grew by
   arena memory (+23.7 %). nuc was short of page cache: its slab held
   14.6 GB (12.3 GB reclaimable dentries and inodes), with 395 MB cached and
   983 MB free of 31.7 GB. A concurrency step still raises arena memory that
   is not returned while the load lasts (the reap returns threads and
   buffers only once connections go idle). A repeat of run F, and a run with
   a forced leader change, would show how robust the plateau is.
2. **Run F's truncate EIO.** The load mount logged `efs-fuse flush-meta:
   protocol error (efs_rc=-7) ino=33585 off=0 len=0` at 12:18:09Z (about
   minute 22). The servers logged 130 ms apply and read waits just before,
   and no REPORT `outcome=partial`. Not investigated; its logs are in the
   raw evidence (`logs/mnt-l.log`).
3. **intel2:** a gate run of `66a3a4024` started there on Oct 25 stayed
   unread; from about 00:05 on Oct 26 intel2's sshd reset every connection
   (from the Mac and from xorinox) until a reboot, which ended the run with
   no verdict. The same gates then ran on xorinox (table above).
4. **amd went down during this session.** At about 21:08 on Oct 25, 30
   minutes after this session's gates and another session's 60-minute D4
   run started on it, amd stopped answering ping and ssh. The other session
   reported that its D4 load ran amd to ENOSPC at about 45 minutes. The
   cause is not established; amd hosts the user's live cluster.
5. Seen again, owned elsewhere: rewrite EIO after STALE/BUSY REPORT rounds
   (F1 follow-ups Open 1, K38); pack candidates left past a 900 s drain
   (Open 5; the packer-drain session traced it to the D25 truncate-sweep
   backlog, fixed on its branch at `b7bf18bdd`); packer put errors (Open 2).

## Environment

nuc: Intel Core i5-1135G7 (8 CPUs), 31.7 GB RAM, Rocky Linux 10, kernel
6.12.0-211.67.1.el10_2, gcc 14.3.1, `/home` on XFS, THP `always`. xorinox
(run F): AMD Ryzen 9 7940HS (16 CPUs), 60 GB RAM (49 GB free and a 1.6 GB
slab at the start), the same OS, kernel, compiler, XFS `/home` and THP.
Run F's sampler came from `e64ad4604` (the scripts were committed after the
tested code); the tested tree was `git archive 66a3a4024`. Private
tree under `~/efs-rss`, `TMPDIR=~/efs-rss/tmp`, loopback ports the test
chose. Gates at `a6720c088`: intel2 (8 CPUs, 15 GB, Rocky 10); at
`66a3a4024`: xorinox; `EFS_TEST_JOBS=8` both. The unit test and the reap
test also ran on xorinox. No 17xxx port, `/data1` mount
or other user's process was touched. The private trees were deleted on nuc,
xorinox and, after its reboot, intel2.

## Raw evidence

Run F and the xorinox gates: xorinox `~/efs-artifacts/efsd-rss-x60-20261026/`
(66 MiB, 31 files: `test.log`, `d4-*.json`, `mem.jsonl`, `build.log`, daemon
and mount logs in `logs/`, the gate summaries, sanitizer verdicts and K33
rerun logs in `gates/`; sha256 of its `SHA256SUMS`
`f88b3e71aa54a918d565599feac54ac8a74020e0e421824ba501702ab35c406f`).

intel2 gate logs (the `a6720c088` gates, the packer tests and the
unfinished `66a3a4024` run): intel2 `~/efs-artifacts/efsd-rss-intel2-20261026/`
(72 KiB, 19 files; sha256 of its `SHA256SUMS`
`fd6f50a4886c5463eaefc76513827c0ae0d5aa8c6b76a90febf83c7be2d9f776`).

Runs A–E: nuc `~/efs-artifacts/efsd-rss-20261026/` (62 MiB, outside Git), with
`SHA256SUMS` (271 files; sha256 of `SHA256SUMS` itself
`be90638e100b4fca8a979d36699f3c79c6a11ddee1e5595d78049316a6e5b458`). It
holds per run: `test.log`, `d4-result.json`, `d4-samples.json` and the
sampler's `mem.jsonl`. The diag runs add `malloc/` (`malloc_info` dumps),
run D adds `heap/` (heap-site dumps) and run E adds `logs/` (daemon and
mount logs). It also holds the run and analysis scripts, the diag patch and
the heap shim. The stores and trees were not kept: they are recreated from
the commit and the command.

## Reproduction

```bash
git archive e64ad4604 | ssh xorinox 'mkdir -p ~/efs-x && tar -xm -C ~/efs-x && cd ~/efs-x && make -j16 test-bins'
```

(`e64ad4604` is `66a3a4024` plus this page and the two scripts.) Substitute
`nuc` for `xorinox` below to repeat runs A–E.

A 60-minute run with the RSS sampler beside it (alone on the host). Delete
`~/efs-x`, `~/efs-x-tmp` and `~/efs-x-out` afterwards:

```bash
ssh xorinox 'cd ~/efs-x && mkdir -p ~/efs-x-tmp ~/efs-x-out && (python3 scripts/rss_mappings_sample.py efs-x/ ~/efs-x-out/mem.jsonl 30 & echo $! > ~/efs-x-out/sampler.pid) && TMPDIR=$HOME/efs-x-tmp EFS_D4_LOAD_S=3600 EFS_D4_SET=256 EFS_D4_DRAIN_S=900 EFS_D4_QUOTA=8G EFS_D4_ARTIFACTS=$HOME/efs-x-out python3 tests/test_d4_plateau.py; kill $(cat ~/efs-x-out/sampler.pid)'
```

Expected: the RSS table with every efsd and efs-fuse `PLATEAU` for a pass,
and the D4 line. The split of a window, with `LOAD_START` being the mtime of
`d4-samples.json` minus its last `t`:

```bash
ssh xorinox 'python3 ~/efs-x/scripts/rss_mappings_split.py ~/efs-x-out/mem.jsonl LOAD_START 40 60 24576'
```

The fixes' own tests: `./tests/test_reclaim` (`make test-long`) and
`python3 tests/test_conn_idle_reap.py` (`make test-long`).

## Task I — second confirmation queued (October 26)

**NOTRUN, queued; no new PASS.** Branch `agent/devel-task-i`, source
`931845748` (product base `394f2b791` unchanged), uses the existing 60-minute
fixed-set load and 5% RSS rule with a **150-second** drain bound. Nuc must
first be released by task H, including removal of H's private tree. A private
node9901 TCP/RDMA stage follows a passing nuc result. [Committed controller,
exact commands, evidence locations and pending gates](task-i-20261026.md).
The original run F's RSS PASS and overall D4 FAIL remain as reported above.
