# Shared-container range experiments

`efs-bench --bench ranges` tests concurrent positional I/O to one logical 1 GiB
container. It is independent of the EFS engine and network. A common file does
not imply a common range: disjoint records can be accessed concurrently without
an application lock covering the entire file. Accessing one changing record is
a separate correctness and contention problem.

```sh
make efs-bench
./efs-bench.sh --storage-ranges --storage-root /data1/efs/bench \
  --output /data1/efs/bench/ranges-UNIQUE --seconds 10 --io-modes direct
# Deliberately expose a multi-call update window, with and without a lock.
./efs-bench.sh --storage-ranges --storage-root /data1/efs/bench \
  --output /data1/efs/bench/split-UNIQUE --qds 4 \
  --accesses hot-locked,hot-unlocked --split-write --seconds 5
# Reclamation racing a reader: release the range, then rewrite it.
./efs-bench.sh --storage-ranges --storage-root /data1/efs/bench \
  --output /data1/efs/bench/punch-UNIQUE --qds 4 \
  --accesses hot-locked,hot-unlocked --punch-before-write --seconds 5
```

Each case has a fresh private root. The file is populated with nonzero records
and flushed before measurement; this is an overwrite experiment, not a sparse
allocation experiment. A record has a 64 KiB payload and an aligned 4 KiB
checksum tail. The fixed logical file length includes unused tail rounding.
All final records are read and verified outside the measured clock. The default
matrix tests 1/4/8 workers, all four modes and separate buffered/direct cases.
There is no global cache drop or mount change; buffered data is warmed by setup.

| Access | What is shared | Correctness contract |
|---|---|---|
| `disjoint` | One descriptor/inode; separate record lanes per worker | Each worker owns its lane and verifies the exact expected generation/digest. |
| `hot-read` | All workers read record zero | Record stays immutable; verified concurrent reads need no exclusive application lock. |
| `hot-locked` | All workers read/replace record zero | A per-record read/write lock covers I/O, verification, version update and required body sync. Readers share the lock; writers exclude readers and writers. |
| `hot-unlocked` | Same record, with no application coordination | Diagnostic only. Readers check record identity and self-consistency, not a synchronized expected version. |

`pread`/`pwrite` use explicit offsets and do not share a mutable seek position.
The range lock is a `pthread_rwlock` within this process; it is not a
cross-process, distributed, Raft or storage-ownership protocol. Disjoint lanes
avoid application lock contention, but still share XFS inode/journal and device
resources. `fsync` acts on the common file, so flush traffic can couple workers
that update different ranges.

## Mixed-record diagnostics

`--split-write` writes two aligned parts with a deliberate 50-microsecond pause.
It demonstrates a multi-call publication window; it does **not** test atomicity
of a single write. `--punch-before-write` releases the aligned record before
rewriting it. Both options apply only to hot locked/unlocked cases in the
matrix. The locked cases must verify with zero mixed-record observations.

An unlocked reader can observe a partial update or an intervening hole. Such
observations are counted as `torn_observations`. The diagnostic also checks the
final file for a mixed record left by competing writers. A whole older version
with a valid checksum is not counted as torn; this mode does not validate
freshness, ordering or lost logical updates. Zero observed tears therefore does
not establish a safe storage protocol or an atomic-write guarantee.

The harness labels valid unlocked runs `OBSERVATION`, rather than `PASS`.
I/O errors, incomplete duration, zero-work workers and locked/disjoint integrity
errors invalidate a run. Ordinary failures clean test-owned files; timeout or
kill can leave a private fixture for diagnosis.

## Timing and interpretation

`--seconds` requests a minimum measured duration per worker. `--steps` remains a
hard work cap: reaching it before the requested duration invalidates the run.
The matrix uses a ten-million-operation cap by default. Syscalls and lock waits
can extend the elapsed time past the requested deadline; the harness imposes a
separate process timeout. Raw invocation should set both arguments explicitly:

```sh
./efs-bench --bench ranges --storage EMPTY_PRIVATE_ROOT \
  --access hot-locked --qd 8 --seconds 10 --steps 10000000 --direct-io
```

The deterministic workload is 50/50 reads/replacements, except `hot-read`.
`--persist none|each|batch` controls body flushes; batch size defaults to 32.
Timed completion includes the final required batch flush. Intermediate batch
operations are not individually durable. No durable namespace/index/recovery
protocol is modeled. `none` still flushes outside timing before final checking.

`cycle_p99_us` and `cycle_max_us` cover the complete foreground operation,
including lock wait, checksum/generation work, I/O and required sync.
`lock_p99_us` and `lock_max_us` isolate lock acquisition. `read_io_max_us`,
`write_io_max_us`, `sync_max_us` and `syncs` help distinguish lock contention
from lower-layer delays. Write I/O time includes deliberately split writes and
punching when requested. Min/max worker operation counts expose unequal progress.
These are observations on one shared host, not isolated kernel/device diagnosis.

For eventual containers, favor immutable record locations, conditional mapping
publication and reader pins over modifying a published record in place. Pins
need to last through I/O completion. Reuse/punching also needs authoritative
retirement and generation fencing; a checksum or local lock is not collection
authority. See the [storage-engine proposal](../backlog/storage-engine-v2.md).

API semantics: [POSIX file-operation atomicity](https://pubs.opengroup.org/onlinepubs/9799919799/functions/V2_chap02.html),
[positional I/O](https://www.man7.org/linux/man-pages/man2/pread.2.html)
and [Linux direct I/O](https://www.man7.org/linux/man-pages/man2/open.2.html).
