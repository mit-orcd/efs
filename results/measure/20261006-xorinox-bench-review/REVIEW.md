# Xorinox benchmark review — 2026-10-06

Evidence: `logs/efs-bench-20261006T222653.471481Z`, commit
`023685d839443ab73a30c1218c3644bd886e622b`, binary SHA256
`f5186c9ed9d6f6ba856ea57efa3d558d82fd81d26e99a5bfc8872aedb9774b26`.
Host: Ryzen 9 7940HS, 8 cores/16 hardware threads; local XFS NVMe.
The run selected raw I/O, I/O+BLAKE3 and CPU BLAKE3; it did not measure the
production storage engine or daemon. Original results/profiles remain unchanged.

## Findings

128 cases: 106 passed, 18 baseline failures, 4 perf workload failures.
Failures were buffered QD64/256 cases with workers completing zero timed
operations. The busy-wait readiness/start gate is a likely contributor to
starvation with 256 workers on 16 hardware threads. No evidence here proves a
storage corruption failure. The reported `File exists` was stale errno from
scratch directory setup. Failed rates such as 239.66/268.09 GiB/s are invalid;
passing buffered rates also measure a warm page-cache working set, not NVMe.

Pure 1 MiB direct I/O is saturated around QD16. Increasing workers substantially
increases average latency without improving aggregate throughput:

| QD | Write GiB/s | Read GiB/s | Write average ms | Read average ms |
|---|---:|---:|---:|---:|
|16|6.276|6.740|2.488|2.317|
|64|6.214|6.735|10.034|9.257|
|256|6.250|6.722|39.637|36.842|

This is a workload observation, not a universal QD recommendation. Smaller I/O
still benefits from higher concurrency.

The buffered 64 KiB QD1 read annotation attributes 24.65%/27.59% of worker
samples to deadline pointer/value reloads and 14.16% near the window division.
The old loop made three clock reads per operation. vDSO/time samples also take
substantial CPU share. These are benchmark overhead, so optimizing the harness
is justified before choosing production changes.

BLAKE3 already uses AVX-512: its inner loops account for roughly 97–99% of
CPU-only samples. One-shot 1 MiB throughput is 7.158 GiB/s at one thread and
58.982 GiB/s at 16 threads. No hash implementation change is justified here.
CPU sample fractions do not measure I/O wait. Reports recorded `cycles:u` and
contain unresolved vDSO/kernel addresses; those addresses cannot safely be
attributed to storage functions.

## Changes and validation handoff

Replaced start spinning with a sleeping all-ready gate. Removed per-operation
modulo, floating-point deadline arithmetic and two clock calls. Worker counters
stay local until completion. Latency now explicitly means complete operation
cycle, including bookkeeping/scheduling, so compare old/new throughput and
profiles but account for the changed latency definition.

Failures now preserve actual error/phase, emit BENCH_FAIL, and expose idle and
minimum/maximum worker counts. Analysis rejects failed baselines, retains valid
baselines with failed profiling reruns, separates time/unresolved samples and
records affinity/perf policy and effective events. Regression cases cover those
reporting rules and successful worker participation.

Only syntax/static checks were performed; execution testing belongs to the user.
No speedup is claimed until a rerun. On Linux, run the benchmark CLI/profile
regressions and rerun the same matrix:

```sh
python3 tests/test_bench_cli.py
python3 tests/test_bench_profile.py
./efs-bench.sh --modes io,io-blake3,blake3 --storage-root /data1/maik/efs
```

Expect no idle workers in normal-duration cases; failed cases must show their
actual reason and must not appear in throughput comparisons. Inspect QD64/256
buffered participation, 64 KiB QD1 timing overhead, and 1 MiB direct throughput.
Very short runs or extreme scheduler pressure can still yield idle workers;
such measurements remain invalid rather than being silently accepted.
