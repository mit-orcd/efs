# SUMMARY — results/measure/20261005-014440-w23-stalled-compactor

W23/P2.3 stalled-compactor test — SETUP ONLY: the fault hook is built and
documented, no measurement ran.

## Purpose and linked item

- **W23 / P2.3** (stalled-compactor behavior): the `EFS_FAULT_COMPACT_STALL=1`
  hook (built with `make EXTRA_DEFS=-DEFS_FAULTS=1`) parks the compactor
  between iterations (no `l->mu`, no pinned view) while `/tmp/efs/fault`
  exists; memtable flushes to L0 continue, L0→L1 and D12/D13 L0→L0 merges
  stop. Logs `kv-fault: compactor parked/released`, 100 ms poll cadence.

## Verdict

**NOTRUN**: setup evidence only — the measurement run did not complete; no
numbers exist in this dir. The run that produced measurements is
`20261005-040810-w23-stalled-compactor`.

## Source commit and build identity

Tree state Oct 4/5 2026, uncommitted, no commits made (the hook lived only in
the working tree at this point).

## Environment

Build with `-DEFS_FAULTS=1`; the hook is compiled out otherwise.

## Method

See `20261005-040810-w23-stalled-compactor` for the executed method.

## Results

None — setup only.

## Limitations and remaining gates

Keep NOTRUN; do not cite this dir as a measurement.

## Evidence index

- `SUMMARY.txt` — hook description + tree state (run `shasum -a 256
  SUMMARY.txt | cut -c1-16` here)
