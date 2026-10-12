# P4.1 · Container profiling rounds 7–8

**Historical experimental evidence:** container-specific tests and follow-up measurements are no longer required or authorized by this page. [Standing container testing policy](container-testing-policy-20261012.md).

Follow-up to [rounds 1–6](container-profiling-20261008.md), on
`devel-storage-engine` starting at `73b402cd`. Same private xorinox three-VM
Raft group and gateway, TCP/port 27432, fresh roots under `/data1/efs/bench`.
Same workload: 256 × 512 KiB files, four workers, 128 KiB durable overwrites
across 32 worker-disjoint hot files; 15-second read/overwrite diagnostic phases.
All three daemons profiled with cycles at 199 Hz and task-clock/counter stats.
Every case must verify data, restart all daemons and reverify, then delete and
reach zero mappings/allocated container blocks on each node. Installed services
remain running; no shared devel edits or pushes.

## Round 7: remaining duplicated work

Profile the current final build from rounds 1–6 before further changes.
Representative direct node-1 read: payload verification inside container GET
3.27% inclusive cycles, handler-level hashing another 3.12%; mapping reads 4.12%.
Direct overwrite includes `server_container_info` at 0.69% before the backend
reads the same mapping again. These are sampled shares, not absolute savings.

Changes for round 8:

- A storage GET can report an explicit internal VERIFIED checksum state only
  after validating the complete stored payload against its digest and its
  mapping/header. The RPC handler can use this result without hashing it again.
  Files retains handler verification. Partial results report PRESENT; a payload
  stored without a supplied digest reports NONE and remains unavailable through
  the data RPC. This changes no on-disk format or wire protocol.
- Container PUT reads one validated mapping for both quota admission and old
  extent retirement. Quota is reserved before I/O, rolled back before publication
  on failure, and reconciled with the same publication flag afterward. Existing
  fragment lifetime locks cover the whole sequence.

Regression gates cover quota shrink/growth/failure, full/partial/missing digest
states, actual corruption, one mapping read per replacement, concurrent access,
restart, and failure injection. All persistence barriers are retained.

## Validation and results

Both rounds passed (four cases), with zero lost samples in all 48 perf reports.
All eight matched unprofiled cases also passed. The comparison pairs the
pre-round build with the new build, two repeats per mode, ABBA version order
and reversed mode order in the later pair. Read/overwrite phases are 30 seconds.
All ranges include metadata ReadIndex stalls. No engine speedup is claimed
from stalled baseline runs. The final clean Linux build/full suite ran after
the timed runs to avoid adding host load during measurements.

## Round 8: check the changes

The handler's duplicate payload hash disappears from the direct-read call stack;
backend payload verification remains present. The redundant quota mapping read
is removed and asserted by the backend test. No additional safe hot-path change
was justified by the remaining profile.

| Mode/phase | Round 7 daemon CPU ms/MiB | Round 8 daemon CPU ms/MiB |
|---|---:|---:|
| direct read | 21.37 | 21.10 |
| direct churn | 103.44 | 101.08 |
| buffered read | 20.52 | 20.58 |
| buffered churn | 102.18 | 103.42 |

Summed task clock of all three daemons, divided by logical client MiB; includes
profile attachment/finalization overhead. Direct costs fell slightly, buffered
costs did not. This is mixed diagnostic evidence, not a general CPU or throughput
improvement claim. Unprofiled rates and repeated variation determine that claim.

Shared `devel` was observed at `28f1ff04` during this follow-up and contains
separate Raft/metadata work after this branch's base (including `534341af`,
`ec192d17`, and `54e800cd`). It was not merged into this experiment: changing the
common metadata implementation mid-comparison would confound the container-only
result. Current evidence describes this branch snapshot; integration will require
revalidation against the newer Raft code. No conclusion here attributes stalls to
or claims a fix from those newer commits.

The first optimized unprofiled direct case also reproduced the slow metadata
path: read 5.98 MiB/s, overwrite 3.70 MiB/s. Node-1 logs include GETCHUNKS
ReadIndex waits of 404–449 ms with `rc=-13` (busy), followed by client retries.
All content/restart/reclamation gates still passed. Earlier rounds showed the
same pattern on buffered cases; it is not specific to buffered data I/O. These
stalled measurements remain included rather than being replaced by reruns.

## Matched unprofiled results

Ranges across two repeats per version/mode. Read/create/overwrite are logical
client MiB/s; delete is unlink files/s, with physical GC completion checked
separately. All stalled runs are included. Original means the final build from
rounds 1–6 (`73b402cd`), not the initial container implementation.

| Mode | Phase | Before rounds 7–8 | After rounds 7–8 |
|---|---|---:|---:|
| direct | create MiB/s | 8.62–45.55 | 10.96–12.20 |
| direct | read MiB/s | 123.64–136.36 | 5.98–136.81 |
| direct | durable overwrite MiB/s | 22.25–22.28 | 3.70–22.65 |
| direct | delete files/s | 23.28–460.67 | 22.10–516.61 |
| buffered | create MiB/s | 43.24–46.17 | 13.53–44.54 |
| buffered | read MiB/s | 141.71–142.75 | 141.83–143.00 |
| buffered | durable overwrite MiB/s | 21.93–22.35 | 22.72–22.79 |
| buffered | delete files/s | 409.71–499.48 | 360.53–464.96 |

Buffered durable overwrite median rose 22.14 → 22.75 MiB/s (+2.8%); its p99
range fell from 50.19–52.46 ms to 46.83–47.94 ms. Buffered read median is
142.23 → 142.42 MiB/s (essentially flat). These two-repeat results show a small
observed buffered-write gain, not a general engine improvement or confidence
claim. Buffered create/delete results worsened in this sample; direct reads and
writes include a severe stalled optimized run. The healthy optimized direct run
reached 136.81 MiB/s read and 22.65 MiB/s overwrite, but it cannot replace the
stalled repeat in a summary. No direct-mode improvement is established.

The two changes demonstrably remove repeated work and pass correctness gates.
Whole-daemon CPU normalization and end-to-end performance remain mixed on these
shared VMs; files stays the default. Compare again after integrating current
devel's independent Raft/metadata changes before ranking production engines.

[Raw results, source identities and reports](../../results/measure/20261008-container-rounds7-8/README.md)
retain all per-run values and p99 latencies. The clean Linux build and full test suite passed with zero compiler warnings.

## Final qualification

`make clean && make -j4 all && make test` passed with zero compiler warnings,
including the final regression harness. Twelve additional VM cases passed
(four diagnostic and eight matched), with zero client errors and complete
integrity/restart/physical reclamation checks. All 48 perf reports have zero
lost samples. Installed-service PID/command snapshots are unchanged.
The final runtime differs from the measured round-8 source only in comments;
final test code makes its instrumentation counter atomic and build rules track
the shared checksum-state header. Exact sources and hashes are archived.
Documentation gate passes all five checks. Files stays default; containers
remains experimental. No push or shared-devel modification.
