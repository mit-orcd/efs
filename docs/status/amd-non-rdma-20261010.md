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

**Planned A/B:** both builds include the coordinator-routing repair. Compare
separate versus combined lease adoption with eight workers, 8192 deterministic
512-byte files, equal synchronous-close semantics and fresh private stores.
Run four alternating repeats for direct and buffered TCP; retain open/close
latency distributions, wall throughput and exact cold bytes/reclamation for
every case. A small case checks committed lease identities and flags directly in the
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
sampled-zero GC counters. Independent stopped-store inspection verifies the
completed controls and first four long cases. A bounded untimed profile shows
background Raft/GC work while the client waits idle; this reclamation phase is
excluded from creation throughput. The first matched pair is provisional;
three additional alternating repeats per mode remain.

Additional live gates are queued for the fault-only consumed combined-adoption
reply loss (failed descriptor cleanup, original inode reclamation, name reuse
and cold bytes), and LOCAL/cross-group HASHED crashes before/after adoption.
Normal and fault-enabled actual-source lease compile/regressions pass; these
new live variants are not yet accepted.
