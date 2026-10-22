# SUMMARY — results/measure/20261008-amd-functional

AMD functional package — Oct 8: full units + direct/TCP service suites +
targeted buffered/RDMA gates, on baseline devel `53be1da7` plus three commits.

## Purpose and linked item

- AMD functional package acceptance (per README.md): baseline devel
  `53be1da7` (v0.2.1-pre-alpha) + `fe2ce494` (append identity/authority/
  overflow), `311c4e4e` (concurrent retry ownership), `ec5e0387` (lazy RDMA
  send registration).

## Verdict

**PASS**: full `make test` passes (`full-units.log`); direct/TCP service:
POSIX 216 pass / 0 fail / 1 unsupported skip; peer 64/64; persist
prepare/verify 26/26 each. Targeted buffered/RDMA gates are identified in
README.md's table.

## Source commit and build identity

Baseline `53be1da7` + the three commits above (README.md).

## Environment

Four daemons on one AMD host; RDMA via software RXE/rxe0; functional short
cycles direct I/O over TCP; targeted buffered/RDMA gates explicitly marked.

## Method

Full unit run, then service suites (posix/peer/persist) — README.md's check
table carries each suite and its log.

## Results

| check | result |
|---|---|
| make test | PASS (full-units.log) |
| POSIX direct/TCP | 216 pass, 0 fail, 1 skip |
| peer (2-client) | 64/64 |
| persist prepare/verify | 26/26 each |

## Limitations and remaining gates

Software RDMA (rxe), not native IB; see the orcd-* runs for native.

## Evidence index

- `README.md` — check table (run `shasum -a 256 README.md | cut -c1-16` here)
- `full-units.log` and the suite logs/TSVs in this dir
