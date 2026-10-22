# mac-retained-orphan-query-log.txt — evidence summary

## Purpose and linked item
Daemon-side query restore/refresh log excerpt captured for the
retained-orphan review, added by commit `4124876fc` ("Reproduce
retained Mac directory backlink refusal with safe negative controls",
2026-10-11). Linked status page:
`docs/status/mac-retained-orphan-review-20261012.md`. Shows the live
daemon repeatedly refusing query restore for the retained-orphan shape.

## Verdict
Informational capture supporting the review — it documents expected
refusals, not a pass/fail gate by itself. `query-restore` returns
rc=-3 (disk backend) on every attempt, and the one `query-refresh`
attempt returns rc=-13 (refusal) after 11 ms with cap_mib=256; no
restore ever succeeds in the window.

## Source commit and build identity
Not recorded in this file; same deployment as
`mac-retained-orphan-runtime.txt` (`efsd v0.2.2-pre-alpha-55-ge798c2aa`,
efs1). Log window 2026-10-12T01:27:10Z–01:27:37Z.

## Environment
Live single-node deployment efs1 (192.168.2.11:17432, buffered I/O,
quota 10G per the sibling runtime capture); query disk backend.

## Method
Capture daemon log lines for the query restore/refresh path while the
retained-orphan reproduction is exercised against the live node.

## Results
| Event | Count | rc | Notes |
|---|---|---|---|
| query-restore backend=disk | 14 | -3 | ~2 s apart, 01:27:10–01:27:37Z |
| query-refresh cap_mib=256 initial=1 | 1 | -13 | elapsed_ms=11 |

## Limitations and remaining gates
27-second excerpt only; rc mnemonics are not decoded in the log itself.
Interpretation depends on the review page and the function-test file.

## Evidence index
- `mac-retained-orphan-query-log.txt` sha256:8b060b74dbea3f9b — live refusal log (this file)
- `mac-retained-orphan-function-tests.txt` sha256:046c98e9826a67e6 — fixture reproduction of the same refusal
- `mac-retained-orphan-current-metadata.txt` sha256:8e722ce2f19db2bd — live inode shape
