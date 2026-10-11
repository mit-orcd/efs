# AMD sustained tiny/large service gate — Oct 11

W94's finite CREATE adoption A/B and full normal/fault/direct/buffered gates
pass on the [captured integration](amd-integration-20261011.md). This follow-up
targets sustained service, without claiming full production session/PUT-ticket
integration or selecting a new storage format.

The existing private four-node TCP fixture runs direct then buffered, 600
seconds each. Eight tiny workers rotate 64 names per worker, writing 512-byte
generation-specific bodies. Every acknowledged generation is byte-checked
before unlink; names are reused only after unlink. Four large writers overlap
16-MiB rings of 1-MiB writes, fsync every four writes and at completion. Thirty-
second windows record CREATE/close/delete completions and latency, with process
CPU ticks and RSS samples. Explicit per-daemon quota is 4 GiB; installed data
and daemons are retained. This measures the accepted combined-adoption build;
it is not a new same-source A/B throughput claim.

After each timed interval, the private servers crash and restart. All 512 tiny
survivors and all four large files must match exact expected bytes. Unlink must
then produce both empty GC queues and empty physical fragment inventories.
The harness drains/restores the two installed mounts with TCP and commands off.
No RDMA, D25 activation, production packing or pushes are part of this gate.

[Reproducible runner](../../results/measure/20261011-amd-sustained/run.sh).
Acceptance and remaining blockers will be recorded after the actual verdicts.

## W95 retained AMD experiment review completed

All 24 balanced samples / 184,320 operations are reconciled against the raw
CHURN_OK logs; every captured field and expected model/cohort/mixed/repetition
cell matches. Indexed manifest accounting independently matches the format:
48-byte header + 128 × 80-byte entries + 32-byte checksum = **10,320 bytes**
per publication. Cohort 1 writes 10,320 index bytes per operation; cohort 32
writes 322.5. Those index writes are additional to body writes, not payload.
[Reproducible audit](../../results/measure/20261011-amd-sustained/review-w95.py) /
[derived review](../../results/measure/20261011-amd-sustained/w95-reviewed.json).

| Cohort | Payload mix | Files mean ops/s | Indexed mean ops/s | Indexed maximum sampled allocation |
|---|---|---:|---:|---:|
| 1 | tiny | 1,790 | 756 | 540,672 B |
| 1 | tiny + 64 KiB | 1,781 | 753 | 1,130,496 B |
| 32 | tiny | 4,290 | 27,030 | 53,248 B |
| 32 | tiny + 64 KiB | 4,259 | 25,284 | 708,608 B |

Source review confirms body fsync and parent durability precede checksummed
manifest fsync/rename/parent durability, with ACK afterward. A failed writer
cannot continue publication/collection before reopening; writable recovery
stabilizes validated authority before collecting. Generation-conditional
updates, root identity and live segment references prevent stale deletion and
neighbor reclamation in this independent model. Existing actual-source fault
and process-exit regressions passed in the captured full native AMD gate.

The experimental review obligation is complete. Carry forward immutable bodies,
versioned checksummed authority, cohort durability accounting and reference-
based reclamation. Do **not** port the full-snapshot index as a scaling claim:
its index cost grows linearly with capacity even for one changed object. At its
65,536-entry limit, a snapshot is 5,242,960 bytes, before body writes. A bounded
journal or equivalent delta format needs its own recovery/compaction gates.
The current index is serial and not a distributed production session/GC ledger.
Sampled allocation excludes transient manifest replacement; pre-publication
admission is a separate check. Process crashes do not prove hardware power-loss
safety. W95 stays open for mounted/shared-object production integration and P4.1
format/compatibility decisions, already assigned to ORCD; no packing is enabled.
