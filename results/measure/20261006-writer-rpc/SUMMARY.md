# D25 writer snapshot RPC validation — Oct 6 2026

## Purpose and linked item

Checkpoint for D25 writer admission (`docs/status/d25-admission-routing.md`):
expose writer snapshots with publication lane validation over the native
wire. Added by commit
`38e7256c9 feat(d25): expose writer snapshots with publication lane validation`.

## Verdict

PASS — all unit, sanitizer and live wire-probe checks green; staged only
(no production FUSE admission claimed).

## Source commit and build identity

Working-tree snapshot under `/data1/efs/review-20261006-lease/src`; without
Git metadata its displayed build ID is `unknown`. Feature commit named by
the adding commit: `38e7256c9`.

## Environment

Live private cluster `/data1/efs/review-20261006-writer-rpc` on the NUC:
3 fresh private stores, 2 GiB quota each. Original NUC mounts and older
diagnostic clusters unchanged. The private cluster was initially launched
before the updated server finished linking; clients were cleanly drained
and only that private cluster restarted on the completed binary before the
successful probe. Transport/features not recorded in the run.

## Method

- Full Linux `make test` on the isolated NUC source snapshot.
- Production client RPC extraction, normal and ASan/UBSan.
- Metadata tests: absent/lagging lanes, missing active stamp, first
  publication, stale publication, committed/undecided transaction
  visibility.
- Native wire probe `live-probe.py` (in this dir) against ports
  20432/20433/20434: holes, FileID bootstrap/mismatch, published rows;
  zero-inode and truncated requests. The script records the successful test
  against this private cluster's native 64-bit Linux wire ABI; its mount
  paths and ports are specific to this run; it creates its own temporary
  directory and removes only its test files; it is not a general production
  cluster runner.

## Results

| check | result |
|---|---|
| Linux `make test` | PASS |
| Client RPC extraction (normal + ASan/UBSan) | PASS |
| Metadata visibility/admission tests | PASS |
| Native wire probe | 216 successful checks; zero-inode and truncated requests returned INVAL |
| Client shutdown | both fresh clients cleanly stopped without discard |

## Limitations and remaining gates

- No live logical truncate, fault injection, RSS, performance or production
  FUSE admission gate claimed.

## Evidence index

- `live-probe.py` (c25d294ec2c98604) — native wire probe script (216 checks)
- no other raw files in this run; unit/sanitizer output not retained
