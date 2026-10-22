# SUMMARY — results/measure/20261009-append-admission-audit

Legacy APPEND_RES admission expansion: the 38-byte command's legacy apply may
sweep/delete an entire reservation set — it now carries an unbounded
pending-work charge instead of claiming a finite bound.

## Purpose and linked item

- Append-resolution admission accounting (staged pressure); consistent with
  historical whole-lane sweep/reap and session LEASE_DROP commands.

## Verdict

**PASS** (per README.md): with staged pressure enabled, new admission refuses
APPEND_RES rather than claiming a finite bound; committed apply/replay
unchanged; the exact charge regression fails before this fix and passes after
it (tests in this dir).

## Source commit and build identity

Per README.md; installed pressure policy remains off.

## Environment

Staging fixture with pressure enabled; installed service unchanged.

## Method

Charge regression test (fails pre-fix, passes post-fix) + admission tests.

## Results

Fix verified by the before/after regression pair.

## Limitations and remaining gates

Production append completion uses the transaction path; this governs the
retained legacy command's admission only. Pressure policy off in production.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  test logs in this dir
