# BB — client overlays and metadata authority

`agent/devel-bb`, base `74d34d588`; gate NOTRUN. Untouched-product control
`e6856dd08` on xorinox: 8 mounted cases, 5 FAIL, 3 PASS, checked cleanup PASS.
Evidence: `docs/status/bb-overlay-authority-audit-20261029.md`.
ORCD TCP/RDMA NOTRUN. Findings BB1 (attribute overlays), BB2 (cached mutation
shortcuts) and BB3 (cached DAC checks) have candidate fixes; qualification RUNNING.
BB4 additionally captures reuse of a pre-truncate mapping after fresh metadata.
Registered as K59 (attributes/EOF/mapping), K60 (mutation authority), and K61
(permission authority); all require integration and acceptance before alpha.
No merge, push, new product switch, cache protocol or wire/store change.

## Authority contract from the actual paths

The local `metadata.c` export is a bounded staging/index view. The Raft KV
rows applied by `meta_apply.c` supply committed metadata; the inode/lane reads
in `server_raft_host_getattr`, `server_raft_host_lookup` and inode RPC collectors
supply operation-specific authority. A local mutex, row or routing hint supplies
no ReadIndex, namespace receipt, publication proof or permission lease.

**R** below means the successful RPC's authority point, not "latest forever".
Concurrent requests can complete in another order. A previously captured reply
is not a token authorizing a later operation. **L** means only locally accepted
unpublished data, identified by `efs_client_ino_has_unpublished_data` (dirty
inode OR pinned dcache body), never a whole cached inode. **C** is a hint only.

| Field | LOOKUP / strict refresh | Open-handle getattr | REPORT | SETATTR / truncate | Unlink-open / reload |
| --- | --- | --- | --- | --- | --- |
| Size / EOF | R, with L EOF while unpublished | Same field-scoped overlay; clean R even after backdated mtime | Accepted local EOF to owner/lane reductions; tickets/epoch/CAS validate publication | Explicit requested size must reach owner even if C equals it; drain per-inode report first | Held ghost retains L bytes; clean reload adopts R |
| Content epoch / generation | R epoch; never copied back from dirty C | R epoch, not local data overlay | Captured publication/ticket identity and current read views; no cache-issued fence | Server TRUNCATE records history; local dropped mappings invalidate clean cache | OPEN/FileID generation and leases supply lifetime; inode row is not the generation certificate |
| nlink | R; local dual-apply is not a peer link count | R; actual absence gives leased ghost nlink 0 | Does not confer link/unlink authority | Link/unlink commit at owner | Ghost permits existing fd use; never authorizes a new path |
| mode / uid / gid | R, preserved over dirty EOF | R on success; local fallback is informational only | Not exported as a permission update | Owner mutation is never skipped because C matches | Every new DAC/namespace decision requires strict refresh; failed authority is refused |
| mtime | R when clean; L mtime while unpublished | Same; timestamps do not prove clean EOF authority | Accepted data time / explicit pinned utimens rules | Requested times, including backwards values, commit at owner | Fresh clean row can be older in wall time |
| ctime / atime (including nsec) | R | R on success | Atime/time flags use existing publication contract | Explicit requested fields commit at owner | No blanket dirty-row overwrite |
| parent / name | Binding resolved by LOOKUP; RPC row retained | RPC row; fd identity is independent of names | Cannot rename a file | Namespace RPC / transaction commits; cached type cannot veto it | Drop stale cached binding without destroying a possibly held file's bytes |
| Packing / chunk mappings | Separate authoritative GETCHUNKS and fence views, not an inode packing flag | Attribute merge never proves data mapping current | Exact object/ticket and pack ledger rules remain | D25 masks and clean-cache invalidation; no tail rewrite | Dirty-body ownership retained; read mapping identity/revision revalidated by existing data path |

`efs_client_stat_ino` and `stat_local` remain local-view interfaces. They may be
used for staging, counters and already-committed reply fallback; they are not
permission authority. Strict `stat_refresh` propagates every metadata RPC error.
Open-fd `stat_open` still asks the RPC on every call (the old local-first comment
was false). A held ghost may return local attributes on transport failure, but
new OPEN/ACCESS/parent mutation checks must not use that fallback.

FUSE's ordinary LOOKUP validates parent search permissions from a strict row,
then resolves the child name at the owner. New OPEN, ACCESS, CREATE, MKDIR,
UNLINK/RMDIR, SYMLINK, CHOWN, LINK, RENAME, path truncate and the fallocate EOF
check use strict metadata. Readdir already did so and continues to do so.
Root-export DAC bypass remains the existing explicit policy, not a cache grant.
An already-open descriptor retains its POSIX access rights after chmod/chown;
that is different from admitting a new OPEN. Committed CREATE/MKDIR reply
fallback does not turn an unavailable RPC into permission for another mutation.

Cache hydration may fail while a valid RPC row remains returnable. Pulling file
layout is not what authorizes an attribute answer: later reads need their own
mapping/view checks. Local alias/name tables are not server namespace replicas.
AW/AX own their index performance; this audit does not alter those indexes.

Remaining local-row uses in FUSE are intentional hints/informational views:
virtual stat presentation after its separate access check, directory-handle
`.`/`..` presentation, immutable symlink type/length, opendir's type check
(readdir independently refreshes permissions before enumeration), a pre-truncate
size for kernel invalidation, and the reply fallback for a known committed
CREATE/MKDIR. They do not grant a new DAC/namespace mutation. The stale comment
claiming strict refresh maps every failure to NOT_FOUND is corrected too.
Fresh DAC checking and the later namespace RPC remain separate operations;
this audit adds neither an atomic server-side ACL transaction nor hostile-client
authentication to the existing trusted-network model.

## Demonstrated differences and repairs under qualification

* **BB1:** `overlay_local_size` replaced a fresh regular inode with the entire
  dirty cached row. `stat_open` discarded all successful RPC fields except
  nlink/ctime changes, even when clean. The candidate overlays only unpublished
  EOF/mtime; clean RPC EOF wins without comparing wall clocks. Fresh mode,
  ownership, names, epoch and timestamps survive. Existing pending-body EOF
  protection stays in place.
* **BB2:** cached equality skipped CHMOD/CHOWN/TRUNCATE RPCs even when a peer
  had changed the authoritative value. A cached type mismatch also refused
  UNLINK/RMDIR before asking the owner. Remove those shortcuts. Cached file
  bindings retired by RMDIR still retain a ghost instead of discarding held
  bytes. The local staging view cannot decide the operation's outcome.
* **BB3:** FUSE ACCESS and other DAC/parent checks used `stat_ino`'s local-hit
  fast path; an injected stale writable row granted ACCESS despite a fresh
  read-only owner row. Switch those decisions to strict refresh, retaining
  authority errors (including ACCESS's previous false ENOENT mapping).

Devel `126ba81d4` reconciled AY as K57 and AZ as K58; BB uses K59–K61
in both tables, preserving those independent findings.

## Reproduction, environment and current results

xorinox: unprivileged `maik` (uid 1000), x86_64 Linux
`6.12.0-211.67.1.el10_2.x86_64`, system libfuse 3.16.2; private trees under
`~/efs-taskBB-20261029`, raw captures under `~/efs-artifacts/taskBB-20261029`.
The existing `efs` user's `/data1/efs/intel-mnt` was left untouched. Another
agent's sanitizer build finished before this task's host builds/tests began.
No timings are performance claims; no exclusive-host lease is claimed.

Base source `e6856dd08` adds only tests to `74d34d588`; all product inputs are
unchanged. Build: `make -j8 all tests/faults/efsd` with matching EFS_GIT identity.
Native Linux base `--case overlay` and `--case mutations` both fail as expected.
Later component controls additionally exercise cached type refusal and ACCESS
using the original source, not old stores or mixed-build binaries.

Committed [live matrix](../../tests/live/bb_overlay_authority.py), tools
`ba57eb170`, invocation on the base:

```sh
EFS_TRANSPORT=tcp python3 -u ~/efs-taskBB-20261029/tools-ba57eb170/bb_overlay_authority.py \
  --source ~/efs-taskBB-20261029/base-e6856dd08/src \
  --output ~/efs-taskBB-20261029/base-matrix-ba57eb170
```

It starts three private 1 GiB logical members on dynamically held loopback
ports, two FUSE clients, O_DIRECT fragments, normal recovery/packing and the
current commands policy; expiry is not enabled. Files range from empty to
300,000 bytes. RPC-failure checks stop/restart only those owned daemons; clean
shutdown of both clients is checked after all members return. Eight cases:

| Base case | Verdict |
| --- | --- |
| Remote chmod alongside accepted local writes / open-fd stat | FAIL, old mode |
| Remote CHOWN via trusted management while fd has accepted bytes | FAIL, old uid/gid |
| Remote rename/link/unlink, fd lifetime and exact accepted bytes | PASS |
| Same-cached-mode chmod after peer chmod | FAIL, mutation skipped |
| Same-cached-size truncate after peer extension | FAIL, mutation skipped |
| Truncate under a clean open reader, then grow masked tail | FAIL, old fstat EOF |
| Remote directory write permission | PASS |
| RPC failure refuses new OPEN, recovery preserves exact bytes | PASS |

All original base stores/logs/results are retained. Matrix `cleanup_errors=[]`;
both owned mounts and daemons retired. At this baseline checkpoint, candidate
qualification was NOTRUN; subsequent closed results and failures follow below.
Do not infer a completed BB audit or alpha PASS from the candidate source alone.

## First candidate failure (retained, not a passing qualification)

`ff91e2b9d`: native controls and ASan/UBSan/leak PASS; mounted matrix
**7 PASS / 1 FAIL**, clean shutdown verified. The remaining reader test had
correct fstat EOF but `pread` still returned bytes beyond it: stat-open updated
the answer without updating the clean staging row that `client_read_run` uses
for its length bound. FOPEN_DIRECT_IO does not clamp that read to kernel i_size.
The failed fixture/result stays at
`~/efs-taskBB-20261029/qualification-ff91e2b9d/matrix`, logs at
`~/efs-artifacts/taskBB-20261029/qualification-ff91e2b9d.log`. Full gate was
NOTRUN because this demonstrated failure stopped qualification.

The next candidate uses existing adoption/invalidation for a clean open file
before returning fresh fstat data; unpublished bodies still own their local
EOF. A production-block regression now checks the staging EOF and invalidation,
not only fstat output. Each candidate gets unique TMPDIR/GC/full-gate paths;
none reopens or overwrites failed captures. ORCD login's previous master has
expired at that checkpoint; authentication was later restored with the user's
explicit `ssh -A erbmi1@orcd-login.mit.edu` form. No credential was entered by
the agent.

`c0266ad36`: the strengthened shrink test passed its fstat and beyond-EOF
check, then failed the post-regrow zero-tail assertion (7/8 matrix PASS;
cleanup PASS). **The original returned bytes were not captured**, so this is
not evidence of resurrected bytes. The test had not refreshed the held
descriptor's metadata EOF after regrowth, contrary to D25's recorded
OPEN/GETATTR freshness boundary (§3.5 / audit 2 of its design note). The
corrected harness explicitly verifies fresh fstat EOF before requiring a full
zero tail, keeps the exact byte oracle, and records lengths/hashes and raw
mismatches. The original failure is retained unchanged; no original readback
is reconstructed or fabricated. The product is unchanged for this harness
correction. Its failed root is `qualification-c0266ad36/matrix`.

## BB4 — inode invalidation left old map authority reusable

`01c84c49e` (same product as `c0266ad36`, corrected capture harness) again
FAILS 7/8. This time the fresh fstat EOF is 300,000 and the read returns
4,096 nonzero bytes at offset 131,072. Actual SHA256
`63d320b7f817c1523404fbc90872912e4301f30cd7d69aadbcf74a13c5179743`,
expected zero SHA256 `ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7`.
The actual bytes match the exact discarded original synthetic slice, confirmed
by independent regeneration on xorinox (no store was reopened).
Actual/expected files, result and all stores remain under
`qualification-01c84c49e/matrix`. This is now a captured byte mismatch, unlike
the preceding unclassified read.

`invalidate_file_layout` drops decoded-cache entries but never invalidates
`map_fresh`; `pull_file_layout` is intentionally a no-op after D2. A cached
pre-truncate mapping therefore remains eligible within 200 ms even after
metadata exposes the new size. The candidate uses the existing map-open
freshness boundary when adopting an invalidated inode layout. Even a zero-size
truncate fences mappings; a late pull stamped before the boundary cannot
restore freshness. No TTL, clock policy, flag or wire protocol changes. The
production-function fake-clock regression fails without this fence and passes
with it; the mounted exact-byte check is unchanged.

## Closed xorinox gate failure and corrected continuation

Source `4d4f5f7ba`: native positive/negative controls, ASan/UBSan/leak, mounted
matrix **8/8**, GC reclamation, POSIX core **218/218** and peer **72/72** PASS.
Full gate: **TOTAL 553 FAIL 1 NOTRUN 10 STATUS FAIL (995s) OVERBUDGET 2**.
The sole failure is `test_truncate_prefix.py` linking its extracted real
truncate body without an adapter for the new cold-path `stat_refresh` call.
The existing warm-cache assertions remain unchanged; the corrected adapter
asserts if refresh is unexpectedly called. This is a test-adapter defect,
not a passing full gate or an environment waiver. Original log/result remains
at `source-4d4f5f7ba/tmp/fail-229.log` and `qualification-4d4f5f7ba/`.

Ten NOTRUNs: absent external cluster scripts, declaration RDMA path, native
RDMA transport and seven segmented modes (no usable HCA on xorinox). Advisory
OVERBUDGET: geometry grow-b 124.4/120 s and pack duplicate candidate 69.3/55 s;
no budget/deadline was increased. Source `17027103e` changes only that adapter
after `4d4f5f7ba`; fresh `qualification-17027103e` reruns the complete sequential
qualification. No failed export/output is edited or overwritten. The later
comment and documentation correction changes no compiled product behavior.

ORCD authentication restored using the user's explicit login form. Private
node9900 build uses `/tmp/efs-taskBB-orcd-17027103e`, with the existing
`/data1/efs/orcd-mini-mnt` client left untouched. Hardware tests remain gated
on the closed matching-source xorinox zero-failure result. Build is not a
hardware qualification. Committed reproduction driver:
`tests/measure/bb_orcd_qualify.py --phase build|qualify`.

## Cold-cache reply race (K59), retained control and serialized rerun

The cold open-stat path could miss its local row, receive a valid RPC, then
return the local row inserted by another lookup during that RPC. The actual
production wrapper fails the new deterministic hydration schedule against
source `17027103e` (stale mode/uid/gid assertion), even though its warm path
is already repaired. New source `791d57787` returns that successful RPC row
plus only the unpublished EOF/mtime overlay in the cold path as well. Native
regression passes. Raw Linux negative control:
`~/efs-artifacts/taskBB-20261029/cold-race-negative-17027103e.log`; failed
component source/binary copied and independently hash-compared into
`~/efs-taskBB-20261029/cold-race-negative-17027103e`, original retained.

The `17027103e` full gate is allowed to close unchanged. A bounded 3600 s
predecessor wait queues exactly one `791d57787` qualification after its exit
file and closed state exist and no process argument still names the previous
source. Runs do not overlap. Both outputs and the queue's screen transcript
remain under the existing private/raw roots. Hardware source `791d57787` is
built separately on node9900; live checks still require its own closed
xorinox zero-failure proof. No preceding gate can qualify the new source.

`17027103e` closed: **TOTAL 553 FAIL 0 NOTRUN 10 STATUS INCOMPLETE
(932s) OVERBUDGET 1 BUILD 0s**; the sole advisory overrun is K22 leader
admission 109.9/90 s. The cold-cache negative control still fails this source,
so it does not close BB. Its untouched-base truncate-prefix test PASS is
retained in `base-truncate-prefix.log`, confirming the earlier failure was
the adapter added by this branch. Serialized `791d57787` qualification started
only after that predecessor closed and its process guard passed.

## Final-source run stopped on POSIX deadlines; foreign overlap observed

`791d57787`: native controls, targeted ASan/UBSan/leak and the exact mounted
matrix **8/8 PASS**. GC's core POSIX phase **216/218 FAIL**: unchanged 30 s
deadlines expired for `concurrent_appends_two_proc` and
`mtime_monotonic_many_writes`. Peer/full gates were NOTRUN because the
controller stopped. Original state, logs and stores remain at
`qualification-791d57787/` and `gc-791d57787-a/gc-_7fiiul7/`; no error is
waived and no deadline/workload is changed. Own processes/mounts retired.

Read-only all-user inspection found another agent's `efs-k42-gate` running
on xorinox: its screen/test controller and many daemon/mount fixtures overlap
this run. Snapshot `~/efs-artifacts/taskBB-20261029/overlap-791d57787.json`
records PID, UID, command, cwd and boot-derived start times (48 foreign
processes at capture). This demonstrates overlap, **not causation** for the
timeouts. The existing user's intel-mnt client and the K42 cluster were
never stopped or changed.

Next is one explicit guarded comparison after the competing gate ends:
`tests/measure/bb_quiet_qualify.py PRIVATE SOURCE BASE NEW_OUTPUT 791d57787`.
It waits at most four hours for 60 seconds without observed foreign test/build
work, then runs untouched-base GC/core/peer and the unchanged candidate
qualification sequentially with fresh `791d57787-quiet1` fixture paths.
The existing tested Task-I all-user guard samples every 10 s throughout;
overlap or unknown metadata prevents quiet qualification. The single resident
intel-mnt client is identified by PID/start time, UID, endpoint and mount and
its CPU ticks recorded; it is not an arbitrary ignore rule. This is sampled
observation, not an exclusive-host lease. This changed precondition justifies
the planned comparison; the original FAIL stays FAIL. No retry-until-green.
The hardware driver additionally requires the closed quiet comparison receipt
and matching candidate-state hash before admitting this continuation.
