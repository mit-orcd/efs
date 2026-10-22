# SUMMARY — results/measure/20261009-revoked-append-retirement

Revoked append receipt retirement on an AMD private pressure-enabled fixture.

## Purpose and linked item

- Append receipt retirement after revocation (fixture gc-direct-8rrp1udl; the
  manifest captures live binary/source identities before launch).

## Verdict

**PASS** (per README.md): final full Linux regression passes; final build has
no warnings; final tests add prepared receipt refusal, foreign/newer receipt
preservation and cold lost-COMMIT recovery for both eight-row DONE cleanup and
final receipt deletion.

## Source commit and build identity

Per fixture manifest (README.md).

## Environment

AMD private fixture gc-direct-8rrp1udl, direct I/O/TCP, pressure enabled.

## Method

Full regression + targeted refusal/preservation/cold-recovery tests.

## Results

All listed tests pass.

## Limitations and remaining gates

Fixture scope.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  fixture logs in this dir
