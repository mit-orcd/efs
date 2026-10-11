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
