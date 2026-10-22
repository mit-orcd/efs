# SUMMARY — results/measure/20261009-lease-ownership-guards

Legacy lease ownership guards: all OPEN protocols respect prepared inode and
exact close-obligation keys; legacy OPEN/CLOSE respect prepared lease keys.

## Purpose and linked item

- Lease ownership guards: a successful legacy descriptor must not bypass a
  cleanup transaction or an atomic pending lease image.

## Verdict

**PASS** (current source; per README.md): legacy session apply preserves
BUSY/business verdicts while permitting later RESOLVE entries to run; IO/NOMEM
still retain replicated retry. **Retained failure**: the previous session
source fails the actual prepared-close legacy OPEN assertion (kept visible).

## Source commit and build identity

Per README.md.

## Environment

AMD direct/TCP fixture.

## Method

Prepared-close legacy OPEN assertion + session-apply verdict preservation
tests.

## Results

Guards hold on current source; previous-source failure retained as the
regression record.

## Limitations and remaining gates

Legacy-protocol scope.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  test logs in this dir
