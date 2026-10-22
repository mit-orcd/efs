# SUMMARY — results/measure/20261009-registered-append-revocation

Registered append ownership and bounded revocation on an AMD private
pressure-enabled fixture.

## Purpose and linked item

- Append ownership registration + bounded revocation.

## Verdict

**PASS** (per README.md): full Linux regression passes; the final host
admission extraction test was separately rerun with GCC -Wall -Wextra -Werror
after correcting test stub array declarations. Core POSIX 212/212, peer 64/64,
exact cold append bytes, physical GC counters, empty cold fragment
inventory/quota and late PUT fencing pass.

## Source commit and build identity

Per README.md (fixture build).

## Environment

AMD private fixture gc-direct-sf05s26e, direct I/O/TCP, pressure enabled.

## Method

Full regression + -Werror extraction test + core/peer suites + cold append/GC/
fencing oracles.

## Results

All listed gates pass.

## Limitations and remaining gates

Fixture scope; pressure enabled only in fixture.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  fixture logs in this dir
