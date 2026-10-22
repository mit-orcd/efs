# SUMMARY — results/measure/20261007-w84-envelope

W84 envelope acceptance on private NUC four-node TCP fixtures.

## Purpose and linked item

- **W84** (envelope): fixed direct/buffered runs with 144-participant
  transaction capacity and derived PREPARE buffers.

## Verdict

**PASS** (acceptance completed): fixed direct and buffered runs completed with
the 144-participant capacity; interrupted preliminary runs are retained
separately. Per the status ledger for limitations (README.md).

## Source commit and build identity

Per README.md narrative in this dir.

## Environment

NUC, four-node TCP fixtures, direct and buffered I/O; 144-participant txn
capacity, derived PREPARE buffers.

## Method

Fixed direct/buffered runs per README.md; preliminary interrupted runs kept
separate.

## Results

Envelope acceptance closed; details and the ledger references in README.md.

## Limitations and remaining gates

See the status ledger named in README.md.

## Evidence index

- `README.md` — narrative; fixture logs in this dir (run
  `shasum -a 256 <file> | cut -c1-16`)
