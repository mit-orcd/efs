# SUMMARY — 20261008-w93-w96-review

## Purpose and linked item
Source review of the W93–W96 work (`docs/backlog/work-items.md`: W93 group
commit, W94 atomic CREATE/leases, W96 KV WAL preallocation) at `devel` commit
`cde67dca`. Headline artifact is a macOS reproduction of a W96 KV WAL
discarded-tail defect; the w94-* subdirs hold the review's guarded-wire,
pending-admission, fence-validation and transaction-owner gate evidence.

## Verdict
- WAL tail reproduction: **FAIL — defect confirmed**. `wal-tail.txt` shows
  record A replayed, append/sync of B returning success, but the next replay
  again stops before B; `exit_code=2`. No production fix or cluster data-loss
  claim is made by this reproduction.
- Review gate subdirs: **PASS** — w94-fence-validation and w94-transaction-owner
  POSIX 216/0/1 each; w94-guarded-wire and w94-pending-admission Linux/sanitizer
  gates pass (per their READMEs/logs).

## Source commit and build identity
Reviewed `devel` commit `cde67dca`; production files were clean at review time.
The reproducer links the actual `src/kv/kv_wal.c` and `src/common/checksum.c`
with bundled BLAKE3. No deployed binary identity — local review build only.

## Environment
Local macOS reproduction linking production KV WAL/checksum sources; creates
and removes one private temporary file, no server or user data involved. The
w94-* gate fixtures ran on AMD: one host, four daemons, direct-I/O/TCP,
disposable private stores; installed service restored after each run.

## Method
Reproducer build/run (from the README, executed on a built tree):

```sh
cc -std=c11 -D_DARWIN_C_SOURCE -DBLAKE3_USE_NEON=0 \
  -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 \
  -Iinclude -Isrc/kv -Ideps/blake3 \
  results/measure/20261008-w93-w96-review/wal-tail.c \
  src/kv/kv_wal.c src/common/checksum.c deps/blake3/blake3.c \
  deps/blake3/blake3_dispatch.c deps/blake3/blake3_portable.c \
  -pthread -o /tmp/efs-w96-tail-review
/tmp/efs-w96-tail-review
```

The deliberate invalid tail is eight zero bytes (invalid record length).
Exit 2 records the defect; exit 0 would require both records to replay.

## Results
| Check | Result |
| --- | --- |
| WAL discarded-tail reproducer | exit 2 — B lost after successful append (defect) |
| w94-fence-validation POSIX | 216 pass / 0 fail / 1 skip, 17.0 s |
| w94-transaction-owner POSIX | 216 pass / 0 fail / 1 skip, 18.0 s |
| w94-transaction-owner full Linux tests | PASS |
| w94-pending-admission (Linux + strict ASan/UBSan) | PASS |
| w94-guarded-wire session wire gates | PASS (no production piggyback activation) |

## Limitations and remaining gates
The reproduction is a review finding only; the follow-up must also test
short/torn records, post-repair writes, repeated restart, compaction and Raft
applied-index/rebuild recovery. (The fix for this defect is the W96 recovery
work evidenced in `../20261008-w93-w96-implementation/w96-recovery/`.) No
hardware power-loss or distributed-RDMA claim from any phase.

## Evidence index
| File | sha256 (16) | Carries |
| --- | --- | --- |
| `README.md` | 77a2da470f23f4bb | Review scope and reproduction recipe |
| `wal-tail.c` | a77ba08f9cf88e80 | Reproducer source |
| `wal-tail.txt` | 30cc1249a083dc22 | Defect output, exit_code=2 |
| `w94-fence-validation/posix.tsv` | 5956e91e6b40ab7a | Fence-validation POSIX summary |
| `w94-transaction-owner/posix.tsv` | f19fc04c76aa3bfb | Transaction-owner POSIX summary |
| `w94-guarded-wire/README.md` | dd572e3de243b1b4 | Guarded-wire gate scope |
| `w94-pending-admission/full-linux-test.log` | 20b3d3b0cb43bf6b | Pending-admission regression PASS |
