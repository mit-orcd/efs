# K88

[K88](../../2026-10/bench-published-readindex-20261031.md): **filed OPEN, blocks alpha: yes; FIXED by Task CK** (Task CJ, Nov 1): the "`rc=-4` / `stage=3 rc=-13`" failures of `test_bench_published` at 24 copies are a lost chunk (`report-stuck` after an async REPORT, 11 of 1920 runs; 1 of 1320 at 12 copies, 1 of 1280 at 8), reported as `BENCH_OK`; `rc=-4` is the test's expected collision control and the read-index BUSY is retried. Not fixed.
