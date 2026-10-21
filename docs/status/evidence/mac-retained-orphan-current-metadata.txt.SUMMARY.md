# mac-retained-orphan-current-metadata.txt — evidence summary

## Purpose and linked item
Live metadata snapshot captured for the retained-orphan directory
review, added by commit `4124876fc` ("Reproduce retained Mac directory
backlink refusal with safe negative controls", 2026-10-11). Linked
status page: `docs/status/mac-retained-orphan-review-20261012.md`.
Records the on-disk state of the retained unlinked directory
(ino 4091, parent 2098) and its replacement (`level_4`, ino 8187) on
the deployed node.

## Verdict
Informational capture — not a pass/fail run by itself. It supports the
retained-orphan review whose function tests PASS (see
`mac-retained-orphan-function-tests.txt`). All recorded RPCs returned
status=0; the readdir of the retained directory returned count=0.

## Source commit and build identity
Deployed daemon: `efsd v0.2.2-pre-alpha-55-ge798c2aa` (branch
`devel-mac`, built 2026-10-11T23:46:09Z on efs1), reported by the
`version` RPC in this file.

## Environment
Single deployed node efs1 at 192.168.2.11:17432 (per the version line
and the sibling runtime capture: `--node-id 1 --storage /data1 --quota
10G --no-direct-io`, i.e. buffered I/O). Metadata read via raft
getattr/lookup/readdir RPCs; no workload running during capture.

## Method
Issue `version`, `raft-getattr` (ino 4091 and 2098), `raft-lookup`
(parent 2098, name `level_4`) and `raft-readdir` against the live
primary and record status, mode, nlink, size and mtime.

## Results
| RPC | Result |
|---|---|
| getattr ino=4091 (retained dir) | status=0, primary=2, mode=040755, nlink=2, size=0 |
| getattr ino=2098 (parent) | status=0, primary=1, mode=040755, nlink=4 |
| lookup parent=2098 `level_4` | status=0, primary=2, ino=8187, mode=040755, nlink=2 |
| readdir (retained dir) | status=0, count=0, no names |

## Limitations and remaining gates
Point-in-time snapshot of one node; no cross-primary comparison here.
The behavioral verdict lives in the function-test file and the review
status page.

## Evidence index
- `mac-retained-orphan-current-metadata.txt` sha256:8e722ce2f19db2bd — live metadata snapshot (this file)
- `mac-retained-orphan-runtime.txt` sha256:9dab03ca20b06ae8 — daemon identity and restart history for the same node
- `mac-retained-orphan-function-tests.txt` sha256:046c98e9826a67e6 — behavioral PASS/negative-control run
