# Direct-I/O follow-up — three rounds, Oct 7

**Measurement caveat (Oct 7):** these short bounded-write runs mixed initial file creation and replacement. Per-run p99 averages below are historical summaries, not pooled p99. The [corrected latency study](../20261007-latency-validation/SUMMARY.md) separates population from timing and retains every observation. Do not use these older tail figures as steady-state acceptance evidence.

Round 1: eight matching original-candidate/current perf profiles and eight new
strace runs (each root, sync on/off), direct write only, auto FOUR writers,
QD16, bounded 64 MiB, 64 KiB fragments, 1.5-second diagnostic phases. All pass.
Non-sync NVMe current profile samples XFS leaf-key work 6.88% and extent-busy
trim 3.79%; original candidate removes these leading allocation symbols.
Approximate sampled cycle counts normalized by profiled operations: current
9.09 billion / 53041 ~= 171k cycles/op; candidate 2.79 billion / 51957 ~= 54k.
This single-capture CPU reduction is not a reliable throughput prediction.
Synchronous profiles have only 29–34 cycle samples on NVMe: percentages there
are too sparse for precise hot-path rankings. Default strace -c is CPU time.

Round 2: previously retained wall-clock strace demonstrates original candidate's
extra fsync (~5.1 ms per synchronous NVMe write), beyond writev (~5.5 ms).
That supports the ~42% synchronous throughput penalty, independently of admission.

Round 3: private conditional-length candidate. Direct writes retain extents,
fstat completed length, and truncate only an older longer tail. O_SYNC already
persists the write; only a subsequent length change receives another fsync.
Buffered path and fair admission unchanged. Forty fresh randomized untraced
runs, five repeats per variant/root/sync, same geometry: all pass.

| Storage/sync | Current GiB/s | Conditional GiB/s | Mean p99 current/conditional µs | Worst max current/conditional ms |
|---|---:|---:|---:|---:|
| SATA/off | 0.412 | 0.412 | 4887 / 4063 | 26.3 / 26.1 |
| NVMe/off | 2.148 | 2.122 | 1339 / 1169 | 4.4 / 70.4 |
| SATA/on | 0.306 | 0.351 | 4738 / 4342 | 26.6 / 29.0 |
| NVMe/on | 0.034 | 0.034 | 78609 / 72449 | 99.7 / 150.2 |

The conditional version removes the prior sync penalty and improves SATA sync
throughput ~15%. No non-sync throughput gain is established. NVMe maximum tails
worsen in individual samples, so this is not blanket adoption evidence.
Final separate wall-clock strace: 1042 writes, writev average 5709 us; no
ftruncate/fsync calls for this fixed-size diagnostic. This verifies the redundant
length/barrier work is absent; it does not establish device cache behavior.

Expanded actual-store tests pass: body/checksum and padded direct/bounce images,
grow/shrink, same-size barrier omission, shrinking fsync failure, fstat/truncate
failure, empty writes and create guards. The first added empty-direct fixture
incorrectly expected zero bytes; existing direct format has a minimum 4 KiB
zero-padded page. Correcting that expectation preserves the existing layout.
Direct candidates remain private; production code/deployment unchanged and
private store source restored. Longer matched runs and xorinox validation are
still required before proposing production adoption.
