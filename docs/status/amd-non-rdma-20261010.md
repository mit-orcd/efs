# AMD non-RDMA closure round — Oct 10

Branch `devel-amd` starts at devel `746ae9f0`. The AMD cluster is exclusively
reserved by the user. This round uses TCP, preserves the installed stores and
keeps `.efs` commands disabled. RDMA acceptance is outside its scope.

## W89

**Closed: measurement correctness.** The fresh fault-enabled build
`cae522ff038a-fault` passed the production-cap gate and raw-field comparison.
Normal and fault-enabled full native suites also passed before this fixture.

| Observation | Fresh result |
| --- | --- |
| Acknowledged producer writes | 2240 × 1 MiB |
| Follower pressure | unchanged 1 GiB L0 cap reached |
| Healthy quorum | continued admitting acknowledged writes |
| Recovery | InstallSnapshot; exact-value checks after private cold restart on all three nodes |
| FUSE oracle | passed |
| TSV/raw agreement | 276 complete observations, every field equal |
| Unavailable counter | `apply_sleep_us` remains `NA` |
| Installed client restoration | passed |

The driver requires `raw-proof` after `cap-proof`. Regressions reject valid-width
but shifted numeric rows, missing/extra observations and unavailable values
invented as zero. The old Oct 5 malformed samples and their withdrawn
interpretation remain preserved. D30/W23 memory/lag bounds, production policy
activation and permanent-stall throughput are separate open obligations.

Evidence: [gate log](../../results/measure/20261010-amd-three-hour/evidence/pressure.log),
[raw observations](../../results/measure/20261010-amd-three-hour/evidence/pressure-current/samples.jsonl),
[TSV](../../results/measure/20261010-amd-three-hour/evidence/pressure-current/samples.tsv),
[build identity](../../results/measure/20261010-amd-three-hour/evidence/pressure-build-identity.txt),
[binary hashes](../../results/measure/20261010-amd-three-hour/evidence/pressure-binary-sha256.txt).

## Acceptance checkpoint

At the initial `8cb41c929ca0` baseline, direct/TCP passed all 216 core and 67 peer
POSIX cases plus the private cold/replay/physical-GC gates. Buffered/TCP passed
216 core cases but failed the concurrent peer MKDIR race (66/67 peer cases,
errno EIO). That failure is retained as the pre-fix result. The routing repair below and
the later combined candidate both pass the buffered matrix. Both installed
mounts were restored successfully.

## W66 — unresolved namespace coordinator routing

The baseline buffered peer MKDIR EIO reproduced in multiple isolated fixtures.
Instrumentation localized it to the preceding name LOOKUP, before a MKDIR RPC.
A prepared namespace transaction needed a coordinator group absent from the
serving replica; `host_txn_coord` correctly refused to invent a decision, but
the read handler had no way to distinguish placement from a hosted I/O error.
The competing client then waited at its next barrier, explaining the roughly
30-second suite duration rather than a 30-second failed mutation.

A per-read context now records only the unhosted coordinator condition.
LOOKUP/GETATTR and inode-lane reads forward that case to a host covering both
current groups. Hosted ReadIndex/storage failures keep their error semantics;
no unknown decision becomes absence. Default placement and quorum are unchanged.

Focused actual-source regressions pass for pending dentry/attribute routing,
true absence, captured-name replacement and genuine hosted I/O failure.
The unchanged legacy coordinator-authority regression still passes.
The repaired private fixture passes **64 competing mkdir races per mode** and
verifies all directories after cold restart, followed by physical reclamation.
Both installed mounts restore successfully. The combined candidate's full
native suite also passes; broader direct/buffered live candidate acceptance has also passed.

[Buffered regression](../../results/measure/20261010-amd-three-hour/evidence/buffered-mkdir-race.log),
[direct regression](../../results/measure/20261010-amd-three-hour/evidence/direct-mkdir-race.log),
[native suite](../../results/measure/20261010-amd-three-hour/evidence/adopt-native.log).
This closes the reproduced placement defect, not W66's D36 protection,
independent leadership, migration or scale obligations.

## W94 — combined first-descriptor adoption candidate

The first retained CREATE descriptor can use one existing guarded
`OPEN | CLEAR_PENDING` edge instead of separate guarded OPEN and pending-clear
requests. An existing normal descriptor still uses only CLEAR_PENDING.
The inode stripe, immutable request identity, inherited deadline and retained
ambiguous ownership remain in force; cleanup resolves the combined receipt
before submitting CLOSE. This changes no wire format or protection policy.

Focused actual-source tests cover the combined edge, existing descriptor,
lost reply/reopen barrier, unallocated admission pressure and inherited expiry.
The server batch regression checks atomic effect/verdict publication, retry
with a cleared token, changed-payload rejection and final-close removal.
Full native tests and the direct/buffered TCP core (216/216), peer (67/67), sync, replay and physical-GC acceptance matrix pass. Actual-source ownership checks pass ASan/UBSan and
Valgrind (20 allocations/frees, zero errors and leaks). Native-ISA server
Memcheck stopped at an unsupported AVX-512 instruction; that attempt is not a
passed server Valgrind gate. The portable actual-source ownership test supplies
the successful Memcheck result.

**Completed A/B:** both builds include the coordinator-routing repair. Four
alternating repeats per TCP I/O mode compare separate and combined adoption,
eight workers and 8192 deterministic 512-byte files per case, equal synchronous
close semantics and fresh private stores. All 16 cases pass exact cold bytes
and physical reclamation (131072 files). A small case checks committed lease identities and flags directly in the
CRC-validated Raft log, bounded by each group’s persisted applied index. These
are finite creation workloads, not a universal sustained-service throughput
bound. W94's full integration and outstanding lifecycle gates remain open.

The finite benchmark's first baseline case used serial cold verification;
subsequent cases use eight independent verification workers to shorten the
untimed oracle phase. Every deterministic file is still checked before unlink,
followed by physical reclamation. This does not change or extend the timed
creation interval; cold verification throughput is not used as an A/B result.

The completed 64-file durable-log comparison proves **192 creation/close lease
commands for separate adoption versus 128 for combined adoption** (three versus
two per file). Every baseline file has OPEN, CLEAR_PENDING and CLOSE; every
candidate file has OPEN|CLEAR_PENDING and CLOSE. Both subsequently add the same
OPEN/CLOSE pair for the cold byte oracle. This counts distinct committed Raft
entries, not replicas, retries or log lines. Successful deferred lease edges
return before the ordinary server log message, so text logs alone cannot prove
this reduction. [Decoded committed identities and applied bounds](../../results/measure/20261010-amd-three-hour/lease-consensus-proof.json).

The early-exit tiny fixture now requires actual physical absence alongside
sampled-zero GC counters. Independent stopped-store inspection verifies all 16 completed long cases
and both controls, including absent object and checksum files. A bounded untimed profile shows
background Raft/GC work while the client waits idle; this reclamation phase is
excluded from creation throughput.

| TCP mode | Separate files/s median (min–max) | Combined files/s median (min–max) | Median paired gain | Open p99 ms separate → combined | Close p99 ms separate → combined |
| --- | --- | --- | --- | --- | --- |
| Direct | 280.0 (277.2–282.3) | 330.8 (323.9–343.6) | +18.7% | 15.98 → 7.12 | 110.57 → 102.95 |
| Buffered | 274.6 (270.8–278.3) | 337.1 (333.6–340.4) | +23.3% | 14.19 → 9.31 | 113.71 → 96.16 |

Latency values are medians of per-case percentiles, not pooled percentiles.
Throughput CV is 0.65%/2.35% for direct separate/combined and 1.06%/0.72% for
buffered separate/combined. Maximum sampled single-process RSS is 42848/43132
KiB direct and 42892/43148 KiB buffered; these are not whole-host bounds.
All production C/header sources outside FUSE adoption and non-identity compiler
flags match; fault injection is disabled in the performance builds. Every
variant keeps its executable hashes unchanged across repeats. This finite
creation gate does not establish sustained-service performance.
[Complete observations and verification](../../results/measure/20261010-amd-three-hour/evidence/w94-ab/comparison.json).

The fault-enabled native suite passes on `9c7c7a28-retained-combined-fault`.
The new **direct/TCP** live scenarios pass retained CREATE committed-reply loss
and BUSY retry with identical identity/inode; combined-adoption consumed reply
loss; and a durably negative combined verdict. Both combined cases release the
failed descriptor's original inode, allow name reuse and verify exact cold bytes
and reclamation. LOCAL and cross-group HASHED pending CREATE cases pass crashes
before and after combined adoption, exact replay, CLOSE and physical reclaim.

The complete live matrix **did not pass**: the direct run subsequently aborts
in `lease_edges --legacy-guard` while PREPARE attempts to insert an absent
CLOSE_STEP marker (`owner_rpc` retry budget exhausted). The available evidence
does not establish whether this is retained cleanup/placement or a fixture
issue. Buffered follow-up was consequently not started. Keep the legacy guard
failure and the full two-mode failure matrix open; individual earlier passes
are not a substitute. [Full failed run and partial passes](../../results/measure/20261010-amd-three-hour/evidence/adopt-fault/direct.log).

The initial attempt failed closed because its legacy lost-CREATE-reply hook
never exercised current retained CREATE framing. That attempt is preserved;
`9c7c7a28` adds the fault-only hook to actual retained framing and the rerun
exercises all three stages. Neither failed attempt is presented as acceptance.
[Initial framing mismatch](../../results/measure/20261010-amd-three-hour/evidence/adopt-fault-unexercised-legacy-marker/direct.log).

### Negative combined verdict follow-up

Review reproduced a new-path ownership bug: a durably refused combined edge
could remain marked as leased. The actual-source regression fails against the
first candidate and passes after the repair, including ASan/UBSan. Negative
proof, whether immediate or learned on a later probe, retains the
pending token, allocates a distinct CLEAR_PENDING identity using CREATE's
captured FileID, then retires the known negative edge. Admission failure keeps the old body; neither path sends a
fictional CLOSE. Unknown and successful receipts retain their existing rules.

The direct/TCP fault gate corrupts only the guarded request's FileID before send,
so the actual server records STALE while the caller still owns CREATE's
correct generation/token. It checks terminal proof, original-inode reclamation,
name reuse and cold bytes. This deliberately invalid request is not a claimed
production race. The new server-batch regression passes in the full fault-enabled native
suite; the targeted direct/TCP live scenario passes. The later legacy guard
failure still blocks full live acceptance and buffered follow-up. The performance builds remain frozen at the earlier
combined candidate; this later failure-path repair is separately gated.

### Final handoff

The requested timed work window has ended. Remaining priority is to diagnose
the legacy guarded-lease PREPARE timeout, then rerun the complete direct and
buffered fault matrix. Sustained-service acceptance and full session/data-plane
integration remain separate W94 obligations; W66's broader scaling and D36
decisions remain open. No RDMA work, shared devel changes or pushes were made.

The final read-only restoration audit confirms the original four installed
daemons and both installed TCP clients, `.efs` commands disabled, mounted
paths accessible and no private test daemons remaining.
[Restoration proof](../../results/measure/20261010-amd-three-hour/evidence/restoration-proof.json).

A clean isolated normal build (`EFS_FAULTS=0`,
`9c7c7a28-final-normal`) passes `make -j8 all test`. The first queued attempt
refused to start after the live matrix failed; that refusal is preserved as
`prior-live-gate-wait.exit=1`. The independent native rerun, after restoration,
completes with `native.exit=0` and `round.exit=0`; it does not override the
failed live verdict.
[Normal native log](../../results/measure/20261010-amd-three-hour/evidence/final-native/native.log).
