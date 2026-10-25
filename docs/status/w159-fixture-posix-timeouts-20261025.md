# W159 · Fixture POSIX timeouts: slow durable barriers and the 400 ms applied-wait (Oct 25, 2026)

**Purpose.** Find the cause of W159 ([work queue](work-queue.md)): on a
private four-member fixture with every daemon and the client on one host
(intel1, direct I/O on one NVMe), the expiry fixture's closing core POSIX
run timed out `dir_many_files` (75 s) and `concurrent_creates_same_dir`
(30 s) with metadata RPCs of 2–7 s in the client log (`inode-rpc:
slow-ok`, REPORT_CHUNKS and CREATE_RETAINED, one with seven attempts after
BUSY); the same build passed both cases on the real four-host cluster
([intel run](intel-devel-a23d45540-20261023.md), defect 4). A rerun on
orcd-mini failed `dir_times_mkdir_rmdir` with rmdir EBUSY
([orcd-mini run](orcd-mini-deploy-cb970ca7c-20261024.md)).

**Verdict: cause established, PASS (scoped, loopback).** The fixture's
disk is the trigger and the product amplified it. The metadata path costs
tens of durable barriers (fsync/fdatasync) per create-and-unlink across the
members, every barrier on the fixture's one consumer NVMe takes 7–28 ms
instead of 0.1–0.3 ms, and the four daemons queue on that one device. The
Raft pump then applies with a lag of several hundred milliseconds while
still advancing; the server's applied-wait had one fixed budget of 400 ms
(`HOST_READ_TRIES × HOST_TICK_US`) and answered BUSY past it, which sent
the client down its 50..800 ms backoff ladder and re-proposed the request
behind the same lag: a 0.4 s wait became the 2–7 s RPCs. On amd the same
symptom reproduces on the untouched base with an emulated 7 ms barrier
queued on one lock (`dir_many_files` timeout after 75 s twice,
`concurrent_creates_same_dir` after 30 s). Fixed: the applied-wait is
bounded by progress (`src/server/apply_wait.h`): a grant of 400 ms is
renewed while the member applies, a stalled apply still gets BUSY, and
the whole wait is capped at 2 s. Branch `claude/devel-w159-fe09e8` on
`merge-wave7`, not pushed. Multi-host and RDMA NOTRUN (parent). What the
fix does not change: the per-operation barrier count, so the fixture stays
3–4× slower than the real cluster on such a disk (a follow-up, below).

## Environment

- **Host:** amd (`amd-395`, AMD Ryzen AI MAX+ 395, 32 CPUs, 122 GiB,
  Ubuntu, gcc 15.2), stores and mounts under `~/w159/` on the ext4 root
  NVMe (Sandisk Optimus GX 7100M; 4 KiB write + fdatasync p50 250 µs,
  p99 932 µs, max 1.2 ms). Another session's W123 campaign (user `efs`:
  three efsd, two efs-fuse, an `strace` at 80 % of one CPU) ran on the
  host throughout, on `/data1` (a different NVMe); load average 5–8. Every
  run's `host.log` records the load and the five busiest processes.
- **Source:** this branch at the commit named in the commit log of this
  page; the base for the A/B runs is `merge-wave7` `0b01a2a1e`, built in
  `~/w159/base` with the same flags (`make -j32 all`).
- **Fixture:** `scripts/w159_fixture.py`: four efsd (`efsd ADDR:PORT STORE`
  seed, `--join` for the others, `--discovery-port 0`, `--quota 4G` or
  `1G`, `EFS_TRANSPORT=tcp`, `EFS_RAFT_OBS=1`, direct I/O, the build
  default), `raft-mkfs`, one `efs-fuse ... -f` with `EFS_RPC_PROF=1`, loopback
  ports 24510–24660. `EFS_QUERY_REFRESH_MS=2000` where the expiry fixture
  sets it (`tests/live/gc_reclamation.py`). Phases: `core` is the fixture's
  own closing run (`posix_suite.py MOUNT --jobs 1 --timeout-s 30`); `loaded`
  runs the three W159 cases, `--repeat` rounds, while `--load-threads`
  writers create 1 B–8 KiB files in their own directories (every eighth
  create unlinks an older file). `efs-mgmt raft-status` of every member is
  sampled every 2 s. `--fsync-delay-us N` runs the daemons under
  `tests/live/w159_slow_fsync.c` (LD_PRELOAD: every fsync/fdatasync takes
  N µs more and is counted); `--fsync-lock` queues the four daemons'
  delays on one flock, the single-device model.
- **intel1 for comparison:** Intel Core Ultra 7 256V, 8 CPUs, 15 GiB, RHEL;
  the fixture's stores were on `/data1` (nvme0n1, "PCIe SSD", not writable
  for this session); `/home` on the other consumer NVMe (nvme1n1, EXT X200E)
  answers a 4 KiB write + fdatasync in **p50 7.2 ms, p90 12.3 ms, p99 15.2 ms,
  max 27.8 ms** (`/tmp/fsync_probe.py`, 300 rounds: 29× amd). The fixture
  logs on intel1 (`/data1/efs/expiry-fixture-a23d45540/A/gc-direct-qinu3xj6/`,
  `fuse.log`, `n1.log`–`n4.log`) are owned by `efs` and unreadable for this
  session; the orcd-mini fixture directory under `/tmp` is gone. Their
  console outputs: the intel run took 513.8 s for 216 cases against 118 s
  on the real cluster, the orcd-mini rerun 143.5 s.

## Reproduction on amd

| Run | Conditions (`scripts/w159_fixture.py`) | Core run | Three cases under load | RPCs ≥ 1 s | Server `wait_timeouts` (BUSY from an expired applied-wait), max per member |
| --- | --- | --- | --- | --- | --- |
| 1 | base binaries, quota 4G, refresh 2000 ms | 218/218, 173 s | 9/9 PASS, 27 k creates in 49 s | 0 | — |
| 2 | quota 1G (the expiry fixture's), refresh 2000 ms | 218/218, 73 s | 9/9 PASS | 0 | — |
| 3 | run 2 pinned to 8 CPUs (`taskset 0-7`, all five processes) | 218/218, 75 s | load hit the 1 GiB quota (ENOSPC), cases otherwise PASS | 0 | — |
| 4 | run 2 pinned to 4 CPUs | 218/218, 74 s | 9/9 PASS | 0 | — |
| 5 | quota 4G, refresh 2000 ms | 218/218, 82 s | 9/9 PASS | 0 | 0 |
| 6 | quota 4G, refresh default 60 s | 218/218, 82 s | 9/9 PASS, 48 k creates in 90 s | 0 | 3 (one 350 ms pump hold on every member at once, `pub_max` 401 ms) |
| 8 | **fsync + 7 ms** (no queueing), quota 4G, 4 load threads | 218/218, **302 s** | 6/6 PASS; `dir_many_files` 39–49 s per round (75 s budget) | 1 (REPORT_CHUNKS 1.10 s) | 17 |
| 9 | fsync + 0 ms (count only) | 218/218, 193 s (host busy) | 6/6 PASS | 0 | — |
| 10 | **fsync + 7 ms queued on one lock** (the one-disk model), 4 load threads | 218/218, 323 s | **`dir_many_files` timeout after 75 s (rounds 0 and 1), `concurrent_creates_same_dir` timeout after 30 s (round 0)**: 3 of 6 FAIL | 3 (REPORT_CHUNKS up to 1.97 s) | 4 |
| 11 | fsync + 15 ms | formation: `raft-mkfs rc=-13 (resource busy)` for 120 s, fixture never started | — | — | — |
| 12 | fsync + 0 ms, the three cases alone (no load): barrier count | — | 3/3 PASS | 0 | — |

Neither CPU count (runs 3, 4) nor the quota (2 vs 5) nor the catalog
refresh interval (5 vs 6) reproduces it; the fixture passes on a fast disk.
The disk's barrier latency does (runs 8, 10), and the one-device queueing of
run 10 is what the fixture host had: the two cases that timed out on
intel1 time out here, on the untouched base, with the same budgets.

**Where the time goes.** With `EFS_RAFT_OBS=1` every member prints
`raft-host: apply-sleep us=N`: a handler waiting for the pump to apply its
committed entry (or the ReadIndex it serves behind). On the fast disk the
pump cycle already holds `h->mu` for 20–60 ms (`apply_max` 20–60 ms over
19–31 applies per cycle, 1–3 ms per apply) and every such wait is 40–50 ms
(run 5: 6,591 waits, average 46 ms; run 6: 3,755, average 50 ms, 31 of them
over 200 ms, max 400 ms = the budget). Inside the cycle a follower
fdatasyncs its AppendEntries batch (`raft_log_defer_end` under the pump),
the leader fdatasyncs every proposal, and the KV WAL is fsynced once per
cycle; the client's REPORT then PUTs fragments with their own `put_sync`
barriers on three members. Run 12 counts them: the three cases alone (460
creates, 462 unlinks, two mkdir/rmdir, no load) cost **17,928
fsync/fdatasync calls** across the four members (n1 224, n2 5,572, n3
6,795, n4 5,337): about 39 barriers per create-and-unlink pair, 13 per
member. At 7 ms each, queued on one device, the pump's lag passes the 400 ms
budget: `wait_timeouts` 2–17 per member in runs 8 and 10 against 0–3 on the
fast disk, each one a BUSY answered to the client, whose `rpc_writer_retry_pause`
ladder is 50, 100, 200, 400, 800, 800 … ms. Seven attempts (the intel line)
sleep 2.35 s on top of seven server waits of up to 400 ms: the 2–7 s RPCs.

**The 1 GiB quota** (run 3): 24,835 small-file creates filled the fixture's
1 GiB quota and the load then failed with ENOSPC and EIO; the cases
themselves passed. Not part of W159 (the expiry fixture's closing run makes
far fewer files); noted for the small-file space accounting.

## Fix

`src/server/apply_wait.h`, used by `host_wait_applied` (raft_host.c): the
wait is bounded by progress, not by one clock. One grant is the old budget
(400 ms). When it expires and the progress value has advanced since the
grant was set, the waiter gets the same grant again from the new value;
when it has not, the answer is BUSY as before (a stuck apply, a replaced
replica); the whole wait never exceeds `HOST_APPLY_WAIT_CAP_US` (2 s, five
grants: it must end well inside the client's 8 s REPORT and resolve
budgets, so a wait the server cannot finish still answers BUSY while the
client has time to retry; with an 8 s cap `test_k33_payload.py`'s writes
during a member outage ran the client out of budget and its read-back
got EIO in 5 of 9 runs, against 0 of 5 on the base). The progress value is the group's
published applied index plus the member's count of applies so far
(`apply_progress`, one relaxed atomic increment per apply): the first
version used the applied index alone and still expired on a follower
whose single pump cycle (an AppendEntries batch of a hundred entries,
its fdatasync, a hundred applies) ran longer than one grant without
publishing (gate run: `wait_timeouts` 10 on node 2 with no renewal), since
the view is published only at the end of a cycle. The progress-bounded
wait covers a waiter's **own committed entry** (`host_wait_verdict`,
`host_wait_settled`, background proposals, the REPORT batch release), where
a BUSY would only re-propose the request behind the same lag; a ReadIndex
served behind a follower's apply keeps the one fixed budget, because its
BUSY lets the client ask the leader instead and holding the read at the
follower for up to 8 s only moved the wait to the client's read deadline
(`test_k33_payload.py` read EIO twice in four runs on the tree that
extended every wait, against 0 of 2 on the base). `raft-obs` prints the
renewals as `wait_extends=`. No wire, store or setting change; no new
`EFS_*` reader.

| Check | Result |
| --- | --- |
| `tests/test_apply_wait.c` (make test): no progress → BUSY at the grant; progress → renewed from the new index; stalled → BUSY; renewed up to the cap, BUSY at the cap; a cap shorter than one grant | PASS on amd |
| `tests/test_w159_apply_wait.py` (make test-long): three loopback members under the preload (20 ms per barrier, one lock), 128 creates from 16 threads then their unlinks: no error, `wait_timeouts_own=0` on every member; the followers' expired ReadIndex waits, the client's slow RPCs after a BUSY/STALE retry and `wait_extends` recorded | Final tree (`08850a2d3`): **3 of 3 PASS** on amd (128 creates in 25–30 s, creates and unlinks in 36–40 s; expired ReadIndex waits 0–19 per member, own 0; one client RPC over 1 s after a retry in one run; no renewal needed in these runs). History: the first version (applied index alone) FAILed in the gate with `wait_timeouts` 10 on node 2 and no renewal; with the apply-progress counter 1 of 3 runs still saw one client CREATE retried after a BUSY from another source (an allocation intent), so the server's own-entry count decides now. At 50 ms per barrier and 256 creates, every run renews hundreds of grants and 3–30 waits per member still outlive them (the 8 s cap, a cycle without an apply): not a pass/fail setting. |
| A/B at the reproduction's conditions (7 ms, one lock, 4 load threads, 2 rounds): base run 13, fix run 14 | **Base:** `dir_many_files` timeout 75 s and `concurrent_creates_same_dir` timeout 30 s in both rounds (4 of 6 FAIL), `wait_timeouts` up to 4 per member, 3 RPCs ≥ 1 s (max 1.37 s). **Fix:** `concurrent_creates_same_dir` PASS in both rounds (28 s and 29 s, budget 30 s), `dir_many_files` still times out at 75 s in both rounds (2 of 6 FAIL), `wait_extends` 17 and 7 on the two members that waited, `wait_timeouts` 2–8 (a pump cycle longer than one grant publishes no progress), 2 RPCs ≥ 1 s (max 1.95 s). Core run 332 s (base) and 320 s (fix). Partial: the amplification is gone where the apply still advances within 400 ms; the barrier cost itself stays. |
| Refined fix (apply-progress counter, `da75f8527`) at 7 ms, one lock: run 17 (with the base reruns of two gate tests on the host at the same time), run 18 (core run alone, quieter host) | Run 17: `concurrent_creates_same_dir` PASS in both rounds, `dir_many_files` timeout in both, `wait_extends` 75 on n3 and 2 on n2, `wait_timeouts` 4 on n3 and 0 elsewhere; the core run took 438 s under the extra load and timed out `mtime_monotonic_many_writes` (30 s). Run 18: core run **218/218 in 336 s**, no RPC ≥ 1 s (slowest receive 491 ms), so that timeout was the host's load, not the change. |
| Final tree (`08850a2d3`: own-entry waits progress-bounded, reads at the fixed budget) at 7 ms, one lock, 4 load threads, 2 rounds: run 20 | Core run **218/218 in 328 s**; `concurrent_creates_same_dir` PASS in both rounds, `dir_many_files` timeout in round 0 and PASS in round 1 (74 s of 75): **5 of 6** against the base's 2 of 6 (run 13); expired waits 22 on n2 (ReadIndex, by design), own-entry 1 on n3 (one renewal, then the cap); 2 RPCs ≥ 1 s (max 1.35 s); host load average 6.2 (the W123 campaign). |
| Final cap (2 s, `19f8aa426`) at 7 ms, one lock, 4 load threads, 2 rounds: run 21 | Core run 218/218 in 325 s; `concurrent_creates_same_dir` PASS in both rounds, `dir_many_files` timeout in both: **4 of 6** (base 2 of 6, 8 s cap 5 of 6); own-entry waits reached the cap 3 times on n1 and 2 on n2 (two renewals), 6 RPCs ≥ 1 s (max 1.24 s); host load average 7.4. |
| `test_k33_payload.py` (K33 acceptance, step 5 reads with a member down) | Untouched base on amd: **5 of 5 PASS**. Fixed trees: 8 s cap 4 of 9 PASS (every failure a read EIO in step 5), 2 s cap **3 of 4 PASS** (one EIO, same step). The same EIO is on record for an untouched base on intel2 ([b6-k37](b6-k37-merge-tests-20261024.md), one gate), so it is intermittent there too, but the rate on the fixed trees looked higher than this host's base showed. **Settled** (`78d8c9148`, the flake-sweep client fix `fc79fe086` taken: an away member is a spare of last resort, an acknowledged fragment clears its mark): **10 runs each, alternating on amd, this tree 9 of 10 PASS and devel `ec3e09306` 9 of 10 PASS**, the one failure on each side the same step-5 read EIO in the same (third) round. The rate is the base's; the earlier difference was the away-mark defect plus small samples. Raw: `payload-10x/`. |
| Final tree at 15 ms, no queueing: run 19 | No wait on any member outlived a grant (`wait_timeouts` 0 everywhere, against 296 on the base in run 15), so the run says nothing about the change; the host was at load average 6.6 (2.9 in run 15) and the core run took 435 s with `concurrent_appends_two_proc` and `mtime_monotonic_many_writes` timing out, `dir_many_files` PASS in round 0 and timeout in round 1 as on the base. |
| A/B at 15 ms per barrier, no queueing (base run 15, fix run 16) | **Base:** `wait_timeouts` **254** on n1 and 42 on n3 (296 BUSY answers from expired waits), `dir_many_files` PASS in round 0 (69 s of 75) and timeout in round 1, other cases PASS, no RPC ≥ 1 s. **Fix:** `wait_timeouts` **3** (2 on n2, 1 on n3), `wait_extends` 80 on n3; the same case verdicts (`dir_many_files` timeout in round 1 only), no RPC ≥ 1 s. Core run 275 s (base) and 272 s (fix); host load average 3.7 in both. The fix removes the BUSY storm (296 → 3) and leaves the barrier cost, which decides `dir_many_files` here. Formation itself is sensitive to the barrier latency: run 11 at the same 15 ms never formed (`raft-mkfs rc=-13` for 120 s), runs 15 and 16 formed within seconds. |
| amd `make test-all` on the final tree (`19f8aa426`: 2 s cap, all harness updates; `EFS_TEST_TARGET=test-all bash scripts/make_test_summary.sh`) | 473 commands, **10 FAIL, 6 NOTRUN** (2746 s): the 9 RDMA host limits (`test_declaration_gate`, `test_rdma_xprt`, 7 × `test_rdma_segmented`, as on every amd gate and on devel there) and `test_stop_progress.py` (the clean stop could not unmount the test's FUSE mount: `fusermount3: failed to unmount`; in the two earlier gates the desktop's `gvfs-udisks2-volume-monitor` was named as holding it; the test passed 2 of 2 standalone on the base and 1 of 3 gates); `test_k33_payload.py` and `test_w159_apply_wait.py` PASS in this gate. NOTRUN: cluster scripts, EFS_FAULTS build, no first-catalog race in 12 formations, valgrind absent, TMPDIR not XFS, the runner self-test. Raw: `testall-src5/`. |
| amd `make test-all` on `08850a2d3` (8 s cap, with the test copies of that commit; `EFS_TEST_TARGET=test-all bash scripts/make_test_summary.sh`) | 473 commands, **15 FAIL, 6 NOTRUN** (2383 s): the same 9 RDMA host limits; `test_k33_payload.py` (read EIO) and `test_stop_progress.py` (`gvfs-udisks2-volume-monitor` holding the test's mount) as below; and four source-slice harnesses whose stubs did not know the renamed wait or the new field (`test_unlink_verdict.py`, `test_gc_verdict.py`, `test_txn_apply_exact.py`, `test_apply_ring.py`: compile errors, no product failure), fixed in `55b0d21c1` and the commit after it and PASS standalone on amd. `make -k test` on the final tree: see below. |
| amd `make test-all` on the first fix version (`8b860a554`) | 473 commands, **12 FAIL, 6 NOTRUN** (2563 s). 9 FAIL are RDMA on this host (`test_declaration_gate`, `test_rdma_xprt`, 7 × `test_rdma_segmented`: `CQ moderation off failed`), the same set as every earlier amd gate ([pack-unindex](pack-unindex-race-20261024.md): 9 RDMA FAIL on devel too). `test_k33_payload.py` (EIO, the intermittent noted at [b6-k37](b6-k37-merge-tests-20261024.md)) and `test_stop_progress.py` (the desktop's `gvfs-udisks2-volume-monitor` held the test's FUSE mount, so the clean stop refused) are host conditions: base reruns TBD. The twelfth was the first version of `test_w159_apply_wait.py` (above), fixed by the apply-progress counter and PASS standalone since; `make test` on the refined tree: TBD. NOTRUN: cluster scripts, EFS_FAULTS build, no first-catalog race in 12 formations, and the RDMA/valgrind host limits. |

## Limitations and follow-ups

- `test_k33_payload.py`'s step-5 read EIO is a base intermittent (1 of 10
  on devel `ec3e09306` and on this tree with the flake-sweep client fix,
  table above); the earlier higher rate on the fixed trees went with the
  away-mark defect that `fc79fe086` fixes. The `EFS_PUT_AWAY_MARK_MS`
  inventory row belongs to that branch.
- Single host, loopback, TCP, emulated barrier latency: regression
  evidence, not qualification (A4). The intel1 fixture logs could not be
  read (owned by `efs`); the mechanism is shown on amd with the emulation,
  and intel1's barrier latency was measured on its other consumer NVMe.
- amd was shared with the W123 campaign; core-run times vary with it
  (73–193 s for the same configuration), so only the A/B pairs run back to
  back under the same emulation are compared.
- **Follow-up (product, Stage 3):** the barrier count itself. About 13
  fsync/fdatasync per member per create-and-unlink pair, and the follower's
  AppendEntries fdatasync runs inside the pump under `h->mu`, so on a
  slow disk every handler waits behind it. A fixture or a cluster on
  consumer NVMe stays 3–4× slower than one on enterprise NVMe until that
  path batches better. The fix here removes the amplification (0.4 s →
  seconds), not the base cost.
- **Follow-up (harness):** `posix_suite.py --suite expiry` passes
  `--disable-efs-commands`, which `gc_reclamation.py` refuses since D42, so
  the expiry fixture itself cannot start on this base (`SystemExit`); and
  `raft-mkfs` answered `rc=-13` for 120 s at 15 ms per barrier (run 11;
  checked with the fix in run 16).

## Reproduce

On a Linux host with `/dev/fuse`, from a private copy of the tree
(`make -j32 all`):

```
python3 scripts/w159_fixture.py --work ~/w159/run --out OUT --port 24510 \
    --quota 4G --query-refresh-ms 2000 --repeat 2 --load-threads 4 \
    --fsync-delay-us 7000 --fsync-lock
```

Exit 0 when every case passed and no RPC took 1 s or more. `OUT/summary.json`
holds the verdicts, `OUT/slow-rpc.txt` the client's slow RPC lines,
`OUT/raft-status.log` and `OUT/host.log` the samples, `OUT/n*.log` the
members' `raft-obs` lines (`apply-sleep`, `wait_timeouts`, `wait_extends`,
`slow-fsync: ... calls=`). Compare two trees with `--source`. The barrier
count alone: `--fsync-delay-us 0 --phase loaded --repeat 1 --load-threads 0`.
`make test-apply-wait` and `python3 tests/test_w159_apply_wait.py` are the
regression tests.

## Raw evidence (outside Git)

amd `~/efs-artifacts/w159-20261025/`: one directory per run
(`run1-fixture-refresh2000` … `run21-fix5-fsync7ms-lock`), each with
`summary.json`, `posix-*.tsv`/`.out`, `fuse.log`, `n1.log`–`n4.log`,
`raft-status.log`, `host.log`, `slow-rpc.txt`, plus the driver console
`runN.log`; `testall-src/`, `testall-src4/`, `testall-src5/` for the three
gates (`summary.txt`, `timings.tsv`, `cmds.txt`, `fail-*.log`); the
standalone reruns (`basefail*`, `fixre*`, `v3`–`v5-*`, `basepay*`,
`w159test*`, `maketest-*`), the build logs and `fsync_probe.py`.
`payload-10x/` (the ten alternating `test_k33_payload.py` runs per tree).
`SHA256SUMS` over 616 files, sha256 `f5b13550…4fc5`. Private trees
`~/w159/src`, `~/w159/base`, `~/w159/devel` and the work directories
deleted after the run.
Source commits: `8b860a554` (first fix, reproduction script, tests),
`da75f8527` (apply-progress counter), `1da68234a` (own-entry waits only),
`08850a2d3` (own-entry counter), `19f8aa426` (2 s cap), harness updates
`6bc5ac5ea`, `6e17db494`, `55b0d21c1`, `f0216b72d`, `78d8c9148` (the
flake-sweep client fix taken).
