# K38 / K45 / K22 publication repair — October 26, 2026

> Migrated from `agent/task-h-repair` at `cd822e2f7` on Oct 30, 2026, by Task BO. Nothing was shortened: the page already held conclusions, methods, hashes and raw-capture locations only. Scripts and tests it names in backticks exist only on that branch; see [the migration page](task-h-evidence-migration-20261030.md).

**Current reconciliation:** [devel wave 33 and the per-fix inventory](task-h-wave33-reconciliation-20261029.md) supersede this page's branch-head and acceptance recommendation; all evidence below remains historical.

Branch `agent/task-h-repair`, on Task H `bf143e486` (product base `394f2b791`).

**Integration supersedes this source's acceptance:** the branch was
merged with devel `b2d57e421`; [the new integration record](task-h-devel-b2-integration-20261028.md)
tracks reconciliation and fresh gates. The results below remain historical,
not acceptance of the integrated head.

**PRIVATE TARGET QUALIFICATION PASS; FULL SUITES INCOMPLETE.** Final product
source `210a0ad2c` passes K38, K45/D4 and K22 on private nuc TCP and
node9901 RDMA/TCP profiles, including all three four-way loaded nuc rounds,
three forced-RDMA K38 repetitions, RDMA D4, both slow-member durability
variants, exact cold reads and the log-watermark gate. The twelve retained
fixtures produced **44,777 exact cold payload checks, zero errors**. Each
full suite completed **513 entries / 0 FAIL / 14 NOTRUN, INCOMPLETE**.
Native and ASan/UBSan regressions pass on Mac and both Linux hosts. No test
limit was relaxed; all five binaries match after each run, with zero owned
native processes or mounts remaining. Closed evidence and retained original
failures stay on their hosts. No merge, push or whole-release PASS is claimed.

The pending-snapshot mechanism is reproduced in the real Raft core; the old
code fails and the repair passes. The original growth trace fits it, but its
wire ordering was not captured. The supplementary live growth test passed
without exercising an import; it is not a live proof of that interleaving.
Original K33 EIO, fifteen earlier uncertain outcomes, and the older K36
incident whose stores were deleted remain historical limitations, not newly
certified successes. The completed profiles are single-host private fixtures,
not a multi-host/fabric-wide release qualification; node9901 LSan remains
excluded and the fourteen NOTRUN entries on each host remain outstanding.
Original failures and hashes remain unchanged in
[Task H evidence](task-a-finding-b-rerun-20261026.md).

## October 27: remaining D4 failures and current qualification

### October28: shared receipt-window contention and next candidate

#### Pending snapshot import after a delayed log ACK — scoped qualification PASS

Re-examining nuc67's preserved four-member growth failure found that node4
started the group2 snapshot import at index3657; flush/export/diff all
returned0, with162 prepared KV changes in311ms. Its6,465-byte `.part` image
remained, but no snapshot-install record was persisted. All3,692 group2 log
entries arrived while applied membership remained0x7. Numeric analysis is
retained at `/home/maik/efs-artifacts/taskH-repair-20261027/attempt-67/bgeo-import-analysis.json`,
SHA-256 `41dd6394086a7e1b5965f22fae4532bc9e26a2c3a644d2ac6cfc0456a7900a2f`;
the original node4 log remains unchanged, SHA-256
`f21fa1e27c8f51cd9f12a4b4ee1e12ab6bbdcfcdd08a9ed570286307377db19a`.
No snapshot or payload bytes were exported.

The deterministic regression `test_snapshot_busy_survives_delayed_append`
in `tests/test_raft.c` reproduces a concrete mechanism: delay an entry batch,
compact its committed prefix, return BUSY while the follower prepares the
snapshot's final import, then deliver the old entry batch. The successful
AppendEntries reply raises the leader's `next_index` past the snapshot.
Previously that ended snapshot retries, although the follower's KV import
still required the final-chunk callback. The follower can retain all log
bytes yet never finish apply. This is compatible with the original evidence;
that run lacks the frame ordering needed to prove the precise original
interleaving. Its original90s FAIL is retained, never relabelled PASS.

The repair owns an in-flight snapshot separately from ordinary log progress.
AppendEntries ACKs still prove their log prefix, but cannot release that
snapshot. Ordinary heartbeat/retransmit scheduling continues the final chunk
until a completed-image ACK covers the current image. A delayed ACK for an
older image cannot release a newer one. A new leadership term resets local
transfer ownership/offsets. Existing term, log, quorum, persistence and import
validation remain; no store/wire format, product switch, deadline or quota
changes. This is a bounded per-peer state bit, not a new retry loop.

On the Mac, the complete native Raft suite passes with and without
`EFS_TEST_EARLY_READ_ACK=1`; each runs both existing ACK-window modes.
ASan/UBSan repeats pass in both configurations. The same regression linked
with pre-fix `raft.c` fails its three completion/state assertions in both
ACK-window modes (six expected failures). The fixture's import-block release
is restricted to this new asynchronous-import test; other tests retain their
apply controls. Raw local unit output and reproduction command/source hashes
are at `/Users/mike/Documents/Codex/artifacts/taskH-snapshot-progress-20261028/`,
manifest SHA-256 `25cde63f11adbf416b3609520fd2ad4f226d58697cec2645a7bb74c497e6eb1f`.
The manifest names base `75f08b6ca` plus hashes of repair commit
`210a0ad2c`'s two changed source files. Linux native and ASan/UBSan repeats also pass on nuc and
node9901, with and without early read ACKs (both ACK-window modes in each
run); node9901 retains its documented broken-LSan exclusion. Unchanged
live-profile qualification now runs from exact commit `210a0ad2c`.
The archive SHA-256 is
`121275511b97c9ce2e04f9d279c7ee5151bf1dc10b6f72a07a2b4fed56fb8f35`,
verified before extraction on each host. Nuc69 source/evidence are
`/home/maik/efs-taskH-repair-20261027/attempt-69` and
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-69`; private RDMA64
uses `/tmp/efs-taskH-repair-20261026/attempt-64` and
`/data1/efs/evidence/taskH-repair-20261026/attempt-64`. Each evidence folder
has the exact launcher and source manifest. Native/sanitized checks precede
the original standalone K42/K36 tests and the same committed qualifier
profiles. No source overlays, timeout, workload, quota, transport or feature
changes; earlier source-owned processes/mounts were absent before launch.
The successful `ece63c2a6` campaigns below do not qualify this later repair.

Private RDMA64 completed every targeted row and all three retained cold
fixtures (**37,014 exact payload checks, zero errors**). Its full suite
reported **513 entries / 0 FAIL / 14 NOTRUN, INCOMPLETE**, in666s, with
6OVERBUDGET observations. No time/resource limit changed. All five recorded
binaries match after the suite and the focused growth check below; executable
paths from `/proc/*/exe` (including relative launches) and private mountinfo
show zero owned processes/mounts. Its332-file closed manifest SHA-256 is
`0ff6ccc95e59421cc3967b8ae04c8e033862191833ea56e829748e1a3d05e01e`;
qualification result SHA-256 is
`e19def3d7af5d219a65825030a1c44f3aab841c721ac97a4293c44aaa04df8df`.
The native-unit verified manifest on each host is
`f9240ea4b6717a98cd366c555ff17d30ba83af9b73b0b846d5592d5cd6f4e307`:
four native/sanitized checks each end OK with zero assertion, ASan or UBSan
markers. Node9901's LSan exclusion remains unchanged.

The original passing geometry harness removes its own logs/stores, so the
completed full suite cannot supply an independent live snapshot-import trace.
A supplementary **unchanged** `EFS_B_PHASES=grow-a` run on private node9901,
with1GiB stores and the original90s voter-settlement/900s outer runner bounds,
passed in47.2s. An external observer pinned only private fixture log file
descriptors, copied/hash-verified them after all owned native processes and
mounts closed, and retained ten captures plus the driver's existing JSON
artifacts in `attempt-64/focused-growth/`. It did not change test code,
assertions, timing or product behavior. Node4's persisted group2 configuration
completed7/14 to14/0. **No snapshot import occurred in that focused run**;
this is live growth evidence only. The pending-import interleaving is proven
by the deterministic real-core regression, not this live run. Its result
SHA-256 is `94ecfe4727063a5da859f76b53889843f902f1093624564d4ae8bb4086d590a4`,
reference SHA-256
`a74b0a1ba92d7a3d1ce6658147bcdbb19a3288267196feeea643ea6b6a504513`.
Passing fixture stores were removed by the unchanged harness; closed captures
remain on host and no whole-store replay capability is claimed. All original
failed stores remain separately preserved. Nuc69 has passed all three loaded
rounds, nine cold fixtures (**7,763 checks, zero errors**) and the log-watermark
gate; its full suite completed **513 entries / 0 FAIL / 14 NOTRUN,
INCOMPLETE**, runner-reported1,412s (driver1,413s), with13OVERBUDGET
observations. All target rows passed. Its381-file closed manifest SHA-256 is
`73d6953488f414ff013f644a61078133df29d62a5eaa37085901916789bf8961`;
qualification result SHA-256 is
`cd59f01b39134ff893f5902a11cec5145deba6158f8dd6dc421b523ba0f8f6f2`.
All five binaries match, and `/proc/*/exe` plus private mountinfo show zero
owned processes/mounts. These outcomes close the requested private target
qualification on this branch; the platform/tool/transport NOTRUNs, advisory
budget observations and historical evidence limitations remain recorded.
No tests were retried to erase a failure, no deadline or quota was relaxed,
and no previous FAIL or uncertain outcome is relabelled PASS.


#### Combined exact-source qualification: nuc68 / private RDMA63 closed

Both hosts launched commit `ece63c2a6` from the same Git archive, SHA-256
`dc9f92eab1838a23fc8805c563efd3a9eec50148e85535fffc87265628739a25`,
without source overlays. Nuc uses `/home/maik/efs-taskH-repair-20261027/attempt-68`,
with evidence `/home/maik/efs-artifacts/taskH-repair-20261027/attempt-68`.
Node9901 uses `/tmp/efs-taskH-repair-20261026/attempt-63`, with evidence
`/data1/efs/evidence/taskH-repair-20261026/attempt-63`. Each stable evidence
folder records `source-manifest.json` and the exact `run.sh`.

The source combines `bd9540650`, `977d10c7c` and `ece63c2a6`. Each launcher
builds `all tests/faults/efsd`, runs unchanged standalone K42 and retained
K36, then the committed `qualify_publication_repair.py` with `--profile nuc`
or `--profile rdma`. The topology, transport, quotas, concurrency, durations,
cleanup and cold-read acceptance are the same as the reproduction instructions
below. First-fence and fragment diagnostics do not change behavior. Node9901
keeps its documented broken-LSan exclusion; no leak qualification is claimed.
All earlier failures and stores remain on their hosts. Nuc67's original
four-member clone baseline and RDMA62's original K36 cold baseline completed
before launch; neither baseline waives its original live failure. No overall
PASS, completion, merge or push is claimed while these campaigns run.

Private RDMA63 has now closed normally: every targeted row passed, including
both slow-member durability cases, three forced-RDMA K38 repetitions, RDMA
D4 progress/fixed-set, TCP K22 memory pressure and the log-watermark gate.
All three retained fixtures passed **35,178 exact cold payload checks, zero
errors**. The full suite completed **513 entries / 0 FAIL / 14 NOTRUN,
INCOMPLETE**, in684s (5OVERBUDGET observations; no budget or timeout changes).
The existing platform/transport/tool exclusions remain NOTRUN; the scoped
results are not whole-suite or multi-host release qualification.

On-host finalization copied and hash-verified320 closed text artifacts under
`attempt-63/final-closed`; all five recorded binaries still match, and zero
source-owned native processes or mounts remained. The final manifest SHA-256
is `5a094bbde9d1494d8cebe55881ddaa8d3ad257d2f2bdc62a201935c8ca185b02`;
qualification result SHA-256 is
`876db517f98c49415e0ab9f0790b495215e5c44c116672f80d61831093851a93`.
Original failed stores/logs remain separately preserved. The earlier RDMA62
K36 FAIL stays FAIL; its retained mixed-fragment signature has the tested
repair described below. Nuc68 has passed all three four-way loaded rounds,
nine cold fixtures (**7,639 checks, zero errors**) and the log-watermark gate;
its full suite completed513/0 FAIL/14 NOTRUN INCOMPLETE in1,503s
(12OVERBUDGET observations), with no target-row failures. All five binaries
match and zero owned natives/mounts remain. Its370-file closed manifest is
`ed167856b62ffc9ee6c6c28ddb7180fb818498ca0730d5083d322d0c4a8a347c`;
qualification result SHA-256 is
`6104ed08074b4ebdd205bb4fee90e989844703d6de676b3e4a2548594c6e4f3b`.
Both runs stayed at the original limits. The original growth failure did not
recur; the further deterministic pending-import repair above still needs live
qualification.

#### K36 packed-read EIO: retained evidence, cause and tested repair

The private branch also takes devel's diagnostic-only `44cc4e3e2` as
`977d10c7c`: the first Raft storage fence logs group/source location,
term/commit/applied/snapshot coordinates. No protocol, persistence rule,
acceptance predicate or test limit changes. It makes a recurrence of the
unexplained growth failure diagnosable; it is not claimed as that bug's fix.
No broad devel merge or uncommitted work from other sessions is included.

Private RDMA attempt-62 (`f533e6054`) completed its targets and25,958 exact
cold payload checks, then full **513/1 FAIL/14 NOTRUN,694s,5 OVERBUDGET**.
K36 failed at its step8 read after the compaction crash. This time the
harness retained the stores and exact successful-close oracle. Failure log
SHA256 `72d3e65420336162d8485413871de8bfc4e8c1fed4ee16f0ee930fb9c84db887`;
oracle16 files/11,955,445 bytes, SHA256
`1cd2772b731e479400f639ae43bd9bf580c321c444e6856ab8d115ecdfdc2b19`.

The failed packed read obtained **node1 NET, node2 OK, node3 NOT_FOUND**:
one verified fragment, with a returned holder still skipped by the client's
down-mark. The segment reader returns its final fragment's NOT_FOUND;
the old client re-probed down holders only for final NET. It therefore
returned EIO before retrying the recovered holder. `mnt-b2.log` SHA256
`9ccffac3f4161376daaca446a7b598217accd375b3c1527500a4cea272d84a43`.

Preserved617 files/30,952,705 bytes before any restart; on-host manifest
`attempt-62/k36-before-cold/manifest.json` SHA256
`7cfd761767997d9627295eae05bd8a3de2f20e897247a578f4131abcfa2189b7`.
`scripts/k36_verify_retained.py` cold-restarts allthree original stores with
original binaries, faults scrubbed, fresh reader and every recorded payload:
**16/16 exact, zero read/mismatch/cleanup errors**, readiness0.821s. Result
SHA256 `1c8cbc82c2245375a12a18c9c60e610c194ce34f8039bc3e02c99373749bd318`.
Original live gate stays FAIL; this proves retained payloads, not that the
failed live read succeeded. It does not retroactively explain the older
K36 failure whose stores were deleted.

The repair allows one retry for final NET or NOT_FOUND when an earlier
fragment attempt failed NET and its recorded holder is now reachable.
It also handles the async health probe clearing that down-mark before the
recovery check. The retry uses the same reference, fragment identities,
checksums, record digest and caller deadline. No new placement, ignored
error, fabricated fragment, timeout increase or repeated retry loop.
No returned holder means the short read still fails. Corruption/stale
reference/deadline refusals stay refusals.

The production reader/framing regression now proves both retained signature
and async-clear race; unavailable holders and genuinely insufficient copies
still fail, and an expired caller makes no fetch/probe. Native GET integrity,
packed reader and peer ordering PASS; ASan/UBSan GET integrity PASS. The old
reader compiles and fails the new runtime case. An intermediate missing-only
fix compiles and fails the async-race case. Three unchanged K36 private live
runs and one unchanged growth-to-five run PASS on the final candidate.
These are focused loopback gates, not a new physical power-loss claim.

Node9901 evidence:
`/data1/efs/evidence/taskH-repair-20261026/packed-reprobe-race-unit/`;
source archive SHA256
`a087552bf1c5e11526c27c1c72ce7562c20ca37198c89102383fb38ebee1bf53`
(bd9540650 plus the read repair, GET test and retained-verifier helpers);
verified-result SHA256
`62bf5c16d5a33b3fd1d36eeb0cabe3f1c3c7ba490a1df5359682d60d3b9cb758`.
`run.sh` records native/sanitized build commands and unchanged live tests.
The GET harness now accepts the standard sanitizer flags/wrapper; this
instruments real packed/framed reads and linked library code. LSan remains
NOTRUN due to node9901 ptrace restrictions. Full exact-commit qualification
of the combined fixes is next; no overall PASS, merge or push.

#### f533 superseded campaigns closed; growth failure remains OPEN

Nuc67 passed allthree four-way loaded rounds and **8,041/8,041** exact
cold payload reads. Its completed child suite reports513/1 FAIL/14 NOTRUN,
1517s,17 OVERBUDGET. It failed
one: `EFS_B_PHASES=grow-a tests/test_b_geometry.py`, with node4 still
reporting group2 voters7 while nodes2/3 reported14 after90s. Node1 was
properly retired. This is a real recorded failure, not a NOTRUN.
Failure-log SHA256
`15b3df3f94118c532a8ff35b01ba5ce6eeddb6c93e114efa31b36ec0cbd6029d`.
The controller was frozen after the corrected receipt build was ready;
its original stage was FULL-GATE. Its already-running suite finished rc1
normally. Only the frozen orchestration process was then killed, after
all owned test children/native processes/mounts exited. Controller137 is
**CANCELLED_SUPERSEDED**; the observed completed child is **full FAIL**.
No controller `result.json` is fabricated and no failing result is discarded.

The growth fixture's original stores remain unchanged. The same-build clone
verifier now accepts the supported4-member topology (existing safety checks,
full inventory hashes and exact expected payload retained). Allfour clones
cold-started and settled; the one B2 file left after the successful initial
driver cleaned its own files matches exactly, no unexpected/missing files,
clean stops. Result SHA256
`53714bba8cacd397ba608e20971ccd638bba10d79ad043c15420bbf7c4eede2e`;
original store inventory SHA256
`f52ce2c930b8f77010a944b4897cbbb95372ad0f6de7bbc361d4462e238cd8af`.
This proves retained data and cold recovery; it does not waive or explain
the original90s live voter-settlement failure. Its cause stays OPEN and
requires the unchanged gate on the combined repaired build.

Both f533 campaigns are sealed, allfive binaries match, no owned native
process/mount remains. Nuc67 final380-file manifest SHA256
`7af5fbd00267b236eb4fdc274263946296671ab58d8d2d4226af9a2149813587`;
node9901 attempt-62 final322-file manifest SHA256
`47b09cc0ddb5738a7e932a8a9191d329171fe19bd8a29422bc705023a70330b0`.
All failed stores, clone baselines and raw logs stay on their respective
hosts. The original fourteen NOTRUN exclusions are still exclusions.

#### Receipt proof must follow KV scan release

Self-review of `f533e6054` found its coordinator callback runs inside a KV
prefix scan. Production LSM holds its reader gate across that callback;
ReadIndex may wait for apply, which needs the same gate released. The
memory-KV tests alone did not expose this ordering defect. The f533 live
campaigns remain immutable and are retained as that candidate's evidence;
they cannot qualify the corrected build.

The correction copies one bounded exact pending record during the scan,
validates the whole existing bounded prefix, releases the scan, then asks
for fresh coordinator COMMIT authority. Duplicate exact pending records
remain BUSY, as do covering ACKs and all uncertain decisions. No network
wait or host-lock acquisition occurs in this receipt scan callback. The
result remains a read-only staged verdict with the same child-row checks.

A real-KV scan wrapper asserts that the coordinator callback executes only
after scan return. Linking the new test against the prior library compiles
and fails that runtime assertion; the corrected library passes. Native
full build plus four C/five Python regression gates and ASan/UBSan-built
real-KV transaction/meta-apply tests all PASS. Original old-host replay
negative control also still fails at its new assertion. LSan remains
NOTRUN under the known node9901 ptrace restriction, not a passing leak gate.

Evidence: node9901
`/data1/efs/evidence/taskH-repair-20261026/receipt-read-gate-unit/`;
source archive SHA256
`b79979356d88d3e6b8292e8286a243f8e7f4524d834cac9b2d966571e75a8eda`
(2429b79c2 plus src/meta/txn.c and tests/test_txn.c), verified-result
SHA256 `fbc68ddc02eb651cd6aa002d6d246c763cc00d86f9b8258d1711c48388654017`.
The native commands and instrumentation match the focused gates below;
`old-scan-control` links current test_txn.c against the retained prior
libefs.a to prove the new assertion detects the error. No backwards
compatibility requirement is introduced. Corrected live qualification is
still owed; budgets and acceptance criteria stay unchanged.

#### f533 qualification running

Fresh nuc attempt-67 and private node9901 attempt-62 run exact commit
`f533e6054`, archive SHA256
`1066943fba9a8726b7f72575845a58047e70cb501e040d85b26c77793e29bde6`.
Commands and environments are retained in each artifact root's `run.sh`
and `source-manifest.json`: unchanged standalone K42, then the committed
qualifier with `--profile nuc` or `--profile rdma`. No overlays or relaxed
limits. Nuc66 stores and original-build verification completed first.
Additional closed-text copy nuc66 `closed-before-receipt-replay/index.json`
SHA256 `a484967fc45b22d8dcee4bbaa9bc03aed731530f76c80a1121e759df168c952e`.
Node9901 attempt-61 closed-text copy index SHA256
`6db3ea614b01574d0477da0b60bdebc2218a261f270ee81ac4cffb24726ad255`.
Those f533 campaigns subsequently closed as documented above; focused regression PASS alone does not close the corrected live gate.

#### Pending committed receipts: candidate repair and focused gates

Nuc66's retained history contains the8 affected cross-group MKDIR
transactions: their staged receipt reductions and COMMIT/RESOLVE/RETIRE
commands are present on allthree nodes. Retained commands alone are not
commit proofs. Separately, daemon recovery reports COMMIT (resolved) at
ages18.0–49.1s; it recovered already committed transactions rather than
turning these failures into aborted work. The pending-receipt probe refused
client retries until materialization caught up, which can exceed the
unchanged30s MKDIR budget. No claim is made that every original flush or
operation satisfied all durability requirements within its deadline.

The new read-only `efs_txn_opid_replay` shares the existing strict bounded
pending-record parser. Only an exact sequence's staged verdict, plus fresh
coordinator COMMIT authority through `host_txn_read_coord`, may answer a
retry before physical receipt materialization. Its rc/ino/extra are returned
unchanged. Unknown/undecided/ABORT, missing or reclaimed decisions, covering
ACKs, malformed records, EXCL window ownership, scan overflow and authority
errors still refuse. No receipt is written, ACK advanced, intent removed,
new transaction authorized, retry budget extended or command format changed.
The replayed inode still passes the existing fresh child-group read and
transaction-aware row visibility checks. Background RESOLVE remains required.

Focused **PASS**: native `make -j8 all tests/faults/efsd tests/test_txn
 tests/test_meta_apply tests/test_namespace_session tests/test_session`;
allfour C binaries,5 Python namespace/cohort/publication regressions, and
ASan/UBSan-built real-KV transaction/meta-apply binaries. Real-KV tests prove
the exact committed result is available before RESOLVE, that reading leaves
the receipt unmaterialized and the pending apply predicate BUSY, and that
the ordinary later RESOLVE produces the same result. The old host helper
compiles but aborts at the new committed-pending-receipt runtime assertion.
No product live qualification is inferred from those tests.

Node9901 focused evidence:
`/data1/efs/evidence/taskH-repair-20261026/receipt-read-unit/`;
archive SHA256
`54c58a23355b071a98d57911049b93824a072ce24c0438dc3b0c7ef71d81f87a`
(HEAD f5523ec9c plus the named receipt repair files/tests), verified-result
SHA256 `579abb80557c2821b97a482f869c830462201bc414752744784f80e2206601c7`.
Sanitized builds use `EXTRA_DEFS='-fsanitize=address,undefined -fno-omit-frame-pointer'`
and matching linker flags. Node9901 LSan remains NOTRUN due to ptrace;
`ASAN_OPTIONS=detect_leaks=0` does not change live workload limits.
Fixed-build loaded qualification is next, without changing limits.

Before any new live run, original dbf binaries cold-verified all9 nuc66
fixtures:7,897 exact payload reads, zero errors. Eight fixture verifiers
PASS. The failed K22 discovery verifier stays INCOMPLETE, but its discovered
668-file contiguous writer set exactly matches the stable, error-free
writer acknowledgment counts `[159,176,158,175]`; its original gate stays
FAIL. This supplementary prefix check is not a full-cluster namespace or
metadata-load verification. On-host pre-cold preservation includes20,254
files/1,082,557,469 bytes, manifest SHA256
`a7f3408db682459eecff85d3e966aeedd93c10190b95e3a43d44cb2c7e2f1828`.
`attempt-66/verify-retained.py`, per-fixture results and
`retained-cold-summary.json` record exact commands and scope; stores remain.

#### dbf qualification: nuc66 K22 FAIL; private RDMA61 closed scoped PASS

Nuc66 passes standalone K42 and all three K38/D4 progress/D4 fixed-set
loaded rounds. K22 rounds1–2 pass; round3 **FAILS**, so cold/full automatic
stages are NOTRUN. One of15 metadata producer invocations exits1:
8 MKDIRs fail BUSY after30.002s;1000 dependent CREATEs fail INVAL. No
writer application errors: stable acknowledged/attempted counts
`[159,176,158,175]` (668). That producer's revoke completes rc0 in3724ms,
so this is not the K42 cleanup diagnostic failure.
Server MKDIR errors include4 at transaction commit stage10 and323 at
receipt probe stage13. This suggests pending transaction receipts prolong
retries, but does not yet prove the causal mechanism or committed outcomes.
No deadline or error is waived; investigation remains OPEN.
Failure log `attempt-66/qualification/test_k22_leader_admission-3.log`
SHA256 `1f0409882dd49a09a3d091a542a9e41f13ead35613e66c3f30e3e04b6a540d3a`;
failed producer log SHA256
`accbc2aaccc20226f7b98ab9a9b8935587b20b2e10e868ff777a3ec564d11a05`.
The stopped failed fixture is
`attempt-66/fixtures/loaded-3/test_k22_leader_admission/efs-k22-admit-djci06ku/admit34`.
All nine fixture stores are being copied/hash-checked on nuc before original
build cold verification (`attempt-66/verify-retained.py` and its closed
summary); original workload FAIL stays FAIL regardless of payload readback.

Private node9901 attempt-61 completes standalone K42, forced-RDMA targets,
TCP K22 pressure and **38,664/38,664 exact cold payloads**, zero errors.
Full **513/0 FAIL/14 NOTRUN,660s,4 OVERBUDGET,BUILD0s,INCOMPLETE**.
Missing cluster scripts, Valgrind, forced-TCP recipe RDMA exclusions,
fault-build follower-bound and nested-runner exclusions remain NOTRUN.
Allfive binaries match initial hashes; no owned native process/mount remains.
The315-file closed manifest SHA256 is
`86ccecf3ee4fc5dae5d88dd50d58dc4fe324c98ea2537d787f5b1bc1fd5fe049`;
qualification result SHA256
`f5b6e7a25b0a2977d00a8eb8980faae2f2c7905e59482a0506760ddee8b44b42`.
All stores and original failed evidence stay on host; no overall PASS,
merge or push is claimed.

#### Cleanup deadline repair committed; qualification restarted

`dbf87d011` is the current candidate. Exact git archive SHA256
`8b329ef584bbffc97d2db5c5f4924cea3a7ad15c3fe0429d32649b53cb43c184`.
Nuc attempt-66 and private node9901 attempt-61 use fresh source/fixtures,
first running unchanged `test_k42_peer_stop_open.py` alone, then
`scripts/qualify_publication_repair.py --profile nuc|rdma --root RUN/fixtures
--output ART/qualification`. Their `run.sh` and `source-manifest.json` record
all commands/build identity. Original limits, workloads, enabled features,
transport and quotas are unchanged; `EFS_READ_FRAG_PROF=1` is diagnostic only.
Both standalone K42 runs PASS at the unchanged3s/20s limits. The original
node61 launcher passed an invalid `node9901` profile; argparse exited2
before any qualification fixtures were created. Its invocation and hashes
are retained in `controller-invocation-correction.json`; the corrected
`qualification-run.sh` selects `rdma`, with a separate controller log/exit.
This launcher error is not a product test failure or a discarded run.
The qualifier retains original stores and exact cold verifiers. No other
cluster or main node9901 store is touched. Those campaigns have now closed as described above; no full PASS.

Closed prior text is additionally copied and hash-verified on host:
nuc65 `closed-before-cleanup-deadline/index.json` SHA256
`9d8449c2b6b206a8241a8884515d1e3e9e7e31d1c3f0cab570971570f9f0962f`.
Node9901 attempt-60 closed-text index SHA256
`9d837d2642db64825a2d1c478c4dc377bab4b942a48b30ca64c8bf91f586a07b`.
Original K42 failed stores and all prior failed stores remain retained.

#### Current6c nuc full FAIL: K42 cleanup deadline diagnostics

Nuc65 full suite closed **513/1 FAIL/14 NOTRUN,1509s,12 OVERBUDGET,
BUILD0s**. The sole failure is `test_k42_peer_stop_open.py:128`: after
certified foreign-OPEN waits, the last receive exhausted the unchanged20s
cleanup budget and returned NET without the prior certified reason. The
client exited1 and did not claim clean unmount. Short-hold cleanup passed.
This is a failed qualification, not payload corruption or a passing full run.
Original full failure log SHA256
`a152cf72193093e0358260c79c23d8e0091a19e72cda9155a24b3aad72bed414`.
The final sealed manifest is nuc `attempt-65/final-closed/manifest.json`,
SHA256 `577ca1f22e7438a353c1a829009db3d2dbf9bc924441ef3059cfbcbe6836156a`.
Allfive binaries match and no owned mount/process remained after closure.

Before any repaired-build live tests, preserved all96 failed K42 fixture
files (11,363,002 bytes) on nuc under `attempt-65/k42-before-cold/`;
manifest SHA256
`780664700d0d67d9db0ac75b03aae585e1beca0b9df3c2dd6a193729932d964c`.
`scripts/k42_verify_retained.py` uses original6c binaries, the retained
private ports/stores/quota and scrubbed fault environment. It cold-restarted
allthree daemons and read both exact three-line payloads, completed the
previous peer's operator fence, then read both again: **2 unique files,
4/4 checks, zero errors**, clean client/daemon stops, readiness0.816s.
Result at nuc `attempt-65/k42-original-cold/result.json`. Original FAIL
remains FAIL; this establishes retained payloads and resumability only.

Candidate cleanup repair preserves the last certified wait as a separate
diagnostic when the invocation's deadline expires; its actual final RPC
reply stays NET and cannot certify retry. No ambiguous resend, success
conversion or budget extension. Parallel stages retain the first failed
reply rather than allowing later successful workers to overwrite it.
Mac and native focused cleanup, FINISH, coordinator, redirect and parallel
regressions PASS; native `make -j8 all tests/faults/efsd` PASS. Actual
extracted cleanup/server-route and actual coordinator harnesses PASS under
ASan/UBSan. The prior loop compiles but fails at the new late-transport
runtime assertion, as expected; this is a regression negative control,
not a compatibility requirement. LSan on node9901 failed with its known
ptrace restriction; that attempt is retained and is not a leak-check PASS.
ASan/UBSan ran with `ASAN_OPTIONS=detect_leaks=0`; no live limits changed.

Native evidence: node9901
`/data1/efs/evidence/taskH-repair-20261026/cleanup-deadline-unit-v2/`;
source archive SHA256
`113cb12b62eb7425b7bded6de03046940c1c71b35800bd93a2a44e5fafec0a89`
(HEAD c0eb2058e plus the named cleanup repair files and retained verifier),
verified-result SHA256
`7f56a844efa047704f2c88f9780671c05f1ff29658313abc2e542ce0036dd7a4`.
Run each `tests/test_session_{cleanup_cli,fence_finish,finish_retry,revoke_parallel}.py`
and `tests/test_revoke_redirect.py`; for cleanup instrumentation set
`CC='cc -fsanitize=address,undefined -fno-omit-frame-pointer'`; for the
coordinator set `EFS_TEST_SANITIZE=1`. The new regression explicitly checks
NET before deadline, no previous certified wait, intervening progress,
IO/PROTO errors, no ambiguous retry and first-failure reply retention.

Original cold verification command, from nuc attempt-65/src:
`python3 scripts/k42_verify_retained.py --fixture tmp/case-446/efs-k42-kzxmnhnl/c
--owned-root /home/maik/efs-taskH-repair-20261027/attempt-65
--binaries /home/maik/efs-artifacts/taskH-repair-20261027/attempt-65/qualification/binaries.sha256
--output /home/maik/efs-artifacts/taskH-repair-20261027/attempt-65/k42-original-cold`
(join wrapped lines). Requires the preserved original three stores and
binaries and free private endpoints; output must be new. No formatting or
deleting stores. Fixed-build qualification rerun is next; no full PASS yet.

#### Current6c node9901 qualification closed

Private node9901 attempt-60 (`6c1f9dd1e`, product84d) completed all
required forced-RDMA targets on mlx5_0, TCP memory/log-pressure gates and
**36,855/36,855** exact cold payload reads across three fixtures, zero
errors. Full **513/0 FAIL/14 NOTRUN,652s,5 OVERBUDGET,BUILD0s,
INCOMPLETE**. The full recipe forces TCP; its RDMA-specific exclusions are
separate from the explicitly forced-RDMA target runs. Missing devops scripts,
Valgrind and the existing fault-build/nested-runner exclusions remain NOTRUN.
No skipped gate is relabelled PASS. This is private colocated hardware,
not a multi-host fabric or physical power-loss qualification.

Allfive product binaries match their initial hashes; no owned native
process or mount remains. All stores stay on host. The309-file closed-text
manifest at node9901 attempt-60 `final-closed/manifest.json` has SHA256
`7df7311cceebbfd743ea70149a230f1f8c67a2d581cf147e2500791c4d08391e`;
qualification result SHA256
`41bb2d6ae1835bbf67d75e0b33d40c88460f2d2041b1603f8a50d9a4dfd090ec`.
Nuc65 subsequently closed full FAIL at K42; see the current finding above.

#### Current6c nuc targeted/cold PASS; full subsequently FAIL

Nuc attempt-65 completed all three **four-way concurrent** loaded rounds
with the original limits: K38, progress D4, fixed-set D4 and K22 all PASS.
K38 verifies14,400 distinct47,008-byte records on both clients and after
client remount. K22 acknowledged663,766 and651 writes (2,080 total), with
actual KV-pressure refusal/handoff/drain and clean stops in each round.
Memtable values at refusal:3,170,767;3,165,062;3,167,906 bytes against the
same4,194,304-byte budget. All metadata load workers passed.

The six D4 runs report zero operation errors, no skipped last-write payloads
and no foreground operations past10s. Progress write+fsync1MiB maxima:
3.104/2.658/3.605s; fixed-set maxima8.179/6.111/4.599s. Drain times:
progress6.0/6.3/6.9s, fixed-set26.7/24.5/32.5s. This is the existing short
fixed-set qualification, not a new long-duration RSS-plateau claim.

All nine original-binary daemon cold-restart/fresh-reader verifiers now
PASS **8,394/8,394 exact payloads, zero errors**. These are2,080 K22 and
6,314 D4 payloads, distinct from the K38 client-remount record checks.
Current evidence is nuc `attempt-65/qualification/`: each closed target
log/exit, retained oracle and cold result remains on host. The full suite
subsequently failed K42. Node9901 attempt-60 has closed as documented above.
Historical failures and15 earlier uncertain outcomes remain unchanged.

#### Prior allocator build closed; latest private RDMA run started

Node9901 attempt-59 (`a10f96604`) completed targeted PASS,25,543 exact
cold payloads, full **513/0 FAIL/14 NOTRUN,753s,5 OVERBUDGET,BUILD0s,
INCOMPLETE**. No owned process/mount remained and all five binaries matched.
`final-closed/manifest.json` retains309 closed text artifacts, SHA256
`f51b7704009e563ec022ae28772c1a4e9fade916f143ae53a78833a424ea45e9`;
qualification result SHA256
`df1768ee0b3c14576c3096214f536c35b110f97f12e1ba0d4e667cfcd3f3faac`.
The waiting launcher completed normally and started exact6c attempt-60
only after this sealing step. Nuc65 first four-way loaded round PASS (K38, both D4 tests and K22;
663 acknowledged K22 writes, actual handoff/drain). Round2 is running.
No late receipt PARTIAL was observed in round1; its deterministic
regression/sanitizer proof is separate from this clean live outcome.
Both current qualifications remain pending. Earlier15 uncertain D4 outcomes and all
historical failures retain their original verdicts.

#### Preflight wiring correction — `6c1f9dd1e`

Nuc attempt-64 stopped before live tests: `test_k38_report_bound` still
searched for a direct call to `stale_repull_replay` after that call moved
behind the shared helper. Original failure/log/result remain retained;
the fixture root was empty, so there were no new payloads to verify.
`6c1f9dd1e` updates that source-wiring check to follow the shared helper
and verify that it calls the same repull function. The executed two-loss
bound, error recording and all negative cases are unchanged. Native and
ASan/UBSan/leak-enabled bound tests PASS; product code is byte-identical
to84d. This is a harness correction, not a passed live gate.

Exact6c archive SHA256
`41c3913eef0329c4cd92bc7eee96ccd0fc2c768735c1a3f660dfea259f9f048f`
is now running nuc attempt-65. The waiting84d node9901 next-run launcher
was superseded before it started, with `queue-superseded.json` retained;
the active attempt-59 full suite was not stopped. The new6c node9901
attempt-60 launcher waits for59 to close and seals its evidence first.
All live limits/workloads remain unchanged; both outcomes pending.

#### Late PARTIAL must replay the sent snapshot — `84d3c8f3a`

Nuc attempt-63 (`a10f96604`) first loaded round: K38, progress D4 and K22
PASS, including all metadata producers, actual KV-pressure admission at
3,166,256/4,194,304 bytes, handoff, drain and641 acknowledged payloads.
Fixed-set D4 **FAIL**: one overwrite close/flush EIO and subsequent open
BUSY. Failure log SHA256
`fe410cf51165cf8315c2532ce52ebd90f78b8953420910a48386a696c58c4a7c`;
D4 result SHA256
`393da83f9b1f1c8d2e11a7d5542424e3d76f7ef0114875374d9c4b4d026e7ea4`.
The first K22 pass supports the allocator repair but does not replace its
remaining two loaded rounds or establish overall qualification.

Failed fixed-set client evidence identifies inode165387, two report records:
transport NET, then a same-identity receipt query returns definite PARTIAL/
STALE in1ms; a new report retry returns BUSY, total8,002ms. Server recorded
one earlier successful report, then a partial report with one apply-time
CAS loser (no per-record stuck diagnostic). This is **not** proof that the
failed overwrite committed in full. Readback never upgrades its EIO result.
The remaining cause of the original transport failure is not established.

Before more runtime work,6,292 store files/345,992,330 bytes were hash-copied
on nuc under attempt-63 `failed-round-stores-before-cold/`; manifest SHA256
`0bbb7fbba61885e4baf85a1bb51b4e47a925f5ba0aaf38bd276d16e33ae4019d`.
Original a10 binaries cold-read641 K22 and1,076 progress payloads exact
(PASS). Fixed-set cold-read1,038 paths with no read/mismatch/cleanup errors,
but15 uncertain records have hashes only:1,023 known versions checked exact,
15 outcomes remain unresolved, verdict INCOMPLETE. All original failures,
stores, oracle files and byte hashes remain on nuc; no off-host payload copy.
`failed-round-cold-summary.json` SHA256
`0853259d1e2faa87b302025b1fc59c39875be3cbf9ea11de1557a15a02b47e53`.
Use the existing retained K22/D4 verifiers with the original oracle,
`qualification/binaries.sha256`, owned fixture root and a fresh output path.

Code review found an independently testable gap: a late receipt PARTIAL sets
`resolved_stale` and frees the original sent records/sequence snapshot,
then re-enters ordinary report construction without having performed the
ordinary STALE path's authoritative repull/replay. New live cache rows are
not a substitute for those sent identities. `84d3c8f3a` shares that existing
replay helper and calls it on the original snapshot before release. Only a
definite STALE receipt uses it; unknown remains a failure. It runs within
the already established absolute completion deadline, refuses an expired
deadline, propagates replay errors while retaining ownership, and only then
allows a new report to prove publication. No deadline/retry count is renewed,
no error ignored and no receipt is treated as a data durability proof.
Whether this resolves the observed loaded EIO is still a live gate.

`tests/test_report_resolved_retry.py` now executes the actual inner receipt
block and shared replay helper, in addition to the outer completion wrapper.
It checks exact sent generation/sequence arguments before release, replay
failure, unknown outcome, expired/inherited deadlines and fresh publication
before success. Unchanged a10 source compiles and fails the added assertion
at runtime (SIGABRT); this is not a compile-failure negative control.
Native nuc full client build plus seven focused report/pressure/stale-span/
PUT budget/EOF tests PASS. ASan/UBSan/leak-enabled **extracted production
block/helper and completion-wrapper harnesses** PASS; this does not claim a
fully instrumented client. Nuc attempt-63 `late-replay-unit/result.json`
SHA256 `5e59915953f498370f5cbb2d33cef12c490af459a9d1ec8df87368ae6eb47c3c`.

Exact archive SHA256
`0ac7eaf5cac5c3433ebfaa3008eaa8a68d4147ababeefc1758abd87566ac49dd`:
nuc attempt-64 runs the original committed qualification profile on fresh
fixtures; private node9901 attempt-60 is queued only after attempt-59 full
suite closes and its text evidence is hash-sealed. The earlier run is not
interrupted. Each uses source84d and normal read-fragment diagnostics,
existing quotas, unchanged timeouts/limits, all three loaded or RDMA rounds,
cold verification and full suite. Nuc63 closed75 text files separately at
`closed-before-late-replay/index.json`, SHA256
`af3f7edf2fd313ff10a5cd4c9f609360718a74ed78c4af4c10b6326d7092dd0a`.
No existing/main cluster deployment changed. Outcomes pending.


#### Apply-time allocator reservation — `a10f96604`, live qualification pending

Trace-only nuc reproduction on the unchanged0b binaries completed:
K38/progress PASS, K22 FAIL (metadata worker3 round2, one MKDIR timed out),
fixed-set D4 FAIL (one foreground write+fsync1MiB took11.37s against10s).
Logging overhead is a confounder, not permission to erase either failure.
Results at attempt-62 `diagnostic-loaded/result.json`, SHA256
`fefcafa44ef80c5adf1775e5f176d414f21c81430030f3064721f9e6e05133f0`.
Before reading those stores again, all three fixtures were hash-copied on
nuc:7,389 files/389,140,732 bytes, manifest SHA256
`375257b9954468712fe052791bb135f4cf5828fbb9585918ba30308a7261a7d0`.
Original-binary cold reads matched635 K22 files,896 progress files and
1,037 fixed-set files, zero read/mismatch/cleanup errors. K22 discovery and
fixed-set's uncertain-write oracle remain INCOMPLETE, not PASS; original
failures stand. `diagnostic-cold-summary.json` SHA256
`ca618f270cb577beecc455a0ef461f9041cb3d425355ff59e55d29649df5d404`.

`scripts/inspect_allocator_refusals.py --fixture <retained-admit34>
--output <new-on-host-json>` reproduces the structured allocator analysis
from node1's diagnostic log and CRC-checked retained Raft frames, without
emitting filenames or payload contents. It found15 logged allocator
refusals on shard1688, including14 STALE comparisons. This is real progress
by competing allocations, not a fixed stale snapshot: expected watermarks
advance from5,784 to226,968 while each MKDIR PREPARE loses again before
apply. Root log SHA256
`b2b8752119e2216084c80e68e245fd7a005e07413b91968e2f684e8493957c84`;
Raft log SHA256
`5f21bb3216fcbfeff3466f862f57148578a31563fbaefdb52051a6bd37082ed2`.
The analysis selects transactions with logged apply refusals; retained
entries alone are not claimed as commit proof.

Repair: a new current PREPARE kind12 (`EFS_TXN_ALLOC_INODE`, allocator key,
empty payload) selects the free inode at serialized apply and installs the
ordinary EXCL allocator intent. Dependent dentry/inode rows are built only
after its exact verdict and owned intent value are observed. Another owner
stays BUSY; retries of the same pending reservation return that reservation;
COMMIT advances once and ABORT/DROP leaves the watermark unchanged. No
proposer-side watermark CAS is needed for cross-group MKDIR. Early failures
retain cleanup ownership; once COMMIT starts, the decision/finisher owns it,
so an ambiguous result cannot trigger a contradictory DROP. Malformed new
payloads/keys fail closed. Zero-length packing does not pass NULL to memcpy.
This extends current command semantics: use the one exact build on fresh
qualification fixtures; no legacy decoder, mixed-build or downgrade mode.
Stored reservations still use the existing EXCL row representation.

Native seven focused gates PASS: actual transaction and metadata SMs,
namespace sessions, allocator value/host reservation helper, cohort handling,
receipt pending and routing. The regression runs16 real old-style captured
CAS failures while ordinary allocation advances, then proves serialized
reservation succeeds without extending its wait; it covers owner/parts
checks, COMMIT/ABORT and malformed encoding. ASan/UBSan with leak detection
PASS for the changed transaction/metadata modules and test_txn (other library
objects normal). Final zero-payload server rebuild and allocator recheck
PASS. Mac full build is unsupported (`fopencookie`); syntax checks passed,
and all claimed execution is native nuc. Changed-source hashes match
`a10f96604`. Nuc attempt-62 `allocator-reservation-unit/result.json` SHA256
`18fb8fdaef4c6cd2b3df379b27596d2c584f31cbb2adbf2f3d32dd13318a04fc`.

Exact archive SHA256
`d2b98fc1103c7d28a2ba5642869c1ec36f8e7f5b88654b3618c35e830759428e`
now runs the same committed qualifier: nuc attempt-63 (three four-way loaded
rounds/cold/full) and node9901 attempt-59 (private RDMA/cold/full), fresh
fixtures under the existing host roots, trace-only overload flags removed,
normal `EFS_READ_FRAG_PROF=1` retained. No test budget, deadline, attempt
count, quota, quorum, flush or verification requirement changed. Both
source manifests and launch scripts are retained with the evidence.
Previous closed-text indexes: nuc attempt-62 (159 files), SHA256
`c007c16a997c01a10b9fc7999ad8e92a9947623e57e2acb4b7c89224112d336a`;
node9901 attempt-58 (314), SHA256
`14b9bfd623f54924f1cc635ddf1ed083d2c05f66ff28a90de478a272cc2aad4f`.
No original store or failed payload was deleted/exported.

Node9901 attempt-58 final: targets/cold36,520 PASS; full **513/0 FAIL/
14 NOTRUN,735s,5 OVERBUDGET,BUILD0s,INCOMPLETE**. Allfive binaries match;
no owned process/mount remains. `final-closed/manifest.json` retains309
closed files, SHA256
`1b142af2b4b31ab3c941cddf0bc2fe279c42a3a39cb589354bfcec6f7d44a3eb`;
qualification result SHA256
`7c28e32c2f0b2cc0484270100930f04150ed18251079070baefa541b37710e3d`.
NOTRUN limitations are the same separately documented missing devops,
Valgrind, forced-TCP RDMA exclusions and fault-build/nested-runner skips.
This older result does not qualify the new allocator protocol.


#### Attempt-62: remaining preparation failure, retained payloads verified

Exact product/binaries `0b0502dd3`, unchanged nuc profile: loaded round1
all four targets PASS; round2 K38 and both D4 targets PASS, K22 **FAIL** at
its original no-client-errors assertion. Metadata worker1 round1 created14/16
directories: two BUSY results in30.003s;250 subsequent CREATE/INVAL errors
follow those missing parents. No payload writer errors; stable confirmed and
attempted prefixes both `[150,157,153,156]` (616). Sampled MKDIR refusals
were stage9 (transaction preparation). **The BUSY log is globally throttled:
absence of stage12/13 samples does not exclude session/receipt contention.**
The exact-sequence guard is independently tested but has not solved every
loaded failure. Round3 and the driver's cold/full stages remain NOTRUN.
Failure log SHA256
`056ccc2dfd6dd4a44c99e021fa45fd7313f6a2f5ff46ab6fcb8cbc7aa0c0b2ce`;
producer log SHA256
`e0b531679ebc288ce95eaa059c9a813a92204606ea022cb04893afd8ee2ae43d`.

All raw evidence stays on nuc at
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-62/`.
Before cold reads, `failed-k22-round2/stores-before-cold/manifest.json`
hash-verified2,116 files/119,020,300 bytes against the stopped original stores;
manifest SHA256
`68a9b14c7f4f4ab99513b6c95d0c45fe5dcbc78ddfb52a0915912a440a440a76`.
Original stores and forensic copies remain retained for this open investigation.

Original-binary `scripts/k22_verify_discovered.py` cold-read616/616 discovered
payloads, exact per-writer counts matching the separately retained stable
checkpoint, no read/mismatch/cleanup errors. Its conservative **INCOMPLETE**
verdict remains unchanged; the original loaded gate remains FAIL. Result at
`failed-k22-round2/cold/result.json`, SHA256
`e657e08476bfd0d1953da0f4a12ebb0d4bf2b12596bf96935b435a043cc46c30`.
The four D4 oracles independently cold-read1,090,1,037,868 and1,044 exact
payloads, and `scripts/k22_verify_retained.py` cold-read the first round's
457 acknowledged files: all PASS, zero errors and clean shutdowns. Combined
`retained-verification-summary.json` SHA256
`0e968b16d73b14ed3ea84dc6fd7bab8b92c2b88875471cd61aa7f3580f3724a4`.
Use each retained `expected-payloads.json`/`confirmed.json` with the original
`qualification/binaries.sha256`, an explicit owned fixture root and a new
output directory to reproduce those verifiers; never replace failed evidence.

After all six verifiers exited, diagnostic runner
`scripts/diagnose_publication_loaded.py` (commit `e94f090c8`) was staged beside
those unchanged0b product binaries. Command from attempt-62/src:
`EFS_GIT_ID=0b0502dd3 python3 scripts/diagnose_publication_loaded.py --root
/home/maik/efs-taskH-repair-20261027/attempt-62/diagnostic-loaded-fixtures
--output /home/maik/efs-artifacts/taskH-repair-20261027/attempt-62/diagnostic-loaded
--binaries /home/maik/efs-artifacts/taskH-repair-20261027/attempt-62/qualification/binaries.sha256`
(on one shell line). It runs the same four tests concurrently once with
existing Raft/opid/read tracing, disables the diagnostic refusal throttle,
records binary/script hashes and environment, retains every fixture and
changes no test limit. Logging overhead makes it **diagnostic only**;
outcome pending. No product configuration or deadline was weakened.


**Active exact-source candidate `0b0502dd3`:** nuc attempt-62 and
node9901 attempt-58, same qualification profiles and original limits,
`EFS_READ_FRAG_PROF=1` diagnostics. The source includes the earlier failure
oracle/retention fixes: no test overlay is needed. Archive SHA256
`a4a20a5678b13568af810d57ef2f35631ee36f4ce9765f0d80560daca6949484`.
Both use `scripts/qualify_publication_repair.py` and fresh fixture/evidence
paths under the existing host roots. Previous closed text indexes at
`closed-before-exact-receipt/index.json`: nuc attempt-61 (73 files),
SHA256 `50e83fa6a4cdc013dd409ed3b28ae4038ac1395bd1f56789e569f74ad36c4e2b`;
node9901 attempt-57 (310 files), SHA256
`bd31e8547dbd675b366cc5d1a9a334bf1ee71a4c73da69085ea0b43dfa5de508`.
Stores/forensic clones remain untouched by staging; all prior private
processes and mounts were absent before each launch. Nuc outcome and separate retained verification are above; node9901 full outcome pending.

Node9901 attempt-57 completed targeted PASS; full **513/0 FAIL/14 NOTRUN,
INCOMPLETE,764s,6 OVERBUDGET, BUILD0s**. Retained315-file final manifest
SHA256 `b6992642592e6f6fb2d399a8764d1d70e07a6a6e1cabc4f3a21d8ab7dc9d9ccd`;
result SHA256 `ed272d8f0bd20af44cdad9256449f489c82c082eee0a2eda0016b0b4d0c32441`.
All five product binaries still matched; no owned daemon/client or mount
remained. This older candidate's result does not qualify `0b0502dd3`.

Nuc attempt-61 (`b4331aef8` plus diagnostic-only `fdaa276da`) **FAIL** in
loaded round1: K38 and both D4 gates PASS; K22 memory refusal, actual
pressure handoff and drain PASS, but metadata worker1 round3 exits1.
Its MKDIR phase creates13 of16 directories and returns BUSY for3; CREATE
then reports375 invalid-parent failures derived from those missing dirs.
No writer payload error was reported. Admission at210s:3,173,122 bytes /
4,194,304 budget,367 refusals; low-capacity handoff2.747s after the first
logged admission refusal, drain3s. This was not a missing-pressure-proof
failure. Later rounds, cold/full driver stages NOTRUN.

The old MKDIR diagnostic stage4 conflates session admission and receipt
probe, so it does not prove which check caused those refusals. The next
candidate gives them distinct stages12/13. A code defect was independently
reproduced: `host_opid_replay` refuses a raw receipt miss while **any**
reduction on that client's shared window is pending, including unrelated
sequences that commute. A new exact-sequence guard still refuses an EXCL
window replacement, this sequence's pending verdict, or any pending ACK
covering it. Unrelated sequence/older ACK reductions no longer serialize
the client. It neither reads a guessed transaction decision nor turns an
unknown outcome into success. It validates every scanned envelope and
stops at the existing64-bit window width; malformed state is an error,
overfull state is BUSY. No format, identity, receipt window, retry cap,
operation deadline, workload or test budget changes. Live causality and
qualification of this correction are still owed.

Real-memory-KV transaction tests cover own/unrelated pending sequences,
covering ACK, exact COMMIT materialization and ABORT behavior, exclusive
window replacement, malformed records, and the64-record scan bound.
Native normal tests plus ASan/UBSan on the modified transaction module and
test PASS (the rest of the library is the normal build). The unchanged
original blanket predicate fails the independent-sequence assertion at
runtime. Actual host helper tests PASS. Raw:
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-61/receipt-unit/result.json`,
SHA256 `f8adc07b6e65b2b577bd180a5ec0be815c89070907dda2735915d4092939ec4b`.
The regular existing `test_txn` command carries the added cases; recipe
check stays within the unchanged1650s declared aggregate budget.

**Failed evidence and existing payloads:** stable before/after checkpoints
retain confirmed/attempted prefixes `[114,115,115,110]`, no active workers,
and the metadata failure. Failure log SHA256
`466af8c9a4f9aacaf8f5e27ee0c3c97146b46ec5d03ac546de2460e409cf5304`.
All three K22 stores were byte-hash copied before restart:
`failed-k22-preserved/stores-before-cold/manifest.json`,2,665 files /
107,962,303 bytes, SHA256
`aec4514e81ce744a4666fb9c899b56c5e3f0a1f251213ed86c4b799e4be8b384`.
Same-build discovered readback matches454/454 writer payloads with zero
errors and clean shutdown. Per-writer discovered counts exactly match the
stable acknowledged prefixes. Its discovery-only tool still reports
INCOMPLETE; it does not certify the metadata producers or clear K22 FAIL.
Result SHA256 `337069d3209667a28f0244217daabf89bfd514047535b498acaaa0704b7dcacb`.
Separate same-build D4 cold results PASS819/819 and1,040/1,040, zero errors
or uncertain paths. Their result hashes are respectively
`bcc9084ff5d8b425db4748dc1fb64cbbe54779aebbd26ae31eb967ca712efc87`
and `b4daadd7a4010b01aca4392193d919e7e299e051f8084ee5d92babd9642a4792`.
All stores/logs remain on nuc. Node9901 attempt-57 has separately passed all
`b4331aef8` targets and20,784 exact retained cold payloads plus32 log payloads;
its full suite is running, not yet a final verdict.

### Current candidate and active runs

`b4331aef8` (after evidence-only `dd8794804`) is now running the same full
qualification on fresh fixtures: nuc attempt-61 and node9901 attempt-57,
under the existing host roots below. Both native builds and initial focused
units passed; live/full outcomes are pending. Source archive SHA256
`0483f31588ac9c8dc7c6cfa679ce1141bfcae0c3ffef29ca96f17fb19014c303`.
Run command remains `scripts/qualify_publication_repair.py --profile nuc`
or `--profile rdma` with fresh root/output paths. `EFS_READ_FRAG_PROF=1`
adds read diagnostics only; every original test limit and verdict remains.
Each artifact root's `run.sh` and `source-manifest.json` record the exact
command/environment/archive. K36 failure retention is enabled explicitly
and in its failure handler. Failed attempt-60 stores/forensic copies remain;
its lost K36 evidence is still a limitation, not cured by a future pass.
No main cluster was changed and nothing was merged or pushed.

Before either full suite started, a committed diagnostic-only test overlay
`fdaa276da` was installed: `tests/test_k36_recovery.py` records the known
successful-close filename/size oracle and the failed stage/name/errno on
read failure; `tests/test_k33_setup.py` exercises EIO and mismatch paths.
Successful-test behavior and limits are unchanged; errors still fail. All
five product hashes match before/after, and the actual helper unit passes
on both Linux hosts. Reproduce the harness by taking these two files from
`fdaa276da` over product archive `b4331aef8`. Both artifact roots contain
`harness-overlay-fdaa276da/manifest.json`, SHA256
`bda2a7362e4a1a7b18f32d3f7de064f79cb9b23514a0a2131a166d6d0970a8cb`,
with exact before/after hashes and retained unit output. Native product
identity remains `b4331aef8`; do not describe this as an unmodified archive.

### Full-suite follow-up: two nuc failures remain open

Nuc attempt-60 full gate on `16728b167` completed **FAIL:513 commands,
2 FAIL,14 NOTRUN,1,652s,15 OVERBUDGET, BUILD0s**. Targeted results below
remain scoped PASS and do not override these failures. The closed357-file
index `final-closed/manifest.json` SHA256 is
`864ecd07ac4f7633dbad1267780b3778d605710331322c8cc3119c9e6b4f49e9`;
`qualification/result.json` SHA256
`1690ab3a76f3e72d0402950a419c4b46679f966100f3b0f9e070305edcfa3af5`.
All five binaries still matched their initial hashes and no owned native
process or mount remained after the run.

- `test_stop_progress.py`: busy-client stop failed at line79. Its session
  fence completed, but lease cleanup shard2341/index12712 returned BUSY
  without a known exact-entry verdict;1,337 touched shards,839 selected
  for cleanup, zero probe failures. Fence1,066ms/cleanup8,894ms; it correctly
  did not report successful revocation. Failure log SHA256
  `41d86c7ab5e3d722ec6a86edf547814f81c4c0d193d5afc5aa9e04537accc2fd`.
- `test_k36_recovery.py`: read errno5 at line433, after the compaction-crash
  restart, recovery verification and successful data-commit. No mismatch
  is proven; a read failure remains FAIL. Failure log SHA256
  `7fca3d232820c3fa0a00966076e6208ecd282824e44000416e9ac57db03b98fc`.
  The old test removed its fixture in `finally` without the opt-in KEEP:
  those stores/client logs are lost and this failure cannot be fully
  replayed or its original payloads reverified. Only the captured failure
  survives under `full-failures/`. The test now retains every failure and
  the private qualifier requests KEEP. A test executes all four actual
  retention-condition cases. This prevents future loss; it does not repair
  the missing evidence. An original-build isolated control at
  `k36-isolated-167/` passed all11 stages with `EFS_READ_FRAG_PROF=1`,
  retained fixtures and the unchanged900s command limit. This does not
  clear the loaded failure; the next loaded run needs retained read
  diagnostics to distinguish transport deadline, missing fragment and
  metadata-view causes.

The stop stores survived. Before cold restart, all three were copied and
byte-hash verified on nuc: `failed-stop-preserved/manifest.json`,18,339 files,
142,714,214 bytes, SHA256
`8d009de7bbc7f6437b56562337a9ca4a21dc6d82478efe6cd247d378cb74b559`.
The test reached its stop assertion after the completed population's worker
pool, so all1,500 expected `dN/f` files and their SHAKE-256(`stop-N`)64-byte
payloads are known. `scripts/stop_verify_retained.py` validates that exact
manifest/test hash, original binaries, private endpoints and original
per-member automatic quotas; it never formats. The same-build cold result
is **PASS1,500/1,500, zero read/mismatch/cleanup errors**, without clearing
the stop FAIL. Result SHA256
`4a0233323ce71cc7b42684a3f5d4851094b281e6696c9f1f5c68ee9b4e05f228`.
All copied payloads remain on nuc.

The proposed lease-cleanup fix retains the bounded exact-entry wait. Only
when that submitted entry returns unknown BUSY, it repeats the existing
owner-floor plus fresh ReadIndex selection, without another proposal. If
that independently proves the entire old-epoch lease/lock phase empty,
it reports that empty-phase proof, never an invented entry verdict. The
floor prevents new old-epoch ownership; late exact batches cannot recreate
close obligations for absent leases. Nonempty selection or a failed proof
keeps ambiguous BUSY; storage failures remain failures. Focused actual-route
normal/ASan/UBSan tests PASS, including failed/nonempty proof and unchanged
IO refusal. The previous source fails the new empty-postcondition case.
Native qualification is owed; no deadline, budget or acceptance change.

### Bounded flush candidate: native targeted qualification complete

**Tested product `16728b167`: targeted PASS on both hosts. Node9901 full513/0 FAIL/14 NOTRUN
(INCOMPLETE); nuc full FAIL as recorded above. No full-suite PASS claimed.** Nuc attempt-60 completed all three
original four-way loaded rounds (K38, D4 progress, D4 fixed working set,
K22 memory admission). Node9901 attempt-56 completed the RDMA targets,
slow-member dirty/clean durability, and TCP K22 memory admission. Both
passed operator resync, automatic IDs, PUFA, quota/reclaim, fast units and
actual K22 log-watermark admission. No workload, concurrency, quota,
foreground/drain/retry limit, admission threshold or durability check changed.

| Native target | nuc TCP, three concurrent rounds | node9901 RDMA, separate targets |
| --- | --- | --- |
| K38 shared-chunk replay | 14,400 records exact | 14,400 records exact |
| D4 progress write+fsync maximum | 3.592 / 4.258 / 2.424 s | 0.296 s |
| D4 fixed-set write+fsync maximum | 4.544 / 5.254 / 3.584 s | 0.069 s |
| D4 progress drain | 7.2 / 24.9 / 14.9 s | 111.2 s |
| D4 fixed-set drain | 42.6 / 41.1 / 38.2 s | 43.8 s |
| K22 memory-admission refusals | 787 / 900 / 1,277 | 251 |
| K22 client writes, zero errors | 641 / 421 / 420 | 3,288 |
| Exact retained payloads after orderly cold restart | 7,766 / 7,766 (nine fixtures) | 37,461 / 37,461 (three fixtures) |
| Separate actual log-watermark, exact cold payloads | 32 / 32 | 32 / 32 |

D4 foreground limit stays10s and drain150s. All foreground classes,
payload checks and cleanup passed; nuc's largest `.efs/stats` latency was
2.917s. K22 memory pressure was observed on the intended leader, followed
by a low-capacity election hold, leadership transfer, and drain3s after
release in each run. Nuc's memory-admission proof arrived at224/212/217s,
inside the unchanged900s bound. The log test independently observed
committed-minus-applied768 at watermark768/cap1024 with KV pressure0;
host admission rose0→1,100 on nuc and0→1,108 on node9901. Both sets of
cold log-test shutdown codes were all zero. Its own write limit is30s,
with measured maxima8.384s/9.209s; this is not the D4 foreground oracle.

The formerly stalled nuc victim's first loaded-round KV flush log now has
maximum300ms among recorded flushes (the log emits only totals>=100ms),
versus2,617ms in failed attempt-59. This supports the measured fix for that
case, not a guaranteed storage-latency bound or a universal election proof.
The 45s/75s D4 loads are short regression checks: their plateau threshold
is unset and nuc has too few samples for a plateau verdict. Long-duration
RSS/space plateau, physical power loss and multi-host transport behavior
are not qualified here. K33's original EIO cause remains OPEN.

Exact archive SHA256:
`77390c57fe3bb9a7d4f176f7c9e81b39e4fd40c3969c77f8be6b248d139f6159`.
Reproduction is the committed `scripts/qualify_publication_repair.py`, from
an exact-source Linux build tree, with `EFS_GIT_ID=16728b167`:
`python3 scripts/qualify_publication_repair.py --profile nuc --root FRESH_FIXTURES --output FRESH_EVIDENCE`,
or `--profile rdma` on node9901 with the private libfuse environment below.
The runner builds with8 jobs, retains fixtures, runs the original fault
hooks and enables packing/query/GC; O_DIRECT fragment I/O (KV segment
files retain their existing buffered checked-fsync path), three loopback
members, original per-test quotas (D4 progress1G, fixed-set2G, K22 memory
and log1G). RDMA targets use `mlx5_0`; K22 memory deliberately forces TCP.
Full gates use eight slots and their existing command limits/budgets.
No live main cluster was deployed or changed.

Raw target evidence remains on its host; payloads were not exported:

- nuc: `/home/maik/efs-artifacts/taskH-repair-20261027/attempt-60/`;
  source/fixtures `/home/maik/efs-taskH-repair-20261027/attempt-60/`.
  `closed-targets/manifest.json` (120 closed log/exit copies, nine cold results),
  SHA256 `6b786829bdfa3d9cbb498ebf675b2398f8aa6a6c90c52e926049e84254e8165c`.
- node9901: `/data1/efs/evidence/taskH-repair-20261026/attempt-56/`;
  source/fixtures `/tmp/efs-taskH-repair-20261026/attempt-56/`.
  `closed-targets/manifest.json` (100 closed log/exit copies, three cold results),
  SHA256 `e61689f2f1a4e42baf4cbffc95df0882b8a1d68c025223799f4882828993a764`.

Each `qualification/binaries.sha256` records all five binaries. Fault-daemon
hashes are nuc `723b9ff26d214f05ec43e79fd59f8d7e34949f23c61f4341b74afc9d68d30b22`
and node9901 `84835af7dda61999c94697251875de9ccc6a9a87df58da53d3cb2a9e1a436d7f`.
Initial compiler logs were separately sealed before full-suite collection:
`initial-build.log` hashes nuc
`14cc16f894e6aa0eadbcf032401a27aaf3b792820256966a8647a6cd6aa6ac9f`, node
`186dd658f1fed05743f87d9c04e58f71d51407506c50c9d702fa91b18b723ec0`.

Evidence-only follow-up `dd8794804` fixes the collector's `build.log` basename
collision: the full suite's compiler output now goes to `full-build.log`,
with exclusive creation so repeated collection cannot overwrite evidence.
The actual helper regression verifies both logs survive and repeat collection
refuses replacement. Local helper and recipe checks PASS. Active archived
sources/binaries were not edited; native qualification remains of `16728b167`.
Original failed stores, forensic clones and recorded failures remain preserved.

Node9901's full gate completed **513/0 FAIL/14 NOTRUN, INCOMPLETE,669s,
7 OVERBUDGET, BUILD0s**. These existing scheduling-budget overruns remain
reported; neither their budgets nor test deadlines were raised. The14 marks
include unavailable cluster-stop scripts/Valgrind, RDMA paths unselected by
the full suite's forced TCP profile, a non-fault follower-bound binary, and
nested leak/sanitizer runner refusal self-tests. They are not14 demonstrated
product failures, and this remains the runner's INCOMPLETE verdict. The
separate RDMA target results above do not silently turn the TCP suite into
full RDMA qualification. `final-closed/manifest.json` seals301 text artifacts,
SHA256 `8979f82d0f0ab7517821aaea63f78367c77e2d3f9079ccb4897c4e82cb99ccae`;
`qualification/result.json` SHA256
`bcb8e6a373cd165cfad1276e50084a1e6849b5a6ca40871994bbf089d708fa8d`.
All five binaries still match their initial hashes; no owned daemon/client
process or mount remained. The collector helper also passes on both Linux
hosts in separate `collector-unit/` directories (no active source edits),
with common log SHA256
`5f5afa797d1226f251adf72e4bda00aa246f3f87917601ca20274fe252ba1245`.

### Serial segment durability stalls the pump before the intended K22 trigger

**Nuc attempt-59 FAIL**, `c26286eab`: first loaded round K38/D4 progress/
D4 plateau PASS, but K22 fails its unchanged900s memory-admission deadline.
The victim loses both leaderships to higher terms before memory pressure;
its later bounded follower backlog is not the required leader proof.
No alternative proof or longer timeout is accepted. Later rounds/full NOTRUN.
D4 progress write+fsync max4.890s/stats0.592s/drain4.4s; plateau
5.161s/7.256s/30.3s, zero payload errors. Their same-build cold results
are PASS936/936 and1,046/1,046 respectively, at nuc
`attempt-59/cold-after-failed-k22-test_d4_{progress,plateau}/result.json`.

The victim's log records `kv-flush: files=28 io_ms=2369 manifest_ms=248`
at02:14:53.770 UTC October28, followed by the group0 higher-term step-down
at02:14:53.774. Code confirms this synchronous automatic flush runs under
the pump's host mutex. That is a directly observed long pause correlated
with the election; other scheduling/storage contributors are not excluded.
The change keeps the memtable frozen and existing publication order, but
finishes independent segment files in cohorts of **at most16**. Each child
still executes the unchanged checked segment finish/fsync/rename. Every
accepted child is joined even after another fails; all files must finish
and open successfully before the existing MANIFEST publication/WAL reset.
Thread-creation failure uses the same checked finish inline. An impossible
owned-child join failure stops the process before publication. No format,
quorum, memory/admission threshold, timeout or user-facing switch changes.
This allows the OS/device to combine durability work; it does not prove
that a filesystem latency bound is guaranteed. Live qualification is owed.

`tests/test_kv_flush_finish.py` compiles the actual compactor against the
real LSM and segment writer. It covers35 ranges (multiple16-file cohorts),
one failed finish, a failed thread creation, one-file completion, unchanged
manifest during outstanding work, and abrupt exit followed by exact cold
replay of every key. The failed finish drains all16 accepted jobs and
publishes nothing; WAL replay still recovers all35 rows. GCC normal and
ASan/UBSan PASS; serial `c26286eab` source fails specifically at the runtime
overlap assertion. This control's artificial overlap wait is not a native
performance measurement. Raw results/hash index:
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-59/flush-unit/result.json`,
SHA256 `3104e0fa0091d2b76138ff7fe9d8cfdb1ffb4678468dc8e65c206997cc289386`.
The new regression and existing metadata-flush unit share one fast command
(`python3 tests/test_meta_flush_batch.py && python3 tests/test_kv_flush_finish.py`):
measured0.626s on nuc, within their existing1s group budget. Recipe check
PASS at the unchanged1650s aggregate bound. No budget was increased.

**Failure preservation/readback:** original K22 stores were hash-cloned
before restart at nuc `attempt-59/failed-k22-preserved/manifest.json`, SHA256
`7a2eb3c1ed89e1ba3655189f995901b4d4f8286c140a2872f26b912b1081cca8`.
The failed old harness wrote no acknowledged high-water journal. The new
`scripts/k22_verify_discovered.py` therefore cold-reads every discovered
writer file and checks deterministic contents and internal sequence gaps;
it always returns nonpassing status because missing acknowledged paths
beyond the observed namespace cannot be excluded. On original binaries:
**16,215 discovered/checked, zero mismatch/read/cleanup errors, INCOMPLETE**.
Raw `failed-k22-preserved/cold/result.json` SHA256
`f216277c6cdc1313c6cb0f7a6a169c8a2c50d9b58cb5f02fc62f7d2ae2db0f7a`.
This is not full acknowledged-payload verification and never clears K22 FAIL.
Future failures save known/attempted writer counters before and after
shutdown, errors, metadata-producer results and active-worker uncertainty.
No acceptance or workload changes; checkpoint-write failure is reported
without preventing cleanup. Actual checkpoint helper tests PASS, including
a simulated checkpoint disk error. Existing successful manifest rules stay.

**Node9901 attempt-55 completed**, `c26286eab`: targets PASS, full
**513/0 FAIL/14 NOTRUN, INCOMPLETE,654s,6 OVERBUDGET, BUILD0s**.
All23,603 retained payloads cold-exact (18,849 progress+1,056 plateau+
3,698 K22), plus32 log-gate payloads. Its100-file closed-target manifest
at `/data1/efs/evidence/taskH-repair-20261026/attempt-55/closed-targets/manifest.json`
has SHA256 `c901934d0365c748696101bbda755f979db11e2d350307dcb659e40ef97663e0`.
It does not clear the concurrent nuc K22 failure or qualify the newer flush.

### Retired-replica-safe background submission and completed RDMA baseline

Review of the new batched finisher found that foreground-style local waits
would strand cleanup on a retained but retired replica after membership
placement changes. The corrected builder accepts an internal settled-submit
callback; the finisher uses **the existing `host_bg_propose(..., verdict=1)`**
for each bounded group entry. Its exact leader verdict and retired-local-
replica handling remain authoritative. Foreground batching is unchanged.
No caller waits on the callback's unused index references; all group
submissions must complete before retirement. This is an internal call path,
not a configuration switch. Batching saves per-participant rounds without
claiming one shared physical barrier across the background groups.

Nuc attempt-58 (`d49fb47ca`) stopped in the fast suite because the separate
finisher election/handoff test still expected the old transport seam;
no live workload started. Its full fast log remains at
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-58/qualification/fast-unit-gate.log`,
SHA256 `deb36c8ed98c675d71bdd57aed965ea127d3ffc0d83a9b4436f54c3c2e518d8e`.
The updated handoff test keeps its original election, decide-once, timeout
and surviving-COMMIT assertions. Actual batching/lifecycle is tested in
`test_prepare_cohort.py`; actual retired-replica remote verdict handling in
`test_gc_verdict.py`. The append, K32, finisher/cohort, election/handoff and
GC-verdict tests are run together with GCC before another native candidate.
Normal/local sanitizer controls pass. Native qualification remains owed.

**Node9901 attempt-54 completed on `b158403d3`: targeted PASS, full
INCOMPLETE.** Full **513 commands, 0 FAIL, 14 NOTRUN, 645s,
5 OVERBUDGET, BUILD0s**. The result hash is
`aeddb4efadf908b51bb391da1d99705bd511314df36f450965c9ad252987bb5b`.
Three exact cold fixtures: progress30,918, plateau1,056, K22memory3,827
(**35,801 total**), plus32 log-watermark payloads. Closed target manifest
`/data1/efs/evidence/taskH-repair-20261026/attempt-54/closed-targets/manifest.json`,
SHA256 `0f868bbf0c3ea408a39bc50a8d4b72d8e19ac212cee79b4c676e14dcbb2b0bc2`.
It indexes100 closed text/exit files and all three cold result hashes.
D4 progress write+fsync max0.340s, stats0.096s, drain5.3s; native resync
cleared at commit589 after100 operations (15.5s). Actual log gate: cap1024,
watermark768, leader applied13/commit781/pressure0, admission0→940,
32 exact cold payloads and both orderly stop sets `[0,0,0]`. Its own
30s fsync bound had max8.810s; that is not the D4 ten-second metric.
The14 NOTRUN items remain absent tooling/infrastructure and unselected
RDMA paths in this TCP full suite; separate RDMA target scopes stand on
their own. This baseline does not qualify the newer cleanup code or clear
the nuc15.717s failure.

### Source-slice integration correction before live cleanup qualification

`a42933a72` nuc attempt-57 compiled the native binaries but stopped in
`test_append_prepare_batch.py` before live workloads: the test's broad
batch-builder slice accidentally included the newly adjacent finisher
helper, whose types/functions are outside that fixture. This is a test
integration failure, not a native workload failure. Raw output remains in
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-57/qualification/`.
The helper definition is moved outside that existing slice; the finisher
regression explicitly selects its helper. Runtime logic is unchanged.
All three affected actual-source slice tests (append PREPARE, K32 entry
counts and finisher/cohort) now PASS with GCC on nuc; finisher/cohort and
append PREPARE also pass locally, finisher under ASan/UBSan. The isolated
GCC results/hashes remain in `attempt-57/layout-unit/result.json`.
A fresh exact-source native rerun is still required; attempt-57 stays FAIL.

### Further loaded D4 failure: retry and background cleanup

Candidate `b158403d3` includes resync fix `db289451c` and regenerated
documentation. Archive SHA256
`0572288b618b41bd18579b94535c9e9f46b6b1128e37859497b9e738a902233c`.
Nuc attempt-56: fast units, native operator resync (28.6s, resync cleared),
automatic IDs/discovery/restarts (56.0s), and loaded round1 K38 and K22 PASS.
**D4 plateau FAIL:** foreground write+fsync15.717s exceeds10s, of which
14.936s is publication preparation, COMMIT/proof wait0ms. Stats max1.376s;
no workload read errors or mismatches; drain23.1s. The isolation watcher
made174 scans with no foreign recognized EFS workloads; this does not
prove absence of unrelated filesystem I/O. Later rounds/full suite NOTRUN.
Original tests, limits, quotas and concurrency remain unchanged.

Before restart, all three failed stores and closed text were copied on nuc
and every file hash compared: `attempt-56/failed-plateau-preserved/manifest.json`
under `/home/maik/efs-artifacts/taskH-repair-20261027/`, SHA256
`f7a8a0bc2e282cccc9e71f3bb02f956e0ff343aa94d946a766f6ee32187e81e6`.
Same-build cold readback matched **1,035/1,035 recorded payloads**, zero
uncertain/read/cleanup errors. Outcome **INCOMPLETE**, not payload PASS:
the failed workload's expected manifest is incomplete. Failure and forensic
clones remain unchanged; nothing copied to the Mac.

Trace for inode15315/publication `18e28e0020faac47:6` shows repeated BUSY
attempts under the same identity, then `commit-abort`, receipt STALE and a
new publication succeeding. This supports investigating unnecessary
re-preparation/cleanup work, not a claim that every millisecond is explained.
Two follow-up changes are under qualification:

- A REPORT already fenced its coordinator group. When its exact transaction
  has a known ABORT (live or retired), resolve that existing decision before
  returning STALE; do not install PREPAREs under an identity that cannot
  commit. Failed cleanup remains an error. Missing/reclaimed decisions
  grant no abort proof, and known COMMIT retains the existing validation path.
- Background transaction finishers now batch RESOLVEs per group using the
  existing bounded TXN_BATCH builder and settled background path.
  Every exact apply verdict is still required before RETIRE. The original
  ten-second finisher lifetime, cleanup reserve, durability barriers and
  BUSY/NOT_PRIMARY retry rules remain. This removes scalar per-participant
  consensus rounds; it does not relax D4's ten-second foreground limit.

`test_prepare_cohort.py` executes the actual finisher plus helper for both
COMMIT and ABORT, 144-part/two-group and cap-split cases, storage failure,
BUSY through expiry, shutdown and already-expired work. Fixed normal and
ASan/UBSan PASS; replacing only the finisher body with `b158403d3` fails
its two-entry/completion assertion at runtime. `test_k32_entry_count.py`
uses the real in-memory decision table for live/retired ABORT and verifies
no new stage/pin, complete cleanup before STALE, and BUSY/IO preservation.
Linux GCC normal/sanitized PASS; disabling only the shortcut fails the
no-PREPARE assertion. Raw control/fix logs and hashes remain in nuc
`attempt-56/focused-resync/cleanup-unit-result.json`; local finisher control
and sanitized logs are `/private/tmp/taskH-finisher-{scalar-control,cohort-san}.log`.
Native qualification of the cleanup changes is still owed.

Node9901 attempt-54 on `b158403d3`: targeted stages/cold/log-watermark PASS;
full eight-job suite completed with0 FAIL/14 NOTRUN (INCOMPLETE). No claim of full-suite PASS or of nuc latency
qualification is made from those independent scopes.

### Resync after leadership replacement, and second readiness race

Nuc attempt-55 full: **513 commands, 1 FAIL, 14 NOTRUN, 1316s,
22 OVERBUDGET, BUILD0s**. Only `test_operator_workflows.py` failed at
its unchanged60s catch-up bound: original inspected leader had become a
follower of member2 at commit/applied610; the resyncing peer stayed at
applied3, resync=1. Captured failure, exact test/helper and binary identities:
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-55/failed-operator-preserved/manifest.json`,
SHA256 `48ae2413a4b68d63dfd890e14db6cd0cb5863fa8f0c0af9af45e327156e14952`.
The test deleted its failed fixture even after an exception. Those original
stores/full daemon logs are unavailable; no original payload verification
or complete raw revalidation is claimed. The captured evidence remains FAIL.

A deterministic three-core regression independently reproduces a matching
liveness defect: resync refuses ordinary AppendEntries with a zero-prefix
rejection. A replacement leader has no successful match recorded for that
peer. The old snapshot-request guard required an earlier successful match,
so it never offered the image needed to end resync. The new guard requests
an image on any non-BUSY negative zero-prefix rejection. A valid image also
suits a fresh empty follower (which may now receive a snapshot sooner).
Successful zero-prefix replies and capacity BUSY still bypass this path.
No wire/store format, durability condition, retry deadline or admission
limit changes. Snapshot-unavailable handling keeps ordinary retry behavior.
The focused test elects a replacement leader, obtains a real resync rejection,
checks success/BUSY controls, exports/installs the requested image and proves
normal payload replication resumes. Both ACK-window fixture variants fail
the old core at the image-request assertion; the fixed complete Raft unit
passes normally and under ASan/UBSan. Receive-persistence regression PASS.
Mac full-library build cannot compile Linux `fopencookie`; Linux follower
capacity and native resync/election/growth gates remain to run.
Local raw: `/private/tmp/taskH-raft-resync-{before,after,san}.log` and
`/private/tmp/taskH-resync-receive.log`; reproduction is the committed
`tests/test_raft.c` linked with `src/raft/{raft,raft_mem}.c` and
`src/common/batch_obs.c`, C99/GNU_SOURCE/pthread, optionally address+undefined
sanitizers. No claim that the lost native fixture proves this exact cause.

Node9901 attempt-53 test-overlay full (`39965d653` runtime,
`git diff39965d653..6827929a9 -- tests`): **513 commands, 1 FAIL,
14 NOTRUN, 643s, 4 OVERBUDGET, BUILD0s**. Binary before/after checks match.
Only `test_k33_autoid.py` failed: another Raft-up-only wait read the later
listener-derived node ID too early. Failure/source/helper/binary checks
and summary retained at
`/data1/efs/evidence/taskH-repair-20261026/attempt-53/full-harness-6827929a9/failed-autoid-preserved/manifest.json`,
SHA256 `8de35685c704229acbf8c7e6252ee9db8a1d8593672d96f947fcaf2c1b4403b5`.
That original fixture was also automatically deleted; same evidence limit.
The automatic-ID wait now requires both milestones within its original60s,
with an actual-expression delayed/stuck regression. Operator/automatic-ID
failed fixtures are always retained, and qualification explicitly keeps
successful fixtures too. No application failure is retried or ignored.
Full suites stay FAIL; NOTRUN items remain outstanding even after a zero-
failure rerun. Neither fix is merged or pushed.

### Growth harness failure: stricter readiness and preservation; full rerun

**Node9901 attempt-53 original full FAIL**, runtime source `39965d653`:
**513 commands, 1 FAIL, 14 NOTRUN, 625 s, 6 OVERBUDGET, BUILD 0 s**.
The only failed command was `python3 tests/test_k33_growth.py`
(`tmp/fail-430.log`), at the simultaneous joins of logical nodes 6–8.
The test waited for `raft-host: up ... formed`, then immediately called
`node_id`, which parses the later public `node N listening on` banner.
The captured tail shows a committed node 8 identity and Raft/packer startup,
but no listener banner before that assertion. Source startup order confirms
that the Raft-up message precedes the listener banner. Waiting for the first
milestone is not proof of the second. This is a definite harness ordering
defect, not evidence that membership duplicated an ID or lost payloads;
the original daemon's later behavior cannot be reconstructed from this tail.

**Evidence loss:** that test's old `finally` deleted its fixture even after
an assertion. Its original stores and full daemon logs are already gone.
No original-payload verification or independent full raw revalidation is
claimed for this failure. The full captured failure, exact test/helper
sources and source/binary identity records are hash-copied at
`/data1/efs/evidence/taskH-repair-20261026/attempt-53/failed-growth-preserved/manifest.json`,
SHA256 `8e053f517bab14588378e7665d13fff1c0d00cf3d8c1667e39824a55de90e101`.
The full result hash is
`f768e67fd310d34fdce19c64281cd6babc659e8b99594c4a8aca283d39a117e7`;
189 other closed native text files are indexed in `closed-native/index.json`,
SHA256 `46c7a74493a86a1bc08e4c0dadd3210c7ca5c3a31e7b5230e70221335a18cb98`.
The 14 skipped commands/reasons match the prior node9901 suite. The separate
K33 five-node payload command ran and passed (43.8 s), not the failed growth
command. Target/cold PASS scopes below do not overwrite this full FAIL.

**Test-only fix `6827929a9`:** the same single 90 s wait must observe both
Raft-up and this process's listener banner before parsing node IDs. There
is no extra 90 s allowance or sleep/retry of an application operation.
The test now retains its fixture on exceptions; successful cleanup remains
unchanged. `test_k33_setup.py` executes the actual readiness regex with the
real helper and a controlled clock: delayed listener waits; no listener
fails at 90 s; exited process fails; the actual finally condition retains
failed fixtures. The previous test fails this runtime delayed-listener
control. No product source behavior, data format or qualification limit
changed.

After finalizing the failed suite and checking that its private native
processes had exited, node9901 started a **new full-suite rerun**, not an edit
to the failed verdict. It uses the same exact `39965d653` product binaries
and `git diff 39965d653 6827929a9 -- tests` (three Python test files only),
patch SHA256
`759fb5374eed276e7935932b47c374006d668134b7b69fbaf3ca5c8c13acdb3d`.
The original eight-job scheduler, 900 s command bound, per-test deadlines,
oracles and successful cleanup remain. Before/after binary hash checking
is mandatory. Source/test composition, commands and environment are retained
at `attempt-53/full-harness-6827929a9/`; its manifest SHA256 is
`4e8946bb5915a337a465089eda67a4bad43b7470b71d7052be9ea58579896eb7`.
The rerun is **RUNNING**, no passing verdict yet. Nuc attempt-55's already
active full suite was not edited or interrupted; it remains on the original
runtime/test archive and is reported separately.

### Current query candidate: both targeted profiles PASS; full suites running

Exact runtime **`39965d653`**; test/comment-only follow-up `1a52cf4a1`
changes no runtime behavior. Source archive and corrected-unit identities
are below. Both hosts use the unchanged committed qualification driver,
profiles/topology/dependencies below; O_DIRECT, packing and queries on,
original concurrency, quotas, deadlines and payload oracles. These are
private colocated fixtures, not a multi-host/fabric or power-loss gate.

**Nuc attempt-55 targeted PASS:** all three four-way concurrent rounds
(K38, D4 progress, D4 plateau, K22 memory), 14,400 K38 records across both
mounts/remount; no EIO or other target errors. Plateau max foreground
write+fsync **6.226 / 4.502 / 8.552 s** and `.efs/stats`
**0.873 / 3.497 / 3.280 s**, all within unchanged 10 s. Drain
35.6 / 31.1 / 30.4 s within 150 s. Nine orderly cold restarts/fresh readers
checked **7,435/7,435 recorded final payloads**, zero uncertain paths,
errors or cleanup errors; K22 confirmed 631 / 499 / 436 writes.
The independent actual log gate checks 32 more cold-exact payloads:
cap 1,024, watermark 768, leader applied 12 / commit 780, pressure 0,
admission 0→1,325, both stop sets `[0,0,0]`; max write+fsync 8.892 s within
its original 30 s, not D4's 10 s.

116 closed target log/exit files and all nine cold result hashes are in
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-55/closed-targets/manifest.json`,
SHA256 `2e71439313483266b31037a0c59388372a954aa7434fc4b7db73491366b44350`.
Full suite is **RUNNING**, not a full PASS. In particular, the prior
full-suite stats tail was not reproduced by these dedicated rounds; that
alone does not close the full-suite failure.

**Node9901 attempt-53 targeted PASS:** both slow-member durability variants
(each 22 acknowledged payloads exact after restart), forwarded-prepare
abort, quota/reclaim, three RDMA K38 runs (14,400 records), RDMA D4 progress
and plateau, TCP K22 memory/log gates all pass. D4 max foreground write+fsync
0.119 / 0.220 s within 10 s; drain 5.4 / 48.2 s within 150 s. K22 confirms
3,168 writes, zero errors. Three cold restarts/fresh readers checked
**37,542/37,542 recorded final payloads**: progress 33,318, plateau 1,056,
K22 3,168; zero uncertain paths/errors/cleanup errors. The independent
log gate checks another 32 exact payloads: cap 1,024, watermark 768,
leader applied 13 / commit 781, pressure 0, admission 0→956, both stop sets
`[0,0,0]`, max write+fsync 8.808 s within its original 30 s.

96 closed target log/exit files and all three cold result hashes are in
`/data1/efs/evidence/taskH-repair-20261026/attempt-53/closed-targets/manifest.json`,
SHA256 `b2e8a3cc5d78a4cff2270f9f7eadb65c0e69b4661c098684847b0520a0eb56e1`.
The original full FAIL and corrected-harness rerun are recorded above.
Neither host's short target profile proves a
30-minute RSS/space plateau. All original failures, their verdicts and
stores remain retained; no merge/push or overall qualification PASS yet.

### New full-suite stats latency failure and query LOOKUP fix

**Nuc attempt-53 full FAIL**, source `2636d5a60`: **513 commands,
1 FAIL, 14 NOTRUN, 1,515 s, 23 OVERBUDGET, BUILD 0 s**. Its three prior
four-way target rounds/cold checks below remain scoped passes. In the full
eight-job suite, `python3 tests/test_d4_plateau.py` (`tmp/fail-456.log`)
failed one `.efs/stats` operation at **10.339 s > unchanged 10 s**. The
497 foreground operations included max write+fsync 3.536 s, no operation
errors, no bad payloads, all 1,024 working files plus 32 foreground files,
and 66.6 s drain within 150 s. The short space samples marked GROWING;
that reporting is not a 30-minute plateau acceptance result. This distinct
stats tail is not relabeled as an EIO or as an ABORT-cleanup failure.
The guarded attempt-54 launch correctly refused to proceed after this new
failure; no attempt-54 deployment happened.

Before further deployment, byte-hashed pre-restart forensic clones of all
three failed stores and closed logs/results were preserved on nuc at
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-53/failed-plateau-preserved/`.
Manifest SHA256
`740c07b916d3a0e6eeeaa40e2577f39bd51835b912ffdb69f165f494580b563a`.
Original-build `scripts/d4_verify_retained.py` then checked all
**1,056/1,056 recorded final payloads**, zero uncertain paths/errors/cleanup
errors. The conservative incomplete manifest remains INCOMPLETE, never an
overridden PASS. `cold/result.json` SHA256
`168b4ed036447e6ed0dfbde8c0701a16c406a0234420ff625657492d2fabba88`.
The 225 closed native text-file index SHA256 is
`d02726ef8706a9e9eb665da4b51b83d4dfff2876577555f49c84ca967340464a`
at `attempt-53/closed-native/index.json`. No failed payload/store was
exported to the Mac or pruned.

**Code finding and correction:** query-result LOOKUP already obtains a
fresh, authorized server reply, then calls generic GETATTR to construct its
entry. Following the pinned-size branch precisely shows that it normally
reuses the result length, **not a second query fetch**: it still makes two
extra parent metadata reads (`virtual_access`, then `efs_client_stat_ino`).
The initial assessment that it fetched the query twice was wrong and is
withdrawn. `efs_client_stat_ino` serves a cached row when present and makes
an RPC only on a cache miss; these are two redundant parent accesses, not
two proven network round trips. They are unnecessary after this callback's
fresh server query proof. The exact decomposition of the observed 10.339 s
remains unmeasured.

Runtime candidate **`39965d653`** returns a value-only identity/attribute
snapshot from that callback's successful query reply and uses it to build
the LOOKUP entry, preserving the same inode reservation/lookup-reference
handoff. Another callback replacing/evicting shared cached bytes cannot
replace these captured values. Every independent LOOKUP/GETATTR/OPEN keeps
its own authorization; denied/unknown replies grant nothing, and OPEN
still checks immutable-version matching. No new caching interval, wire or
store encoding, deadline, quota or test bound changes. This removes proved
redundant metadata work; it is not yet a demonstrated fix of the stats tail.

Actual-function tests check one query proof/no extra parent reads per
LOOKUP, exact server owner/timestamps/content-version/size, cache eviction
between proof and reply, later permission denial despite cached bytes,
allocation refusal and unchanged outputs on failure. Mac normal and
ASan/UBSan checks, authority errno, query page/serialization/routing tests
pass. Test/comment correction **`1a52cf4a1`** models the two removed parent
reads precisely; its runtime negative control restores those calls and
fails. It changes no runtime behavior beyond comments, so live archives
remain exact `39965d653`; corrected-test GCC checking is separate.

Exact runtime source archive SHA256
`511a5de1308b4ef13097af6662e0a28d8b470434c475ad7f966c42357488a98d`.
After failed-run cold verification and checked native-process absence,
**nuc attempt-55** and **node9901 attempt-53** started the unchanged committed
qualification profiles. Both native builds and full fast gates pass; nuc
entered loaded round 1 and node9901 entered slow-member durability. No new
target/full PASS is claimed.

The corrected `1a52cf4a1` actual-function unit test also compiled with GCC
against the exact `39965d653` functions in a separate evidence-only tree:
normal and ASan/UBSan pass; the runtime control restoring the two extra
parent reads fails its assertion. Node9901's documented broken LSan runtime
is excluded there only; no native source or qualification limits were
changed by this unit run. Stable manifest:
`/data1/efs/evidence/taskH-repair-20261026/attempt-53/query-lookup-unit/manifest.json`,
SHA256 `34af9575f975beb3ae9b814a2220734b43e79f69d21883020759a467ffb17d28`.
It names the test-source hash and each closed output hash.

### Combined candidate: node9901 targeted PASS; full INCOMPLETE

**Node9901 attempt-52 targeted PASS**, exact source `3987609ff`, unchanged
private colocated RDMA/TCP profile below. Native build, GCC actual-function
checks, full fast gate, both slow-member durability variants, forwarded
prepare-abort and quota/reclaim gates all pass. Each slow-member variant
cold-read all 22 acknowledged files. All three RDMA K38 runs pass
(**14,400 records**), as do RDMA D4 progress and plateau: max foreground
write+fsync 0.169 / 2.249 s within 10 s, drain 4.2 / 46.6 s within 150 s.
K22 memory-pressure/handoff passes with 3,525 confirmed writes, zero errors.
No test limits or durability checks changed.

Whole-cluster orderly restarts and fresh-client readers verified
**36,971/36,971 recorded final payloads**, not samples: progress 32,390,
plateau 1,056, K22 memory 3,525; zero payload or cleanup errors. The actual
K22 log gate separately verifies 32 more cold-exact payloads: cap 1,024,
watermark 768, leader applied 13 / commit 781, pressure 0, admission
0→923, both stop sets `[0,0,0]`; max write+fsync 8.405 s within its original
30 s, not D4's 10 s. This is not nuc qualification,
a 30-minute plateau, a multi-host fabric gate, or power-loss evidence.

The full suite has now finished: **513 commands, zero FAIL, 14 NOTRUN,
741 s, 7 OVERBUDGET, BUILD 0 s; INCOMPLETE**. The 14 skipped commands and
reasons are identical to attempt-51; none is silently counted as PASS.
K33's five-node payload command ran and exited zero (40.1 s); this does not
prove the cause of its earlier EIO. An explicit process scan found no owned
efsd, FUSE or bench process after completion. The final
`qualification/result.json` SHA256 is
`5e9c1855017651c32e1d144a3348b589e2de4b3c3aeabfc7950ee43a5bd111cf`.
192 closed native text files are hash-checked in `closed-native/index.json`
under the same attempt-52 root, SHA256
`15d106b3c27f99d0567e31186260f49d7197ac9da058f0578b6c1b68386d0203`.

96 closed target log/exit files were hash-verified and copied, with all
three cold result hashes in
`/data1/efs/evidence/taskH-repair-20261026/attempt-52/closed-targets/manifest.json`,
SHA256 `74c622fa6035c74113858f986f80fc4fd05ad5eebaf2d287c393a34fe4122811`.
The original fixtures, logs, source archive and binaries remain on-host.
Nuc attempt-54 is still guardedly queued behind the earlier scheduling-only
candidate's full suite; its required live checks have not run.

### Inbox scheduling candidate: nuc targeted PASS; later full FAIL

**Nuc attempt-53 targeted PASS**, exact source `2636d5a60`; this does not
qualify the later ABORT-batching change. Same committed nuc profile below,
private colocated three-daemon fixtures, TCP, O_DIRECT, packing/queries on,
all original workloads, quotas and acceptance bounds. All three rounds of
four concurrent targets passed: K38 **14,400 records** exact across both
mounts/remount; both D4 workloads; K22 actual memory refusal/leadership
handoff and error checks. Plateau max foreground write+fsync was
**8.372 / 5.808 / 4.890 s**, all within unchanged 10 s, with drain
32.1 / 31.0 / 30.3 s within 150 s. The three K22 cases confirmed
579 / 428 / 633 writes, zero errors. These short loads do not establish a
30-minute space/RSS plateau or multi-host fabric behavior.

All nine retained D4/K22 fixtures then underwent orderly whole-cluster
restart and fresh-client exact readback: **7,580/7,580 recorded payloads**,
zero uncertain paths, payload errors or cleanup errors. The separate real
K22 log-watermark gate passed 32 additional exact cold payloads: cap 1,024,
watermark 768, leader applied 13 / commit 781, pressure 0, admission
0→1,067, both stop sets `[0,0,0]`. Its max write+fsync was 8.361 s within
that gate's unchanged 30 s, not a substituted D4 bound.

116 closed target log/exit files were hash-verified and copied on nuc;
each of the nine cold result hashes is recorded in
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-53/closed-targets/manifest.json`,
SHA256 `5dcddf23a6afdd6b7678e6e64bc50858be336329663953151195162062dc39dd`.
Original fixtures/logs remain on-host. The later full FAIL and blocked queue
are recorded above. Combined candidate `3987609ff` was queued as nuc attempt-54,
with a guard requiring the prior controller to finish, its native processes
to be absent, and its verdict to contain no new test failures before launch.
A new failure blocks that queue for preservation/investigation instead of
being skipped. Node9901 attempt-52 continues independently on `3987609ff`;
both slow-member durability variants and all three RDMA K38 runs passed,
with remaining D4/K22/cold/full stages pending.

### Publication abort cleanup: candidate under qualification

**`3987609ff`** adds one targeted change on top of inbox scheduling
`2636d5a60`: `host_pub_unit_abort` now uses the same bounded per-group
TXN_BATCH resolution helper as COMMIT. A typical nine-part W77 unit had
nine scalar ABORT RESOLVE proposals, despite COMMIT already batching those
participants by group. The attempt-52 trace encountered uncertain abort
cleanup before retrying the aborted identity. Reducing cleanup proposals
is justified independently of proving that it alone caused the tail.
ABORT is still decided and waited first; every participant's resolution
must apply successfully before the caller can claim cleanup/new-identity
retry. Any submission, persistence or verdict failure goes to the same
finisher and stays uncertain/error. Existing forwarded-prepare cleanup and
decide-once behavior are retained. No deadline, result oracle or durability
requirement changes.

The expanded actual-function `test_prepare_cohort.py` checks COMMIT and
ABORT batching across both groups and one group, capped splitting, the
unchanged single-command encoding, remote forwarding outside a local log
hold, conflicting decisions, failed submissions/fsync and unknown apply.
No failure can authorize retirement. Mac normal and ASan/UBSan tests pass;
the old scalar ABORT implementation fails the new proposal-count assertion
at runtime. Append PREPARE and original apply-wait tests also pass. These Mac checks alone are not native/live qualification; the node9901
scope is recorded above, and nuc qualification remains required.

Exact archive SHA256
`975356db3a08dd53d0d7342dfa5ec214592ae431e9d58c307a3a666e135e883c`.
Node9901 **attempt-52** runs the unchanged committed RDMA profile on this
source. Nuc attempt-53 remains on `2636d5a60`; its live run is not interrupted
or credited to the newer source. Failed stores are not deleted.

The preceding node9901 **attempt-51** (`bf3a4d7e9`) has now finished:
**513 commands, zero FAIL, 14 NOTRUN; full INCOMPLETE**. Targeted/cold
results below stand on their own and are not qualification of `3987609ff`.
189 closed native text files are hash-checked in
`/data1/efs/evidence/taskH-repair-20261026/attempt-51/closed-native/index.json`,
SHA256 `d23a327292605530d0a8c5aa7e81175ffedfe52cb5aa17c9c70a322083d6f9dd`.

### Current: second-round latency failure preserved; pump scheduling candidate

**Nuc attempt-52 FAIL**, exact source `bf3a4d7e9`, same committed
`qualify_publication_repair.py --profile nuc` and configuration below.
Three focused K33 runs passed (109/109/106 files), W166 passed both original
30 s close bounds, and the full fast gate passed. Loaded round 1 passed
all four concurrent targets: D4 progress max write+fsync 3.129 s, plateau
6.388 s, no errors or payload mismatches. Loaded round 2 passed K38,
progress and K22, but plateau failed **13.16 s > unchanged 10 s**. There
were 277 foreground operations, 1,024 working files plus 21 foreground
files, zero operation errors, zero bad payloads, and 31.7 s drain within
150 s. Round 3 and the full suite were NOTRUN. No pass from another round
or host supersedes this failure.

The failed foreground inode 6531's client trace records fsync preparation
12.878 s, final commit/proof wait zero. Coordinator traces show first
activation BUSY, then publication-unit BUSY with uncertain abort cleanup,
then a retry encountering commit-abort and rebuilding a new publication;
the final publication applied. Receipt resolution observed that outcome.
This supports retry/queue latency, not a proved device failure, payload
corruption, or an epoch-discovery defect. The FileID optimization did not
eliminate this tail.

Original closed logs/results and all three byte-hashed pre-restart forensic
store clones are preserved on nuc at
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-52/failed-plateau-preserved/`.
Manifest SHA256
`517cec1bf22e0ff9bd6ac6f747c9255d901a353b2fd61b107056171207f920af`.
Then `scripts/d4_verify_retained.py` restarted the original stores with
their exact original binaries, checked **1,045/1,045 recorded payloads**,
zero uncertain paths/errors/cleanup errors, and stopped cleanly. Its
`cold/result.json` SHA256 is
`cb7112c1022c0eebd6d4512aad41161b8cf00e8cad925d7896d242364e1339ed`.
It remains INCOMPLETE because the failed workload's manifest is not marked
complete; neither the workload FAIL nor that conservative verdict is
changed. The forensic clones remain pre-restart; no payload left nuc.
The separate 98-file `attempt-52/closed-native/index.json` SHA256 is
`9bf0b6c711535d2b431e5a00ef3d80cb59259901e2c14bbad8d271aeaff9437b`.

The earlier diagnostic-51 batch observations show pump drain maxima
1.51–1.87 s and inbox ages up to 3.62 s, while individual Raft sync maxima
were 0.168–0.186 s. This is scheduling evidence, not an off-CPU proof that
identifies every wait. The previous 32-frame inbox prefix could contain
many alternating-group receive batches before the pump returned to
proposal flush, requested read rounds, ticks and its KV-release boundary.
**Candidate `2636d5a60`** instead takes one contiguous group prefix, at
most the receive core's 16-message limit, per pump turn. The FIFO tail
stays admitted and forces an immediate next turn. Short/invalid headers
get an isolated turn; normal decoding and every term/identity/configuration
and persistence check still decide authority. Receive core boundaries can
still split a prefix; this does not promise one physical fsync. No deadline,
wait grant, quota, admission capacity, wire format or durability rule changes.
This is a scheduling fix under qualification, not a demonstrated resolution.

Actual-function tests cover alternating groups, full homogeneous bursts,
raced tail arrivals, exact byte accounting, short/invalid headers and
backlog wakeups. ASan/UBSan and the existing complete-batch handoff,
receive-persistence and unchanged 400 ms/2 s prepare-wait tests pass on the
Mac. The strengthened inbox test fails by assertion against the previous
production implementation. Native build, fast and loaded gates are still
required. Exact source archive SHA256
`6ddc8ce0f4c830f66c8db72dbbebdd3631254b1759673dfb6233988bb37bd795`;
nuc **attempt-53** uses it with the original qualification profile/limits.

**Node9901 attempt-51 targeted PASS**, source `bf3a4d7e9`, same private
colocated RDMA/TCP profile and dependencies below. All targeted exit
markers are zero: slow dirty/clean member durability, PUFA/quota, three
K38 RDMA runs (14,400 records), D4 RDMA, K22 memory and actual log watermark.
D4 progress max write+fsync 0.143 s, plateau 3.780 s; drains 4.1/34.8 s.
Cold restarts read all **37,048/37,048 recorded final payloads**, not
samples, with no errors or cleanup failures. Under
`/data1/efs/evidence/taskH-repair-20261026/attempt-51/qualification/`, the
cold result SHA256 values are:

- `cold-test_d4_progress/result.json` (32,209):
  `66415fccddc6149fb4c80b6a0d094fc1d160a96bb8c3326f9ae3556f08e26279`.
- `cold-test_d4_plateau/result.json` (1,056):
  `2c95296855c5106845aa7daf430d2d4971203c22c816138ee0e23d2bdfdfc03a`.
- `cold-k22-memory/result.json` (3,783):
  `d6838743630c23efd8b5fa70ec114bb05acbaa0680c600aa813ea45913fd88fd`.

K22's separate log gate cold-checks 32 further payloads; cap 1,024,
watermark 768, leader applied 13/commit 781, pressure 0, admission
0→1,116, both stop sets `[0,0,0]`, max write+fsync 9.294 s within that
gate's original 30 s (not D4's 10 s). Full suite is still RUNNING; no
full PASS or qualification of the newer inbox candidate is implied.

### Latest full-suite failures, baseline verification and next candidate

### Remaining strict latency failure and FileID-handoff candidate

**Nuc attempt-50 FAIL**, source `436d1698c`, first four-way loaded round.
K38 PASS (4,800 records), D4 progress PASS, K22 memory PASS (677 confirmed
writes, zero errors). D4 plateau failed one of 217 foreground operations:
**11.695 s > unchanged 10 s**, with open 3.458 s, write 0.003 s, fsync
8.212 s, close approximately zero. No operation errors or payload
mismatches were observed; drain was 23.2 s. Later rounds/full suite were
NOTRUN. Successful data readback never changes this latency verdict.

Before cold verification, all closed failed logs and byte-hashed forensic
clones of all three stores were retained under
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-50/failed-plateau-preserved/`.
Manifest SHA256:
`874c7a0f6cbb576bfe6176b5bb1ec79246f62821fa832ed9fcc8dc7263a77fe0`.
The original stores were then restarted with their original binaries using
`scripts/d4_verify_retained.py`; pre-restart clones remain untouched.
**1,041/1,041 recorded final payloads matched**, zero uncertain paths,
zero errors and no cleanup errors. Cold result remains **INCOMPLETE**:
the failed workload conservatively recorded `complete=false`, and the
verifier did not override it. Result `cold/result.json` under that root,
SHA256 `b9713dc37891c9f3666bb665dfba18f502ee0b310b4b2b86658185c2d02a25fd`.
No payloads were copied off-host or pruned.

The retained original trace places all 8.213 s of fsync in preparation,
with `proof_wait_ms=0` and final `commit_ms=0`. Inode 7740's REPORT took
2.082 s (finish 1.333 s); the remaining earlier flush work is therefore
material. Storage write samples in this window were milliseconds, but
that is not an off-CPU proof or a whole-host disk-health assessment.

`scripts/diagnose_publication_latency.py` (`74077baae`) reproduced the same
four original workloads concurrently with existing observational traces
and an explicit 150 s/99 Hz CPU-only capture of the plateau's three
private daemons and two FUSE clients. **DIAGNOSTIC ONLY**: all four commands
exited zero; plateau max 7.763 s, so the original 11.695 s breach did not
reproduce. A slow diagnostic fsync split into 4.512 s PUT plus 3.041 s
REPORT, with zero WB/append-lock wait. CPU capture had zero lost samples,
approximately 16.9 CPU-seconds for selected processes over 150 s; it does
not identify off-CPU waits or qualify host-wide CPU utilization. Stable
raw root `/home/maik/efs-artifacts/taskH-repair-20261027/diagnostic-51/`:
manifest SHA256
`16006edff193804865c542c20b1a3f50e0c6a473a1cd6fc01e77714a625c5da2`;
`perf.data` SHA256
`67d324f47f6ce97a908add98a09e48de5e3bde0005c3b437b453496c44e6e081`.
Original binaries/symbols and every fixture remain on nuc.

**New product candidate `bf3a4d7e9`**, archive SHA256
`96a3298e2f6d0a16428d8762afbf3682810e4be8a1749e880484d6aab3dc1e40`:
validated GETCHUNKS replies and known-successful guarded OPEN/CREATE-adoption
receipts seed the existing immutable FileID cache. Formerly chunk workers
could repeat a FileID discovery the descriptor had already proved. Unknown,
negative, malformed or late replies never seed it; ADMIT's current
FileID/session/epoch checks and every durability barrier remain. No wire,
store format, flag, capacity or timeout changed. This is a tested reduction
in redundant metadata work, **not yet a proved fix of the latency tail**.

Focused GCC tests execute the actual modified functions: eight chunk
workers reuse a primed FileID without a new RPC; invalid identities and
cache collisions behave correctly; only known-successful OPEN/adoption
seeds; negative/unknown CLOSE/OPEN receipts and invalid/late GETCHUNKS do
not. K32 batching/entry-count checks also pass. Runtime negative control
omitting the GETCHUNKS handoff fails, rather than merely failing to compile.
Raw preflight uses modified functions and the read-only `436d1698c` common
archive, so it is unit evidence, not a new native build:
`/home/maik/efs-artifacts/taskH-repair-20261027/fileid-preflight2/manifest.json`,
SHA256 `a3c8fc5d635b5a3cc2cab0314871bcf08d3997e8820b9484213a66be9e8a810c`;
negative log SHA256
`1761b611ab17074880720d056a10889834d3050e0965491bca73ec92f2474552`.
The Mac GETCHUNKS/lease tests pass; ticket/entry-count tests require the
absent Mac `libefs.a` and were run with GCC on nuc instead.

Nuc **attempt-52** and node9901 **attempt-51** now test exact `bf3a4d7e9`
with the same committed qualification driver and original limits. Nuc's
first focused K33 repetition has passed; loaded/cold/full stages remain
pending. Node9901 passed its native build and full fast gate and entered
slow-member durability. The preceding node9901 `633aafc12` full suite
finished **513 commands, zero FAIL, 14 NOTRUN, 708 s, 6 OVERBUDGET**;
INCOMPLETE, never a full PASS. Its 191 closed native text artifacts are
indexed at `attempt-50/closed-native/index.json` under the node9901 raw root,
SHA256 `730a20f4e259bb38ae52e1f5ab5a346c66609bf7a91a88462743e2b55cb17e9e`.


**Node9901 attempt-50 targeted PASS**, exact source `633aafc12`; full suite
running. Same private colocated profile/hardware/dependencies as attempt-47,
O_DIRECT, packing/queries enabled: slow dirty/clean-member durability gates,
PUFA and quota/reclaim on TCP; three K38 runs and both D4 gates explicitly
RDMA on `mlx5_0`; K22 memory/log gates on TCP. All 14 independent target
exit markers are zero. K38: **14,400 records** exact on both mounts and
remount. D4 progress: 389 foreground operations, max write+fsync 0.348 s,
drain 4.1 s, 20,373 files exact. Plateau: 781 foreground operations, max
write+fsync 0.288 s, drain 36.0 s, 1,056 files exact; all original 10 s /
150 s limits retained. K22 memory: 3,793 confirmed writes, zero client
errors, three-second drain. All three retained fixtures then restarted
cold and read **25,222/25,222 recorded payloads exactly**, not samples.
Independent actual log-watermark gate: cap 1,024, applied 12 / commit 780,
watermark 768, pressure 0, admission 0→940; **32 more cold-exact payloads**,
both three-daemon stop sets `[0,0,0]`, max write+fsync 8.819 s within that
gate's unchanged 30 s bound (not D4's 10 s bound).

Stable raw root:
`/data1/efs/evidence/taskH-repair-20261026/attempt-50/`.
`closed-targets/index.json`: 110 hash-checked copied closed files, SHA256
`d13b1bb3c13d833730eeb443a3ead432ca5588c6c078f7e68e3b740cce1f3d55`;
`qualification/independent-target-verdict.json`:
`5cfd9631032b1fc341429ac9160ae33932f125e02cc656257510542992e0488d`;
`qualification/cold-total.json`:
`91ce52ff37354d949d76c1046926141c4b09fdd7ea5325203abc07c2361b3e65`.
Binary hashes are in `qualification/binaries.sha256` and each cold result.
The full concurrent suite remains pending; nuc's `436d1698c` loaded target
rounds are pending. These short/private gates do not qualify multi-host
fabric behavior, 30-minute RSS/space plateaus or power loss. Historical
failures remain failures; the original K33 EIO's precise cause is OPEN.


Latest branch test/verification update: **`436d1698c`** (archive SHA256
`17c98308d0a9bd07c381811317b7c13c9a682922fd38fdaa6bfcc00dea4a893a`).
There is **no product-source diff under `src/` or `include/` from
`633aafc12`**. Nuc attempt-50 tests this update; node9901 attempt-50 continues
its independent exact-source `633aafc12` profile. Neither full result is
complete yet; do not combine them into a claimed same-commit full PASS.

Nuc attempt-49 (`633aafc12`) focused K33 PASS three times: 105, 104 and 105
files, zero retried writes. W166 then **FAILS in PUT setup**, not close:
REPORT close passed in 0.01022 s; its subsequent operator transfer returned
`rc=-13` (target catching up), and the old single-attempt setup asserted.
Original stores remain at
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-49/focused-fixtures/w166/efs-w166-5axhedoa/authority/`.
Nine closed failure files are indexed at
`attempt-49/closed-w166-failure/manifest.json` under that artifact root,
SHA256 `e27ea532e70c68afd11b548eb627d6d99d09937d995a4aab63b3e7a6663e1b8c`.
The correction uses the existing setup helper for documented BUSY within
the **original 20 s transfer budget**, beginning before transfer; REPORT's
90 s setup and both 30 s close bounds remain unchanged. W166 now records
setup/close/drain/verification stage and an explicit overall failure in
`result.json`, including unhandled setup exceptions. It never retries an
application outcome to manufacture a pass.

Before new native deployment, same-`633aafc12` binaries restarted a fresh
hashed clone of the failed W166 stores. **6,658/6,658 files exact; all 208 directory names matched**: all 6,656 prefill files empty, REPORT's expected 2,048 B
payload exact, initial PUT file empty. Full reconstructed manifest:
`attempt-49/w166-full-baseline-manifest.json`; result
`attempt-49/w166-baseline-full-clone/result.json`, SHA256
`5076e5dc6223cb2475aafe82c38aa2bc9f446d4f6e8ca47f2552b3d05472b41d`.
Original inventories unchanged; unmount/client/all three daemon exits
checked clean. The expected namespace uses 13 successful 512-file/
16-directory benchmark iterations plus their observed PID prefixes, not a
contemporaneous acknowledgement journal. An earlier two-file manifest
read both known payloads exactly but **failed completeness** on the 208
prefill directories; that result and clone remain at
`attempt-49/w166-baseline-clone/`, not relabelled PASS. PUT close was NOTRUN
in this failed qualification, and no baseline readback can qualify it.

Closed node9901 fast-gate failure SHA256s: attempt-48 log
`9a9c83492983e0f2046b09be3b151343f768f7b0c73d9fde0eca182b2fdf4a22`,
attempt-49 log
`8c4f031666f64645440d855acbceb2e5265ae40f44ed0350c5a7c8c39e4f637b`.
Corrected GCC/ASan/UBSan regression log SHA256:
`d799ad5c0eb5de0a2aae3f82574d705073b20932556cab9bfdd7c3d34bc01400`.


Candidate **`633aafc12`** now includes bounded span re-probe `e2e9030cd`,
retained-failure verification/W166 setup `e9994d908`, setup-helper regression
coverage `5dd27b218`, and a GCC-clean test stub. Its archive SHA256 is
`83bc4cf190fe5456d189f4cfa7e667f40cb9ebef51e91d8187b99ae64c9b7881`.
New nuc attempt-49 is queued strictly after attempt-48; node9901 attempt-50
is running. Neither is qualified yet. Each has a separate exact archive,
binaries, fixtures, controller and stable evidence under the same roots
below. No previous result is overwritten.

Additional **focused evidence only**, nuc attempt-47 (`e9994d908`): three
original K33 runs PASS (110 files each, including all small packed/large
payloads, 3→4→5 addition, cold restart and every single outage). W166 PASS:
REPORT 0.00845 s and PUT 0.01173 s, two exact fresh-reader payloads, original
30 s close bounds, full-L0/leader setup still asserted. These quiet runs do
not establish the cause or resolution of attempt-46's EIO under the
concurrent full-suite workload. Stable index:
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-47/focused-closed-index.json`,
114 files, SHA256
`3d71ce5f1be42ca0c3c107afc8d2fcd0fb395bad716ec0be96fbe01d89e0a1f6`.

Two new **fast-gate FAILS** are retained, not discarded as retries:
node9901 attempt-48 (`e9994d908`) caught the changed setup-helper timeout
failure path; attempt-49 (`5dd27b218`) caught GCC's misleading-indentation
error in the new span regression stub under `-Werror`. Their logs are
`/data1/efs/evidence/taskH-repair-20261026/attempt-{48,49}/qualification/fast-unit-gate.log`.
The helper now preserves existing no-deadline behavior and tests expired,
late-inspection, second-group/shared-budget and BUSY cases. The stub is
reformatted, not compiled with weaker warnings. Corrected span production-
function regression PASS under Clang and node9901 GCC/ASan/UBSan (host's
existing `detect_leaks=0` exclusion); sanitized raw output remains at
`attempt-49/delta-harness-fixed-gcc-sanitized.log`. Node9901's RDMA/live
stages were NOTRUN in these two attempts. Nuc attempt-48 remains in flight
and will finish before its queued successor starts. All native limits,
quotas and verification requirements remain unchanged.

Local six read-path regression commands/log hashes are recorded outside
Git in `/Users/mike/Documents/Codex/artifacts/taskH-repair-20261027/span-reprobe/manifest.json`,
SHA256 `58d3f7c7ba0ef43039fda4ae5138074b665fca95f6209dbf069a74f7b121d289`.
The tests execute committed production functions; these are unit results,
not native-cluster qualification.


Nuc attempt-46's original failed stores remain under
`/home/maik/efs-taskH-repair-20261027/attempt-46/src/tmp/`:
`case-435/efs-k33p-m1q8uxhk/p` and `case-475/efs-w166-2kowuvm1`.
Twelve closed failure logs/results are copied and hash-checked at
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-46/full-failures-closed/index.json`,
SHA256 `9143fe8a692513ef7ea11b877a631c1c283652e8e03dc40837f5e73d325dc917`.
No failed store or payload was copied off-host or removed.

Before changing binaries, `scripts/k33_verify_clone.py` restarted hashed
on-host reflink clones with the **original `5ecd9cefe` binaries**. Reproduce
on quiet nuc with `python3 k33_verify_clone.py MANIFEST NEW_OUTPUT`; the
retained manifest is `attempt-46/k33-baseline-manifest.json` under that
artifact root. It specifies original binary hashes, fixture/source paths
and all expected names/sizes. **70/70 full payloads exact**, no unexpected
or missing names, no read errors; original inventories unchanged; unmount,
client and all five daemon stops checked successful. Result:
`attempt-46/k33-baseline-clone/result.json`, SHA256
`f43fc1d6ee9eef407ef3a28da9de00a234bdf21fb3a79694875129ea68d9bcad`.
The expected set was reconstructed from the test, retained starts, completed
outages 1/2 and the `w-19` error, **not a contemporaneous acknowledgement
journal**. This proves exact retained readback of that set, not absence of
the original availability failure or a crash/power-loss guarantee. The
fresh mount performs read-only workload operations; it is not an enforced
read-only FUSE mount. Background server work touches clones only.

`w19-chunks.txt` in the same directory shows two base chunks on nodes
1/2/3 and 2/3/4, plus its tail span on 3/4/5. Therefore the newly committed
span re-probe fix (`e2e9030cd`) is a separately demonstrated recovery gap,
**not an established explanation of this K33 EIO**. Its actual-production-
function regression covers a recovered recorded member still marked down,
a genuine double outage, integrity failures, failed decode and an expired
budget. It preserves the existing deadline, recorded placements and exact
hash checks. K33 must rerun with trace evidence; no retry of the failed
application read is promoted to a PASS.

W166's correction re-establishes its controlled leader placement after
prefill **within the same original 90 s setup budget**, with L0 still full
and memtable headroom, then checks placement, terms and full L0 again after
REPORT. Its measured close bound remains 30 s; PUT fault, verification and
all failures stay. This corrects setup assumptions rather than removing a
pressure assertion. It is not yet live-qualified.

Node9901 attempt-47, the same source and unchanged private RDMA profile,
finished targeted PASS: three K38 runs (14,400 records), RDMA D4 progress
and plateau, TCP K22 memory and real log-watermark checks. Its three
recorded-payload cold fixtures verified **22,622/22,622 exact** plus 32
separate log-gate payloads. Full suite: **513 commands, zero FAIL,
14 NOTRUN; INCOMPLETE**, controller exit 3. Stable target index:
`/data1/efs/evidence/taskH-repair-20261026/attempt-47/closed-targets/index.json`,
SHA256 `7c62c45764385ee05591de8b16becaa24fea74e2720fe9e77b085bcb676f41a5`.
These are colocated private loopback daemons, not multi-host fabric or
30-minute RSS qualification. New candidate deployment/reruns are in progress as recorded above; main
node9901 and orcd-mini are untouched.


### Nuc attempt-46: targeted PASS; full suite FAIL

Source **`5ecd9cefe`**, private three-daemon loopback cluster on nuc
(8 CPUs, 32 GiB RAM, XFS/NVMe), TCP and O_DIRECT, packing and `.efs`
active. Native build, full fast gate, PUFA and the real quota/reclaim gate
passed. All three unchanged-limit loaded rounds passed K38, D4 progress,
D4 fixed-set plateau and K22 memory admission, transfer, drain and every
client/producer error check. K38 verified **14,400 records** in total on
both mounts and after remount (records, not 14,400 distinct files).

| Round | D4 case | Foreground operations | Max write+fsync | Drain | Per-stream index advances |
| --- | --- | ---: | ---: | ---: | ---: |
| 1 | Progress | 99 | 3.793 s | 5.0 s | 21 |
| 1 | Plateau | 323 | 6.284 s | 35.4 s | 20 |
| 2 | Progress | 113 | 2.872 s | 7.0 s | 20 |
| 2 | Plateau | 279 | 5.052 s | 28.8 s | 21 |
| 3 | Progress | 109 | 2.593 s | 6.9 s | 19 |
| 3 | Plateau | 317 | 7.664 s | 34.5 s | 20 |

All 1,240 foreground operations met their original 10 s bound; all six
cases had zero operation errors/payload mismatches and drained within
150 s. These short gates report RSS slopes; they do not qualify a
30-minute RSS/space plateau. K22's three real memory-refusal/handoff runs
confirmed 733, 706 and 663 writes, zero client errors, and 3 s recovery
drains. Nine **orderly cold restarts** then read every recorded D4/K22
payload: **8,196 / 8,196 exact**, not sampled (progress 2,949; plateau
3,145; K22 2,102). This is verification of those fresh fixtures, not a
claim to have cold-verified every historical failed store or power loss.

The independent actual **log-watermark** gate also PASS: cap 1,024,
watermark 768, leader applied 13 / commit 781, admission count 0 to 1,158,
KV-pressure flag 0. All 32 payloads were exact after cold restart; both
sets of three daemon stop codes were zero; max write+fsync 8.832 s. It did
not substitute a KV-memory refusal for Raft-log admission.

Full suite finished: **513 commands, 2 FAIL, 14 NOTRUN, 1,544 s**, 23 over budget. K33 payload readback returned EIO for `w-19` during the second single-member outage; W166 lost its required leader placement during prefill, before its REPORT close case. Neither is a passing result.
Current-source node9901 attempt-47 is separately running its RDMA profile;
predecessor results do not substitute for it. Failed historical fixtures,
logs and binaries remain untouched. Current fixtures stay kept while this
qualification remains open.

Stable nuc evidence: `attempt-46/qualification` beneath
`/home/maik/efs-artifacts/taskH-repair-20261027/`; private fixture path
`/home/maik/efs-taskH-repair-20261027/attempt-46/fixtures`.
The 147-file closed targeted text index SHA256 is
`6ef33e5562b2384de8c95b64d212a3941cae1630f0076cf6a799e3d1e3fd8c1e`.
Independent terminal target-exit/log-hash record (24 checks) SHA256:
`d68e5db91214e74d4a2fb0a9ed90c134f8cf93f6011feb7ff42d6925f4d3c43a`.
Cold-total record SHA256:
`e3209b9a65993ae119b5c3722de0a8877a37d97901f2faf6462a0a438483d588`;
loaded-metrics record SHA256:
`739ba9ab8769e8d98c3f971c1b4f96e685d3c457a2db5cbf4583beac6659934f`.
These are respectively `closed-targets/index.json`,
`qualification/independent-target-verdict.json`,
`qualification/independent-cold-totals.json` and
`qualification/independent-loaded-metrics.json`. Reproduction and exact
source/environment manifest are given below and in the run directory.

### Current candidate `5ecd9cefe`: acquire MKDIR's allocator before dependent PREPAREs

**IN PROGRESS, not qualified.** Fresh nuc attempt-46 runs this source;
node9901 attempt-47 is queued behind active attempt-46. Source archive
SHA256 `e5549bb38ab1f92988b00f9d84eff68c65ed64b3d8177246b09351632c0b7d8d`.
All original workloads, concurrency, quotas, retry/operation deadlines,
pressure, drain and exact-payload checks remain unchanged.

Nuc attempt-45 (`c88c0324c`) **FAIL** in loaded round two. The full first
round passed. Round two passed K38 and both D4 cases; K22 again proved
memory admission (3,163,179 / 4,194,304 bytes, `admit_busy=2024` at 198 s),
capacity handoff and 3 s drain, but metadata worker 2 round 1 exited 1.
The test correctly retained that client error as a failure. Round three,
cold reads and full gate were NOTRUN. No final qualification is claimed
from either first-round success or the successful pressure sub-checks.

The failing worker's initial MKDIR `bench-meta-3743757-2000001-d1`
(opid suffix `3b97636c/1/15`) exhausted 30 s plus the existing 8 s
same-identity resolution scope. Logs identify repeated **STALE allocator
PREPAREs**, not a malformed decoder: kind 7, key `0aaa01` (shard 2730),
while proposed inode numbers advance from 6,826 through 502,442. The same
worker then has 125 creates beneath the missing parent fail INVAL; these
are downstream failures, not independent inode corruption evidence. Its
unmount completed with session revocation rc 0. Original logs and stores
remain under nuc `attempt-45`; the 93-file closed text index SHA256 is
`24f78f61cd08cfbba3ebd6001fce5af70c77f813b4eaf3994eaccf44c8724615`.
Failed K22 cases do not write a successful `confirmed.json`; no complete
acknowledged-set cold verification is claimed for this failed fixture.

The transactional MKDIR captured the allocator, prepared dependent parent,
dentry and child rows, then submitted its allocator compare last. Continuous
ordinary creates on that shard could invalidate every delayed comparison.
`5ecd9cefe` submits that existing value-guard PREPARE first and waits for its
exact applied verdict using the same own-entry bound before preparing any
dependent row. Once applied, the existing per-shard intent makes ordinary
allocators BUSY until COMMIT/DROP; it does not publish the next watermark
early. All comparison, decision, receipt, abort and durability rules remain.
Failed or uncertain claim waits use the existing DROP path; later PREPARE
failure also releases the claim through that path. No new command, format,
mode, global lock or timeout is introduced. Other transactional operations
are unchanged; they were not the producer failure observed here.

`tests/mkdir_alloc_claim_gate.py` executes the actual MKDIR PREPARE/decision
block against a continuously advancing allocator. The former block fails
at runtime; the fixed block claims and verifies first, then commits once.
Refused claim, an ambiguous claim wait, and later dependent failure all
preserve the old watermark and go through abort cleanup without publishing
an inode. Normal and ASan/UBSan runs pass. It runs from the existing
`tests/test_alloc_value_guard.py` gate alongside the real value-CAS tests,
without adding a test command or changing its budget. Cohort ordering,
MKDIR committed-reply and recipe-coverage tests also pass locally. Full
native build/fast tests and strict live qualification run again before any
PASS can be claimed.

Focused Mac raw manifest:
`/Users/mike/Documents/Codex/artifacts/taskH-repair-20261027/mkdir-allocator-first/manifest.json`,
SHA256 `0a59c8a4f95aec5c2e710ca94c07741c9ae8a632b75767ae8f632a6e1ff638f5`.
Reproduction: `python3 tests/test_alloc_value_guard.py` after building
`libefs.a`; the standalone ordering model is
`python3 tests/mkdir_alloc_claim_gate.py`. The manifest records normal,
sanitizer and before-source commands plus adjacent regression commands.
Native current artifacts are
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-46` and
`/data1/efs/evidence/taskH-repair-20261026/attempt-47`; scratch/source roots
use matching attempts in the private trees documented below. Failed bulk
remains kept for this open investigation. A transient Mac network outage
interrupted monitoring only; both detached controllers retained their own
bounds and results. Access recovered without restarting any cluster.

### Superseded candidate `c88c0324c`: queued heartbeat contact is still liveness

**Superseded by `5ecd9cefe`, not qualified.** Nuc attempt-45 failed as
detailed above; node9901 attempt-46 is active. Its obsolete queued
measurement-only attempt-45 was skipped before native launch, with that
NOTRUN recorded in `superseded-queue45.json`; no active test was stopped. Source archive SHA256
`2e3cecee488eee544aa6773499cba28ff2102972663892fbe8ad78491474a05c`.
All prior fixes and the member/boot-aware D4 measurement remain; the
unchanged qualifier is rerun from its native build/fast prerequisites.

Nuc attempt-43 (`b36444b29`) **FAIL**, at K22 round one: node 1 lost both
groups to higher terms at 20:54:43 UTC before memory refusal, around
commit 5,899 in group 0. Independent status later recorded FOLLOWER,
`admit_busy=0`, `capacity_stepdowns=0`, memory 4,194,465 / 4,194,304 and
11,507 unapplied group-0 entries. The original 900 s condition expired
(maximum observed backlog 16,379); no later round, cold read or full gate
ran. Round-one K38 passed all 4,800 records on both mounts/remount;
progress passed 881 exact files, 19 index advances and 16.4 s drain;
plateau passed 1,052 exact files, 23 index advances, 35.7 s drain and
maximum write+fsync 3.831 s. The corrected measurement did not erase the
separate K22 failure. Its 100-file closed text index SHA256 is
`580bf564647eb9ccbe6de6d58e2aea982996bb1781f5c5977aab4b290a480af5`.

A fresh diagnostic attempt-44 used exactly attempt-43's built binaries
without modifying them. All four cases passed, including K22's real
memory refusal at 205 s, handoff, 3 s drain and 473 writes without error;
no recognized foreign EFS workload overlapped. This is **DIAGNOSTIC_ONLY**,
not qualification and not an erased failure. Existing RPC, Raft/pump,
read/write, batching and query-stage traces were enabled by the separately
hashed runner `924035f2b`; the runner adds no new product behavior or bounds.
A long catalog refresh coincides with the formal run's election, but that
is not a proven causal explanation. A header-only scan of the completed
attempt-40 log also rejected the proposed 1 MiB preallocation-boundary
explanation: the election's nearby entries lie around 1.5 MiB, not on a
reservation boundary. No speculative allocation change was made.

The actual-core test separately reproduces a liveness defect in
`efs_raft_read_control`: an ordinary heartbeat from the already-known,
current-term/boot configured leader returns to ordered receive when it
advertises pending commit/apply work, without resetting the election
timer. A locally caught-up follower can consequently lose its known leader
and start pre-voting despite continuously inspected leader traffic. The
old core fails this runtime assertion in both ACK-window configurations.
`c88c0324c` resets only that contact timer before the prefix/work decision,
matching ordinary AppendEntries' existing reset-before-prefix semantics.
It retains the frame for ordered receive, sends no new acknowledgement,
and grants no commit, apply or read proof. Frozen receive, pending
configuration, role, member, boot and term checks stay mandatory. Wrong
boot, wrong term and a different configured sender do not suppress a real
timeout; normal valid receive restores the known leader afterward.
Election durations, pressure watermarks, durability and all test limits
are unchanged. This proven mechanism is not yet proof that it accounts
for every native failure: fresh qualification is still required.

The entire `tests/test_raft.c` suite passes normally and with
`-fsanitize=address,undefined`, in both ACK-window configurations. The
before-core control fails at the new queued-contact assertion. Actual
receive-persistence and host inbox tests pass. Reproduction:
`cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude -Isrc/raft tests/test_raft.c src/raft/raft.c src/raft/raft_mem.c src/common/batch_obs.c -pthread -o /tmp/test-raft`
then `/tmp/test-raft`, with the sanitizer flags for the second build;
`python3 tests/test_receive_persist.py` and
`python3 tests/test_host_inbox.py`. Mac focused raw manifest:
`/Users/mike/Documents/Codex/artifacts/taskH-repair-20261027/queued-heartbeat-contact/manifest.json`,
SHA256 `7d20ca2bb7b4fa82c8d1ab57eeda3762c733eaee15f244243d13767e5cf5c4ed`.

Nuc artifacts remain under
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-{43,44,45}`;
private stores/sources remain under the corresponding
`/home/maik/efs-taskH-repair-20261027/attempt-*` paths (diagnostic 44 uses
43's binaries). Attempt-44's 41-file closed text index SHA256 is
`733b7f9172d0c866be3e2bc94f7fc4cbeac35434f02bf5e4e15ad8fa7b1d632a`.
All failed evidence and stores remain. Current node9901 artifacts/scratch
use `/data1/efs/evidence/taskH-repair-20261026/attempt-46` and
`/tmp/efs-taskH-repair-20261026/attempt-46` after the preceding runs exit.
Main node9901 and orcd-mini were not changed.

Preceding node9901 attempt-43 (`602cccc0a`) completed its targeted gates
and cold-verified **36,283 / 36,283** files: 31,593 progress, 1,056 plateau
and 3,634 K22. Full suite: 513 commands, zero failures, 14 NOTRUN,
771 s, six over-budget observations, **INCOMPLETE**; total 1,657.6 s.
Its 190-file closed index SHA256 is
`8585c8778af4133b07998c83b3197a52692be347e61eb66632670ae388c71032`.
These scoped results do not qualify the newer candidate or erase nuc's
failure. Node9901 attempt-44 (`c4d2fba18`) completed; current attempt-46
(`c88c0324c`) followed it directly. Queued attempt-45 (`b36444b29`) was
superseded before launch, not counted as a run or a pass.

### Candidate `b36444b29`: identify catalog counters before measuring refresh

**Superseded by `c88c0324c`, not qualified.** Nuc attempt-43 failed as
detailed above; node9901 attempt-45 follows the existing attempt-44. No active run
is interrupted and no failed fixture is reused. Source archive SHA256
`5904dd6e22b3f57137fae2b96bfda75fe9e681bdb196b67eca59e2e805d97e26`.
All workload, concurrency, quota, latency, drain and payload checks remain.
The server adds the serving `node_id` to snapshot JSON. D4 retains it with
`boot_id` and measures generation advances within each node/boot, never by
subtracting unrelated members' counters. Switching static catalogs or
restarting alone supplies no progress; a same-stream regression fails.
Missing identity is an error, with no legacy fallback. No scheduling,
refresh interval, product mode or admission rule changes in this commit.

Nuc attempt-42 (`c4d2fba18`) remains a retained **FAIL**. Round one K38
passed all 4,800 exact records, progress passed, and K22 passed its actual
3/4-memory refusal at 197 s (3,163,716 / 4,194,304 bytes; `admit_busy=1081`),
capacity step-down to node 3, 3 s drain and 458 writes with zero client
errors. The earlier pre-pressure leadership-loss failure did not recur in
this run. Plateau alone failed: its old index calculation subtracted a
final generation 29 from initial 30 despite intermediate observations
31, 33, 36, 39, 45, 47, 50 and 53. Source confirms generations belong to
each daemon's `query_view`, while requests may route among members; the
old samples omitted the member/boot identity needed to compare them.
This is not evidence of no publication. It also cannot retroactively give
that run the corrected gate: later rounds, cold reads and full stages
were NOTRUN, and a fresh run is required. Other plateau checks passed:
1,024 fixed-set files, 23 foreground files, 285 foreground operations,
maximum write+fsync 4.683 s, index pending age 8,628 ms, 34.1 s drain,
zero operation errors and zero payload mismatches. These figures are not
an all-host RSS plateau or final qualification claim. Isolation recorded
136 scans and no recognized foreign EFS process.

Raw nuc paths are `/home/maik/efs-artifacts/taskH-repair-20261027/attempt-42`
and `/home/maik/efs-taskH-repair-20261027/attempt-42/fixtures`; failed stores,
binaries and original results remain untouched. Its 53-file closed text
index SHA256 is
`697bf490dec7221799a279770b753ccd9ff9e839302fe47c8f79f6e9ef0f6926`.
The current run uses the same roots with `attempt-43`.
Node9901 uses `/data1/efs/evidence/taskH-repair-20261026/attempt-45`
and private `/tmp/efs-taskH-repair-20261026/attempt-45`. Exact launch source,
archive hash, environment and binary hashes are recorded in each run.

Focused tests execute the actual snapshot-header serializer, D4 verdict,
query-page admission and root-refusal paths. The cross-member subtraction
mutant fails the new runtime assertion. Cases reject frozen streams,
restart-only observations and same-stream regression, and accept an
advancing stream even when the final member's counter is numerically
lower. Commands: `python3 tests/test_d4_driver.py`,
`python3 tests/test_query_stats_snapshot.py`,
`python3 tests/test_query_page_barriers.py`, and
`python3 tests/test_query_root_refusals.py`. All pass on the Mac; native
build/full fast prerequisites run again before live qualification.
Focused raw manifest:
`/Users/mike/Documents/Codex/artifacts/taskH-repair-20261027/index-streams/manifest.json`,
SHA256 `9f8ea692755e6fbd9fcfd19ffe4c75834e7aaae17879a5b69b355d2bca25eb91`.

### Latest candidate `c4d2fba18`: wall-clock heartbeat for a delayed pump

**Superseded by `b36444b29`; not qualified.** Nuc attempt-42 failed as
detailed above; node9901 attempt-44 remains queued after attempt-43. Source archive SHA256
`6bf4cfbfae17d304a3a5fba6efc944d3ad24393eeeac0ea1753504870369f3cb`.
The strict workload, quotas, concurrency, 10 s foreground, 150 s drain,
K22 pressure/transfer bounds and exact-payload checks are unchanged.

Nuc attempt-40 (`602cccc0a`) **FAIL**, solely at K22's required pressure
proof in loaded round one. K38 passed 4,800 exact records, progress passed,
and plateau passed 1,038 exact files, 265 foreground operations (maximum
write+fsync 7.028 s), 16 refreshed index generations and 36.4 s drain.
But node 1 lost group-0 leadership to a higher term at 20:09:17 UTC,
before pressure. The separately retained `independent-k22-status.json`
then shows it FOLLOWER with `admit_busy=0`, `capacity_stepdowns=0`, memory
4,194,703 / 4,194,304 bytes and unapplied backlog 16,384. A full follower
is not the required proof that a pressured leader refused work and stepped
down. The original 900 s condition check failed; later rounds, cold D4/K22
and full stages were NOTRUN. Its closed text index SHA256 is
`d06b452e25c801efc81f2bfe41ec6b601bae468139c3559b80b9a140998770bc`.

The pump advances one logical 5 ms tick per cycle, dropping missed quanta.
That safely delays local elections but also stretches its leader heartbeat
relative to faster followers. With 200 ms cycles its nominal 50 ms
heartbeat can become 2 s, beyond a healthy follower's 500+ ms election
window. The actual Raft-core model reproduces this scheduling asymmetry.
`c4d2fba18` sends one additional wall-clock empty acknowledged-prefix probe
when a pump cycle loses an extra tick and the existing 50 ms interval is
due. On-time cycles retain normal heartbeats. Missed sends coalesce; no
frame storm, historical election catch-up, new mode or configuration knob.
Ordinary election/pressure tick ordering and all durations stay unchanged.
The probe uses the existing receive/term/boot/prefix validation and frozen
receive/storage-failure refusal. It neither retransmits the outstanding
data window nor supplies new replication/durability authority. Logical
heartbeat/data/snapshot retry aging is not reset by this extra probe.

Executed actual-core tests, in both ACK-window configurations, show:
coarse healthy leader stable; the old cadence loses leadership (negative
control); stopping heartbeats still causes ordinary failover; unanswered
probes earn no commit, duplicate no data and do not postpone normal data
retry. The entire Raft core suite passes normally and with ASan/UBSan.
A no-send mutant fails the healthy-leader assertion. The actual host clock
adapter is tested against event storms, fractional intervals, on-time
cycles and a very large pause. Native/live results remain pending; this
clock defect is proven, but it is not yet proof that every K22/D4 failure
is eliminated. Focused Mac raw manifest:
`/Users/mike/Documents/Codex/artifacts/taskH-repair-20261027/wall-heartbeat/manifest.json`,
SHA256 `531afca955afbda5b170457a21fa0d9948cf4b1d56d2ddf59b789e9058595e13`.

**Separate shutdown gate PASS, scoped to the new worker lifecycle.** Test
`fa69fc679` ran against unchanged `602cccc0a` native binaries after
attempt-40 ended, before the diagnostic. The parked cluster-worker phase
forced rc 3 at 6,000 ms (stop wall 6,056 ms), with all durability barriers
NOT_REACHED and stores preserved. The ordinary case returned rc 0 in
1,878 ms; existing parked packer/connection cases also passed. The
forced unformed-store restart correctly returned barrier-failed rc 4,
not a recovered/clean claim. Source/binary/command manifest and all raw
fixtures are retained at nuc `.../cluster-stop-602cccc0a/` and scratch
`.../cluster-stop-602cccc0a-fixtures/`. Manifest SHA256
`f2217446cd9545de7fff53ed75a26dfb4d0b102f6c02eb235ef69190783f112d`;
run log `0fb320ef1d9a91a2862cc7b58969424d35f579b73dba64f62e29b0dca3b187a8`;
closed text/exit-record index
`1d142393df78b39d6fc1884c0647eb18ad5f8cbb1f06cdc1cdbb00bfac50a3e1`.
This does not erase the heap abort in attempt-39.

**Diagnostic attempt-41 is not qualification.** Runner `816894354`
adds existing pump/Raft/read/write/batch timing traces to the same original
four workloads, using `602cccc0a` binaries read-only from attempt-40 and
fresh stores. K38/progress passed; plateau failed an overwrite close EIO;
K22 reached real memory admission and capacity step-down, then failed on
metadata producer 0 round 2 exit 1. Binary hashes before/after matched;
no foreign EFS was found. The K22 trace includes 351 ms pump holds and
98 pending transaction finishers; it supports measuring the scheduling
problem but is not a passing gate. Diagnostic manifest SHA256
`0fa3af28d82f02ffcecbd786c3320e9e56117142b4e34dddf5d295019359c324`,
result `bba439bd3978dab9e4cf4c04755dc56f6732854727f1d0b909295d68e649ffea`,
closed text index
`4648bb994cef30fb32f401cb1072f0cbd7269fa934b89d9a2200e7f1adb35ef5`.
All are at nuc's matching attempt-41 artifact directory; failed stores stay.

Node9901 attempt-42 (`5db04aab3`) finished scoped targets and **36,648**
full-manifest cold reads (31,020 progress + 1,056 plateau + 4,572 K22),
zero errors. Full **INCOMPLETE**: 513 commands, zero failures, 14 NOTRUN,
581 s, two over-budget reports, exit 3. Its closed text index SHA256 is
`aafae7ace68f8804f5a5f9643a364ce8c23b2f4b747783b6df233ba8f51b0f67`.
The current candidate still owes all fresh qualification stages. No merge,
push, release-wide verification, main-node9901 or orcd-mini change.

### Preceding candidate `602cccc0a`: join cluster workers before teardown

**IN PROGRESS, not qualified.** Fresh nuc attempt-40 is building/running;
node9901 attempt-43 waits for attempt-42 to finish. Source archive SHA256
`0bf52a02e5136e005ce4f3423a987abedbbb5af0aacb031abc73fefb43df2abe`.
All original workload/deadline/cleanup assertions stay unchanged.

Nuc attempt-39 (`5db04aab3`) stopped **FAIL at quota-reclaim**, before
loaded targets. The actual quota wait recovered in 1.319 s; every retained
payload was verified before and after a cold restart. However node 2's
second stop aborted with `double free or corruption (fasttop)`/SIGABRT.
Its log says `shutdown: outcome=orderly rc=0`, then the detached rejoin
worker continues HELLO attempts and reports peers unavailable, followed by
the allocator abort. The process exit wins over its premature orderly
record: **not a clean stop and not a passing prerequisite**. Loaded/cold
D4/K22/full stages in this attempt were NOTRUN. Raw logs/binaries/stores,
including the failed node, remain in attempt-39. Closed text index SHA256
`926923a2826eaf7e1d118c63de9e0a722cac8c924a9ff20a421de212cd6e83e5`.

The code has a concrete lifetime gap: detached rejoin and heartbeat users
were never joined before peer-pool connections, the Raft host and main's
stack-owned server/export state were torn down. `server_join_cluster`
uses that peer pool. `602cccc0a` makes these two workers joinable, records
successful starts, starts them after running/owner initialization and
joins both in a named `cluster-workers` phase before any shared teardown.
It rechecks stop after sleeps and before each additional rejoin peer, so
stopping admits no new background round. In-flight work is not cancelled.
An unexpected join error leaves teardown blocked for the existing global
stop deadline, preserving state rather than claiming quiescence. The total
stop/report bounds and forced/barrier-failed semantics are not extended.
This repairs the proven unsafe lifetime; native reruns must still confirm
whether it eliminates this observed heap crash.

Nuc attempt-40's native build, full fast-unit subset, PUFA and quota-reclaim
prerequisites passed. The quota test read all 29 retained expected payloads
after cold restart and checked all daemon stops, unlike attempt-39. Loaded
round one has K38 and progress passes; plateau/K22 and subsequent gates
remain pending at this update. This single cold-stop pass does not erase
the original allocator abort or finish the qualification.

The actual worker start/join functions pass a deterministic C test with
both workers held, release of one while the other remains held, failed
starts, duplicate starts and repeat stop. A no-join mutant fails at runtime.
The shutdown phase ordering and unchanged total bound are checked by the
existing test. Focused Mac evidence:
`/Users/mike/Documents/Codex/artifacts/taskH-repair-20261027/cluster-worker-lifetime/manifest.json`,
SHA256 `78c4ebdd04940fdba94240102060a50a7be079d857c2272cca6b80eb5617fb7d`.
This is not native/live qualification. Test-only `fa69fc679` adds a
fourth parallel forced-stop case to `tests/test_shutdown_stuck_worker.py`:
park `cluster-workers`, require forced rc 3 at the existing 6,000 ms test
deadline, preserve stores and require all durability barriers NOT_REACHED.
The 2,500 ms process slack and existing 10 s suite budget are unchanged.
`SHUTDOWN_FIXTURE_SOURCE` can select the exact built source for this test;
`EFS_SHUTDOWN_KEEP=1` retains its raw fixtures. This added live case has not
yet run; it will follow the current controlled qualification without
interfering with it. Earlier product fixes remain in the
candidate; the remaining D4 and MKDIR failures are still OPEN.

Node9901 attempt-41 (`fcc44fe5c`) finished scoped targets/cold stages PASS,
full **INCOMPLETE** with 513 commands, zero failures and 14 NOTRUN. Its
closed text index SHA256 is
`fdd294f869065588adb410a7e55e887cc94ed0c5f35261f8afaafc80320c51f8`.
Attempt-42 uses `5db04aab3` and remains running; it is not interrupted to
replace the binaries. Main node9901 and orcd-mini remain untouched.

### Preceding candidate `5db04aab3`: cleanup reserve and paired REPORT proofs

**IN PROGRESS, not qualified.** Nuc attempt-39 is a fresh build/run;
node9901 attempt-42 is queued behind attempt-41's full suite. Source
archive SHA256 `43c5daed2b5f7d9cb68713bf5946f031b55c3e2d78e5dd524d5ddefe85c5e72b`.
No test/workload/concurrency/quota/deadline/oracle changes, no merge/push.

Nuc attempt-38 (`fcc44fe5c`) **FAIL** at round one with no recognized
foreign workloads (110 scans). K38 passed 4,800 exact records on both
mounts and remount. Progress passed 1,006 exact files, 91 foreground
operations, maximum write+fsync 3.179 s and 5.2 s drain. Plateau failed
an 11.46 s foreground operation (10.819 s fsync) and no index refresh
during load; 26.5 s drain and payload checks had zero recorded errors.
Its close waited through the 8 s REPORT scope, then resolved APPLIED
from the same identity's receipt after another 1.551 s. K22 reached
3/4 memory refusal in 149 s, moved group-0 leadership and drained in
3 s, but failed on a producer and an unclean unmount. Two initial MKDIRs
exhausted their unchanged 30 s deadline; the subsequent 250 invalid
creates were under the resulting absent parent IDs, not proof of a new
invalid-request decoder defect. Another producer's clean-exit log reports
session BEGIN immediately BUSY (`ms=0`, `phase=begin`). Later qualification
rounds/cold/full were NOTRUN. Closed text index SHA256
`55c926eaaf61aca46df26792f43b2f77a2903a9e99f7177092dea346774ebc45`.
Failed stores/binaries/logs remain in nuc attempt-38; nothing is retried
against or overwrites that fixture.

Two separately committed changes follow these observations:

- `1dc4ffb45`: exact-length session BEGIN is existing-session cleanup and
  can use D30's already reserved last quarter, like transaction resolution.
  It stops session growth before reclaim; it does not create a new session.
  The 3/4 threshold for new work, hard Raft/apply bounds, persistence,
  apply verdicts and the coordinator's no-ambiguous-resend rule remain.
  The actual production admission predicate is exercised at memory/log
  pressure; new CREATE/session creation/registration and malformed-length
  BEGIN stay refused. The old predicate fails the same BEGIN assertion.
  This addresses the immediate unmount refusal, not a claim that the two
  timed-out MKDIRs are fixed.
- `5db04aab3`: REPORT's two independent group proofs use the existing
  paired ReadIndex implementation, already used by namespace operations.
  Both groups still require fresh request-local term/configuration/apply
  authority; neither a follower proof nor an earlier round is reused.
  Single-group behavior/refusals and all wait scopes remain. Dispatch tests
  cover both group orders and refusals; the real paired-reader tests cover
  admission before probes, stale/late proofs, role/configuration changes,
  saturation, inherited deadlines and helper lifetime. A serial-dispatch
  mutant fails the runtime check. Also corrects diagnostic attribution:
  the old tail overwrote `read-index/admit` failures with `act`. New logs
  preserve `read-index` or `session-admit` before real activation failures.
  Existing `act` logs therefore **cannot identify activation alone** as the
  source of a stall. This overlap is a bounded latency improvement, not
  proof that the remaining D4 tail or index-refresh failure is solved.

Focused raw source/logs/manifests stay outside Git at Mac artifact root
`/Users/mike/Documents/Codex/artifacts/taskH-repair-20261027/`:
`session-begin-reserve/manifest.json` SHA256
`22d5fd541f12caa038e2129ed487da0932b905c58a0ec0e7e24a437d36dddece`;
`report-read-overlap/manifest.json` SHA256
`c9f8ca626d06b45efcb304845dde11f83686f8da89e9bbae817d3f025ed2bad0`.
These are extracted-function Mac tests, not native/live qualification.
The new runs require native build/full fast prerequisites before live load.
Node attempt-41 passed its scoped targets/cold stages and is in the full
suite at this update. All raw retention and remaining scope limits below
continue to apply.

### Preceding candidate `fcc44fe5c`: coordinator-preserving REPORT retries

**Historical candidate; loaded nuc qualification remains OPEN.** It ran
in fresh nuc attempt-38 and node9901 attempt-41 after their prior
controllers finished. No running fixture was interrupted or reused. Source
archive SHA256 `a2b41d9011eb5a17b2aac6bc7a20a5c5aa40c3ea3baea3e205438c8d61d55c60`.
No merge/push, relaxed limits or weakened acknowledgement/receipt proof.

The preceding candidate `f617577dc` includes `45102379f`: a multi-record
REPORT for one inode follows the same covered primary routing as a
one-record REPORT. Previously `count == 1` excluded a 1 MiB/eight-record
publication. Mixed-inode reports and absent/self/uncovered leaders retain
the normal path. Its regression executes the production routing helper;
the previous one-record-only predicate fails the eight-record assertion.
`f617577dc` also adds a read-only foreign-EFS guard to the nuc qualifier:
it checks before build/load and every two seconds during controlled live
stages, records interference as failure and never signals foreign work.
This detects recognized EFS processes, not all possible host I/O.

**Nuc attempt-36 (`f617577dc`) FAIL:** K38 (4,800 exact records), progress
(800 final files, 113 foreground operations, maximum write+fsync 2.895 s,
5.7 s drain) and strict K22 (446 writes, zero errors, 3 s drain) passed.
Plateau failed one 13.08 s foreground operation, including 11.908 s fsync;
no operation EIO was observed in that run. The isolation guard made 120
scans with no foreign findings. Later rounds, cold and full stages were
NOTRUN. This remains a failure even though other runs pass.

**Diagnostic nuc attempt-37**, using the same read-only `f617577dc`
binaries from attempt-36 and fresh stores, enables the existing timing
trace for one unchanged concurrent round. It never qualifies a gate.
A progress close for inode 76084 took 10.427 s: PUT 1.827 s, REPORT 8.600 s,
zero append-lock/writeback wait. For publication `18e279470a2fc545:36`,
node 2 repeatedly returned BUSY while node 1 returned immediate
NOT_PRIMARY between attempts. The generic client loop deliberately skipped
the BUSY responder. This demonstrates unnecessary coordinator switching;
it does **not** prove it explains every prior tail or will meet 10 s.
The diagnostic finished with progress FAIL and K22 FAIL (no 3/4-memory
admission refusal within the unchanged 900 s condition bound, maximum
backlog 16,382); K38 and plateau passed. There were no foreign-process
findings, and all five binary hashes remained identical. It is diagnostic
evidence only; logging overhead is not proven to explain the K22 outcome.
Prior failures stay.
Reproduce with committed `scripts/profile_publication_tail.py`, exact
completed-source `--source`, fresh `--root`/`--output`, and the source build
ID in the environment. Binary hashes are checked before/after; the runner
requires idle native EFS at entry and monitors foreign processes.

`fcc44fe5c` retains the live REPORT coordinator on BUSY and preserves the
exact packet. A failed selected connection clears that preference; explicit
NOT_PRIMARY redirects still work. Other RPC BUSY routing, the 16-attempt
cap, backoff, inherited deadlines and receipt resolution stay unchanged.
An executed production-loop test covers BUSY retention, send/receive loss,
leader redirects, unaffected non-REPORT routing, permanent BUSY and short
deadlines. The pre-change function fails the same owner-retention assertion.
W155 receipt/lost-reply and identified retry tests, K38 bounds/republication
and routing tests pass locally. These extracted Mac tests do not qualify
Linux/FUSE. Raw focused logs/source/manifest:
`/Users/mike/Documents/Codex/artifacts/taskH-repair-20261027/report-busy-owner/`,
manifest SHA256 `cbbb84e343f81cf9d2ee757e70d409df43e5770e77907b29ffaec655535cf91c`.
Earlier routing/guard tests are in `.../report-primary-f617577dc/`, manifest
SHA256 `226492020495961ad91da0b0fe7c6af34d440ddf53cb389eb39c476ec884e53c`.
Nuc attempt-36 closed text index SHA256 is
`e91e7bc3cbd7a6e7480e0b92a92fbddb575d3c97957031aaa60200316e8a98a3`;
attempt-37 closed index is
`bf070f50f4f54c0acb7f7a0979ee140d83358a17c2e5225f9df7adc9938e757a`.
The diagnostic's `manifest.json` SHA256 is
`db55c702d4b68026078302e121da7c3373abd89c0f4a212d438d683d4c98a61b`,
`result.json` is
`3dadd0f8cb66061c6b2911fc3feb363de87cc2340d0df959bb1f638d2c61e855`.
All these files are in the matching nuc artifact attempt directories.

Node9901 attempt-40 (`f617577dc`) passed its targeted stages and **38,739**
full-manifest cold payload reads (33,210 progress + 1,056 plateau + 4,473
K22; zero errors). Its full suite is **INCOMPLETE**: 513 commands, zero failures, 14 NOTRUN,
673 s, two over-budget reports, exit 3. Its closed index SHA256 is
`4957d719a6af61e70ba34da289576a10851ff24a0df0574f7ff1c77b2b85cf44`.
Predecessor attempt-39
(`0d0552d30`) passed scoped targets/cold reads, but full **INCOMPLETE**:
513 commands, zero failures, 14 NOTRUN, 693 s, two over-budget reports,
exit 3. The missing environment cases are not PASS. Its closed text index
SHA256 is `3f1b8425445d8464dac30907b5be9f693161b7e4d4a277c069c551bbe9fe5f70`.

**Interference preserved separately:** nuc attempt-35 (`0d0552d30`)
failed all four round-one targets: progress 14.14 s, plateau 19.76 s plus
rewrite close EIO, K38 pwrite EAGAIN followed by close EIO, K22 three
metadata producers exit 1. A Task I retry began at 18:36:11 UTC during this
run. It invalidates quiet-host qualification but does not erase failures
or prove causation; attempt-33 predates that retry. The user's authorization
to stop that campaign was applied to this retry: controllers were frozen,
the exact parked test fault marker was saved/released, all four client
stops returned zero and mount absence was checked, then ten private
daemons received orderly SIGTERM and exited. Stores/logs/binaries remain;
there was no native SIGKILL or force-discard. Some parent exit statuses are
unavailable, so this is STOPPED_PRESERVED, not a durability/test PASS.
Cleanup evidence: nuc `.../taskI-interference-2/resume1/`; 57-file index
SHA256 `9b96ee3549e778764c6cc286d1ac79104e051d61bb60a989e0caf99ea7f1c8bb`.
Attempt-35 closed text index SHA256
`7ee87bfb2d45e85b66ae5f24cfe59278c2e9aecca4e0a4c3c5bf59f9a55217bb`.

Raw hosts/roots, configuration, exact reproduction and unqualified scope
remain as below. Failed stores are retained for this open investigation;
main node9901 and orcd-mini remain untouched.

### Earlier candidate `0d0552d30`: first-use bootstrap cohorts

**IN PROGRESS, not qualified.** Nuc attempt-35 and node9901 attempt-39
are fresh builds/runs of this source. Archive SHA256
`6373515b2f12f686cb31a9d5ace5cf55760ed6c7a6be0195b0838c4661c1b10a`.
All original workload, concurrency, quota, deadline and exact-payload oracles
remain. No merge or push.

Nuc attempt-33 (`6412320f6`) **FAIL** in loaded round one: K38, progress
(806 final files, worst write+fsync 6.947 s) and strict K22 (460 writes)
passed, but plateau had a 12.67 s foreground operation and one truncate
close/flush EIO. The foreground trace attributes 12.193 s to publication:
a PREPARE/abort/retry followed by a same-identity receipt resolving APPLIED,
not a slow file barrier. The truncate's four-record publication exhausted
its unchanged 8 s retry budget after a certified admission abort and a
conditional mapping pull. Those failures remain failures; later rounds,
cold verification and full suite were NOTRUN. Its closed index SHA256 is
`d22ef010760e641df96a490dea6f9262b5d1814c49c1608ffba0cf69bc003425`.

Two subsequent product changes:

- `becc5729c`: GC drains pending PUT namespace ownership **before** removing
  empty ancestors. Whole-inode deletion already has a death fence; obsolete
  fragment pruning holds the inode's write guard across drain/prune. Current
  pins are flushed without declaring an unclean previous boot recovered.
  Native production tests cover both GC paths; old `6412320f6` source fails
  the same runtime assertion with a queued pin still owned after pruning.
  The failed synthetic fixture, positive/negative logs and builds are retained
  in nuc `.../scoped-draft1/` under the evidence root below.
- `b88b84459`: first-use cross-group lane stamps use the existing bounded
  command cohort. REPORT still fences the groups, captures the inode epoch,
  activates the lanes, submits each unchanged BOOTSTRAP command and checks
  **every exact apply verdict** before publication. Missing stamps are
  collected once per cached inode instead of serial propose/wait/fsync rounds.
  Present stamps are not rewritten. Partial, role, storage or apply failures
  remain failures; later retries use the same idempotent commands. No wire
  or store format changed. This removes redundant physical barriers rather
  than extending the apply wait or the foreground deadline.

`0d0552d30` additionally moves that same preflight helper outside the W77
transaction-only source region. Nuc 34 and node 38 stopped at the K32
extracted test: its broad slice had included the new helper without its
preflight mocks. Those **prerequisite failures** remain recorded; their
live stages were NOTRUN. K32 passes natively after the move, with unchanged
entry-count assertions. The driver now runs `make test` plus the recipe
check before live load, catching fast-unit dependencies before qualification.
The empty scratch indexes from those attempts do not constitute evidence;
their build, compiler-failure and prerequisite logs are in the permanent
matching `qualification/` artifact directories.

Focused bootstrap/cohort tests and both native daemon builds pass. The
qualification requires the bootstrap, descriptor and production durability
units before live load. Mac tests and native units do not qualify D4.

Node attempt-36 (`1848fe5e0`) finished full **FAIL**: 513 commands,
1 failure, 14 NOTRUN, 702 s, two over-budget reports. Its append-batch
fixture still mocked the former apply-wait name. `eeef43d75` corrects only
the mock; entry-count and verdict assertions remain. The per-image negative
control still fails at runtime. Native fixed fixture PASS was recorded as
a test-only overlay on nuc 33 and node 37 before their full stages.

Node attempt-37 (`6412320f6`) passed its targets and **20,118** cold
payloads (14,165 progress + 1,056 plateau + 4,897 K22, no errors), plus
32 log-watermark payloads, but full
**FAIL**: 514 commands, two failures, 14 NOTRUN, 685 s. The new descriptor
unit hard-coded `/tmp`, and the extra fast command put the predicted gate
at 1651 s over the fixed 1650 s bound. `b88b84459` uses `test_tmpdir.h` and
pairs related fast checks, retaining every test and failure status. Native
pair timings were 0.217 s (cohort checks) and 0.907 s (cold/descriptor checks),
within the existing one-second default; recipe and TMPDIR checks pass.
**The gate target/margin, budgets and scheduling pool were not increased.**
There remain 14 environment NOTRUNs; they are not passing gates. Closed
node indexes: 36 `544b5b8b838f7ebe7882a2b1a730575ca3a6cf41b4341e668b3c2b7e425fcfdb`,
37 `e2ada78aea22413c99d6811773c10656aafa80b4fc3c9b1007dceee73810beed`.

Raw roots: nuc `/home/maik/efs-artifacts/taskH-repair-20261027/`, scratch
`/home/maik/efs-taskH-repair-20261027/`; node9901
`/data1/efs/evidence/taskH-repair-20261026/`, scratch
`/tmp/efs-taskH-repair-20261026/`. Matching attempt directories keep stores,
binaries, command/source manifests and logs. Nothing failed was pruned.
Main node9901 and orcd-mini clusters were not changed.

### Previous candidate `6412320f6`: exact PUT durability cohorts

**Historical launch state, superseded above.** Nuc attempt-33 and node9901
attempt-37 ran this source. Its
first queue check refused to launch against a live predecessor (no mutation);
`queue37.log` is preserved and `queue37b.log` waits for completion. Archive
SHA256 `9c1fa8cbf7ccbde41a7a93f6fd2511b1090dcd1ccda2e9b8cbcf08732c3b9dbb`.
No workload, concurrency, quota, deadline or oracle limit was relaxed.

Nuc attempt-32 (`1848fe5e0`) completed two loaded rounds successfully;
round three passed K38, plateau and strict K22, but progress **FAIL**:
14.31 s foreground operation, including 14.128 s fsync. Its COMMIT waited
behind a 9.355 s whole-filesystem `syncfs` epoch, then its next epoch.
The whole filesystem contains all private stores, so this barrier included
unrelated dirty work. The closed text index is retained at
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-32/closed-native/index.json`,
SHA256 `9e7aa761104e1b7e8755477c98c47c265ed982950418f15215e9d4571229257d`.
Original stores and binaries stay in the matching scratch attempt directory.

`6412320f6` replaces that hot epoch's `syncfs` with a frozen cohort of
pinned fragment descriptors: fsync each exact file, seal its containing
directory and ancestry through the existing pinned-inode namespace barriers,
then write and fdatasync the epoch record. Only complete success advances
the durable watermark. At most eight workers and 128 files per cohort;
retained descriptors are bounded daemon-wide at 256 or one quarter of the
soft descriptor limit. These are internal bounds, not product toggles.
Original writer close errors still fail PUT, pins remain owned through the
barriers, and all pins are released even on failure. A sticky barrier error
still fails COMMIT. Startup and final clean-stop filesystem barriers remain.
There is no store, wire or durability-contract change. Shutdown joins writers
before the syncers and closes storage roots after them.

**Focused checks PASS**, not a qualification result: production Linux
`tests/test_store_durability` exercises real file/name/record ordering,
a PUT arriving during a frozen barrier, no early watermark advancement,
and sticky file/name/record failure with descriptor release, including a
second queued cohort on failure. `tests/test_put_epoch_files.py` checks
128 exact visits, eight-worker/global-credit bounds, a blocked credit waiter,
file/close failures and pthread-create fallback. The qualification driver
requires both tests before its live stages. Draft native build and two
store-unit logs are retained under nuc
`/home/maik/efs-artifacts/taskH-repair-20261027/scoped-draft1/`;
this pre-commit build is not the live candidate. Live attempts rebuild the
committed source and record binary hashes. Mac focused tests are outside Git
under `/Users/mike/Documents/Codex/artifacts/taskH-repair-20261027/scoped-epoch-6412320f6/`,
manifest SHA256 `9f893f3d99e89684f5d8936f5f80c3a3d93a4c10532b2db33d4d19c22968ff8d`.

Node9901 attempt-36 (`1848fe5e0`) has passed its targeted slow-member,
RDMA K38/D4, TCP K22 and real log-watermark stages. Exact cold restart
readback passed **31,187 progress + 1,056 plateau + 4,808 K22 = 37,051
payloads**, no read/mismatch/cleanup errors; the log-watermark gate also
checked 32 payloads. Its full suite is still running, so no full PASS is
claimed. Evidence:
`/data1/efs/evidence/taskH-repair-20261026/attempt-36/qualification/`.
The recorded test-only `e1857539f` overlay uses eight bounded cold readers;
every final expected payload is read, not sampled. Controlled load and
cleanup checks are unchanged. Attempt-35 was superseded **between stages**
only after its current cold reader finished and its private processes were
absent; its journal preserves that result and the remaining NOTRUN stages.

### Previous candidate `1848fe5e0`: unconditional publication reuse

**Historical launch status, superseded above; no completed qualification.**
Nuc attempt-32 and node9901 attempt-36 used this source. Archive SHA256
`607f8ff274dde046e15927986e1e24f25bc753d11c3aeea13ad5636518046c2c`.
All original workload/oracle, concurrency, quota and test deadline limits stay.

Two additional repairs:

- `81008234d`: node9901 attempt-32's full K22 test completed every metadata
  operation but failed participant-floor revocation with NOT_PRIMARY after
  about 3.97 s. The exact monotonic barrier had an existing 15 s deadline,
  but its outer coordinator retried BUSY only; it abandoned that budget when
  the inner eight-redirect window ended during handoff. It now retries
  definite NOT_PRIMARY with the same UUID/epoch/payload inside that same
  deadline, as cleanup already does. Ambiguous transport failure is not
  resent. Regression: transient handoff succeeds, permanent refusal obeys a
  shorter inherited deadline, NET is sent once. Previous source fails the
  transient-handoff assertion at runtime.
- `5e63df1a0` / `1848fe5e0`: an explicitly unconditional full overwrite has
  no mapping-CAS predicate (`host_apply_report_rec` sets `p.unconditional`).
  The owned-body classifier incorrectly required row absence, then replayed
  the same bytes after a proven admission abort. Retention now uses fresh
  FileID/epoch/ticket authority and the exact owned snapshot. It neither
  fetches a chunk map this operation does not depend on nor acknowledges
  publication. Conditional images/spans retain their exact-map requirement;
  changed epoch/owner, terminal ticket or unknown authority falls through or
  refuses. Mixed-unit tests prove that only conditional records are fetched
  and a failed conditional proof acknowledges neither record. Previous helpers
  fail the new assertions. Attempt/deadline and rebase-cycle bounds stay.

Matching D4 attempt-30 trace: inode 65838's four-record unit was certified
aborted (`retry_aborted=1`), then paid 3.660 s for a map pull plus physical
replay before the original 8 s REPORT budget expired. The later receipt
lookup proved PARTIAL/STALE; no successful write is invented. This is a
matching redundant-recovery path; final qualification is still required.

**Results preserved:** nuc attempt-29 (`994c025e6`) passed K38, K22 (448
confirmed writes, no producer errors) and plateau (1,037 final files), but
progress **FAIL**: one foreground write/fsync 10.28 s over 10 s; its phase
log measured 10.199 s in fsync and COMMIT exchanges about 7.225 s on all three
members, checkout 0. Attempt-30 (`79d0811e5`) passed K38, progress and K22
(442 confirmed writes, zero producer errors), but plateau **FAIL** with an
overwrite close/flush EIO. Its slow-barrier trace attributes delays to syncfs
(e.g. 5.954 s) rather than the epoch-record fdatasync (38 ms in that sample).
No hardware-health verdict: privileged NVMe SMART/kernel reads were unavailable.
Read-only I/O counters are retained in attempt-30 `io-pressure.jsonl`; no
storage settings were changed. Both failures remain failures; later stages
were NOTRUN. Closed text index SHA256s: attempt-29
`e8f510d6158d4464db10b77d954b25e0d3154eb29603001948cf78af5d2343b3`,
attempt-30 `9ddb74367d8d02efec275c8f4d99bed8872850baebf5fce1af917a75293a7a03`.

Node9901 attempt-32 (`68e50ead1` with recorded harness overlays) full suite
**FAIL**: 511 total, 3 failures, 14 NOTRUN, 717 s, 1 over budget. The failures
were K22 participant-floor unmount and two extracted transaction fixtures
missing my new diagnostic clock. Adapter and earlier shutdown failures did
not recur. `08c04bad5` fixes those fixtures; real-KV native tests pass on
both hosts. Attempts nuc-30/node9901-35 record that test-only overlay with
before/after hashes; latest `1848fe5e0` includes it directly. Node9901
attempt-32 closed text index SHA256
`04d310da75d5549d31cfec0b5d5429747553d705a0b9ff1098c1354ab71a73c9`.

Nuc attempt-31 was **STAGING FAIL / tests NOTRUN**: extraction began before
source transfer completed. No build, live cluster or store was started.
Its partial checkout and `staging-failure.json` stay; an empty closed-native
index is not test evidence. The completed archive's hash and readable tar
inventory were verified before fresh attempt-32. Node9901 pending attempts
33 and 34 were cancelled before build/live work and remain NOTRUN, with
queue scripts and cancellation manifests retained. No live campaign was
replaced or main deployment changed.

Focused production-function regression/negative-control text is retained at
`/Users/mike/Documents/Codex/artifacts/taskH-repair-20261027/mac-unconditional-proof-1848fe5e0/`,
manifest SHA256 `6ebd13c6a67e546e732bc04b68e199d016b25ddd3b91ea456cb8a6d040984876`.
Mac evidence is not native Linux/FUSE qualification.

### Latest candidate `994c025e6`: own-entry transaction waits

**IN PROGRESS; no completed qualification.** Nuc attempt-29 is queued after
trace-only attempt-28; node9901 attempt-34 after attempt-32. Pending node9901
attempt-33 was cancelled before build/live work, with its NOTRUN cancellation
manifest and original queue script retained. These queues wait for the prior
controller's exit and refuse replacement if its owned daemons/mount clients
remain. No main deployment changes.

`994c025e6` fixes a concrete wait-policy mismatch: `host_wait_refs` waits
for its own submitted PREPARE/RESOLVE entries using the fixed follower-read
400 ms policy. It now uses the **existing own-entry progress policy**, just
like an ordinary submitted command: the 400 ms grant renews only on apply
progress, with the existing fixed 2 s cap. No-progress remains 400 ms; all
exact index/term verdicts are still checked. This changes which existing
policy a transaction uses; it does not increase a test limit, client deadline,
retry count or admission cap. It is a product change, not a harness PASS rule.

The trace-only baseline attempt-28 (`a9b5f9c86`, `EFS_READ_PROF=1`,
`EFS_RAFT_OBS=1`) supplies matching evidence: refused MKDIRs have
`wait_us=400091..400103`, then spend roughly 1.9–3.6 s including abort cleanup.
At 16:03:30 UTC the node records 368 expired waits, 272 own waits, with
`arc_miss=0`, so these include actual fixed-policy batch wait expirations,
not overwritten apply verdicts. No PREPARE business refusal explains those
traces. The deterministic regression executes the production batch waiter
and dispatch wrappers against the real progress-budget arithmetic: a 1.2 s
progressing apply succeeds, a stalled apply still refuses at 400 ms, a
progressing over-cap apply at 2 s, and BUSY/STALE/term/storage verdicts survive.
The previous source fails the progressing batch assertion at runtime.
The test is wired into make test and the live qualification prerequisites.
Native reruns are still needed before claiming the liveness fix.

Nuc attempt-27 (`e7b321502` plus recorded harness timing overlay) **FAIL**:
K38 and both D4 cases passed, including 947 progress and 1,047 plateau final
payloads; plateau maximum foreground write/fsync was 4.371 s. One strict K22
metadata producer failed a MKDIR after the original 30+8 s request/resolution
bounds. The run stops there; later rounds/cold/full are NOTRUN. Closed text
index SHA256 `cedbfadabbc28983a3889afcab9bc674a6060551f1006e036bd04d9f89193689`.
This later D4 pass does not erase the earlier 11.168 s latency failure.

Local focused native/TSan/negative-control text evidence is retained at
`/Users/mike/Documents/Codex/artifacts/taskH-repair-20261027/mac-unit-evidence-20261027T1558/`;
manifest SHA256 `fb1f301f75d594f056e8f903aee0a3e4393cae91a6678c6a5a1d7aab7c2ba448`.
These Mac regressions are not Linux/FUSE qualification. `test_qualify_driver`
on Mac fails at its Linux `/proc` dependency, so no Mac PASS is claimed for it;
the D4 oracle/retention and make-recipe tests pass. The client-discovery test's
actual `/proc` half is NOTRUN on Mac; the separate native Linux result stands.

### Current continuation: K38 limiter proof and remaining latency

**IN PROGRESS; qualification still FAIL/INCOMPLETE.** Nuc attempt-27 runs
product `e7b321502`; node9901 attempt-33 (`a9b5f9c86`, same product plus
harness diagnostics/prerequisite) is queued after attempt-32 closes and
owned processes are absent. No overlapping replacement or main cluster change.

Node9901 attempt-31 (`8dd44c05b`) passed its targeted/cold stages but its
full suite **FAIL**: 511 total, 6 failures, 14 NOTRUN, 638 s, 2 over budget.
Besides the two adapter and two shutdown failures above, TCP K38 failed
with EIO and the session-latch TSan fixture did not compile. The preserved
K38 client log names chunks 57 and 90: `report-stuck`, base zero,
`gen == committed`, after two STALE rounds. That "committed" value was an
export-table observation, which can be the client's own unpublished PUT
placeholder while the authoritative row is absent. It cannot prove a moved
CAS base. `e7b321502` refuses that ambiguous proof in the limiter; only the
fresh authoritative classifier may acknowledge the object. The existing
2-round limiter for an actually different base, 64 REPORT attempts,
8 s REPORT deadline and rebase-cycle bound are unchanged. A regression using
the captured chunk-57 identity fails with the previous helper and passes
with the repair; the authoritative-absence classifier tests still prohibit
ACK from the placeholder. This is a matching cause for this captured EIO,
not a declaration that all K38 failures are fixed.

`c90a8e5ea` supplies missing errno/unistd and the deadline mock in the TSan
fixture. Its 200-round native, 60-round TSan and racy negative control pass
on the Mac; native host/full-suite results remain required. Attempt-26 on
nuc and attempt-32 on node9901 have this **test-only overlay**, recorded with
before/after SHA256, commit, stage and timestamp in their artifact manifests.
Their native binaries retain their original source identities.
`d4f72cc9b` adds only D4 slow foreground open/write/fsync/close timing;
recorded harness-only overlays are on nuc attempt-27 and node9901 attempt-32.
All workload/oracle/timeout limits stay. `a9b5f9c86` requires the limiter
regression before live work in future qualification runs.

Nuc attempt-26 (`e41cd19ad`) passed its entire first concurrent round and
K22/progress again in round 2, but round-2 plateau **FAIL**: foreground
write+fsync 11.168 s over the unchanged 10 s limit. No orphan-limit failure
was reported there. The trace shows fsync preparation 7.303 s and no residual
proof wait, so the earlier serial COMMIT hypothesis does not explain this
sample; finer foreground stage timings are now captured. Round 3, cold
checks and full suite were NOTRUN after that failure. The latency finding
remains open. Closed text index SHA256:
`71b017c64015a04eb3f84bd1b5baca917d753cae8f53f0d5e85ffffeed4ce72f`.
Node9901 attempt-31 closed text index SHA256:
`116e6bfa56c32cb75b7af8f32ebc460d027128266148eaecb9d7367d9b7b2958`.
All original failed fixtures/binaries/logs stay at the host paths below.

### October 27 continuation: owned spans and checked client retirement

**IN PROGRESS; no qualification PASS.** Product candidate `e41cd19ad`
(nuc attempt-26) adds the repairs below. `d6a5d40ff` adds bounded PREPARE
refusal diagnostics only. Node9901 attempt-31 is running the earlier
`8dd44c05b`; it has reached its full suite after targeted checks and cold
readback. Its source does not include these later changes. No deployment,
merge or push; all failed stores and their original binaries stay on-host.

- `3a5ad979f` fixes a regression introduced by the transaction-aware receipt
  lookup: known receipts whose row authority is temporarily BUSY are not
  tagged as terminal. The same identity retries without resubmission during
  that call. Nuc attempt-23's PROTO/EIO failures remain FAIL, not relabelled.
- `fdc0f0900` reuses an unchanged owned full PUT body after repeated
  definitive publication refusal only after fresh ADMITTED-ticket proof.
  `e41cd19ad` extends that rule to immutable spans with unchanged range,
  base, mapping, epoch, snapshot and ticket identity. Changed authority
  still rebases/refuses; neither path acknowledges data or increases any
  retry/deadline bound. Focused replay/ticket/native ledger regressions pass.
- `8dd44c05b` separates fsync preparation from durability-proof wait and
  records slow COMMIT checkout/exchange times. Nuc attempt-24's 13.86 s D4
  latency failure stays open despite later scoped latency passes.
- `0349cd2c1` removes a shutdown race: after checked unmount, retirement
  waits for the client's final stop record instead of sending SIGTERM after
  an endpoint check. The client can remove its handler between check and
  signal, causing a default-action kill during destroy. Native regression
  deliberately closes the endpoint and resets the handler in that window.
  Original 30 s stall / 600 s whole stop bounds stay. Two test adapters gain
  the existing deadline API mocks; failed stop-progress fixtures are now
  retained. Synthetic refusal checks use stricter test-only bounds.

**Retained results:** nuc attempt-24 passed K38, strict K22 and plateau but
failed progress latency. Attempt-25 passed K38 and progress (1,050 exact
files, max write/fsync 4.772 s), but plateau failed with 40 abandoned
uploads after 686 rewrites (5% bound 34), and one K22 MKDIR remained BUSY
through its unchanged 30 s request plus 8 s lost-reply resolution. Its
125 descendant failures are retained. That MKDIR stalled before the actual
memory-watermark refusal and handoff; those do not explain this failure.
The retained log shows repeated PREPARE/DROP attempts for the same name,
with no successful decision. This is still an investigation, not a proven
cause. Attempt-26 is the fresh unchanged-limit rerun.

Node9901 attempt-28 (`f923dcb8c`) completed all **39,641 exact cold reads**:
33,518 progress, 1,056 plateau and 5,067 K22, zero errors. Its full suite
**FAIL**: 511 total, 4 failures, 14 NOTRUN, 641 s, 3 over budget. Two failures
were missing deadline adapter mocks; stop-progress and expiry shutdowns
lacked final stop records. The stop-progress test then deleted its fixture,
so that failure has the suite's truncated client-log excerpt only; no
claim of complete raw replay evidence for it. The expiry fixture remains.
These results do not qualify the latest candidate.

Raw locations: nuc `/home/maik/efs-artifacts/taskH-repair-20261027/`
(attempt directories and `closed-native/index.json`), original stores/binaries
under `/home/maik/efs-taskH-repair-20261027/`; node9901
`/data1/efs/evidence/taskH-repair-20261026/`, stores/binaries under
`/tmp/efs-taskH-repair-20261026/`. Exact source archive SHA256s, build commands,
binary hashes, profile, commands and timings are in each attempt's source
manifest, `run.sh`, qualification result and binary manifest. Commands and
unchanged topology/quotas/deadlines are the committed
`scripts/qualify_publication_repair.py --profile nuc|rdma --root FRESH --output FRESH`.
Node9901 uses mlx5_0 and its existing `ASAN_OPTIONS=detect_leaks=0` exclusion;
this is not leak qualification. Nuc has no such exclusion.

Closed text index SHA256s: nuc attempt-23
`10ccef850d2b9e49a92d7a7005eeb8449c65cb252003085728c79fa4f47bd41f`,
attempt-24 `7fe08c6b79e297c0103463b98835dd512f2f3efaf04e6337f337614f50295dd3`,
attempt-25 `e76ae6894216de8db699f26e7308763c0d196c5e19fb2882c85c8d8d3f43e074`.
The deterministic native retirement race result is retained at node9901
`/data1/efs/evidence/taskH-repair-20261026/stop-retire-race-unit/`.

### Latest candidate `aaf1db324`: namespace replay authority and one bootstrap owner

**IN PROGRESS.** Nuc attempt-23 runs this exact source; node9901 attempt-30
is queued after attempt-28 exits and its private processes are absent. No
overlap/replacement of a live campaign, no main cluster change. The pending
attempt-29 was cancelled before any build or live work; its NOTRUN record
is `queue-29-cancelled.json` at the node9901 evidence root. All original
limits and full payload oracles stay. Reproduce with the committed
qualification driver and profile/environment described below.

Nuc attempt-20 (`f923dcb8c`) passed K38 and both D4 cases in its first
concurrent round: plateau max write/fsync 7.254 s, all 1,042 final payloads
exact. Strict K22 **FAIL**: producer 1 round 2 created all 16 directories,
then four file creates returned NOT_FOUND (worker 10). Retained server logs
identify its exact parent: MKDIR replay for
`bench-meta-2836577-1000002-d10`, inode 2676, returned bare success after a
plain lookup logged `row gone`. A committed row can still be in an unresolved
transaction intent or absent from another group's local applied prefix;
that lookup does not prove deletion. This is a concrete matching trace,
not a claim that every historical failure shares one cause.

`e32d1721a` first obtains fresh child-group authority and reads through the
transaction-aware getter before replying to a successful receipt. BUSY,
failed authority and unknown transaction outcomes remain refusals. Only
an authoritative missing row keeps the legitimate successful receipt for an
inode subsequently deleted. CREATE's parent and final child reads also
resolve committed intents. `1bf8d29f3` delegates the whole same-identity
request if a required transaction coordinator is not hosted. The existing
real metadata fixture proves committed-but-unresolved inode visibility;
the extended actual replay-helper regression covers fresh proof, pending
rows, refusal propagation, remote authority and genuine deletion. Previous
helper source fails the fresh-authority assertion. No new mutation identity
or failure-to-success shortcut is used.

Nuc attempt-21 (`b618b13f7`, diagnostics only) passed its first four-target
round and both D4 cases again in round 2, but strict K22 **FAIL** on four
MKDIR BUSY results after roughly 32.6 s; descendant failures are retained.
`6e2566cfe` gives one thread ownership of mount-session bootstrap instead
of letting sixteen workers issue the same GET/CREATE concurrently. Waiting
workers keep their own existing deadlines and allocate no file identity.
The actual 16-worker start/check regression requires one session CREATE,
checks deadline refusal behind the held bootstrap mutex and retains all
old fencing/transport/error assertions. No namespace operation is serialized
after the established latch. Attempt-22 at `83ddea77a` was **BUILD FAIL**:
missing `<errno.h>` for EBUSY, fixed by `aaf1db324`; no live test ran there.

Node9901 attempt-28 (`f923dcb8c`) passed slow dirty/clean COMMIT, all three
RDMA K38 rounds, RDMA D4 progress (33,518 exact files), plateau (1,056 exact
files) and strict TCP K22. Its cold full-payload readback and full suite are
still running, so this is a targeted reported PASS, not completed
qualification of this or the latest candidate.

Additional closed text index SHA256s:

- Nuc attempt-20: `fbb72ddc124b8c2eca65a8a5b32237e4f5f1b1e1ccd8e1e7eb6cc0a643c7a000`.
- Nuc attempt-21: `e31eb8fcd7b7589a4edb2bd0aa6ea551c969dd78f4219af13d2391babe5a16bc`.

Failed stores, original binaries and all producer logs remain on nuc;
none are silently retried into PASS. Empty `closed-native` for build-only
attempt-22 is not its build evidence: the failed compiler output and
source/build identity remain in its artifact directory's qualification
build log and manifest.

### Current candidate `f923dcb8c`: quota recovery and epoch-bound revoke ACK

**IN PROGRESS; no qualification PASS.** Nuc attempt-20 and node9901
attempt-28 run the committed `scripts/qualify_publication_repair.py` on
fresh private stores. It builds every producer, runs strict producer/cleanup
accounting, original loaded profiles, exact cold payload reads, the real
leader-log gate and the full suite. Same concurrency, quotas, durations,
retry/deadline bounds and failure assertions. No main deployment is changed.

Changes since the candidate below:

- `a10859489`: when a PUT meets transient physical quota pressure without
  enough unused live spares, retain its immutable ticket and acknowledged
  fragments while retrying within the existing 128 attempts and parent
  deadline. Permanent fullness still fails QUOTA; no degraded success.
- `ef896be58`: a storage member lacking fresh local session authority reads
  the complete epoch/state through the existing voter read path. It still
  seals its own physical floor. No unproven epoch/state enables cleanup.
- `67350b59a`: a new CREATE waits for session bootstrap before allocating a
  file operation identity, inside the public call's existing I/O envelope.
  Terminal fencing/errors still fail immediately. Revoke ACK now carries
  the captured epoch, so an exact-epoch BUSY retry is safe, while a delayed
  ACK cannot acknowledge a later barrier. Strict new 34-byte command only;
  no legacy decoder or retained-store migration. Failed old stores remain
  with their matching original binaries. Simulator and live helpers use
  the current ACK encoding.

Native admission, session and drain regressions pass; the production retry
unit tests retain permanent quota/deadline refusals. Shorter inherited
CREATE deadlines send no mutation and allocate no identity. The session
unit test checks future ACK refusal, exact completion retry, and rejection
of a delayed ACK after the next BEGIN with its bitmap unchanged. The Mac
cannot build this Linux library (`fopencookie`); native builds are required.

**Controlled real quota/reclaim gate PASS on both hosts** at `a2e3d6212`
(product includes a108/ef896): three private daemons, O_DIRECT/TCP, node 3
16 MiB, others 1 GiB. Fill to measured quota with acknowledged files, start
another 2 MiB write, require its real quota-backpressure marker, unlink four
acknowledged fillers, then require full protection, checked shutdown and
all 29 retained payloads exact through an orderly three-node cold restart.
Write elapsed 0.958 s nuc / 0.793 s node9901, under the unchanged 45 s bound.
Reproduce: `make all tests/faults/efsd` then
`python3 tests/test_put_quota_reclaim.py` (always retains fixtures).
Initial new-test attempts failed on a tuple-decoding bug and on an
intentional failed setup write poisoning clean unmount; those are retained
FAIL, not positive gates. Current setup fills from measured allocation
without a failing setup application write; the tested write still hits
real quota. Source archive SHA256:
`5a0d3ad5e0a35300653e395419c33f4980cedd2daa0a945aede4b0a280546eab`.

**Remaining observed failures at a2e3d6212:** nuc attempt-19 strict K22
passed including metadata producers and clean stops. D4 plateau failed one
foreground operation at 10.07 s against 10.0 s; no EIO or abandoned-upload
bound failure in that round. Its fsync logged flush 9,691 ms, final COMMIT
0 ms with the early proof exercised. It remains FAIL. Node9901 attempt-27
RDMA progress passed 34,398 exact payloads, but strict TCP K22 failed on
participant-ACK BUSY and a later producer's session-bootstrap BUSY. Those
observations motivate 67350b59a; their qualification is not yet proven.
Later stages of these stopped drivers are NOTRUN.

Raw evidence roots (all failures and stores retained): nuc
`/home/maik/efs-artifacts/taskH-repair-20261027/` and node9901
`/data1/efs/evidence/taskH-repair-20261026/`. Runtime stores respectively
under `/home/maik/efs-taskH-repair-20261027/attempt-N/fixtures/` and
`/tmp/efs-taskH-repair-20261026/attempt-N/fixtures/`. Closed, copied text
indexes are verified against originals before the next fresh attempt:

| Capture | `closed-native/index.json` SHA256 |
| --- | --- |
| nuc attempt-19 | `54bc6feac87d8ac9d2ffbdd0d1ff187c6dc65ad4de287aa0d9e72988a1fa1f59` |
| node9901 attempt-27 | `78b781a81a1c6a8cedec4f867c01233f652ab977d04bd1285b68630b582cd75b` |

Quota results: nuc attempt-19
`fixtures/quota-reclaim/efs-put-reclaim-29i1vtao/result.json` SHA256
`da115b86f42bcb1c381798b533ab2a6654433a6651fd5b810377512490176bff`;
node9901 attempt-27
`fixtures/test_put_quota_reclaim/efs-put-reclaim-ncn16xwq/result.json`
SHA256 `c28a30650b12d5f0b20dd6a60bcf51def754a477444dcdb3057ff946c7e366c4`.
Each artifact directory records source, build logs, binary hashes, commands,
exits and results. Node9901 keeps its existing broken-LSan-runtime exclusion
`ASAN_OPTIONS=detect_leaks=0`; this does not qualify leak checking. RDMA
uses mlx5_0. Nuc retains normal leak checking. Failed old fixtures are not
reformatted or opened with this new ACK encoding.

### Candidate `e9a08cbbc`: direct CREATE admission and revoke redirects

**IN PROGRESS.** Product `6c91140e0` plus `e9a08cbbc` is queued for nuc
attempt-16 and node9901 attempt-23. Earlier controllers must stop before
replacement; failed logs/stores remain. No main cluster is changed.

Node9901 diagnostic attempt-21 (`1aa4260fd`, one K22 fixture, original
limits, opid tracing; not qualification) reproduced failed CREATE with
`EFS_ERR_BUSY` (-13). Receipt-window tracing showed older valid requests
holding the oldest slot about 2.7 s while the direct API used the unrelated
one-second admission fallback. `6c91140e0` gives public library CREATE/MKDIR
one existing `EFS_IO_TIMEOUT_MS` envelope across admission and transport
when no caller deadline exists. An inherited deadline is unchanged, and
both successful/error returns restore it. The receipt limit stays sixteen;
no test timeout or concurrency is increased. The production-function native
regression passes; old API source fails after the premature one-second wait.
It also proves that a short inherited deadline sends nothing.

Retained producer logs separately showed unclean revoke with NOT_PRIMARY
(-15). `session_revoke_submit` repeatedly selected the cached old leader
when it refused without a redirect hint. `e9a08cbbc` clears only that stale
cache value and tries another voter; explicit hints still work. The eight
attempts, inherited deadline, BUSY policy and no ambiguous transport resend
are unchanged. Native regression covers absent/self/valid hints, exhausted
refusals, malformed hints, expired deadline and receive failure; old source
fails the stale-leader case. The harness now also fails a logged unclean
producer unmount even if the benchmark exits zero.

Regression captures are under node9901
`/data1/efs/evidence/taskH-repair-20261026/attempt-21/create-scope-regression/`
and `revoke-redirect-regression/`. Candidate exits are zero; both old-source
controls fail their real assertions. The earlier possible slow-handoff
theory was not supported by the retained admission timeline (about two
seconds to step-down), and no timer-policy change was made.

### Complete-producer rerun: K22 remains FAIL

Nuc `attempt-15` uses `df19be8b7`, the same product as `0ffc905b2` plus
harness changes. The corrected driver builds `all`, including `efs-bench`,
passes K22 `--keep`, and runs the original three concurrent four-target
rounds. Its first two rounds' four FUSE writers had no errors, but metadata
producers returned nonzero: first-round exit lists were `[0,1,0,0]`,
`[0,0,0]`, `[0,0,0]`, `[0,0,1,0]`. The round is therefore **FAIL**, despite
that older harness's PASS label. The retained logs show a failed MKDIR and
subsequent CREATE failures; cleanup warnings are retained too.
`4d3a6c4a6` makes every nonzero producer result fatal. `1aa4260fd` reports
the original CREATE/MKDIR error status rather than discarding it in the
benchmark wrapper. No time/concurrency/quota assertion is loosened.

Nuc attempt-14's K22 cold stage failed before starting daemons because its
launcher omitted `--keep` and had no confirmed manifest. Its successful
K22 fixtures had already been cleaned by the normal harness; their cold
replay is unavailable, not PASS. D4 manifests/stores remain: the subsequent
separate verifier read them with the original binaries. This launcher gap
is fixed in committed `scripts/qualify_publication_repair.py` (`df19be8b7`).

Node9901 attempt-19 finished full suite **FAIL**, 512 cases, one failure,
14 NOTRUN, 558 s plus 14 s build. The sole failure was the SSH-wrapper
self-test assuming that `node9901` was remote even while running there.
`881d01dce` tests local/remote routing against a fixed hostname; the full
self-test passed natively on node9901 (the Mac cannot finish its Linux
`/proc` process-ownership checks). The original suite failure stays recorded.
Separate cold readback passed all 33,642 D4 payloads (32,586 progress and
1,056 plateau), zero uncertain paths/errors. Attempt-20 now uses the strict
producer harness at `4d3a6c4a6`; qualification is pending.

Additional closed native indexes:

- Nuc attempt-14: `0c9ebd7e70cc7c06a810f739143efa8d88bf72a75067a2a44e913a29166c5ba6`.
- Node9901 attempt-19: `14fbdd43cec55ef058c8e8a4f0a1e86e2b5536942cf7e3c13a547d6247fd4647`.

### Qualification correction: K22 metadata producers were missing

**INCOMPLETE, not closure.** Inspection of attempt-14's full K22 output
found four uncaught `FileNotFoundError` exceptions: the focused launcher
built efsd/fuse/mgmt but omitted `efs-bench`. The four successful-payload
writers still reached and proved KV-memory admission, but the four intended
metadata producers did not run. The reported focused K22 PASS therefore
qualifies only that reduced workload; its exact cold readbacks remain real.
Likewise, the simultaneous D4/K38 results do not qualify the intended full
loaded profile. This limitation applies to focused runs using that incomplete
build list; it is not repaired by labeling their slot allocation as eight.

`b7d7de9f7` requires the metadata producer executable before any fixture
starts, catches producer and writer-setup exceptions, requires all four
producers to complete a round, and retains each producer's output and exit
codes. Their deliberate admission-pressure traffic stays separate from the
four exact successful-write oracles. The actual-helper regression passes;
the old source fails on its unhandled synthetic missing executable. No
product code or test limit changes. The next focused runs must build
`efs-bench` and rerun the same three concurrent four-target rounds before
K22 or the loaded qualification can close. Current full suites remain running;
they build `efs-bench` through the normal all-target prerequisite.

### Product candidate `0ffc905b2` (harness correction above pending): start the bounded proof helper

**Qualification remains IN PROGRESS.** Nuc `attempt-14` and node9901
`attempt-19` build this exact source. No test limit, concurrency, operation
bound, quota or assertion is relaxed. All failed and interrupted attempts
remain recorded, including the withdrawn first fsync experiment below.

`5e55c8225` fixes a specific retry handoff: a BUSY publication attempt whose
ABORT and every participant RESOLVE are proven applied cannot succeed under
that transaction identity. It now returns STALE for the existing authoritative
rebuild/new-publication-id path. An unproven abort, competing COMMIT, failed
cleanup, protocol error or storage error does not authorize this transition.
Production-path K32 tests exercise those distinctions; the prior source fails
the settled-abort assertion. `6e10b4d8d` strengthens the existing live gate,
keeping twelve rounds and its 3 s per-write bound, to alternate six STALE and
six BUSY forwarded-PREPARE refusals. Both hosts passed all twelve, all files
exact, six proven BUSY aborts, max write/fsync 0.42 s and no recovery ABORT.
The refusal hook is fault-build-only; no product configuration mode is added.
`de997c721` records the syscall stage of fixed-set D4 failures without adding
flushes or changing operations.

This did **not** close D4. Nuc `attempt-12` failed progress at 17.26 s, without
an application error: its slow operation spent 5.156 s in flush/publication
and then 11.741 s in durability COMMIT. The server exercised the new proven
abort path. K38 (4,800 exact records), plateau and K22 memory pressure (4,632
writes, no errors) passed; later rounds/full gate were NOTRUN. Node9901's
same source passed its focused gates; full suite remained **FAIL**, 512 cases,
four failures and 14 NOTRUN (driver SSH selftest, stop-progress final record,
B6 full/quota scenario, expiry). These are results outside the targeted
scoped passes, not a claimed full-suite or release PASS.

`4c74077e7` implements a **revised** early-proof path, distinct from the
withdrawn `31dcb3cbe` experiment. After PUT ACKs, a bounded helper can acquire
COMMIT facts while REPORT runs; it cannot mark degraded or retire records.
The mandatory post-REPORT proof uses exact local ACK targets and node
verifiers, handles later ACKs through normal COMMIT, and still requires two
durable fragments plus a durable degraded mark when the third is unproven.
A failed early third-node attempt is not waited twice when the other two
proofs suffice; without two proofs the final phase still retries pending
fragments normally. Missing helper capacity falls back to ordinary proof.
Eight helpers at most remain charged through join, including error exits.

**Budgets are the original separate budgets:** REPORT retains its existing
30 s outer phase and inner send/resolution limits; final durability retains
its existing 60 s proof budget and original caller deadline. The early
helper inherits the REPORT deadline, and joining it does not install a new
overall fsync deadline. The caller's prior deadline is restored before the
final proof and degraded mark. The earlier overall 30 s cap is absent.
Negative controls fail if that cap returns, if an ACK watermark is advanced
past a late ACK, or if the failed third-node attempt is waited twice.
Additional actual-code tests cover changed verifiers, late registration,
helper capacity, strict error propagation and no early retirement. Global
capacity sweeps now keep a two-proof record charged when its mapping is
missing rather than assume it was already published and superseded.

`6a63798a4` adds a **manual extra** B6 slow-member dirty-fsync case. It writes
identical bytes before the first fsync and checks that early proof actually
ran, with the same 75 s bound and 65 s injected COMMIT delay. The original
clean-file case stays unchanged. Both run on node9901 before the target
qualification. Attempt-18 **FAIL**: the dirty case correctly refused to
qualify because no helper ran. Attempt-13 also failed both D4 cases;
K38 and K22 passed. Every sampled fsync diagnostic said `precommit=0`.

The linked Linux client has about 451 KiB of static TLS, exceeding the
128 KiB custom helper stack. `pthread_create` failed, and the safe serial
fallback always ran. `0ffc905b2` uses the platform default thread stack,
still bounded to eight joined helpers. The native regression now touches
512 KiB of static TLS: the old stack fails the helper-capacity assertion
(SIGABRT), the corrected stack passes. Both logs/exits are retained in nuc
attempt-13. This is not yet a live qualification PASS.

The new source archive SHA-256 is
`5c57c9c149cc1f146eda8bc70a979304bc5c9a69cf3c8e75111e168e4f1250fb`.
The launchers accidentally retained the previous `6a63798a4` build label;
the per-run source manifests explicitly record that labeling error and
the exact `0ffc905b2` archive hash. No peer uses a different source. The
source identity is the archive/manifest, not that stale displayed label.

**Retained data:** after attempt-12 closed, matching original binaries
cold-restarted the private D4 fixtures from attempts 08–12. Every one of the
11,872 recorded manifest paths was read: zero read errors and zero mismatches
among the 11,842 non-uncertain payloads. Thirty uncertain paths are separately
hashed, not accepted as any-version matches. Six fixtures are data PASS and
six INCOMPLETE because their failed manifests are incomplete. Original
workload failures remain failures. Summary:
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-12/retained-payloads-summary.json`,
SHA-256 `0efab4db879d52b763938899d8a440bb67b668002b058bb08657235e03dec312`.

Additional closed native indexes (host evidence roots below):

- Nuc `attempt-11/closed-native/index.json`:
  `ad6e83ee89d246f6ba56e127b448a4dbc53926e832e2c1b21479c4e4c5466a51`.
- Nuc `attempt-12/closed-native/index.json`:
  `0a04585341372046395ad05fd98abeea609af5e4c761839bb425f7405078a5ce`.
- Node9901 `attempt-16/closed-native/index.json`:
  `4b2fec45af7db4b00c35f5a654692100581d94b1143ca6fd6d834da17254d92d`.
- Nuc `attempt-13/closed-native/index.json`:
  `036142c02fc73528d39af2a3f36d8f1d61e06df3a3460ba87a5d4b575eddb722`.
- Node9901 `attempt-18/closed-native/index.json`:
  `2d4713a11edac9065a46482703bee7ce39913020f5f6cf6da4049e36d5469c0b`.
- Node9901 `attempt-17/closed-native/index.json`:
  `d95684d04e561084f13f02bbe97f2b21edc3156a2265971cbf6de361e2a8f9ae`.

All failed stores remain kept for this open investigation. Main orcd-mini
and other agents' clusters were not changed.

### Earlier candidate `5c81d6070`: fsync experiment withdrawn

**IN PROGRESS, not a qualifying PASS.** Nuc `attempt-11` and node9901
`attempt-16` build this exact commit. No test concurrency, resource limit,
operation deadline or assertion is increased. All prior failures stand.

`31dcb3cbe`'s speculative fsync overlap is **withdrawn by `15562d47d`**.
Node9901 `attempt-13` full gate finished FAIL, 512 cases, four failures,
14 NOTRUN: driver SSH selftest, B6 slow, B6 full and expiry. B6 slow returned
EIO for F0 at 30.1 s. The experiment incorrectly capped the entire fsync at
30 s, leaving no budget for a durable degraded mark after a slow COMMIT.
The original publication/durability path and budgets are restored; no
helper or pre-publication proof path remains. A dedicated unchanged B6 slow
gate runs before node9901's new K22/RDMA qualification. Its result is pending.

**Evidence limit:** B6's existing finally block deleted failed fixture stores
unless its keep setting was enabled. Attempt-13 retains the assertion, not
those native logs/stores, so the mechanism above is inferred from code and
timing rather than a retained native trace. `6816a420c` fixes failure
retention independently of the setting; it is included in `5c81d6070`.
No missing raw evidence is claimed to pass a gate.

`16f9e243f` extends the existing checked command cohort to owned buffers for
large publication batches, bounded by eight entries of the existing 256 KiB
batch maximum. Small commands retain inline storage. All owned copies are
freed on successful persistence, role failure, persistence failure and
pre-submit abort; the hold still closes before remote forwarding. Actual-code
regressions cover caller buffer reuse, size refusal, order, exact entry
verdicts and error cleanup. The prior inline-only implementation fails the
large-batch regression. This adds no product switch or new wire format.
Nuc `attempt-10` (still with the withdrawn fsync experiment) passed K38,
plateau and K22 memory pressure (4,756 writes), but progress failed at
13.16 s; later rounds/full gate were NOTRUN. Node9901 `attempt-15` passed
its first two RDMA K38 rounds but its third failed on pwrite EAGAIN; all
three private daemons stopped orderly and the failed stores remain.
That candidate's remaining D4/K22/full stages were NOTRUN. Attempt-14 was
superseded while queued, before tests started; it is NOTRUN, not FAIL/PASS.

**Remaining pressure caller fixed in `5c81d6070`:** foreground write-capacity
admission still called the whole-inode fsync drain, which can wait for future
PUT windows while trying to free capacity for an unaccepted write. It now
uses the already-tested bounded pressure PUT/REPORT path. The existing
sixteen-round walk can revisit one inode to finish a multi-cohort atomic
unit; neither the round count nor the enclosing write deadline changes.
Fsync/close remain strict. The production admission regression refuses the
old whole-inode caller and passes the new path, including a three-cohort
unit, capacity ownership, deadline refusal and error propagation. The real
client-memory and pressure-completion regressions also pass. Live results
are still owed; the rare RDMA EAGAIN is not declared fixed by the unit test.

Additional retained indexes, relative to the host evidence roots already
listed, with failed stores kept for this open investigation:

- Nuc `attempt-09/closed-native/index.json`:
  `3287adc1e91e0e047ed17b99d6884d6a7d239aee109e703ab1e6377e80d26f88`.
- Nuc `attempt-10/closed-native/index.json`:
  `3af91b28ef44de600ed1964182c23e6c3a5fb9eb58396649f9f05f3d45f01e9f`.
- Node9901 `attempt-15/closed-native/index.json`:
  `5ccb4a44695661a4ce5ba9801a6dab4e37345905e0ce7d009b594ac5450dd8c4`.
- Nuc attempt-07 cold progress `result.json`:
  `bebd834ebc6d83cb9bb03eeba237df4381ec0cd8e3ec47eb9a76772f5588dbe3`;
  cold plateau `result.json`:
  `7d1639ccddbbe02630953a12f56d3eb1cc898cca45800b82e8fd13bc8ab02a9e`.

The following candidate history is retained as evidence, not current advice.

### Latest check: quiet nuc still fails; candidates remain unqualified

After the explicitly authorized Task I shutdown, only this campaign's private
clusters were running on nuc. `attempt-07` (product `40856b152` plus harness
`84b77d5ef`) failed progress at 12.46 s and plateau with one population EIO
(1,023 of 1,024 working-set files created). K38 passed 4,800 exact records;
K22 memory pressure passed 4,636 writes. No later rounds/full gate ran.

`31dcb3cbe` overlaps storage proof acquisition with REPORT on explicit fsync.
Early proof cannot write degraded marks or retire a stripe; successful REPORT
still requires the strict final proof. New PUTs remain unproven until covered
by their own ACK snapshot. Eight bounded helpers include completed-but-unjoined
jobs; capacity/thread-creation failure falls back to ordinary final proof.
One inherited 30 s operation deadline covers drain, REPORT, join and final
proof. Ordinary close does not acquire a new awaited COMMIT. Actual-code tests
cover retention, degraded refusal, late ACKs, helper capacity, inherited
budget and joining after REPORT error. No acceptance bound increased.

This was **not sufficient**: quiet nuc `attempt-08` failed progress (25.79 s
foreground operation and EIO) and plateau (11.12 s foreground operation).
K38 passed and K22 memory pressure passed 4,697 writes. The publication spent
its existing 8 s send and 8 s receipt-resolution allowance, learned a definite
STALE receipt, then exhausted the remaining allowance on a new publication.
The error remains a failure; no unknown receipt is treated as applied.
A short `/proc`/vmstat diagnostic observed 88–89% I/O wait on nuc's `/home`
XFS/LVM Samsung 970 EVO, with direct-I/O waiters. This establishes storage
pressure, **not a proven device fault**. SMART and kernel warnings require a
sudo password and were unavailable. `attempt-08/io-diagnostic.txt` retains
those observations and the refused read-only commands.

`45ad1da38` additionally routes bounded small publication PREPARE batches
through the existing checked local log cohort, closing it before waits or
remote forwarding. Entry counts, exact verdict checks and abort cleanup stay
unchanged; oversized batches keep their existing direct submission. The
production K32 regression now proves two small local PREPARE entries share
the barrier; it fails on `31dcb3cbe` and passes on the candidate. Existing
cohort role-change/persistence-failure and append-batch regressions pass.
Linux K32 entry/ticket/settle tests pass. This is an I/O reduction to measure,
not yet a claimed fix for the loaded failures. Fresh nuc `attempt-09` builds
this exact candidate and repeats the same four-target/eight-slot profile.

Node9901 `attempt-12` (`40856b152`, explicit retention overlay for full gate)
finished **FAIL**, 512 cases, four failures and 14 NOTRUN: driver SSH selftest,
geometry, stop-progress final record, B6 unavailable. All focused target
stages passed. `attempt-13` (`31dcb3cbe`, no overlay) passed the actual K22
leader log watermark (768 unapplied, memory pressure=0), all 32 payloads cold,
three RDMA K38 runs, both RDMA D4 cases and K22 memory pressure; its full gate
is running. These results do not qualify `45ad1da38` or close nuc D4.

**Retained payload recheck:** `scripts/d4_verify_retained.py` (`69b4b35a7`)
uses the original binaries and stopped private stores, checks unoccupied
recorded endpoints, binary/driver hashes and cluster identity, then mounts a
fresh reader and reads every manifest path. It never formats or deletes.
Nuc `attempt-07/cold-test_d4_progress/` read all 1,002 recorded paths exactly;
`cold-test_d4_plateau/` read all 1,037 recorded paths, 1,023 exact known final
payloads and 14 uncertain foreground payloads separately hashed. No mismatch,
read error or cleanup error. Both verdicts are **INCOMPLETE** because their
failed-run manifests are incomplete; the original workloads remain FAIL.
This cannot recover payload namespaces deleted by the pre-`84b77d5ef` harness.

Closed native logs/results/oracles, copied and hash-verified on nuc under
`/home/maik/efs-artifacts/taskH-repair-20261027/`:

- `attempt-06/closed-native/index.json`: SHA-256
  `ffd0d996dbf708a62bfc0f631e9d114b1c52346fab49d6a124ac6f50edb1349c`.
- `attempt-07/closed-native/index.json`:
  `0e887a4b143bbbd350d0fea31d2ae7fa41529e164f4822496df7d473bbafe5fd`.
- `attempt-08/closed-native/index.json`:
  `63ad91b2f7280f02b1e23d7db070d955bbe655168be880a21d1c37709a02f477`.

All failed stores remain kept for this open investigation. Full-suite FAIL,
missing prerequisites and payload-verification limits are not waived.


No test deadline, concurrency, quota or assertion has been relaxed. Nuc's
first repaired full gate (`b807294be`) finished **FAIL**, 512 cases, seven
failures and 14 NOTRUN. Target failures were K38 and both D4 cases; other
failures were packer placement, operator workflows, pre-fault transfer
setup and the W166 compressed-L0 prefill. Node9901's product `537bc94e1`
full gate finished **FAIL**, 512 cases, five failures and 14 NOTRUN. Its
four target commands passed, but the full result remains FAIL (driver SSH
selftest, geometry, stop progress, B6 unavailable and expiry).

**Proven pressure-completion bug, fixed in `4f8f25e7c`:** a bounded background
chunk flush called the foreground REPORT completion wrapper, which could
recursively flush the whole inode and wait for future PUT windows under
shared append serialization. Retained nuc diagnostics show repeated
roughly eight-second completions dominated by this whole-inode flush.
Pressure completion now synchronously settles only its carried records;
held records remain owned and charged. Foreground fsync still waits for
all required data and publication. The production-path regression fails
on the prior call site and passes on the candidate.

Nuc `attempt-02`, product `bee20f99d`, ran K38, D4 progress, D4 plateau and
K22 concurrently using the existing eight-slot allocation. K38 **PASS**,
all 4,800 records exact through both mounts and remount. K22 KV-memory
pressure **PASS**, 449 successful writes, no application errors. D4
progress **FAIL**, one write/fsync 10.183 seconds against the unchanged
10-second bound, no payload errors. Plateau **FAIL**, 19 segment PUT errors;
logs show healthy storage destinations refusing their segment authority
read with NOT_PRIMARY. Failed stores remain for this open investigation.

`939503c75` delegates the complete object-plus-degraded-mark authority read
to another voter when a hosted destination cannot cover its own read.
The serving RPC never forwards, negative authority remains linearizable,
and failed routing remains BUSY. The regression executes production
functions: prior source fails the hosted-follower assertion; the fix
passes, including refused peers, true absence and permanent local errors.
`33ebc9f24` also separates REPORT/flush time from durability-COMMIT time in
fsync diagnostics; the former log incorrectly charged both to flush.
Neither change extends a deadline or bypasses a durability proof.
Nuc `attempt-03` now builds this exact candidate, then repeats the same
loaded profile three times, the dedicated log-watermark proof and full gate.

**K22 correction:** the earlier five-run 63,927-write campaign established
availability, not an independent committed-log watermark proof. It combined
an old memory-pressure admission counter with a later follower backlog.
`da5eb7cbe` separates the KV-memory case from the actual leader log case.
The latter requires a leader stepdown with pressure=0 and at least 768
committed-unapplied entries of the derived 1,024-entry cap, while KV stays
below its watermark. It uses a test-build-only group-0 apply hold, bounded
filler and four real FUSE writers; all 32 payloads must match after an
orderly all-daemon restart and fresh client. It is an explicit manual
qualification phase (`tests/live/k22_unapplied.py`), not an added make-test
case or an increased suite budget. `f1ac9ef65` adds bounded BUSY backoff to
the synthetic filler; that revision still needs live requalification.

Node9901 `attempt-08`, product `bee20f99d`, **scoped PASS**: ten TCP K38
runs, D4 progress (1,165 foreground operations, max write/fsync 0.481 s,
34,862 exact files, drain 111.2 s), plateau (917 foreground operations,
max write/fsync 0.165 s, 1,056 exact files, drain 22.9 s), both distinct
K22 pressure proofs, and 4,616 confirmed KV-pressure payloads exact after
cold restart. The log-watermark proof took 29.924 s, observed actual leader
backlog 768, and verified all 32 payloads cold; max write/fsync 8.759 s
within its unchanged 30-second bound. This does not qualify the newest
packing fix or close the loaded nuc failures. RDMA must also qualify the
current candidate; earlier RDMA passes used prior product revisions.

Nuc closed failure outputs are hash-verified outside Git:
`/home/maik/efs-artifacts/taskH-repair-20261027/failed-fixtures/index.json`
SHA-256 `6d496df775c5c57c1f27e90e5395e87e24a6d048de24933ee683bc940bb950d1`;
`attempt-02/closed-fixtures/index.json`
`8402600ace931edc489c3d1b41b507cfae8b6197bed6c9b28150bf4b1eea066c`;
`attempt-02/closed-native-logs/index.json`
`5551717e5b00b427958c09d968ba7c16b57c559f0ddb33903bdf958f6ea9b980`.
Node9901 closed artifacts remain under the trace path below pending final
relocation to `/data1/efs/evidence/taskH-repair-20261026`. Main orcd-mini
was not changed. All failed stores remain open-investigation evidence.

### Current candidate `939503c75`: RDMA and concurrent TCP

Node9901 `attempt-09` **scoped PASS**: three explicit RDMA K38 runs
(4,800 records each, both mounts plus remount), D4 progress (31,483 exact
files, max foreground write/fsync 0.234 s, drain 6.6 s), plateau (1,056
exact files, max 0.098 s, drain 39.5 s), updated K22 log-watermark (768
committed-unapplied, memory below watermark, 32 exact cold payloads), and
KV-memory admission with all 5,214 confirmed payloads exact after cold
restart. No application errors were waived.

Node9901 `attempt-10` **scoped PASS** runs four private clusters concurrently,
K38 + both D4 cases + K22, two existing pool slots each, repeated three
times. K38 checks all 4,800 records through both mounts and after remount
in each round. K22's 2,753 / 2,652 / 3,132 successful writes are availability
counts, not additional cold-payload proofs. Both D4 cases retain every
original foreground, resource, packer-error and drain assertion.

| Concurrent round | Progress exact files | Progress max write/fsync | Progress drain | Plateau max write/fsync | Plateau drain |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 24,398 | 0.254 s | 4.3 s | 1.104 s | 49.9 s |
| 2 | 26,242 | 0.499 s | 5.5 s | 0.953 s | 48.9 s |
| 3 | 27,230 | 0.210 s | 5.5 s | 1.462 s | 33.6 s |

Each plateau verifies 1,056 files exactly; its working set stays at 1,024.
The bounds remain 10 s per foreground operation and 150 s drain. Node9901
`attempt-11` passed the publication-forward-abort and W166 authority cases;
its full regression gate is running, not yet a PASS.

Nuc `attempt-03`, same product and same four-test concurrent profile,
finished **FAIL**. K38 reports pwrite EAGAIN/EBUSY and fsync/close EIO.
D4 progress has a 24 s foreground operation; corrected fsync diagnostics
attribute 17.894 s of it to the client durability-COMMIT phase (not a
proof that all that time was inside a server filesystem syscall). Plateau
reports five application errors, three foreground overruns up to 47.397 s,
and only 1,022/1,024 population files. Its final sampled pack counters are
put_errors=0 and read_errors=0; this does not turn the failed driver into
PASS. Verification skipped two files whose last write failed, so it is not
full payload verification. K22 memory admission passed with 654 writes.
All application failures remain failures.

A separate Task I controller was discovered running concurrently on nuc's
same `/home` XFS filesystem, launching additional clusters. This confounds
latency attribution; it does not establish the sole cause or waive these
failures. A 15 s diagnostic strace after the run had already failed records
syncfs durations up to 1.877 s; that instrumented interval is not timing
qualification. Nuc `attempt-04` waits for 60 s without foreign EFS processes
before any workload, then repeats the original four-test profile and
limits. It never stops or alters the foreign campaign. The wait itself is
bounded at one hour and is separate from every unchanged test deadline.
Nuc closed attempt-03 text/results are copied and hash-verified at
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-03/closed-fixtures/index.json`,
SHA-256 `ad4dcb8282760f46159d6820c90be894d9d0130653f0dd24e35c357b21903089`.
Stores remain intact for the open investigation. Qualification stays open
until the original-host failures are resolved and remaining gates reviewed.

### Quiet nuc still fails; `40856b152` is the next candidate

With explicit user authorization the old Task I controller was stopped;
its test processes were stopped without allowing fixture-deleting finalizers,
seven mounts unmounted normally, and all 14 stores logged clean shutdown
after SIGTERM (no daemon SIGKILL). Logs/stores remain intact. The interrupted
campaign is marked INTERRUPTED, not PASS, in its artifact directory. Handoff
evidence: `/home/maik/efs-artifacts/taskH-repair-20261027/foreign-taskI-stop-20261027/manifest.json`,
SHA-256 `a06b759bb98a200873d5f7b585bfb9d709bd3d6232e405edcf4e2735051b4612`.

Nuc `attempt-04` started after the quiet guard and captured zero foreign EFS
processes. It **FAILS** on abandoned uploads: 48 after 656 rewrites, bound
32 (5%). There are no application errors or verification mismatches in
this plateau result; all 1,024 population files exist and no failed-write
payload was skipped. Foreground max 8.499 s and drain 24.9 s satisfy the
unchanged bounds, but do not excuse the upload-growth failure. K38,
progress (827 exact files, max write/fsync 3.868 s) and K22 (508 writes)
passed. Closed-output index SHA-256 is
`e407ee961c5ec63a7882030e84be89622a678ae4e9da424965b9ccce2e900268`
at `attempt-04/closed-fixtures/index.json` under the nuc artifact root.

Diagnostic-only `077a59dc0` and `31ee8bee5` expose stage/transaction rejection
causes. Nuc `attempt-05` **FAILS** again: progress foreground 14.964 s
(including a 12.015 s client COMMIT phase), plateau 11.659 s. Its abandoned
count is 24 after 487 rewrites, exactly the unchanged bound; K22 passes
with 4,703 writes. Logs show unit PREPARE results BUSY and subsequent
commit attempts meeting the already-decided ABORT. No staged-image CAS
STALE is logged in those observed aborts. The resulting replay nevertheless
re-uploads entire units, even when some records' authoritative mapping
has not changed. Closed-output index SHA-256
`dabccf9cbf686c1cebc9272c568f8319da151015f1eb376b039d688f0ccf0e95`
is `attempt-05/closed-fixtures/index.json`. These failures are not classified
as merely external load: the host was quiet outside the test profile.

`40856b152` gives an exact owned full-image PUT one retry under a new
publication id when a fresh mapping and FileID/epoch view still satisfy its
original base/fold contract. It neither acknowledges the previous request
nor drops dirty state or PUT ownership. Changed mappings/epochs, spans,
missing ownership and repeated refusal use ordinary replay. A late duplicate
PUT callback cannot reset this allowance. One FileID/epoch read serves the
inode's chunks only in this classifier round; no global cache is added.
The server still validates the ticket and atomic unit and decides the new
publication. This avoids needless physical re-upload and orphan creation
without changing the REPORT retry/deadline bounds or durability protocol.

Production-classifier tests PASS; the prior classifier fails the intended
unchanged-view assertion. Negative cases cover changed FileID/epoch,
unverified absence, changed mapping/fold list, newer PUTs, missing authority,
span replay and repeated refusal. Existing pressure, receipt-budget,
install-order and STALE-bound regressions PASS. Nuc `attempt-06` now runs
three original concurrent-profile rounds, exact K22 cold readbacks, the
separate log-watermark proof and full gate. Node9901 `attempt-12` runs
RDMA target checks plus both K22 proofs/cold readback and full gate. Both
are IN PROGRESS; the efficiency change is not yet live-qualified.

Node9901's previous `939503c75` full gate finished **FAIL**, 512 cases,
three failures and 14 NOTRUN in 583 s (11 s build): driver SSH selftest,
stop-progress missing final stop record and expiry client-stop failure.
All target commands passed in that full run, but no overall PASS is claimed.
Closed node9901 artifacts are now canonically at
`/data1/efs/evidence/taskH-repair-20261026`; the old trace path is an alias.
`closed-native-through-11/index.json` retains 227 closed logs/results,
SHA-256 `7cd62e648acefd0f29ed85d2084b8afedb3ea0d8e84aaa3782b9703af9a17098`.
`manifest-closed-through-11.json` SHA-256 is
`0e6c598e7383e89ded6790cb1ee9dd6469ff139fee29008074802049fadfe44b`.
Failed stores remain in private scratch for this open investigation;
no fixture bulk has been pruned and main orcd-mini remains unchanged.

### `40856b152` first nuc repeat FAIL; repair the inner evidence cleanup

Nuc `attempt-06` **FAILS** plateau: foreground 12.19 / 21.35 seconds, one
foreground EIO, and 26 abandoned uploads after 510 rewrites (bound 25).
K38, progress and K22 passed that round; no later rounds or full gate were
launched. The efficiency fix is not qualified. Its trace shows retained
owned objects (for example two of three records) rather than re-uploading
every record, but that improvement is insufficient for closure.

**Retention limitation, corrected in `84b77d5ef`:** keeping the outer test
store did not stop `scripts/d4_progress.py` from deleting its workload
namespace in its own finally block, even on FAIL. This was missed in the
earlier retention work. Prior logs/results remain and stores were kept as
left by the wrapper, but their original D4 payload namespace cannot be
claimed preserved or replayable. Those deletions cannot be undone.

The driver now keeps failed/interrupted namespaces automatically and writes
`expected-payloads.json` before cleanup. It records the final acknowledged
versions/sizes and deterministic payload keys, incomplete/uncertain outcomes,
driver hash and build identity. Foreground failures conservatively mark
its foreground expectations uncertain; no old-or-new oracle is introduced.
Both outer tests pass the existing `--keep`, so failures in their later
packer/compaction assertions retain the namespace too. PASS cleanup of the
whole private fixture remains the outer wrapper's decision. Real main/cleanup
path tests cover FAIL, exceptions, PASS cleanup and explicit keep. No
resource, workload, foreground, orphan or drain bound changed.

Nuc `attempt-07` reruns the original three-round profile with product
`40856b152` and this committed test-only overlay. Its source manifest and
`harness-overlay-84b77d5ef.patch` are in the artifact directory. Prior
`attempt-06` harness files are copied before overlay. Node9901 `attempt-12`
uses the same overlay for its full gate; it was installed during the build
phase before any full-gate cases started. Its earlier focused RDMA cases
used the original cleanup path. Both campaigns remain IN PROGRESS.

## Candidate mechanisms

- **K38/K45:** `0c1970678` repairs the handoff after a lost REPORT reply
  resolves to definite PARTIAL/STALE. The dirty snapshot and putids remain
  owned; the publisher starts a new publication and uses its normal
  authoritative STALE replay. Only definite resolved STALE takes this
  recovery path. Unknown outcomes remain errors. One absolute deadline
  covers the original eight-second send phase and existing receipt
  allowance; retries never renew it, and shorter caller deadlines win.
  Held-unit waits retain their original send-phase limit. No wire/store
  encoding or product setting was added; the 30-second flush limit stays.
- **Known partial hidden by backpressure:** `84d6c351c` preserves a known
  CAS-loser STALE when a later publication unit returns BUSY. This is not
  success or proof that other units aborted. The client retains ownership
  and repulls/replays; unknown-only BUSY and permanent errors stand. The
  retained attempt-03 logs show known losers repeatedly hidden as UNKNOWN,
  causing same-identity resends until the bound. A new test executes the
  production terminal verdict against unchanged server source; the receipt
  integration test covers a loser plus later BUSY with a dropped reply.
- **K22:** reviewed Task G commits `ddff5fcfd`, `878c00e96`, `430cd676c`
  incorporated as `58378d1da`, `49583981f`, `5857ccc96`, plus selected
  test-only leader setup from `f00c91c41` (`bfa6dfb57`). A lagging hosted
  follower delegates the complete PUT-ticket STATUS authority read to a
  healthy leader; forwarded requests never forward again. A full L0 alone
  no longer rejects REPORT while the bounded tail has room; leader memory
  and unapplied-log watermarks and apply-side hard limits remain.

## Evidence so far

New regression executes the production publisher with a lost-reply/
PARTIAL adapter. Unchanged `bf143e486` write source fails at the expected
completion assertion; the candidate passes. It also tests unknown-outcome
failure, no retry for ordinary unresolved STALE, shorter inherited deadlines,
held units, zero resolution allowance, and a single shared completion bound.
`eaf2aca9f` adds an RPC-level inherited-deadline check (passed in attempt 03).

Attempt 01 failed because the test adapter incorrectly expected the receipt
completion deadline during a held-unit retry. `cd27c1d8a` corrects that
assertion to require the original send deadline. Both logs remain; no product
change was made for that test failure.

Attempt 02, product `cd27c1d8a`, node9901 private O_DIRECT cluster (authority test TCP; K38/D4 auto selected RDMA):
client/receipt/replay/install-order focused units PASS; W166 authority cases
PASS; K38 default 24 writers on two mounts PASS, all 4,800 records exact on
both mounts and after remount (72.1 s). D4 progress also PASS: 1,527 foreground operations, write/fsync max 0.133 s,
35,888 files exact and drain 121.2 s within 150 s; plateau PASS with 1,056
files exact, max write/fsync 0.171 s and drain 45.3 s. This is one
focused run, not completed stress qualification or a full-gate PASS.

## Retention and isolation

Original nuc evidence remains `/home/maik/efs-artifacts/taskH-20261026`,
manifest SHA-256 `29413a5e7e1cca33965ca09365f62efdd1d772b9e5475197d894a4c32d6d5dd2`.
New closed outputs/controllers are under node9901
`/data1/efs/traces/taskH-repair-20261026/attempt-NN/`; private scratch is
`/tmp/efs-taskH-repair-20261026/`. Final manifests/pruning pending.
No raw artifact is committed. Failed D4, K38 and K22 fixtures now remain by
default, without another setting, until reviewed evidence copying/pruning.

Nuc is reserved by Task I's quiet-host measurement, so no test workload is
added there. Node9901 checks use private ports/stores, leaving orcd-mini's
`394f2b791` release, clients and stores unchanged. Attempt 03 (`eaf2aca9f`, explicit TCP): first two K38 repetitions PASS,
third **FAIL**, twelve odd-rank writers EIO on the second client. Full
client/daemon logs were copied to stable `attempt-03/failed-k38-3/` and
verified. All remaining D4/K22 repetitions and the full gate were NOTRUN
because the controller stopped on that failure. Records with known STALE
were hidden behind a later unit's BUSY; receipt lookup kept resending an
immutable stale publication as UNKNOWN. This led to `84d6c351c` above.

Task G forwarding/setup hardening is incorporated through `cf90a64e9`:
wrong-target forwarded STATUS never loops, and only documented pre-fault
transfer BUSY is retried under the existing setup bound. Application errors
are not suppressed. Attempt 04 (`84d6c351c`, explicit TCP) **FAIL** on its first K38 run:
definite PARTIAL now reaches the client correctly, but later flushes return
BUSY/EIO. Full client/daemon logs are copied and hash-verified in
`attempt-04/failed-k38-01/`; the fixture remains for this open investigation.
The remaining focused cases and full gate in that controller were NOTRUN.

An independent five-run K22 campaign on the same candidate **PASS** with
63,927 successful writes (13,560; 12,749; 13,173; 12,596; 11,849), leadership
transfer at the original three-quarter backlog and no application errors,
including inside the pressure window. Evidence is
`k22-84d6c351c/`. These are availability counts: the K22 script does not
read back each payload. No full-payload claim follows from them.

`537bc94e1` adds failure-only REPORT/slot timing and exact failing writer
syscall labels; it does not change publication semantics or deadlines.
Attempt 05 **PASS** on ten K38 TCP repetitions, each checking all 4,800
records through both mounts and after remount. It enabled `EFS_RPC_PROF`,
which can affect scheduling, so this is diagnostic evidence, not closure
of the earlier unprofiled failure. Attempt 06 mistakenly sets that variable
to `0`, which still enables this presence-based trace switch; its
`K38-UNPROFILED` stage label is therefore wrong. Attempt 07 explicitly
unsets the variable and **PASS** on ten K38 repetitions, 48,000 written
records total, every record checked through both mounts and after remount.
Two background publishers returned PROTO and retained their dirty work;
subsequent publication/readback completed. These diagnostic errors are not
application failures, but remain recorded rather than called error-free.
No product behavior changed between the attempt-04 failure and these later
passes; the residual failure's cause is not yet established.

The final node9901 controller has passed two more D4 progress and two
plateau repetitions on explicit TCP, plus the explicit RDMA authority cases.
Its full eight-job gate is now running.

| Additional TCP D4 check | Foreground ops | Max write/fsync | Drain (limit 150 s) | Exact files |
| --- | ---: | ---: | ---: | ---: |
| Progress 1 | 1,395 | 0.136 s | 103.9 s | 34,032 |
| Plateau 1 | 1,003 | 0.217 s | 47.9 s | 1,056 |
| Progress 2 | 1,457 | 0.145 s | 112.1 s | 35,360 |
| Plateau 2 | 1,009 | 0.943 s | 46.3 s | 1,056 |

All original per-operation, memory, backlog and drain assertions passed.
The RDMA authority run verified both exact fresh-reader payloads; closes
took 0.0041/0.0026 s within the unchanged 30 s limit (whole test 14.24 s).
Separate explicit RDMA
K38/D4 repetitions and strict K22 cold readback are queued after that gate.
Nuc became free when Task I finished; its original-host eight-job gate now
runs from `b807294be` in `/home/maik/efs-taskH-repair-20261027/src`, with
stable evidence `/home/maik/efs-artifacts/taskH-repair-20261027`.

`3b7ea7124` incorporates reviewed Task G test-only fix `4bcd52ca0`: a
256-byte cleanup command truncated on nested TMPDIRs; its buffer now covers
all three paths and checks `snprintf`. `b807294be` makes every K22 client
error fail (including before leadership moves), requires stopped workers
and orderly child cleanup, and records per-writer confirmed sequence bounds
plus binary hashes. `scripts/k22_verify_retained.py` (`b8950164d`) restarts
only matching private stores on unoccupied recorded endpoints, verifies
cluster identity and every confirmed payload through a fresh client, and
fails on cleanup errors. This is separate from the availability gate; no
cold-readback PASS has been claimed yet.

The node9901 full gate uses product `537bc94e1` plus the committed test-only
harness overlay through `b807294be`, saved as
`final-537bc94e1/harness-overlay-b807294be.patch` with file hashes. The
standalone verifier is pinned separately to `b8950164d`. Nuc builds the
whole committed tree `b807294be` with that verifier update separately hashed.
No uncommitted code from another session is used.

No final deployment, completed repeat full gate, nuc pooled requalification,
or real-cluster fixed-build qualification is claimed yet.

## Reproducing the qualification

The committed driver replaces the earlier copied host launchers. From a
fresh exact-source Linux archive, set `EFS_GIT_ID`/`EFS_GIT_DESCRIBE` to its
commit and `EFS_GIT_BRANCH=agent/task-h-repair`, then run:

```sh
python3 scripts/qualify_publication_repair.py --profile nuc \
  --root "$fresh_private_root" --output "$fresh_stable_evidence"
```

The root/output must be absent and outside the source tree. This builds all
prerequisites, checks native regressions, runs three rounds of four
concurrent targets (K38, both D4 cases and K22; four FUSE writers plus four
metadata producers in K22), retains their fixtures, cold-reads every
recorded D4/K22 payload, proves the actual leader log watermark, then runs
the full eight-job suite. The original D4 45/75 s loads, eight writers,
1/2 GiB per-store quotas, 10 s foreground and 150 s drain bounds stay.
K38 keeps 24 writers/two mounts/4,800 records. K22 keeps its original
capacity, transfer, drain and join bounds. No instrumentation mode changes
product behavior. Failures remain failures; full exit 3 is INCOMPLETE.

On node9901, set the private libfuse prefix/pkg-config/library paths given
below, `ASAN_OPTIONS=detect_leaks=0` (that host's broken runtime only), and
use `--profile rdma`. That profile adds dirty and clean slow-member gates,
three RDMA K38 repetitions and RDMA D4 on `mlx5_0`, followed by TCP K22,
exact cold reads and the full suite. All clusters are private three-daemon
loopback fixtures on one host, not a multi-host/fabric-wide release gate.
Nothing formats/stops an existing cluster. Keep failed bulk for this open
investigation; no automatic directory pruning is performed by the driver.


Build a fresh archive of the stated commit on a Linux FUSE host with fuse3,
RDMA development libraries, Python 3.9+ and a private scratch directory. Use
`make -j8 all tests/faults/efsd` (node9901 additionally needs the private
libfuse prefix/rpath described in Task H). Set the build ID/branch to the
archived commit when building outside Git. Keep every run's output and
TMPDIR separate:

```sh
env -u EFS_RPC_PROF EFS_TRANSPORT=tcp EFS_K38_KEEP=1 TMPDIR="$private_run" \
  python3 tests/test_k38_shared_chunk_pressure.py
env -u EFS_RPC_PROF EFS_TRANSPORT=tcp EFS_D4_KEEP=1 TMPDIR="$private_run" \
  python3 tests/test_d4_progress.py
env -u EFS_RPC_PROF EFS_TRANSPORT=tcp EFS_D4_KEEP=1 TMPDIR="$private_run" \
  python3 tests/test_d4_plateau.py
env -u EFS_RPC_PROF EFS_TRANSPORT=tcp TMPDIR="$private_run" \
  python3 tests/test_k22_leader_admission.py --keep
python3 scripts/k22_verify_retained.py --confirmed "$confirmation" \
  --owned-root "$private_root" --output "$new_evidence_directory"
EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8 EFS_TRANSPORT=tcp \
  bash scripts/make_test_summary.sh
```

Use explicit `EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0` on node9901 for the
separate K38/D4 RDMA runs; the K22 pressure test itself is TCP. Defaults
retain the original quotas, concurrency, wall-time and acceptance bounds.
The full runner's summary may be FAIL or INCOMPLETE; NOTRUN is never PASS.
Node9901's known broken LeakSanitizer runtime is excluded there via
`ASAN_OPTIONS=detect_leaks=0`; nuc has no such override. Keep fixture bulk
only while this investigation remains open. Before pruning with `edelete`,
copy closed logs/results to stable evidence, hash-verify the copies and
record what replay capability the pruned stores remove. Never clean a
fixture while its clients/daemons still run.
