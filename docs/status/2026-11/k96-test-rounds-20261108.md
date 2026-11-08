# `test_k96_status_slots_live` over its 10 s row on fstor007: one round instead of three (Task EA)

Task EA, Nov 8 2026, fstor007 and node9900, devel `3f9110d52`. Purpose: the K96 live test ran over its 10 s `tests/budgets.tsv` row on fstor007 in four wave gates in a row (11.6, 12.5, 12.3, 12.7 s in waves 60 to 63; node9900 7.1 to 8.3 s) and the task was to attribute the excess and remove it with the row left at 10 s. Linked row: K96 (FIXED since [Task DQ](k96-status-proof-slots-20261106.md); no K row for this, no production change). Rule kept: no timeout, budget or sleep changed.

**Verdict: PASS, test change only.** The test's three rounds of 24 `efs-bench --size 8M --io-size 64K --workers 4` copies are bound by the replica bytes over the store disk's synchronous write bandwidth, and fstor007's `md0` writes at about a third of node9900's; the test now runs one round by default (`K96_ROUNDS`, the TSan list already ran it that way), which still fails 24 of 24 copies on the unfixed tree. Solo after: fstor007 6.5, 5.8, 6.5 s; node9900 3.8, 4.9, 5.7 s; 6.5 s x 1.25 = 8.1 s, under the 10 s row that stays.

## Attribution

Solo on devel `3f9110d52`, 8-MiB copies, three rounds (the test as merged), timestamps on every line the test prints (`ea-k96-solo.sh`):

| Host | Cluster start and leader placement | Round (24 copies) | Stop and remove | Test wall |
| --- | ---: | ---: | ---: | ---: |
| fstor007 (96 cpus, `md0` xfs) | 3.0 s | 2.5, 2.4, 2.2 s | 1.4 s | 10.2 to 10.7 s |
| node9900 (192 cpus, `md0` xfs) | 2.4 s | 0.9, 1.0, 0.85 s | 1.3 s | 5.2 to 5.8 s |

The rounds carry the difference: 2.5 s against 0.9 s each, 4.8 s over three. The probe (`ea-k96-probe.py`, one cluster, one round per shape) scales with the bytes written, not with the copy count: one copy 0.21 s (fstor007) and 0.14 s (node9900); 4 copies 0.50 and 0.23 s; 24 copies 2.4 to 2.6 and 0.9 to 1.1 s. A round writes 192 MiB of logical data, 576 MiB of replica bytes (24 x 8 MiB x 3 replicas), and `/proc/diskstats` counts about 850 MiB written to `md0` per round on both hosts (records, Raft log and fragment files); `strace -c` on the three daemons over one round counts about 45,000 `fdatasync`, 21,000 `fsync` and 15,000 `sync_file_range` calls, the store's durability path. Synchronous write bandwidth of the two disks (`dd bs=64K count=3072` into `TMPDIR`): fstor007 292 MB/s with `conv=fdatasync` and 319 MB/s with `oflag=dsync`; node9900 1.0 GB/s and 639 MB/s. 850 MiB at 300 MB/s is 2.8 s; at 1 GB/s, 0.9 s: the round is the disk. Not the cause: CPU (`vmstat 1` on fstor007 through the three-round runs: idle never below 86 %, user plus system at most 10 %, iowait up to 7 %; one copy alone is already 1.5 times slower on fstor007), the libfuse shim (no mount in this test), the daemons' code (identical binaries, same syscall counts).

The 8-job gate adds about 2 s to the solo time on both hosts (12.7 against 10.5 s, 8.3 against 5.8 s): the pool shares the disk.

## The change

`tests/test_k96_status_slots_live.py`: `K96_ROUNDS` defaults to 1 (was 3); docstring states why. `tests/budgets.tsv`: the row stays at 10 s, its note carries the measurements. Nothing else changes: the TSan list (`tests/sanitizer_check.sh`) already runs the test with `K96_ROUNDS=1`.

Variants measured solo on devel (three runs each, `ea-k96-variants.sh`, `K96_SIZE` patched into a probe copy of the test), test wall in seconds:

| Variant | fstor007 | node9900 | Unfixed tree `3fadcdb7c` (fstor007) |
| --- | --- | --- | --- |
| 8M x 3 rounds (before) | 8.5, 9.7, 10.3 | 5.8, 6.0, 5.8 | 24 of 24 copies fail in each of 3 rounds (Task DQ, node9900) |
| 2M x 3 rounds | 5.0, 5.6, 5.6 | 4.7, 3.8, 4.5 | rounds 0 and 1 pass (slowest copy 5.2 and 9.2 s), round 2 fails 24 of 24: the copies finish before the 30 s client budget runs out |
| 4M x 3 rounds | 7.1, 7.1, 7.1 | 4.7, 5.2, 5.6 | not run |
| **8M x 1 round (after)** | 5.6, 5.1, 5.9 | 4.9, 4.4, 4.6 | **24 of 24 copies fail** (`busy_workers=83`, slowest copy 80.4 s, test 84.5 s) |

Smaller copies lose detection: the stall needs writers still writing when their 30 s BUSY budget runs out. One 8-MiB round keeps Task DQ's reproducer per round (its 24-copy runs failed in 10 of 10 rounds before the fix) and halves the test's disk time. The committed test, solo, three runs: fstor007 6.5, 5.8, 6.5 s (load average 11, another task's runs on the host); node9900 3.8, 4.9, 5.7 s.

## Checks

- Unfixed tree `3fadcdb7c` (the parent of the K96 fix `d8331e756`; `k33_daemons.py` unchanged since) with the new test: FAIL, 24 of 24 copies `rc=1`, 83 `bench worker ... rc=-13` lines, 84.5 s (`unfixed-3fadcdb7c-8M-r1.log`).
- Devel `3f9110d52` with the new test: PASS, six solo runs, 0 failed copies.
- The wave gate that merges this branch records the 8-job figure.

## Reproduce

```sh
# on a build host, tree extracted to /tmp/efs-taskea, TMPDIR=/tmp/efs-taskea-tmp
python3 tests/test_k96_status_slots_live.py                 # one round (default)
K96_ROUNDS=3 python3 tests/test_k96_status_slots_live.py    # the shape before Task EA
```

Raw: `~/efs-artifacts/taskEA-20261108/{fstor007,node9900,mac}/` on the ORCD home (solo and variant logs with timestamps, `vmstat.txt`, the unfixed-tree logs, the probe and run scripts), `SHA256SUMS` beside them.
