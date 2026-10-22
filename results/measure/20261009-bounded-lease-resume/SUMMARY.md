# SUMMARY — results/measure/20261009-bounded-lease-resume

Bounded lease/lock cleanup resume: management `cleanup-leases` command with
durable fencing, exact replay, crash and restart resume.

## Purpose and linked item

- Lease/lock cleanup (candidate based on `9e474f13`) with the
  `cleanup-leases` management command and its live test.

## Verdict

**PASS** (per README.md): full Linux regressions pass; command-loop faults
cover ambiguous transport, incomplete authority, malformed result, explicit
empty proof, 4096-batch work cap and 30 s admission deadline (an
already-started RPC may finish later). Thirteen leases plus eight locks are
durably fenced; eight lease rows removed; exact replay leaves the next five
leases intact; then all daemons crash and the production CLI resumes the same
revoked epoch after restart.

## Source commit and build identity

Candidate based on `9e474f13` (README.md).

## Environment

AMD private fixture gc-direct-fom6diyh, port 29240, direct I/O/TCP, pressure
off.

## Method

Full regressions + command-loop fault matrix + crash/restart resume proof per
README.md.

## Results

All listed gates pass.

## Limitations and remaining gates

Private fixture; admission deadline semantics documented (started RPCs may
finish past it).

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  fixture logs in this dir
