# W59/W67 memory acceptance closure — NUC, Oct 7 2026

Private four-node TCP fixtures only; normal service untouched. All final
commands exited zero. Explicit budget values match the production defaults
or the smaller stress setting; no allocator budget was raised as a repair.

| Mode / body+drain MiB | Accepted sparse writes | Fault client RSS MiB | Whole-fixture peak MiB | OOM / OOM kill |
| --- | ---: | ---: | ---: | --- |
| direct / 32+32 | 245 | 120.3 | 242.1 | 0 / 0 |
| buffered / 32+32 | 245 | 120.4 | 251.9 | 0 / 0 |
| direct / 256+64 | 2037 | 378.4 | 494.1 | 0 / 0 |
| buffered / 256+64 | 2037 | 378.4 | 906.3 | 0 / 0 |

Every fixture runs inside `MemoryMax=1G`, `MemorySwapMax=0`. The driver checks
its actual inherited cgroup ceiling and saves `memory.peak` and memory events.
The whole-fixture peak includes four daemons, clients and kernel file cache;
it is not the FUSE client's RSS. The fault RSS sample includes refusal time.

The compile-only scratch holder uses the REAL production allocator, touches
its bodies, retains their charges and exhausts the combined capacity. Traces
reach exactly 67,108,864 bytes (32+32) or 335,544,320 bytes (256+64), with zero
reservations. Scratch then refuses ENOMEM while REPORT remains withheld.
Removing the scratch fault and releasing REPORT enables fsync and NEW writes
on the same mount. Cold remount verifies all accepted bytes and the new file;
the refused write did not extend its file. Sparse concurrent pressure,
physical reclaim, queues, restart/quota and late-PUT fences also pass.

The first preliminary gates did not sample RSS at refusal and are retained
as preliminary evidence, not substituted for the final measured samples.
Normal/fault allocator tests pass. The fault unit proves physical capacity
exhaustion followed by release through request admission. The production code
path is unchanged when `EFS_FAULTS` is disabled.

Example command (default-value direct variant):

```
systemd-run --user --quiet --wait --pipe --unit=efs-memory-default-direct-20261007 -p MemoryMax=1G -p MemorySwapMax=0 --working-directory=/data1/efs/close-memory-default-src python3 tests/live/gc_reclamation.py --mode direct --production-budgets --publication-pressure --drain-pressure --memory-limit-bytes 1073741824 --client-binary ./efs-fuse --pressure --port 20970
```

Buffered uses `--mode buffered` and its own unit. Smaller-budget variants
omit `--production-budgets`. Each JSON manifest captures exact arguments,
source/test/binary hashes and private roots. Logs and `outcomes.json` retain
metrics. Prior [three-package evidence](../20261007-three-package-gates/SUMMARY.md)
records full normal Linux units and 216 single/64 peer POSIX cases per I/O mode.
This round changes test-only fault/gate code and does not claim new D27
whole-call recovery, crash salvage, mount fencing or universal RSS limits.
