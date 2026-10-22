# Direct-I/O follow-up — three rounds, 2026-10-07

## 1. Purpose and linked item

Follow-up to `../20261007-nuc-direct-candidate/`: explain the original
candidate's synchronous-NVMe regression and test a refined "conditional-length"
candidate (retain direct extents, fstat completed length, truncate only an
older longer tail; fsync only when length actually changes). Linked item:
**unlinked** to a numbered W/D gate — added by commit `c54dfbda1` "results:
investigate direct extent retention and redundant sync costs". Private NUC
experiment; production code/deployment unchanged.

## 2. Verdict

**PASS** as a measurement: all profiled, traced and 40 fresh randomized
untraced runs pass; expanded actual-store tests pass. As an adoption decision:
**not established** — the conditional candidate removes the prior sync penalty
and improves SATA sync throughput ~15%, but worsens some NVMe max tails, so it
is not blanket adoption evidence; longer matched runs and xorinox validation
still required.

**Measurement caveat (Oct 7):** these short bounded-write runs mixed initial
file creation and replacement. Per-run p99 averages below are historical
summaries, not pooled p99. The
[corrected latency study](../20261007-latency-validation/SUMMARY.md) separates
population from timing and retains every observation. Do not use these older
tail figures as steady-state acceptance evidence.

## 3. Source commit and build identity

Not recorded as a commit in the run. Candidates exist only as private patches
(`conditional-store.patch` against `src/server/store.c`) with the added unit
coverage in `conditional-store-test.c`; binaries were private NUC builds, no
hashes recorded. Private store source was restored afterward.

## 4. Environment

Single NUC host; root1 = SATA, root2 = NVMe. Engine `data` mode via efs-bench,
direct write only, auto FOUR writers, QD16, bounded 64 MiB window, 64 KiB
fragments, 1.5 s phases.

## 5. Method

Three rounds:

- **Round 1**: eight matching original-candidate/current perf profiles and
  eight new strace runs (each root × sync on/off); profiles under
  `{candidate,current}-root{1,2}-sync{0,1}/` (flat.txt, cpu-strace.txt,
  perf-diagnostic.txt).
- **Round 2**: reuses the wall-clock strace retained in
  `../20261007-nuc-direct-candidate/` to attribute the sync penalty to the
  candidate's extra fsync.
- **Round 3**: conditional-length candidate; 40 fresh randomized untraced runs
  (5 repeats per variant/root/sync, same geometry) in `results.json`; final
  wall-clock strace `conditional-nvme-sync-wall-strace.txt`. Expanded
  actual-store tests (body/checksum and padded direct/bounce images,
  grow/shrink, same-size barrier omission, shrinking fsync failure,
  fstat/truncate failure, empty writes, create guards).

## 6. Results

Round 1: non-sync NVMe current profile samples XFS leaf-key work 6.88% and
extent-busy trim 3.79%; the original candidate removes these leading
allocation symbols. Approximate sampled cycles normalized by profiled ops:
current 9.09e9 / 53041 ≈ 171k cycles/op; candidate 2.79e9 / 51957 ≈ 54k —
single-capture CPU reduction, not a reliable throughput prediction.
Synchronous NVMe profiles have only 29–34 cycle samples: too sparse for
hot-path rankings. Default `strace -c` is CPU time.

Round 2: retained wall-clock strace shows the original candidate's extra fsync
(~5.1 ms per synchronous NVMe write) beyond writev (~5.5 ms), supporting the
~42% synchronous throughput penalty independently of admission.

Round 3 (40 untraced runs, mean of 5 repeats):

| Storage/sync | Current GiB/s | Conditional GiB/s | Mean p99 current/conditional µs | Worst max current/conditional ms |
|---|---:|---:|---:|---:|
| SATA/off | 0.412 | 0.412 | 4887 / 4063 | 26.3 / 26.1 |
| NVMe/off | 2.148 | 2.122 | 1339 / 1169 | 4.4 / 70.4 |
| SATA/on | 0.306 | 0.351 | 4738 / 4342 | 26.6 / 29.0 |
| NVMe/on | 0.034 | 0.034 | 78609 / 72449 | 99.7 / 150.2 |

The conditional version removes the prior sync penalty and improves SATA sync
throughput ~15%. No non-sync throughput gain is established. Final wall-clock
strace: 1042 writes, writev average 5709 µs; no ftruncate/fsync calls for the
fixed-size diagnostic — verifies the redundant length/barrier work is absent;
does not establish device cache behavior.

## 7. Limitations and remaining gates

NVMe maximum tails worsen in individual samples, so this is not blanket
adoption evidence. Direct candidates remain private; production code and
deployment unchanged. Longer matched runs and xorinox validation are still
required before proposing production adoption. One harness correction retained:
the first added empty-direct fixture incorrectly expected zero bytes; the
existing direct format has a minimum 4 KiB zero-padded page and the expectation
was corrected. Tail figures superseded by the corrected latency-validation
method (caveat above).

## 8. Evidence index

| sha256 (16) | File |
| --- | --- |
| 3d5f17e7358a0431 | results.json — 40 round-3 randomized untraced runs |
| b32b961d6f24e28e | conditional-store.patch — conditional-length candidate diff |
| 1940668f844065af | conditional-store-test.c — added store unit coverage |
| 5cab62ecc77274ac | conditional-nvme-sync-wall-strace.txt — round-3 sync wall profile |
| 82e4bc55dca2a94e | current-root2-sync1/flat.txt — current NVMe sync CPU profile |
| 0096295386153069 | candidate-root2-sync1/flat.txt — original-candidate NVMe sync profile |
| 76702d01acaff150 | candidate-root1-sync0/cpu-strace.txt — candidate SATA non-sync syscalls |
