# Nuc experimental direct-I/O storage comparison — Oct 7

**Measurement caveat (Oct 7):** these short bounded-write runs mixed initial file creation and replacement. Per-run p99 averages below are historical summaries, not pooled p99. The [corrected latency study](../20261007-latency-validation/SUMMARY.md) separates population from timing and retains every observation. Do not use these older tail figures as steady-state acceptance evidence.

40 randomized untraced writes, five repeats each: current/candidate × SATA/NVMe
× O_SYNC on/off. QD16, auto FOUR writers, 64 KiB fragments, 64 MiB overwrite
window, 1.5-second phases. Both binaries use the same fair admission and current
buffered implementation. Candidate alone preserves direct extents, establishes
exact length with ftruncate after each completed write, and in O_SYNC diagnostics
also fsyncs final length. This matches the earlier candidate's direct strategy,
with a synchronous final-length fence for the new benchmark-only O_SYNC mode.
Only private nuc source/build modified, no deployment or production-code change.
Both source syscall paths remain in experimental-store.patch for reproducibility.

All 40 runs pass. The actual-store test passes for buffered/direct/bounce tails,
grow/shrink/empty writes, failure injection, O_SYNC and create guards. Its
assertion that direct never calls ftruncate was removed for this private candidate;
content/length/error assertions were retained. Private source restored afterward.

| Storage/sync | Current GiB/s | Candidate GiB/s | Mean p99 current/candidate µs | Worst max current/candidate ms |
|---|---:|---:|---:|---:|
| SATA/off | 0.401 | 0.403 | 4903 / 5138 | 28.3 / 27.6 |
| NVMe/off | 2.073 | 2.125 | 1418 / 1135 | 73.2 / 68.7 |
| SATA/on | 0.303 | 0.313 | 4809 / 4285 | 27.0 / 26.3 |
| NVMe/on | 0.036 | 0.021 | 59523 / 69010 | 89.5 / 78.3 |

SATA non-sync is essentially unchanged. Non-sync NVMe gains ~2.5%; current
throughput CV 5%, candidate 1.3%, so longer tests are needed to establish a small
benefit confidently. Synchronous NVMe throughput falls ~42% with higher p99.
Wall-clock strace on separate current/candidate NVMe O_SYNC reruns confirms
current writev ~6.30 ms; candidate writev ~5.48 ms plus one fsync ~5.11 ms per
write. Candidate ftruncate ~10 µs. The extra persistence fence is therefore a
strong explanation for the synchronous regression, rather than admission.
The traced rates are diagnostics, not the untraced acceptance samples.

Do not adopt this candidate generally. Next possible experiment: avoid redundant
same-size final truncation/sync while enforcing exact shrinking tails and all
error/durability semantics. Benchmark acceptance remains separate from production
adoption. These bounded tests do not establish sustained large-dataset throughput.
