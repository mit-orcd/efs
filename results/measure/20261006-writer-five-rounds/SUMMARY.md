# D25 five-round checkpoint — Oct 6 2026

## Purpose and linked item

Checkpoint for D25 writer admission (`docs/status/d25-admission-routing.md`).
Five implementation rounds, in order: complete GETCHUNKS reply validation;
FileID/epoch-tagged GETCHUNKS with optional expected-generation validation;
typed base validation for range planning; object/sequence-bound publication
completion; metadata-budgeted writer sidecar lifetime. Added by commit
`7fb1a35a4 feat(d25): budget retained writer state and protect publication
lifetime`.

## Verdict

PASS — all gates below green; staged foundation only (no activation gate
claimed).

## Source commit and build identity

Working-tree source snapshot without Git metadata; displayed build ID is
`unknown`. Feature commit named by the adding commit: `7fb1a35a4`.

## Environment

Live cluster `/data1/efs/review-20261006-writer-rpc` on the NUC; all servers
and fresh clients restarted on matching binaries after the build (GETCHUNKS
wire shapes changed and require matching binaries). Original mounts and
earlier diagnostic clusters unchanged. POSIX mount
`/data1/efs/review-20261006-writer-rpc/mnt`. Transport/features not
recorded in the run.

## Method

- Full isolated Linux `make test`.
- Range/base/publication tests and budget/lifetime tests under ASan/UBSan.
- Single-client POSIX suite (jobs=4) — `posix.tsv`.
- Two-client POSIX suite — `posix2.tsv`.
- Live native wire probe `live-probe.py` (in this dir; native 64-bit Linux
  probe for these specific paths and ports 20432/20433/20434): writer and
  GETCHUNKS holes, FileID mismatch, published rows, malformed writer
  requests. Creates/removes its own temporary files; not a production
  deployment runner.

## Results

| check | result |
|---|---|
| Linux `make test` | PASS |
| ASan/UBSan range/base/publication + budget/lifetime | PASS |
| Single-client POSIX | 216 pass / 0 fail / 1 skip (mmap) / 0 notrun, 15.6 s |
| Two-client POSIX | 64/64 pass, 5.1 s |
| Live wire probe | 432 checks PASS across ports 20432/20433/20434 |
| Client shutdown | both fresh clients cleanly stopped without discard |

## Limitations and remaining gates

- Writer range admission/publication sidecars not yet attached to FUSE
  cache entries.
- No logical-truncate activation, history-retirement, RSS/performance or
  fault gate claimed.

## Evidence index

- `posix.tsv` (d8dae327385b830d) — single-client POSIX results, summary line pass=216
- `posix2.tsv` (ce61c674b982fb73) — two-client POSIX results, summary line pass=64
- `live-probe.py` (0af78c8c43032f12) — native wire probe script (432 checks)
