# SUMMARY — results/leaks/20260927-035622

Four-phase valgrind leak gate: unit suites, efsd, efs-fuse (mini-POSIX), and
efsd + efs-fuse over RDMA. Leak gate passes; a functional EIO in phase 4 is
kept visible.

## Purpose and linked item

- Recurring leak/valgrind gate over the whole stack (tests/valgrind_leaks.sh).
  Added by commit `2dd77dac8` ("Compact by folding L0 when the file cap forces
  it, drop the shard lock").

## Verdict

**PASS** (leak gate): definite and indirect leaks **0 B** in every phase —
all 15 unit suites, efsd (3-node raft + efs-bench), efs-fuse (mini-POSIX
through the mount), and both daemons over RDMA (upgrade exercised;
uninit/invalid skipped due to ibverbs/DMA).

**Functional defect visible**: phase 4's RDMA workload hit
`dd: closing output/input file .../chunk: Input/output error` — the leak
verdict is unaffected but the EIO is part of the run record (pre-D12 tree;
same class as the 20260929 iorperf2 close() failures).

## Source commit and build identity

Not recorded in the run.

## Environment

3-node raft group (`EFS_MD_RAFT_N=3`), 1 GiB quotas, `--writers 0
--no-direct-io` for phases 1–3 (TCP); phase 4 over RDMA at 172.16.223.68.
Valgrind memcheck on all binaries.

## Method

`tests/valgrind_leaks.sh` (phases: build → unit tests under valgrind → efsd
under valgrind with efs-bench → efs-fuse mini-POSIX → efsd + efs-fuse over
RDMA). Leak counters extracted per phase.

## Results

| phase | target | definite | indirect |
|---|---|---|---|
| 1 | 15 unit suites | 0 B | 0 B |
| 2 | efsd (raft+bench) | 0 B | 0 B |
| 3 | efs-fuse (mini-POSIX) | 0 B | 0 B |
| 4 | efsd + efs-fuse over RDMA | 0 B | 0 B |

## Limitations and remaining gates

uninit/invalid classes skipped in phase 4 (ibverbs/DMA); the phase-4 dd EIO is
a functional, not memory, finding.

## Evidence index

- `leaks.txt` — 4-phase log (run `shasum -a 256 leaks.txt | cut -c1-16` in this
  dir)
- `rsync.log` — build/rsync record
