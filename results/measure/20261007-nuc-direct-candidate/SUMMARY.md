# NUC experimental direct-I/O storage comparison — 2026-10-07

## 1. Purpose and linked item

Private NUC experiment comparing the current engine store write path against a
candidate that preserves direct extents (exact length via ftruncate after each
completed write; benchmark-only O_SYNC mode also fsyncs final length). No
deployment or production-code change; private source restored afterward.
Linked item: **unlinked** to a numbered W/D gate — added by commit `a0e88e2df`
"results: isolate experimental direct storage path on nuc"; it is the direct-I/O
follow-up to the writer investigation (see `../20261007-sync-writer-investigation/`).

## 2. Verdict

**PASS** as a measurement (all 40 runs pass; actual-store unit test passes with
the direct-never-ftruncate assertion relaxed for the candidate). As an adoption
decision the verdict is **reject**: do not adopt this candidate generally —
synchronous NVMe throughput falls ~42%.

**Measurement caveat (Oct 7):** these short bounded-write runs mixed initial
file creation and replacement. Per-run p99 averages below are historical
summaries, not pooled p99. The
[corrected latency study](../20261007-latency-validation/SUMMARY.md) separates
population from timing and retains every observation. Do not use these older
tail figures as steady-state acceptance evidence.

## 3. Source commit and build identity

Not recorded as a commit in the run. The candidate exists only as
`experimental-store.patch` (diff of `src/server/store.c`, current vs candidate
trees); both binaries were private NUC builds. No deployed-sha or binary hashes
recorded.

## 4. Environment

Single NUC host, two storage roots on distinct media: root1 = SATA, root2 =
NVMe. Engine `data` mode via efs-bench, direct I/O, QD16, auto (4) writers,
64 KiB fragments, 64 MiB overwrite window, 1.5 s phases, 5 repeats per cell.

## 5. Method

40 randomized untraced writes: current/candidate × SATA/NVMe × O_SYNC on/off,
order randomized, raw per-run metrics in `results.json`. Separate wall-clock
strace reruns of the NVMe O_SYNC case for current and candidate
(`current-nvme-sync-wall-strace.txt`, `candidate-nvme-sync-wall-strace.txt`).
The actual-store unit test was run against the candidate for buffered/direct/
bounce tails, grow/shrink/empty writes, failure injection, O_SYNC and create
guards; only the "direct never calls ftruncate" assertion was removed.

## 6. Results

| Storage/sync | Current GiB/s | Candidate GiB/s | Mean p99 current/candidate µs | Worst max current/candidate ms |
|---|---:|---:|---:|---:|
| SATA/off | 0.401 | 0.403 | 4903 / 5138 | 28.3 / 27.6 |
| NVMe/off | 2.073 | 2.125 | 1418 / 1135 | 73.2 / 68.7 |
| SATA/on | 0.303 | 0.313 | 4809 / 4285 | 27.0 / 26.3 |
| NVMe/on | 0.036 | 0.021 | 59523 / 69010 | 89.5 / 78.3 |

SATA non-sync is essentially unchanged. Non-sync NVMe gains ~2.5%; current
throughput CV 5%, candidate 1.3%, so longer tests are needed to establish a
small benefit confidently. Synchronous NVMe throughput falls ~42% with higher
p99. Wall-clock strace on separate current/candidate NVMe O_SYNC reruns
confirms current writev ~6.30 ms; candidate writev ~5.48 ms plus one fsync
~5.11 ms per write. Candidate ftruncate ~10 µs. The extra persistence fence is
therefore a strong explanation for the synchronous regression, rather than
admission. The traced rates are diagnostics, not the untraced acceptance
samples.

## 7. Limitations and remaining gates

Do not adopt this candidate generally. Next possible experiment: avoid
redundant same-size final truncation/sync while enforcing exact shrinking
tails and all error/durability semantics. Benchmark acceptance remains separate
from production adoption. These bounded tests do not establish sustained
large-dataset throughput. Tail figures superseded by the corrected latency
validation method (caveat above).

## 8. Evidence index

| sha256 (16) | File |
| --- | --- |
| 70b9acd629a574e4 | results.json — all 40 randomized runs with per-op metrics |
| 1d35cb0447168ad4 | experimental-store.patch — candidate store.c source diff |
| 783d793b12c64747 | candidate-nvme-sync-wall-strace.txt — candidate O_SYNC syscall profile |
| 86a3bd02fd7bdaf7 | current-nvme-sync-wall-strace.txt — current O_SYNC syscall profile |
