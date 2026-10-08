# W95 sustained conditional churn — AMD

24 balanced samples, three repetitions, 60 rounds of 128 objects (7,680 operations per sample). Each round overwrites, conditionally deletes, or recreates objects. Tiny sizes are 1/17/128/512 bytes; mixed cases replace every sixteenth object with 64 KiB. Buffered I/O on one AMD filesystem; no concurrent profiler or strace.

| Cohort | Mixed | Model | Mean ops/s | Mean p99 µs | Peak allocated bytes |
|---|---|---|---|---|---|
| 1 | 0 | files | 1790 | 662 | 524288 |
| 1 | 0 | indexed | 756 | 1210 | 540672 |
| 1 | 1 | files | 1781 | 665 | 1048576 |
| 1 | 1 | indexed | 753 | 1213 | 1130496 |
| 32 | 0 | files | 4290 | 9672 | 524288 |
| 32 | 0 | indexed | 27030 | 1127 | 53248 |
| 32 | 1 | files | 4259 | 9965 | 1048576 |
| 32 | 1 | indexed | 25284 | 1291 | 708608 |

All 184,320 operations completed; all samples passed exact generation/content verification on read-only reopen. GC and allocation inventory cost are included in sustained throughput. Publication latency includes required body and directory barriers; it excludes subsequent GC. Peak allocation is sampled before and after collection, not during transient manifest replacement. The model checks actual allocation before publication separately.

The indexed model wins with cohorts of 32 and loses for individual publications. Full index snapshot write amplification is included in raw counters. File cohorts provide per-object atomic replacement; indexed cohorts additionally provide atomic publication of the whole cohort. Neither model represents production EFS storage, hardware power loss, or a selected migration format.

Runner sources: `tests/measure/principles_churn.c` and `.py`. Build from the repository root with `cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -I. -Iinclude -Ideps/blake3 tests/measure/principles_churn.c src/bench/principles_index.c libefs.a -lpthread -lm -o /tmp/principles-churn`. Python accepts `--binary`, `--root`, `--rounds` and `--repetitions`; it creates unique private roots and never touches mounted exports. Failures retain their private root.

Mac ASan/UBSan additionally passed all eight combinations with eight rounds; AMD timings use the optimized strict-warning build. These are serial protocol streams, not a multiclient filesystem benchmark.
