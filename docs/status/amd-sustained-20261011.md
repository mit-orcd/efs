# AMD sustained tiny/large service gate — Oct 11

W94's finite CREATE adoption A/B and full normal/fault/direct/buffered gates
pass on the [captured integration](amd-integration-20261011.md). This follow-up
targets sustained service, without claiming full production session/PUT-ticket
integration or selecting a new storage format.

The existing private four-node TCP fixture runs direct then buffered, 600
seconds each. Eight tiny workers rotate 64 names per worker, writing 512-byte
generation-specific bodies. Every acknowledged generation is byte-checked
before unlink; names are reused only after unlink. Four large writers overlap
16-MiB rings of 1-MiB writes, fsync every four writes and at completion. The
follow-up offers 2 MiB/s per writer; the unpaced failure below is retained. Thirty-
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

## Unpaced mixed-load failure — retained, not closed

The initial invocation missed the fixture's required second root argument and
stopped before private nodes started. It restored both mounts; this setup
failure is retained separately. Both reproducible AMD runners now explicitly
supply two parent roots, which may share the same AMD filesystem.

The corrected unpaced run hit ENOSPC at a large writer's fsync/close about one
minute after starting. Explicit per-node logical fragment quota was 4 GiB;
allocated fragment files approached 4.1–4.2 GiB per node including checksum and
allocation overhead. The host still had 389 GiB available. Four logical large
files occupy at most 64 MiB; this is overwritten-generation/GC pressure, not a
full host filesystem. PUT counters reached roughly 5 GB per node and recorded
quota failures. The 30-second sample completed 2,245 tiny closes, before the
failure. It is not a sustained throughput or cold-byte acceptance result.
[Failed run](../../results/measure/20261011-amd-sustained/evidence/unpaced/direct.log).

A separate 60-second cold restart without writers enables actual GC identity,
delete and ACK diagnostics. Per-node sampled allocation falls from
4,484,579,328/4,382,011,392/4,466,475,008/4,404,641,792 bytes to
3,217,346,560/3,019,173,888/3,202,654,208/3,223,613,440. Node 1 removes 18,274
fragments, reports 1,197,604,864 logical payload bytes reclaimed and zero final
delete/checksum/sweep/reap/ACK/scan errors. Committed GC ACKs succeed. This
establishes progress under quiescence, not complete drain, a proven leak cause,
or exact acknowledged-byte recovery of the failed run. Concurrent inventory
samples explicitly count files that disappeared between scan and stat; the
first inventory-race audit attempt is also retained.
[Cold recovery observations](../../results/measure/20261011-amd-sustained/evidence/unpaced/cold-gc-recovery.json).

The unpaced offered load exceeds the observed quiescent reclaim rate. Source
uses bounded GC scans/ACKs, a 200-ms fragment budget per group and one-second
loop; increasing these or adding pressure admission needs fairness/durability
acceptance. No unsafe GC deletion, timeout increase or quota inflation is
adopted. The new 2-MiB/s-per-writer run is a distinct bounded-service gate;
pacing does not repair or close this overload behavior. W94 full sustained
acceptance and W97 pressure/reclamation work remain open.


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

## Opt-in GC scheduling candidate

`EFS_GC_BACKLOG_IDLE_MS=100` is an experimental scheduling candidate; unset
remains the existing 1,000-ms idle. Strict startup validation accepts only
100..1000 milliseconds. A shorter idle is used only after a leader's capped
reap scan and observed physical deletion progress during that pass. Idle,
blocked/offline work and counter reset retain 1-second backoff. All group scan,
fragment/ACK budgets and 20-ms snapshot/shutdown slices remain unchanged; no
collection authority, mutation ordering, quota or durability rule changes.
The actual-source regression covers default, malformed/range configuration,
idle/blocked/reset and progress cases. Full AMD native and candidate live gates
are required before promotion; this round does not change the default.
