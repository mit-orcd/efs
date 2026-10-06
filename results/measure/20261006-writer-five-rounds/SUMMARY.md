# D25 five-round checkpoint — Oct 6 2026

Implemented in order: complete GETCHUNKS reply validation; FileID/epoch-tagged
GETCHUNKS with optional expected-generation validation; typed base validation
for range planning; object/sequence-bound publication completion; metadata-budgeted
writer sidecar lifetime.

- Full isolated Linux `make test`: PASS.
- Range/base/publication tests and budget/lifetime tests: ASan/UBSan PASS.
- Single-client POSIX: 216 PASS, 0 FAIL, 1 mmap SKIP (jobs=4).
- Two-client POSIX: 64/64 PASS.
- Both fresh clients cleanly stopped without discard.
- Live native wire probe: 432 checks PASS across private ports
  20432/20433/20434, including writer and GETCHUNKS holes, FileID mismatch,
  published rows and malformed writer requests.

Live cluster: `/data1/efs/review-20261006-writer-rpc` on NUC, using a working-tree
source snapshot without Git metadata (`unknown` build ID). All its servers and
fresh clients were restarted on matching binaries after the build completed.
Original mounts and earlier diagnostic clusters were unchanged.

`live-probe.py` is the successful native 64-bit Linux probe for these specific
paths and ports; it creates/removes its own temporary files. It is not a
production deployment runner.

Writer range admission/publication sidecars are not yet attached to FUSE cache
entries. No logical-truncate activation, history-retirement, RSS/performance or
fault gate is claimed. GETCHUNKS wire shapes changed and require matching binaries.
