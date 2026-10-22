# SUMMARY — results/measure/20261009-amd-w73-w64-rxe

AMD RXE W73/W64 gate closure: direct and buffered modes over software RoCE.

## Purpose and linked item

- **W73/W64** gate closure; status report
  `docs/status/amd-rxe-gate-closure-20261009.md`. Source commit
  `f6233987b9d1243d5dc95167e9aaabc77e32b44d`, branch `devel-gate-closure`,
  build ID `f6233987b9d1`.

## Verdict

**PASS** both modes:

| Mode | Core POSIX | Peer | Mount sync | Exact cold append/GC |
|---|---|---|---|---|
| Direct RXE | 212/212, 19.0 s | 64/64, 14.4 s | PASS | 800 records; PASS |
| Buffered RXE | 212/212, 18.6 s | 64/64, 14.2 s | PASS | 800 records; PASS |

## Source commit and build identity

`f6233987b9d1` (branch devel-gate-closure).

## Environment

AMD host, software RXE rxe0, direct and buffered I/O; run Oct 8 New York /
Oct 9 UTC.

## Method

Per-mode core POSIX + peer suites + mount sync + exact cold append/GC oracle
(800 records) — logs and TSVs in this dir.

## Results

See the verdict table; both modes green.

## Limitations and remaining gates

Software RoCE, not native IB (orcd-* runs carry native acceptance).

## Evidence index

- `README.md` — gate table (run `shasum -a 256 README.md | cut -c1-16` here);
  per-mode logs/TSVs in this dir
