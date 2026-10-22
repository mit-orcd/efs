# SUMMARY — results/measure/20261009-pending-create-wire

Opt-in pending CREATE wire flag and cold adoption: CREATE_F_PENDING requires a
valid immutable op-id, established local shard session and authoritative active
global session.

## Purpose and linked item

- Pending CREATE wire protocol (baseline `77d9b7b0`): replicated pending form
  carries full UUID/epoch in its lease slot, uses the CREATE sequence as
  pending adoption token, atomically writes namespace/inode/replay-result/
  pending-lease through the existing metadata batch.

## Verdict

**PASS** (per README.md): malformed pending frames/UUID/epoch/flag
combinations fail before namespace effects; historical CREATE command
interpretation retains its legacy lease epoch1; cold adoption verified.

## Source commit and build identity

Baseline `77d9b7b0` (README.md).

## Environment

AMD private fixture, direct I/O/TCP (per the Oct 9 AMD series).

## Method

Wire-level gate: malformed-frame refusals + pending adoption + cold restart
oracles.

## Results

All listed gates pass per README.md.

## Limitations and remaining gates

Opt-in flag; historical CREATE framing unchanged.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  logs in this dir
