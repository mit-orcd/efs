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
errno EIO). That failure is retained and under investigation; it is not an
accepted buffered matrix. Both installed mounts were restored successfully.

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
native suite also passes; broader live candidate acceptance is running.

[Buffered regression](../../results/measure/20261010-amd-three-hour/evidence/buffered-mkdir-race.log),
[direct regression](../../results/measure/20261010-amd-three-hour/evidence/direct-mkdir-race.log),
[native suite](../../results/measure/20261010-amd-three-hour/evidence/adopt-native.log).
This closes the reproduced placement defect, not W66's D36 protection,
independent leadership, migration or scale obligations.
