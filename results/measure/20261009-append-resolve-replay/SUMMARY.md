# SUMMARY — results/measure/20261009-append-resolve-replay

Legacy append-resolution replay verdict: the retained APPEND_RES decoder no
longer swallows failed KV apply as success.

## Purpose and linked item

- APPEND_RES replay correctness: IO/NOMEM retain the log index for retry;
  deterministic business verdicts advance with their actual result; a short
  command reports PROTO; existing missing-row idempotence unchanged.

## Verdict

**PASS** (per README.md): host decoder/application tests exercise IO/NOMEM,
BUSY, PROTO and idempotence; real metadata fault injection proves a failed
completion retains the visibility barrier and reservation identity, followed
by successful retry and replay.

## Source commit and build identity

Per README.md.

## Environment

Host decoder/application tests + fault injection fixture.

## Method

Decoder/apply unit tests + metadata fault injection for the visibility-barrier
proof.

## Results

All listed cases pass.

## Limitations and remaining gates

Current production append completion uses the transaction path; this fixes
retained command replay, not all append recovery (explicit in README.md).

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  test logs in this dir
