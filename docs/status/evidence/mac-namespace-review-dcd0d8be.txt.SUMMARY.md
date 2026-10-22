# mac-namespace-review-dcd0d8be.txt — evidence summary

## Purpose and linked item
Namespace transaction-envelope review output, added by commit
`0bde74a68` ("Review namespace fixture envelope and bounded cut retry
proposal", 2026-10-11). Linked status page:
`docs/status/mac-namespace-fixture-review-20261012.md`. Verifies that
the namespace fixture's admission limits match the exact extracted
production namespace admission code from release `95d3f1b1`.

## Verdict
PASS. Both sanitizer modes exit 0. The extracted admission matches
release `95d3f1b1` (sha256
`d924f0d38f85fabb9be8f6d64dc1914313572e84d4726ef487a86415d05924ae`);
144 participants / 144 ancestors accepted, 145 participants refused by
the actual participant guard, 145 ancestors refused with rc=-22
(operation exceeds supported transaction envelope); ordinary and fault
fixture construction verified with limits and refusal index unchanged.

## Source commit and build identity
Review commit `dcd0d8be`; compared against extracted release-`95d3f1b1`
admission source (exact sha256 recorded above). No binary hashes in the
run.

## Environment
macOS harness (raft-host in-process fixture), sanitizer off/on matrix.
No live cluster, transport, or quota involved.

## Method
Replay namespace fixture construction against the production
hash/admission code across six salts; drive the capacity phases
(participants, ancestry) to the 144-entry limit and confirm the
145th entry is refused at the guard with rc=-22; repeat with
sanitizers enabled.

## Results
| Check | sanitizer=False | sanitizer=True |
|---|---|---|
| Extracted admission == release 95d3f1b1 | exact sha256 match | exact sha256 match |
| 144 participants + 144 ancestors accepted | PASS | PASS |
| 145 participants refused by actual guard | PASS | PASS |
| 145 ancestors refused (rc=-22) | PASS | PASS |
| Fixture construction, limits/refusal index unchanged | PASS | PASS |
| exit | 0 | 0 |

## Limitations and remaining gates
Fixture-level review only; proves the test envelope mirrors production
admission, not any live-cluster behavior. The companion bounded
cut-retry proposal mentioned in the commit is reviewed separately
(`mac-cut-review-b239dc7d.txt`).

## Evidence index
- `mac-namespace-review-dcd0d8be.txt` sha256:fab0a6ac8da913d5 — full review output (this file)
- `mac-cut-review-b239dc7d.txt` sha256:747f190564ad480e — companion bounded cut-retry review
