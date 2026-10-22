# SUMMARY — results/measure/20261009-put-ticket-transport

Durable PUT-ticket transport groundwork: opt-in admission and pure status use
a fixed 216-byte endian ticket body plus command tag on the existing Raft
submission transport.

## Purpose and linked item

- PUT-ticket durability groundwork (ticketed PUTs).

## Verdict

**PASS** (per README.md): exact lengths, FileID, identity, object derivation,
geometry and membership mask validated before routing; ADMIT replicated on the
lane shard; STATUS crosses ReadIndex and never creates sessions/tickets; both
require the active global session and its registered shard; apply rechecks the
local rejection floor.

## Source commit and build identity

Per README.md.

## Environment

Per README.md.

## Method

Ticket transport gates per README.md (validation, replication, ReadIndex
status, session requirements, rejection floor).

## Results

All listed checks pass.

## Limitations and remaining gates

Groundwork only; the ticketed PUT gates are the later runs.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  logs in this dir
