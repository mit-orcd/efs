# D25 writer snapshot RPC validation — Oct 6 2026

- Full Linux `make test`: PASS on the isolated NUC source snapshot.
- Production client RPC extraction: normal and ASan/UBSan PASS.
- Metadata tests: absent/lagging lanes, missing active stamp, first publication,
  stale publication, and committed/undecided transaction visibility PASS.
- Native wire probe: 216 successful checks across ports 20432/20433/20434,
  covering holes, FileID bootstrap/mismatch and published rows. Zero inode and
  truncated requests returned INVAL.
- Both fresh clients cleanly stopped without discard.

Source: working-tree snapshot under `/data1/efs/review-20261006-lease/src`;
without Git metadata, its displayed build ID is `unknown`. Live cluster:
`/data1/efs/review-20261006-writer-rpc`, 3 fresh private stores, 2 GiB quota each.
The original NUC mounts and older diagnostic clusters were unchanged.

`live-probe.py` records the successful test against that private cluster's
native 64-bit Linux wire ABI. Its mount paths and ports are specific to this
run. It creates its own temporary directory and removes only its test files.
It is not a general production cluster runner.

The private cluster was initially launched before the updated server finished
linking. Clients were cleanly drained and only that private cluster restarted
on the completed binary before the successful probe. No live logical truncate,
fault injection, RSS, performance or production FUSE admission gate is claimed.
