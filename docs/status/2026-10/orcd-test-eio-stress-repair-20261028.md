# ORCD-test EIO and stress repair — Oct 28

> **Coordinator note (2026-10-30):** merged for its live-gate and IOR records and the serial POSIX
> correction only. The branch is ~294 commits behind devel; only the repairs with a failing
> reproduction on devel were ported, see [EIO product port](eio-product-port-20261030.md). The
> diagnostic-only commits and the POSIX fixture-retention change were not ported. Source commit
> hashes below name the old branch, not devel.

**IN PROGRESS / qualification not yet passing.** Repair branch
`agent/orcd-eio-stress-repair-20261028`, based on devel `9c4bfd171`.
This follows [the failed devel qualification](orcd-test-devel-qualification-20261028.md).
No deadline or durability requirement is relaxed, and original failures remain failures.

## Evidence and reproductions

Original deployed source: `714d7e6fc8766798fcaeaca373c337750b3f3368`.
Its retained 441-file manifest was rechecked before investigation:
`7a50fdeaa92b7c90aeb1a06598c638d4b5ab857bf2d329e80733464b693408b1`.
Raw repair evidence stays on ORCD at
`/home/erbmi1/orcd/scratch/efs/orcd-test/eio-stress-repair-20261029T0200/`.
The timestamp reflects UTC; this investigation started Oct 28 locally.
Failed fixtures and existing stores stay on ORCD. No bulk export to the Mac.
Completed diagnostic/build/stop evidence was sealed as 245 files (2576096074
bytes) in `SHA256SUMS-repair-before-qualification`, manifest SHA-256
`0059495406f6c2e9b2379d22db641cf3342ae46bf139e270f9dd972f54e76c14`.
The manifest check passed for every listed file. This excludes source/build
directories and the subsequently restored-state check.

Topology is fcstor003–006 (four storage nodes) and fcstor007–013 (seven clients),
RDMA mlx5_2, O_DIRECT fragment storage, buffered FUSE writes. No fstor007 or
private cluster is involved. The public stores are retained without formatting.

On the original long-running mount, this command reproduced immediately:

```sh
python3 -u tests/live/concurrent_extend_eof.py /tmp/efs-mount --cases 160 --workers 16
```

`baseline-extend-00/` records 159/160 PASS and one close-time EIO, inode 121109.
Readback size was 8388609; this does not turn the failed close into success.
Server logs identify `pack-nosize`: REPORT lacked the required inode size record.
Earlier inode 53179 failed with size 1048577 instead of 8388609. These outcomes
are distinct; neither is dismissed as a timeout or a kernel page-cache artifact.

A separate mount of diagnostic source `e7f9a89a1` passed 3200 cases at the default
metadata cache budget. With the existing `EFS_CLIENT_META_MB=16` diagnostic
budget it failed on round 5 (159/160), inode 66890. Its trace records table
evictions, then `report-size-missing` / NOT_FOUND, then EIO / `pack-nosize`.
This is in `diagnostic-eviction-e7f9a89a1/`; its failed fixture remains
`/tmp/efs-mount-eio-diag/extend-eof-pzkziyr5/125/f` in the shared namespace.
All diagnostic mounts were cleanly stopped. The small cache is a reproducer,
not a replacement qualification profile.

To repeat that comparison, mount with `EFS_CLIENT_META_MB=16 EFS_STAGE_DBG=1`,
then run the above extension command against that mount up to 20 times, each
with 160 cases and 16 workers, retaining each JSON output and stopping at the
first failure. The harness retains failed files and removes passing fixtures.

## Changes and focused checks

- `e7f9a89a1`: POSIX FAIL/NOTRUN fixtures are retained automatically; PASS/SKIP
  fixtures retain the usual cleanup. The real suite finalizer regression passes;
  the original finalizer fails the retained-payload assertion. Original campaign
  cleanup had already removed many failed stress trees; logs survive but those
  trees cannot be reconstructed as evidence.
- `d8a7df06c`: selectively ports reviewed `662789008` from Task H. A mixed inode
  hash replaces a low-bit mask that mapped same-shard allocated files onto one
  I/O mutex for each chunk index. The production lock-selection regression passes;
  original code fails its distribution assertion. No timeout changes.
- `de0bb5dc7`: holds an operation pin across ordinary FUSE OPEN, from before its
  staged lookup until the descriptor's open-reference pin takes over. Previously
  cache eviction could remove the row after stat released its locks, before
  OPEN registered its reference. Buffered writes then had no exact EOF to update.
  Error exits unpin; virtual files bypass the staging pin. The regression now
  executes the actual production OPEN body with eviction scheduled at its
  permission, handle and lease boundaries. The original OPEN body fails the
  retained-row assertion; success and error exits of the candidate pass. A
  redundant wrapper-only adapter was removed.

Candidate `de0bb5dc7` passed 20 × 160 = 3200 extension cases with the same 16 MiB
cache reproducer. Evidence: `diagnostic-eviction-de0bb5dc7/`, including binary
hash and build log. This supports the OPEN pin repair; it is not the full stress
qualification. Four simultaneous full suites on the diagnostic original client
also reproduced 15-second name-case timeouts, without EIO; those remain failures.

Fourteen focused Linux regressions passed against `de0bb5dc7`, covering OPEN,
eviction LRU, REPORT exact/zero EOF, retry ownership, snapshot locks, pressure,
reply handling, lookup memoization, fixture retention and timeout worker census.
Commands and output are retained in `unit-de0bb5dc7/`. Local macOS LSan probing
aborted as unsupported; the Linux LRU test subsequently passed including its
available sanitizer checks. No macOS sanitizer PASS is claimed.

At the original default cache budget, `diagnostic-stress-0958c03dd/` ran four
concurrent full suites (16 workers each): **875 PASS, 5 FAIL, 12 SKIP**. All four
failed `names_crazy_dirs` at the unchanged 15-second deadline; one also failed
`perm_dir_no_x_search` with EBUSY removing `sub`. No missing-size/report failure
was logged. This is still a FAIL, on a diagnostic mount against original servers,
not a full deployment qualification. The old unresolved public clients remained
present during it; no clean-baseline performance conclusion is drawn.

`profile-stress-0958c03dd/` repeats that diagnostic with a 99 Hz cycles profile
of only the separate client (no debugger pause). It retained `client.perf.data`,
1078 samples, no reported lost samples, and a text report. `opid_take_locked`
accounts for 18.54% of sampled cycles, from the retained lease-release worker.
This motivates inspecting admission/recovery progress; sampled CPU proportions
do not establish the cause of wall-time failures. Other retained client logs
show SETATTR operations eventually succeeding after roughly 4–7 seconds and
repeated data-cache body-pressure notices. No backoff or case limit was changed.
Four additional local lease/retry regressions also pass, including unchanged
budget and unresolved-ownership cases.

## Approved failed-client capture and additional repair

The user approved capture and retirement specifically for fcstor008. Its exact
owner was PID 136948, start time 624214817. `gcore -a` captured
`approved-core-fcstor008/core.136948` on ORCD (2214223584 bytes), SHA-256
`dc4d92257c73b90291ae104b20b68ba799be12f91ff0d7d47868d0aedf2b4a4d`.
The hash check passed and GDB opened the core against the frozen original
executable. GDB recovered the expected export, dirty counters and thread state.
The capture reports one unreadable 4 KiB kernel vsyscall page at
`0xffffffffff600000`; this is not a claim that every mapped byte was captured.
The full core is kept on ORCD while this investigation remains open.
The capture, retirement, subsequent build and diagnostic evidence is sealed
in `SHA256SUMS-approved-capture-and-1b`: 137 files, all hash checks PASS;
manifest SHA-256 `7184fc8d7daeb5ac86f3477b463c25569562e0a134f239b12aa93a359570fdc6`.

Offline inspection (`offline-summary.json`) finds 60 dirty inode marks,
20 dirty chunk records across 18 inodes, no active publication slots, and five
chunk-owning inodes whose canonical staging tables contain no indexed row:
25819, 36517, 49293, 53111 and 80201. This independently corroborates missing
staged EOF ownership; it is a post-failure snapshot, not proof of every earlier
failure's interleaving or of payload integrity.

The approved force-discard control unmounted fcstor008. Its subsequent shutdown
still attempted metadata REPORT and exited with `metadata report rc=-5 after
60s`; the final process and mount are both absent. This is an intentional
retirement after capture, **not a clean shutdown/recovery PASS**. No claim of
rollback, of a known outcome for every write, or that all post-capture REPORT
attempts had no effect is made. The initial incorrect CLI argument order
produced only a usage error and was retained alongside the corrected attempt.

`1b840457a9ed7c9ae3cc8478ab9022ee8648efeb` additionally fixes CREATE's
non-exclusive EEXIST fallback. The fallback previously acquired a FileID/lease
without restoring a missing staged inode or taking ordinary OPEN's operation
pin. It now delegates to protected OPEN, including access checks and O_TRUNC,
and returns its real error. The actual CREATE-function regression covers
handoff, intact flags, failure propagation and O_EXCL refusal; the prior source
fails the delegation assertion. Newly created files already stage their result
after installing the open reference, so that path is unchanged.

This candidate built on all eleven hosts (`build-1b840457a/`), passed 18 focused
Linux regressions (`unit-1b840457a/`) and another 3200/3200 small-cache extension
cases (`diagnostic-eviction-1b840457a/`). An orchestration syntax error occurred
before any workload started; its log is retained separately. At the default
cache budget its four concurrent full diagnostic suites still **FAIL: 870 PASS,
10 FAIL, 12 SKIP**. All ten failures hit unchanged 15-second deadlines in deep
nesting, adversarial names or mtime tests; no missing-size or REPORT-sync failure
was logged. These are diagnostic clients against original servers with two
old unresolved clients still present, not the final deployment qualification.

## Public repaired-build qualification — still FAIL

The user approved verified core capture and retirement of fcstor010 and fcstor011.
Both are now retired; their original files and stores remain. Captures under
`approved-core-fcstor010/` and `approved-core-fcstor011/` were hash-checked and
opened with GDB against frozen executables. Full core SHA-256 values:

- fcstor010: `e998d4b111d59a14cb83b48092ca6a2b6812b6a102841ecc9c8a6027f5c064fa`.
- fcstor011: `576a81a2742bfb96d73c41ac61ab6df9bd56dea2618011433cffd92d0a2b056e`.

Each capture has the same unreadable kernel vsyscall page caveat as fcstor008.
Offline inspection finds 28 dirty chunk records across 25 inodes on fcstor010,
and 24 across 21 on fcstor011, without missing canonical inode rows. Their
failures therefore cannot all be attributed to the missing-row OPEN race.
Retirement is not clean-shutdown or payload-integrity evidence.

All four original daemons subsequently stopped cleanly. Source `1b840457a`
was deployed to all eleven hosts on retained stores, without formatting.
Readiness and all seven mounts passed (`deployment-1b840457a/deployment.json`).
`qualification-1b840457a/` records:

- Extension oracle: 160/160 before stress and 160/160 after stress PASS.
- Serial full POSIX: 220 PASS, 0 FAIL, 3 SKIP.
- Seven parallel full suites, 16 workers each: each 220 PASS, 0 FAIL, 3 SKIP.
- Heavy stress, four full suites per client, 16 workers per suite:
  **5766 PASS, 313 FAIL, 46 SKIP, 119 NOTRUN**. Most failures are unchanged
  deadlines; BUSY and two refused-truncate content mismatches also remain.
  No literal Input/output error or missing-size REPORT signature was observed
  in this round; this does not qualify the failed stress round as PASS.
- Three remote client pairs: each 67/68 PASS; concurrent append hits the
  unchanged 15-second SSH step deadline. Shared pwrite now passes.
- Post-stress readiness PASS. No repaired-build IOR or cold-restart PASS yet.

Both truncate failures are on fcstor013, stress-2 and stress-3, in
`truncate_big_ftruncate_honest`. The oracle saw old size but zero-filled bytes
beyond the requested shrink after a refused call. Later read-only inspection
finds both files at the requested 10 MiB size with correct sampled prefix blocks.
This supports an ambiguous accepted shrink plus stale local EOF hypothesis;
it does not prove the failure-time ordering or full durability. The failures
remain OPEN. Source inspection finds that SETATTR releases its opid slot even
on exhausted ambiguous retries, and that the server performs fallible lane reads
after the truncate effect. A retained-identity resolution fix needs a dedicated
regression; neither a larger deadline nor an old-or-new oracle is accepted.

## Further fixes under qualification

`897a79b64` fixes the clean read-cache hash: 64 files allocated on one shard
previously collided in one two-way bucket for the same chunk index. Mixed inode
hashing retains all 64 distinct payloads in the regression; original code fails
that residency assertion. Cache sizes, checksum validation and deadlines are
unchanged. It built on all eleven hosts but is **not deployed**. Linux read-cache,
client-memory, fence-read and GET-pool regressions pass on its staged source.

The POSIX comparator also expanded a 1 MiB failed comparison into a 7,204,913-byte
message. The runner rewrites its result TSV after every completion, amplifying
shared-filesystem traffic and blocking result recording. Diagnostics now retain
length, SHA-256, bounded prefix and first differing byte for large byte strings.
The exact equality oracle and failed fixture retention are unchanged. The real
finalizer regression checks this bound and both hashes; prior code fails it.
This is a harness pressure repair, not an explanation that dismisses the two
truncate failures. Retained raw TSVs remain unchanged.

## Remaining qualification

Resolve truncate ambiguity and remaining loaded deadlines, then repeat the
original heavy profile and remote pairs without increased budgets. Finish
mounted live suites, single/scaled IOR with fsync and verification, and orderly
cold restart. Preserve every failure and leave the public cluster running.
The next deployed source is `585411524` (read-cache repair plus bounded diagnostic
harness). All seven clients and four daemons stopped cleanly before replacement,
readiness passed, retained stores were not formatted, and all mounts returned.
Nine focused Linux regressions passed against that exact staged source.
The unchanged full qualification is running; the first 160 extension cases pass.
The preceding failed round is sealed in `SHA256SUMS-public-1b-failed`: 339 files,
12,316,193,727 bytes, manifest SHA-256
`e677c4db06576e82115c189c54bbb69478353820beea1aa433eba0522afa4900`.
The two surviving truncate payloads were fully read from fcstor013 and fcstor007:
both 10,485,760 bytes, exact repeated-byte pattern, SHA-256
`aecf3c2ab8aca74852bca07b54136cecb3fdafdc35540068ed952c0b89538e0d`.
That verifies the surviving prefix at readback, not discarded bytes or the
failure-time result. No final qualification PASS is claimed.

A further source change, not part of deployed `585411524`, reuses only the
truncate RPC's own completed group ReadIndexes across its fresh row collects,
compare-and-apply retries and post-commit collect. The handler previously fenced
the inode group three times and the other group twice on a normal two-group
truncate. This reduces repeated quorum round trips without caching authority
between requests or suppressing post-commit errors. The executable handler
regression verifies one fence per group per request, a fresh fence on the next
request, preserved row collects/retry verdicts, read failures, forwarding and
post-commit failure propagation. Original code fails the count assertion;
LOOKUP and transaction read-reuse regressions also pass. Live impact is NOTRUN.
This does not resolve SETATTR's ambiguous identity ownership.

## Second public round and cleanup contention repair

`585411524` completed another serial PASS (220/223, three SKIP), and every one
of seven parallel suites passed (220/223, three SKIP, about 33–35 seconds).
The 28-suite stress round still **FAILS: 5710 PASS, 315 FAIL, 42 SKIP,
177 NOTRUN**. Failure classification: 283 timeout details, one BUSY detail,
31 other details (mostly canceled workers, plus one refused-truncate mismatch).
The latter is fcstor011/stress-1; bounded diagnostics preserved both block hashes
and the first mismatch without copying a MiB into the message. All three remote
pairs remain 67/68, concurrent append failing the original 15-second SSH step.
Extension after stress passes 160/160. No improvement in heavy qualification is
claimed from the clean cache or diagnostic fixes.

All five new profiles completed normally (four daemons and fcstor007 client),
45 seconds of cycles:u at 99 Hz with call graphs, retained under
`profile-public-stress-585411524/`. The client shows 10.10% self cycles in
`opid_take_locked`, chiefly retained lease cleanup. CPU percentages are not
wall-time attribution. Source checks identify two bounded-work defects:

- The shared admission scan traversed 512 slots although next-minus-oldest
  is already capped by the 16-position minimum receipt window. The table now
  has exactly that existing window's capacity. This reduces the scan bound
  without increasing the admitted sequence span or dropping identities.
- The cleanup pass could make up to 4096 attempts on unallocated CLOSE rows
  while another queue owns the receipt window. Identified releases still take
  priority. A refused, still-unallocated release now yields the pass to CREATE
  proof outside its inode stripe, preserving pending state and BUSY. A release
  with an existing identity continues resolving that exact identity.

The admission regression exercises all 16 simultaneous owners, sparse ACKs,
blocked allocation, cancellation and exhaustion; it passes with the smaller
actual table. The old 512-slot header fails the work-bound assertion. Retained
CREATE/lease and 800,000 concurrent append-identity checks pass. Cleanup fairness
and real drain-function tests cover yielding only unallocated contention and
preserving identified work; original code fails the yield assertion. Clean
OPEN/lease retention and reply regressions also pass. Live effects are NOTRUN;
these changes and the truncate fence reduction await their own build/deployment.

## Current deployment and next reservation fix

`6cad51b02` built on all eleven hosts and passed 18 focused Linux regressions
(`unit-6cad51b02/PASS.json`), covering the actual admission table, retained
CREATE/lease, cleanup fairness, append IDs, truncate fences, cache and fixture
retention. All clients and daemons stopped cleanly for its deployment on the
same stores; readiness and all seven mounts passed. Original-profile serial,
parallel, stress and remote-pair qualification is running in
`qualification-6cad51b02/`; initial extension is 160/160 PASS. Existing diagnostic
histograms are enabled: server EFS_WRITE_PROF=1 / EFS_READ_PROF=1 and client
EFS_OPID_STATS=1, recorded in deployment `observability.json`. These add
observability, not alternate admission, durability or timeout behavior.

The previous failed round is sealed in `SHA256SUMS-public-585-failed`: 294 files,
7,853,473,262 bytes, manifest SHA-256
`a1b97f84d734ef4e724e56c5e24eb2a5d85e5d20315db5649f6d1efadeb4a4ed`.
Its fcstor011 refused-truncate fixture again fully matches the surviving 10 MiB
pattern on both owner and peer at later readback. That does not erase the failure.

A further append reply fix is local and **NOT deployed**: after proving an
append reservation, the server performed a separate fallible full getattr even
though client and management consumers use only inode identity and the reserved
start/end. A later metadata read error could therefore hide an already-accepted
range, and every reservation paid extra lane/transaction reads. Replies now use
the proved reservation result directly. Full attributes require GETATTR; no
reservation, receipt, allocation, membership, or wire layout changes. The
executable owner-handler regression injects a failing post-acceptance stat and
checks exact range, repeated result, admission refusal, abandon and stopped
service. Original code fails; append authority, PREPARE batching and atomic
append host regressions pass. Live latency impact remains NOTRUN. This is
separate from the still-open truncate ambiguity and does not waive it.

## Cleanup candidate measured result — qualification remains FAIL

`qualification-6cad51b02/` finishes with serial 220 PASS / three SKIP and each
of seven parallel suites 220 PASS / three SKIP. Heavy stress: **5845 PASS,
293 FAIL, 48 SKIP, 58 NOTRUN**, with 27 complete markers. Most failures remain
original deadlines; no refused-truncate content mismatch was observed in this
round. Three remote pairs remain 67/68, failing concurrent append's 15-second
SSH step. Both extension runs pass 160/160 and post-stress readiness passes.
The missing-size EIO repair and absence of a signature do not close the loaded
latency or ambiguous-SETATTR ownership findings.

The new client's >=1%-self-cycle report no longer lists `opid_take_locked`,
versus 10.10% in the preceding capture. This supports the bounded-scan repair;
CPU proportions alone are not wall-time proof. A live histogram snapshot is
retained with the profile. Since daemon startup through that stress snapshot,
leader commit/apply p99 bucket upper bounds are about 16 ms, while follower
LOOKUP averages about 8.3 ms (leader-local about 0.2 ms). These cumulative,
overlapping phases are not additive or an isolated A/B. Admission reports
hundreds of thousands of refused-window attempts. The oldest callsite
`0x4715f6` resolves against the exact non-PIE client to `efs_client_append_begin`
(`inode_rpc.c:517`): append reservations share namespace/CLOSE's 16-position
window, so waiting appends can delay unrelated metadata. This is verified
head-of-line coupling, not proof of a deadlock or of every timeout's cause.
Receipt-domain and ACK isolation must be audited before changing it; simply
raising the window is unsafe and is not proposed as a fix.

Before replacing this candidate, **54 retained large truncate fixtures were
read completely**, 2,998,927,360 bytes total, zero read errors and zero pattern
mismatches, stable sizes of 10 MiB or 300 MiB (the existing honest-refusal test
also permits an unchanged file). Per-file sizes and SHA-256 are retained under
`large-fixture-readback-6cad/`. This covers those fixtures only. Original failed
files and captures remain untouched. The completed round, deployment and
profiles are sealed as 294 files / 7,903,787,247 bytes in
`SHA256SUMS-public-6cad-failed`, manifest SHA-256
`6ddb4a036f7a876506aa559779917d19f61180d610c88f918100309f1c04069d`.
All listed hashes rechecked successfully.

## Latest candidate and active extended gates

`098333947` built on all eleven hosts and passed 22 focused Linux regressions
(`unit-098333947/PASS.json`). It is now deployed after clean stops of every
client and daemon, without formatting. Readiness and all seven mounts passed.
Correction after reading the final TSV: serial has **220 PASS, one FAIL,
two SKIP**; `mtime_monotonic_many_writes` exceeds its unchanged 15-second
deadline. The earlier serial PASS statement for this build was incorrect.
Each of seven parallel suites passes 220/223 with three SKIP. Extension before and after stress
passes 160/160. The new heavy round still fails; observed completed controller
results show 5551 PASS, 272 FAIL, 49 SKIP, 372 NOTRUN, with 21 complete markers.
In addition to loaded deadlines/canceled workers, fcstor010/stress-1 has BUSY
removing a permission-test subdirectory; fcstor009/stress-0 sees 10 MiB rather
than 300 MiB after a refused truncate. The latter is further evidence of the
unresolved ambiguous-truncate result, not a newly proved payload-corruption
mechanism. Raw evidence stays in `qualification-098333947/`.

`extended-098333947/` waits for that controller to finish, then runs the mounted
live suites and all four IOR cases; these are not yet PASS. The extra peer mount
is `/tmp/efs-mount-qualification-098333947-peer` on fcstor007, created with the
same client script, RDMA mlx5_2 and its own log. Reproduction after readiness:

```sh
cd /tmp/efs
EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2 bash scripts/client.sh \
  172.16.223.57:17432 /tmp/efs-mount-qualification-098333947-peer
bash scripts/evidence/orcd/orcd_test_live_suites.sh OUTPUT/live /tmp/efs-mount \
  /tmp/efs-mount-qualification-098333947-peer
```

The IOR runner is copied to evidence with `-k` added to its existing
`-i 1 -w -r -R -e -g` invocation so **all benchmark payloads remain**, including
failed or uncertain ones. This changes cleanup only, not workload, concurrency,
verification, fsync or deadlines. Run `bash IOR_KEEP_SCRIPT OUTPUT/ior
/tmp/efs-mount`; the copied runner and its SHA-256 are retained. It executes the
same 1-rank easy/hard and 56-rank (eight per each of seven clients) easy/hard
cases, with remote read reordering in wide cases. The controller records every
live harness outcome, including the informational W121 fork case, and never
uses the wrapper's aggregate alone as proof. Afterwards it attempts a clean
stop of only the extra peer mount; the public cluster remains running. No
repaired-build IOR, cold restart or overall qualification PASS is claimed yet.

## Completed extended results — status checked Oct 29

`extended-098333947/` has finished with exit status 0: all **14 mounted live
harnesses** return 0, including .efs commands/options/pages/load, virtual
interfaces, query lifecycle/delta, local two-client POSIX, chmod/truncate,
extension, fsync admission and mtime barrier. The fork visibility diagnostic
also passes its 160 cases; this single run does not close W121 generally.
The loaded query test verifies all 75,731 generated payloads.

All four IOR cases return 0 with fsync and read verification enabled, and the
retained logs contain no data-error/mismatch report (only MPI compression
support warnings). Throughput, MiB/s:

| Case | Ranks | Write | Read |
| --- | ---: | ---: | ---: |
| little-easy, 1 MiB transfers | 1 | 401.76 | 385.21 |
| little-hard, 47,008 B transfers | 1 | 20.17 | 19.81 |
| wide-easy, 1 MiB transfers | 56 | 377.68 | 12,384 |
| wide-hard, 47,008 B transfers | 56 | 51.67 | 330.51 |

These are warm campaign measurements, not a cold-storage throughput claim.
The write path still misses the requested >1 GB/s objective and does not scale
in the wide easy case. Benchmark payloads remain on ORCD. The extra peer mount
stopped cleanly; the public deployment remains running. A fresh readiness check
on all four endpoints passes with stable leaders and an applied root.

**Overall qualification remains FAIL.** Final raw results confirm serial's
one mtime timeout, 272 stress failures / 372 NOTRUN, and all three remote pairs
67/68 (concurrent append hits the original 15-second SSH step limit). Existing
ambiguous truncate and BUSY failures remain open. No cold-restart qualification
has run on the repaired build. Next work remains exact outcome/identity handling
for truncate and append-induced admission blocking, followed by unchanged-budget
stress/remote reruns and cold restart. The successful extended gates do not waive
these failures. Latest source fix is `098333947`; newer branch commits document
evidence only, and nothing has been merged or pushed.
