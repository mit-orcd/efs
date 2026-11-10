# K38 recurrence on `9bb50fe79`: a retained span the host refuses by rule (Task FB)

Task FB, Nov 9 to 10 2026, node9900 (private clusters only). Purpose: find why `ior hard` returned `EIO` again on orcd-mini with the candidate build `9bb50fe79` (Task EW: 2 of 8 runs, two-client variant 2 of 4), fix the root cause, prove it. Linked row: K38 (both tables), Stage 1 gate O1.

**Verdict: cause found and reproduced privately, fix and regression test committed, K38 stays OPEN and blocks alpha.** The STALE replay classifier `stale_retry_owned` keeps a span record for an unchanged resend when the pulled map's base, span count and newest span seq equal the record's view. The host also refuses a span that overlaps a live span of another object, and any span on a full chain, whatever the base; so such a record loses the same `OVERLAP` at apply every round (`committed=0 replayed=0 retained=N`, forever) until the REPORT budget ends in `EIO`. `span_refused` now mirrors those two host rules, so the record takes the ordinary rebase (a fold). Private reproduction (24 writers, one client, shared chunks): 4 `EIO` in 40 runs (10 %; 34 valid runs, 6 harness "no pressure" verdicts), the two-client variant 0 in 40. After the fix: 0 `EIO` in 80 runs (69 valid runs, 11 "no pressure" verdicts), and 0 in 40 more on a build with a hit counter in which the new path fired 7 times in 4 runs. The live orcd-mini `ior hard` rerun is owed.

## Source and environment

- Source: devel `98e34842d`; `git diff 9bb50fe79 HEAD -- src include tests Makefile` is empty, so the product code is the candidate's. Before arm: that tree from `git archive` (tar SHA-256 `a38fcf442a46606ad7f5262c077576ec75820569761a850608e4f195df29eb1e`, identical on the Mac and on node9900). After arm: the same tar plus the fix commit `e62b4b907` (applied as the patch of that commit; the diff of `src/client/write.c` and the two tests).
- Host: node9900 (192 CPUs, AMD EPYC), XFS `/tmp`, other sessions' work running (load average 14 to 36 at the start of each batch, recorded per run in `uptime.txt`). gcc `-O3`, `make test-bins tests/faults/efsd`, libfuse from `/data1/efs/deps/libfuse`, `build=unknown` (no git metadata). Private ports only, no live-cluster process touched.
- Test: `tests/test_k38_shared_chunk_pressure.py --clients 1` (three `efsd` of the `EFS_FAULTS` build, one `efs-fuse`, 24 writer threads, 200 records of 47,008 bytes each in the ior hard layout, direct client body budget 32 MiB). The transport is the default `auto`: on node9900 both daemons and client report `transport auto: rdma on mlx5_0 (TCP carries setup)`, so these runs are RDMA over the local device, not loopback TCP.

## Reconstruction from the saved logs

Source logs: `~erbmi1/efs-artifacts/taskEW-20261109/o1b/hard-1/` (ino 4174) and `hard-2/` (ino 6980), read only.

1. **What differs from a passing run.** A passing run also meets the trigger. `o1/hard-1` logs `rejected unowned staging observation … (putid miss)` at 22:22:29 and one `report-stale: chunks=107 … retained=27`, then `retained=28` once, and the next rounds drain (3 `report-stale` lines in the whole run). In `o1b/hard-1` the first line is `report-stale: chunks=87 runs=5 committed=60 replayed=1 retained=0` at 22:25:42.535, then `retained=3`, `3`, `25` within 50 ms, and from 22:25:44.248 the line is the same every ~26 ms: `chunks=93 runs=3 committed=0 replayed=0 retained=49` (797 lines in all; in `hard-2` `chunks=1407 … retained=1407`).
2. **What the client sent and why the host answered STALE.** Each round is a REPORT of the dirty snapshot (1,598 records at 22:26:07 on ino 4174). `host_pub_batch_wait` (`src/server/raft_host.c`) waits for the last proposed entry and reads every entry's apply verdict from the ring; any STALE verdict makes the REPORT STALE (`fail=wait/-14`, `report-split … rc=-14`). The verdict is the apply's: a `PUBLISH` of a span answers STALE (`EFS_PUB_STALE_OVERLAP`) when its range overlaps a live span of the chunk row (`src/meta/meta_apply/publish.c`). The log lines before 22:26:07 are absent because `report-split` is logged only for 256 or more records or 100 ms or more; the apply-time STALE leaves no server line at all, which is why the live logs never named the loser (`stuck=unrecorded (an apply-time CAS loser)`).
3. **Why the replay makes no progress.** `stale_repull_replay` (`src/client/write.c`) pulls the real map and classifies each sent record: done (the host holds the sent object), replay (rebuild on the pulled map), or retained. `retained` counts `stale_retry_owned` returning true. That helper was added (`b7ef01a76`, `7249fc81e`) so a unit aborted by admission BUSY does not upload the same bytes again: it keeps the record when the pulled row's base generation, span count and newest span seq equal the record's `base_gen`, `delta_base_n` and `delta_base_seq`. For a span that is not enough. A span whose range overlaps a live span of another object cannot be published whatever the base, and the span builder says so itself (`span_of`: "Any overlap with a committed span needs a full CAS fold of the freshly fetched image"). The retained record is sent again unchanged, overlaps again, and nothing changes the map, so every round is identical. The K38 bound (`report-stuck`, two unchanged STALE rounds on a moved base) does not see it: it counts records that go through the replay, and these are skipped before.
4. **Why resolution ended `unknown` and not in a rebuild.** The loop runs inside one sync REPORT whose budget is 16 s (`total_ms=16013`). Its retained rounds call `efs_client_put_retry_admitted`, one ticket status RPC (`put-ticket … op=1`) per round after the first; at 22:26:15.782 and again 8 s later that RPC lost its exchange to node 2 (`put-ticket: lost exchange op=1 shard=3124 nid=2 attempt=0 -> NET`), so the round returned NET. `report-resolve` then asked for the receipt: `outcome=partial rc=-14 … stuck=unrecorded`; the second time (22:26:23.771) the REPORT deadline was already spent, `queries=0 ms=0`, so there was no time to ask and the outcome is `unknown`, which the client turns into `EIO` (`report-sync-fail … stage=report-retry total_ms=16013 rc=-1`). That `-1` is what the application saw. The same chain in `hard-2` ended at 22:28:27.
5. **Records that fail with it.** In the private reproduction the first `report-stuck` is on a record of another chunk (`ci=808 … span=2848+128224 stale_rounds=2`). K32 publishes the records of an atomic unit as one transaction: one refused member (the retained overlap) keeps the unit from committing, and the other members, rebuilt but sent unchanged, reach `stale_rounds=2` and are stopped as writeback errors. They are victims of the loop, not a second cause.

Which case the earlier fixes do not cover: they cover a record whose base moved (superseded or rebuilt on the committed image, `report-stuck` after two unchanged rounds) and a body-less span. They do not cover a span whose view is current but whose range overlaps a live span, because `stale_retry_owned` treats an unchanged view as proof that the host will take it.

### Instrumented reproduction of the loser

A diagnostic build of the before tree (not committed; prints from `publish.c` at each STALE return and from `stale_repull_replay`) caught a failing run: `efs-k38-orxy53ct`, ino 4097. The server logged 9,015 `pub-stale line=361` (the `EFS_PUB_STALE_OVERLAP` return), three nodes applying: 1,500 per replica for each of two chunks, `ci=1373 dlen=94016 doff=31776` and `ci=1399 dlen=94016 doff=8480`. The client logged for the same two records `FBDIAG retained ino=4097 ci=1373 uncond=0 dlen=94016 doff=31776 base=b0fe7663d5ff67b8 ce_gen=b0fe7663d5ff67b8 ce_nd=1 ce_dseq=30 rec_dbn=1 rec_dbs=30`: the base, the span count and the newest seq of the pulled map equal the record's view, the record is kept, the host refuses it, 1,500 times. A second diagnostic run (`efs-k38-v3pen1_u`, `FBDIAG overlapped`) shows what the overlapped span is: the writer's own earlier span. The record `rec=27456+94016` overlaps the delta `d.off=27456 d.len=47008 d.seq=10` whose generation `d.gen=4df28ce8ec2fe483` is the generation the builder skipped as its own (`FBDIAG span-skip-mine … mine_gen=4df28ce8ec2fe483`, same chunk `ci=494`). `span_of` skips an overlapped delta that carries the writer's own generation, and the local table gives entries a predicted `seq = obs_seq + 1`, so the builder cannot tell that delta from the host's committed copy; only the host's apply decides. The classifier, which sees the pulled map, is therefore the place for the rule, not the builder.

## The anomaly in the Raft follower log (`why=prev-compacted`)

Not the cause of the `EIO`, and a defect. It is in every run, passing ones too (`o1/hard-*` show the same counter), and was already counting before the first failure. What it is, from `src/raft/raft.c`:

- `prev=0/0 … commit=<n>` is `broadcast_read_probe`, the ReadIndex contact: an AppendEntries with `prev_index 0`, `leader_commit 0` and `vote_granted=1` ("universal empty prefix"), sent to every peer on each read round, about 1.5 per second here.
- A follower with a snapshot (`snap_idx > 0`: all of them after the first compaction) rejects it in `on_ae_req`: `prev_index (0) < snap_idx` gives `why=prev-compacted`, `success=0`, `match_index = snap_idx`. So the probe can never be an acknowledgement; a ReadIndex round gets no ack from a compacted follower through it and relies on the commit probes and heartbeats that follow.
- The rejection is treated as an ordinary log rejection. `on_ae_rep` does not return early for a rejected probe: it counts `peer_rejects` (the `rejects=` counter), sets `next_index[peer] = match_index + 1 = snap_idx + 1` (the leader's own line prints `next=` before that rewrite: `hint=7369006 next=10861667`), and calls `send_ae(peer)`, which ships up to `EFS_RAFT_AE_MAX` (128) entries or `EFS_RAFT_AE_BYTES` (1 MiB) from `snap_idx + 1`, entries the follower has held for millions of indices, or, if that index is below the leader's log floor, goes to `send_ae_behind` and `send_snap`. The next success reply restores `next_index` from `match_index`. So the leader keeps re-sending a stale prefix to every follower, once per read round.
- The comment at the rejection branch ("Success/read probes … returned above and never trigger this branch") holds for successes only.

Impact: wasted log reads and network (bounded at 1 MiB per rejection) and, if the floor is above the follower's snapshot, a snapshot image sent to a healthy follower; it does not touch REPORT results. Fix, not done here (the coordinator gives it its own row): make `on_ae_req` accept a zero-prefix read probe (`vote_granted` set, `prev_index 0`, no entries) with `success=1` and `match_index = 0` before the `prev-compacted` test, or make `on_ae_rep` return after a rejected probe without touching `next_index`.

## Reproduction and rates

Before the fix (`98e34842d`, node9900), commands in the next section: 

| Arm | Runs | `EIO` | "No pressure" (harness, not `EIO`) | Valid runs | Load average at run start |
|---|---|---|---|---|---|
| `--clients 1` | 40 | 4 (runs 2, 9, 17, 19) | 6 (11, 15, 16, 24, 37, 38) | 34 | 12 to 38 |
| `--clients 2` | 40 | 0 | 0 | 40 | 24 to 35 |

The failing runs end with `writer N after M records: pwrite: OSError(5, 'Input/output error')` followed by the same on `close`. In the kept tree of run 2 the client log has the signature of the live failure: `report-sync-fail … rc=-14` every 2 to 3 s growing in `total_ms`, `background-pressure ino=4097 rc=-14 retained`, then `report-stuck` on other records of the same unit (`ci=1424 … stale_rounds=2: not re-sent, writeback error`). The two-client variant did not reproduce here (0 in 40); the live two-client failures (2 of 4) are explained by the same path but not reproduced.

Commands (prerequisites: the build tree from the tar below, `make -j24 all test-bins tests/faults/efsd`, libfuse from `/data1/efs/deps/libfuse`, `TMPDIR` on a scratch directory that is deleted after):

```
tar xf efs-taskFB.tar -C /tmp/efs-taskFB         # git archive 98e34842d
cd /tmp/efs-taskFB && make -j24 all test-bins tests/faults/efsd
bash loop.sh tcp-c1 40 python3 tests/test_k38_shared_chunk_pressure.py --clients 1
bash loop.sh tcp-c2 40 python3 tests/test_k38_shared_chunk_pressure.py --clients 2
```

`loop.sh NAME N CMD...` (kept with the raw output, `scripts/loop.sh`) runs `CMD` N times from the build tree, appends `uptime` before each run and `run I rc=R` after it to `summary.txt`, and keeps the output of failing runs only. The run name `tcp-c1` is historical: the transport is `auto` (RDMA, see above).


## Fix

`src/client/write.c`, new `span_refused` and one line in `stale_retry_owned` (callers: the two calls in `stale_repull_replay`; the first, before the pull, only passes unconditional full images, which `stale_retry_owned` returns for before `span_refused` is reached). `span_refused` mirrors the host's rules in `publish.c`: a span is refused when a live span (length not 0) of another object overlaps its range, or when the chain holds `EFS_CHUNK_DELTA_MAX` live spans. The record's own landed span (same generation, offset and length) is the classifier's done case and is not counted as an overlap; a tombstone is not live. A refused span no longer returns "retain": it takes the ordinary rebase (`dcache_cycle_verdict` and the replay), whose rebuild sees the pulled map and folds the span into the fetched image, as `span_of` already does for any new span. No retry count, timeout or sleep changed; nothing is acknowledged that the host did not take; the host stays unchanged.

## Test

`tests/test_stale_recovery.py` (fast suite, already budgeted) drives the production classifier and `stale_retry_owned`: with a view equal to the host's, a span overlapping a live span of another object, and a span on a full chain, must be replayed (rebuilt); a disjoint span stays retained; a tombstone is not live; the record's own landed span is done. With the old source (`OWNED_RETRY_SOURCE=<old write.c> STALE_CLASSIFIER_SOURCE=<old write.c>`) it aborts at `replays==1&&!commits&&inos[0]`, the retained record. `tests/test_k38_stale_span_replay.py` extracts the same helper and needed `span_refused` added to its harness. Budget row: `tests/budgets.tsv` row `python3 tests/test_stale_recovery.py` (5 s, 1 slot) kept; its note records 0.20 s over three runs on node9900 with the new cases, and its description names the span rule. No new test file and no new row: the case belongs to the classifier harness that already extracts and compiles the production functions (deviation from "one new test", reported to the coordinator).

## Results after the fix

Fix tree: the before tar plus commit `e62b4b907` (identical `src/client/write.c` to the final commit `f4ef17f04`, which only adds a budget note). Same command, same host, same test; loop script `loop-fix.sh` is `loop.sh` run from the fix tree.

| Arm | Runs | `EIO` | "No pressure" (harness, not `EIO`) | Valid runs |
|---|---|---|---|---|
| before, `--clients 1` | 40 | 4 | 6 | 34 |
| **after, `--clients 1`** | **80** | **0** | 11 | **69** |
| after, build with a hit counter (new path fired 7 times in 4 runs, all 4 passed) | 40 | 0 | 8 | 32 |

Valid runs after (69) are twice the valid runs before (34). The "no pressure" verdict is the test's own precondition (`no client ran under dcache pressure`, no `EIO`, the run proves nothing about this bug); it is higher in the after arm because the host was busier (load average 24 to 46), and those runs are neither passes nor failures here: strictly, `rc` was nonzero in 11 of 80 runs, all of them that verdict. Expected value with the before rate (10 % of all runs, 12 % of valid runs): about 8 `EIO` in 80 runs; observed 0.

Gates on the final commit `f4ef17f04` (clean `git archive` tree on node9900, `gate.sh` in the raw output):

| Gate | Result |
|---|---|
| `make all`, `make test-bins tests/faults/efsd` | rc 0 |
| `make test` | rc 0 (1 m 56 s), includes `test_stale_recovery.py` with the new cases and `test_k38_stale_span_replay.py` |
| `bash tests/sanitizer_check.sh tsan` | rc 0, `VERDICT: PASS (31 run, 0 failed, 609s)` |
| `python3 scripts/tests_for.py client/write --run` | rc 0, 50 commands, 10 m 37 s |
| Test against the old source | `OWNED_RETRY_SOURCE=<old write.c> STALE_CLASSIFIER_SOURCE=<old write.c>`: aborts at `replays==1&&!commits&&inos[0]` (checked when the case was written) |
| Docs: `check_k_register.py`, `check_doc_links.py`, `status_layout.py`, `make docs-check` | see the commit of this page |

The `fix` arm is the private reproduction only; the live `ior hard` on orcd-mini is not run.


## Raw output and checksums

Raw output (not in Git): `~erbmi1/efs-artifacts/taskFB-20261110/` on node9900 (772 KB, 50 files), listed with SHA-256 in `SHA256SUMS`, whose own SHA-256 is `3f5793954ebfedf9f4d8314fa9eb5783c5e4262e87c5dfc1c30c9e615843b5c1`.

- `before-c1/`, `before-c2/`, `after-c1/`, `after-fixdiag/`: `summary.txt` (rc per run), `uptime.txt` (load at each run start), output of the failing runs without daemon log lines, `fix-path-hits.txt` (one line per fix-path hit), `run-2-mnt-a-signature.txt` (excerpt of the client log of a failing before run).
- `diag-before/`: summaries of the two diagnostic loops and the `FBDIAG` excerpts quoted above (`v1-orxy53ct-signature.txt`, `v2-v3pen1_u-builder.txt`).
- `gate/`: `make test`, TSan and `tests_for` output and statuses. `scripts/`: `loop*.sh`, `gate.sh`, `diag.py`, `diag2.py` (log filters), `fix-e62b4b907.patch`. `tar-sha256.txt`: the two build tars (before `a38fcf442a46606ad7f5262c077576ec75820569761a850608e4f195df29eb1e`, fix `ca8d7954edc86f12ff1e61774d781b0251932acef6c6cb0d75cb33fb2b13dce0`).
- The input logs of Task EW are in `~erbmi1/efs-artifacts/taskEW-20261109/`, unchanged. The `FBDIAG` print statements were a diagnostic patch and are not committed; the reproduction does not need them.
- Kept test trees on node9900 (`/tmp/efs-taskFB-tmp`) are deleted after this page; the run directories and `/tmp/efs-taskFB*` build trees are removed.

## Not done

- The live orcd-mini `ior hard` rerun (O1) and the two-client variant on the fixed build; this page proves the loop only on a private cluster.
- The unconditional full images that `stale_retry_owned` retains before the pull (`stale_repull_replay`, first loop) are still retained without checking the host's fold rule (a live span of the current epoch newer than the record's `delta_base_seq` makes the host refuse it). The same shape, not seen in any log; left as it is because no evidence of a failure and the Task EO tests rely on it.
- `o1-shared` shared-3 and shared-4 (`rc=3`, EIO from rank 4 to 7 and zero-filled records on verify): those runs have no `report-stale` line; the client log carries `background-pressure ino=6980 rc=-3`, the inode of the removed `hard-2` file, so that client was still the one poisoned by `hard-2`. A client that keeps retaining the dirty chunks of a removed inode and answers other files with `EIO` is a separate defect; not analysed here.
- The Raft zero-prefix probe defect above: described, not fixed.
- The "no client ran under dcache pressure" verdicts of the private test under load (harness check, not `EIO`) are counted apart in the tables and are not passes.
