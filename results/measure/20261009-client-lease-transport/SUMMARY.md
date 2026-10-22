# SUMMARY — results/measure/20261009-client-lease-transport

Actual client deferred-lease transport on an AMD private pressure-enabled
fixture: production RPC paths carry authoritative generation and explicit
identity through OPEN/CLOSE with replay and verdict queries.

## Purpose and linked item

- Client lease transport hardening (deferred lease). Fixture
  `gc-direct-rb8xoei8`.

## Verdict

**PASS** (per README.md): full Linux regression passes; final build has no
warnings. Actual production client RPC functions obtain authoritative
generation (no hardcoded FileID), establish the same explicit identity,
perform OPEN/CLOSE, replay exact edges, and query their stored verdicts. After
an owned daemon crash, the CLOSE probe returns the same result. Unknown OPEN
probes remain unknown and acquire no lease. Cold bytes survive and the
disposable file's physical fragments are reclaimed.

## Source commit and build identity

Per README.md (AMD private fixture build).

## Environment

AMD private fixture gc-direct-rb8xoei8, direct I/O/TCP, pressure enabled.

## Method

Full regression + production-RPC lease scenario with crash probe and
cold-byte/reclaim oracles.

## Results

All listed behaviors verified (see verdict).

## Limitations and remaining gates

Private fixture; installed service untouched.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  fixture logs in this dir
