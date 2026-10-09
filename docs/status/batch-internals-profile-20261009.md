# W93 internal batching and queue measurement — 2026-10-09

Opt-in diagnostic instrumentation (`ebe72fdb`, `9fd7862e`) captures exact internal
WAL coverage, shared hold duration, proposer/inbox depth and host/follower flush
boundaries. Disabled by default; metrics do not control persistence or ACKs.
NUC deployed build identity `9fd7862e-batchobs`, TCP/direct I/O, both mounts and
`.efs` enabled. Five-minute workload: 06:34:13–06:39:13 UTC.

**Finding:** follower backlog is substantial but not readily batchable. n3's
nonempty inbox averages 29.7 messages; 81.9% of host segments end on group
changes. Eligible cross-request follower batches on n2/n3/n4 contain one
request each; 70.3% of n3 and 55.2% of n4 eligible batches end on ineligibility.
Empty and tail-mismatched requests dominate rejection checks. No byte-cap,
entry-cap, admission, term or configuration rejection occurs. Count-cap host
boundaries are only 3.9% on n3 and 0.4% on n4. Per-boundary reject attribution
and behind/ahead classification are not yet measured; do not call all tail
mismatches redundant messages.

Leader arrivals observe only 1.01/1.18 active proposers on n1/n2; mean shared
holds are 12/112 us, with no yield-cap hit. Actual WAL cohorts average
1.01/1.17/2.11/1.41 entries per sync on n1..n4. n3/n4 mean barrier sections are
5.84/5.34 ms; eligible follower assembly is only 12/14 us. Intra-message batching
is separate from cross-request batching; one request is not necessarily one entry.

**Material pressure observation:** n3 logs at least ten inbox-full dropped
replication frames at workload startup. The log suppresses subsequent drops,
so the total is unknown. Investigate pressure/retry traffic and safe coalescing
before increasing limits. No new corruption/fault acceptance is inferred from
this observational run. W93's existing durability contracts and W53's separate
tail gates remain in force; W93 optimization acceptance is not closed here.

Actual rates decline 93.6 → 52.2 → 38.9 files/s. Queue backlog persists.
The restart changes leader/traffic placement, enabled instrumentation has not
been calibrated, and repeated W92 catalog-cap failures add background work:
these are not matched A/B gains or hardware-ceiling evidence.

Raft/store tests pass off/on and an exact 3/1/1 cohort/rotation oracle passes.
Native test recipes complete across the documented staging interruption and
continuation. The loader exits successfully via cooperative file-boundary stop;
all old/new data is preserved. Post-measurement restart disables diagnostics
while retaining direct I/O and `.efs`; consult final-state evidence.

[Raw counters, exact build hashes, methods, validation and limitations](../../results/measure/20261009-batch-internals/README.md).
