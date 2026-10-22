# mac-retained-orphan-function-tests.txt — evidence summary

## Purpose and linked item
Function-test output for the retained-orphan (unlinked directory)
query preflight, added by commit `4124876fc` ("Reproduce retained Mac
directory backlink refusal with safe negative controls", 2026-10-11).
Linked status page: `docs/status/mac-retained-orphan-review-20261012.md`.
Reproduces the retained-directory backlink refusal against two source
trees and validates an unsafe-suppression negative control.

## Verdict
PASS, with the negative control failing exactly as designed. All four
positive configurations (2 sources x sanitizer off/on) pass and exit 0;
both unsafe-suppress builds (product refusal compiled out) abort at the
probe assertion (`orphan_preflight` must return EFS_ERR_IO with
directory==4091), SIGABRT exit=1 — proving the refusal comes from the
guarded code path.

## Source commit and build identity
Sources exercised: `devel-mac` working tree
(`src/meta/query_disk.c`), the frozen `b239dc7d` review tree
(`/private/tmp/efs-mac-cut-review-b239dc7d`), and the negative-control
source `/private/tmp/efs-orphan-unsafe-suppress.c`. Test driver:
`tests/test_query_retained_orphan.py`. No binary hashes recorded.

## Environment
macOS, Python 3.9 harness compiling C probe binaries into temp dirs;
in-process query-disk fixture replicating the live shape
(retained ino=4091, parent=2098, replacement ino=8187). Sanitizer
off/on matrix; no live cluster.

## Method
Run `tests/test_query_retained_orphan.py` against each source with
sanitizers off and on; the probe replays the unlinked-directory
preflight three times and asserts the actual preflight refuses, that
pending/cancel stays inconclusive, and that the immutable old snapshot
plus new repair/backlink controls hold. Then build the unsafe-suppress
variant and confirm the probe assertion fires.

## Results
| Source | sanitizer=False | sanitizer=True |
|---|---|---|
| devel-mac query_disk.c | PASS exit=0 | PASS exit=0 |
| frozen b239dc7d query_disk.c | PASS exit=0 | PASS exit=0 |
| efs-orphan-unsafe-suppress.c (negative control) | probe assertion, SIGABRT exit=1 (expected) | same (expected) |

## Limitations and remaining gates
Fixture reproduces one captured inode shape (4091/2098/8187); broader
live-directory diversity is not covered. Live-system behavior is
documented by the sibling metadata/query-log captures.

## Evidence index
- `mac-retained-orphan-function-tests.txt` sha256:046c98e9826a67e6 — positive + negative-control matrix (this file)
- `mac-retained-orphan-current-metadata.txt` sha256:8e722ce2f19db2bd — live shape the fixture replicates
- `mac-retained-orphan-query-log.txt` sha256:8b060b74dbea3f9b — live refusal log
