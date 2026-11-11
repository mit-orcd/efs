# Descriptive source-name cleanup — Oct 30, 2026

**State (Nov 11):** this page was ported from branch `cleanup-descriptive-names` when the branch was deleted; its head is kept as tag `archive/cleanup-descriptive-names`. The rename it describes (284 file renames and a 1,520-line map) was never adopted in devel and cannot be applied any more; it is recorded here because this page is the only record of the two nuc gate failures below. Whether the rename is still wanted is an open user decision. Paths in this page are as they were on the branch.

Status: **Cleanup implemented; Linux regression qualification FAIL** — cleanup
branch only; no merge or push. Both full-suite controllers have finished.
Purpose: replace deployment/work-item names in source entry points with behavior
names while preserving coverage, assertions, workload bytes and failure evidence.

## Source and scope

Branch `cleanup-descriptive-names`, based on devel `342e9f9d0`. The complete
rename map (`tools/cleanup/rename-map.json` on tag `archive/cleanup-descriptive-names`) records 287 file moves plus
symbol, test-control, binary-target, diagnostic and new-tempfile changes.
Policy and intentional exceptions (`tools/cleanup/README.md` on the same tag) distinguish
new source names from retained evidence and deployed ownership identities.

Batches cover membership/identity, Raft capacity, storage recovery, publication
and receipts, truncate/maintenance, packing, remote observers, host qualification,
measurement tools and model traces. Every batch updates its callers. No old-name
aliases, wire/row/fragment formats, quorum rules, assertions or test deadlines
are introduced or weakened. The Git-dependent rename audit stays in the developer
`code-graph-check` gate; release exports need no original Git history to run the
product code suite.

## Checks

Local Mac checkout, Apple Clang and Python 3.9; no live cluster used locally:

- `python3 tests/test_cleanup_names.py`: six checks PASS: unique destinations,
  definitions/decorators, complete Makefile registration, original byte fixtures,
  product C control/data tokens and absence of old operational source paths.
  Runtime strings are independently covered by CLI/fault tests.
- 763 Python files parse; 181 shell scripts pass `bash -n`.
- Identity model: 118/118 with 19 negative controls at `--seeds 2`, plus direct
  execution of the renamed queued-read-proof race. Shard ordering is preserved.
- Targeted receipt, stale replay, harness, ownership, preservation, readiness,
  verification-resume, test-runner and standalone-driver regressions PASS.
- `python3 docs/gen-architecture-full.py && make docs-check`: 5/5 PASS.
- Canonical graph rebuild and its nine graph/four naming tests PASS; final graph
  refresh includes the last workload-preservation correction: 17,200 nodes,
  71,039 edges, 550 uniquely named communities; indexed-input hashes match. Eight known-bug
  navigation paths remain checks of navigation, not product acceptance.

The full local build is **BLOCKED by a platform prerequisite**: Linux/glibc
`fopencookie`/`cookie_io_functions_t` in unchanged `src/common/log_ts.c`. The
Mac driver regression likewise requires `/proc`. These are not Linux test passes.
Raw local logs are outside Git at
`/Users/mike/Documents/Codex/artifacts/descriptive-names-20261030/`.

## Linux regression

AMD was inspected read-only and had another user's private benchmark clusters;
none were changed. The selected nuc host was inspected across all UIDs and had
no EFS/test workload before the private export was launched. Linux x86_64, GCC 14,
fuse3 3.16.2, verbs library 1.15.61.0; no installed cluster modified.

The source-only immutable exports run the committed
`scripts/make_test_summary.sh` with fresh private TMPDIRs, standard test bounds,
build jobs 4 and command timeout 900 s. `make test-all` retains each failure log
and reports NOTRUN separately. The controller uses nohup because the existing
`screen` runtime directory is misconfigured; host configuration is unchanged.

1. Source `c74e10e5da3b`, 4 test jobs, private source/fixtures
   `/tmp/efs-names-c74e10e5da3b`, evidence
   `/home/maik/efs-artifacts/names-c74e10e5da3b/`. **FAIL**: 556 commands,
   one append-oracle failure (`failure-output/fail-272.log`), 11 NOTRUN,
   1,733 s plus 61 s build; 16 budget warnings at four test jobs.
2. Corrected source `de6ab1b2a1477c83730a8a080662fe48294227b5`, 8 test jobs, private source/fixtures
   `/tmp/efs-names-de6ab1b2a147`, evidence
   `/home/maik/efs-artifacts/names-de6ab1b2a147/`. Queued to wait for the first
   source's closed `exit` file before any build/test; **FAIL**: 556 commands,
   two failures, 11 NOTRUN, 1,751 s plus 73 s build; 34 budget warnings.
   The append-oracle command at index 272 passed (exit 0, 0.1 s).

No performance claim is made from these different job counts. The suite creates
only owned loopback fixtures; sizes, quotas, feature settings, transports and
per-case durations remain those of each unchanged test recipe. The corrected-source controller is FINISHED with exit 1. The two failures
remain unresolved; no error, deadline, orphan bound or workload was relaxed.
The initial 11 skips were nine RDMA hardware paths (no usable device), one
RDMA Memcheck gate (missing `valgrind/memcheck.h`) and the external
cluster-stop/remount-script gate (missing external scripts). No dependency or
cluster configuration was changed to turn these into passes.

## Failures retained and corrections

The cleanup audit caught three altered byte literals (append record, lost-create
reply payload and immutable read probe) and one encoded workload string before
qualification. Restored in `de6ab1b2a`; the byte-fixture guard prevents recurrence.
The append decoder's K09 tag and payload dimensions are intentionally preserved.
The earlier immutable export and every result remain intact; its run alone cannot
qualify the corrected cleanup. Initial developer guard/recipe failures and Mac
prerequisite failures are retained with the local logs; no product error is hidden.

Hardware/deployment controllers were renamed and syntax/caller checked, not
launched against installed clusters. Their existing host/ownership preconditions
still apply. Historical evidence pages and raw stores/captures are not rewritten.

## Unresolved Linux failures

- `tests/test_maintenance_plateau.py` (case 494): one overwrite returned EIO;
  33 abandoned uploads after 612 rewrites exceeded the unchanged bound of 30.
  Checked payloads had no reported mismatch, but the failed last write was
  skipped by verification; this is not full-payload PASS or proof of durability.
  Client logs show repeated REPORT BUSY (`rc=-13`). The root cause is unclassified.
- `tests/test_directory_row_fault.py` (case 516): serve-first halt, healthy-replica
  service and exact directory reads passed. Node 3 then failed to resynchronize
  and serve the group within the existing bound. It remains a failed recovery
  gate, not a passing halt-only gate.

These cases passed in the earlier four-job run and failed in the eight-job run.
That is not a matched-source/concurrency control and does not establish either
a cleanup regression or a pre-existing product defect. A matched untouched-base
eight-job comparison is NOTRUN. No product repair was mixed into the naming
branch to hide or bypass these results. The full code gate remains FAIL.

Original fixtures retained, unopened and unmoved, for the open investigation:

- `/tmp/efs-names-de6ab1b2a147/cases/case-494/efs-plateau-hlajsdhy`
- `/tmp/efs-names-de6ab1b2a147/cases/case-516/efs-dir-fault-_r05zpjn`

Complete selected case logs/JSON (16 files, 2,150,713 bytes) are copied and hashed
under the stable evidence directory's `retained-case-text/`, with a per-case
manifest identifying every original and copied file. Stores remain at the
original paths; their payloads have not been independently reverified. Both
private test exports are temporarily retained for this unresolved comparison.
Post-run `/proc` and mountinfo checks found no owned efsd, efs-fuse, fusermount3
processes or mounted test exports. Another agent's cluster was not changed.

## Exact evidence and reproduction

Run from a fresh source-only export of the named commit on a Linux host with
GCC, fuse3 development files, verbs development files, fusermount3 and `/dev/fuse`:

```sh
mkdir cases
export TMPDIR="$PWD/cases" EFS_TEST_JOBS=8 EFS_BUILD_JOBS=4
export EFS_GIT_ID=de6ab1b2a147 EFS_GIT_BRANCH=cleanup-descriptive-names
bash scripts/make_test_summary.sh
```

The committed runner uses `test-all`, a 900 s per-command limit, independent
command TMPDIRs and the existing budgets/slot accounting. Inspect `summary.txt`,
`timings.tsv`, `tmp/fail-*.log` and every NOTRUN/OVERBUDGET line; exit 0 alone is
not a substitute for those verdicts. Normal tests retire their owned fixtures.
On failure retain the printed fixture and logs; check ownership and mounts
before any later cleanup. Do not open the faulted stores with another binary.

SHA256 (full hashes; raw files remain on nuc outside Git):

| Evidence | SHA256 |
|---|---|
| First summary | `652e1c66e679dc04a3190233db10fb2abae706d36e31b00cff6838eaf1a84a0e` |
| First append failure | `4348f2963f60786c56929735fd05434d3117bc9bf647442d670383cfd5fbe9e0` |
| Corrected summary | `44f9e629a9b14a2fdf25ba76e43291cb706a00783514d217e3743537f683245f` |
| Plateau failure | `ab6a492c5cea6074d408253ff01a91b56a353918706d21d0a9e7e6219f177436` |
| Directory recovery failure | `08a542617e0de37088f82a5e6a02d3de22d28520bbfe9dacacee5d799047e2c8` |
| Plateau text manifest | `23240f3799c6c78eba85ee27a09a891c3c93040a62c132871f31d2d415d7d928` |
| Directory text manifest | `0816c6126f880b1b8feca44b1b4f8d7625b11d731c522e8776b398c77b5caa7a` |

Corrected native binary hashes: efsd
`c268cc706de5be906c2a2d9889a57c85e9e72ecfd33f0f47326158e6582fde6c`,
efs-fuse `3263285c498a1b6faa348b72c7f0e0cc49c1c3c108b120bb9dde719e281e75f8`,
fault efsd `6b3bbb2503f0746f1377b02d28daddf3866c34f57b94d6298f52b8fdb860ba94`.
The EFS_BUILD_ID is `de6ab1b2a147` (verified in the daemon); an archive's
`--version` reports no Git metadata. Kernel: `6.12.0-211.67.1.el10_2.x86_64`.

Devel advanced independently to `72ba517fe` during this work. This branch keeps
its original base and contains no merge from devel. At integration, reconcile
that commit's graph-label test registration with this branch's developer-gate
registration; do not retain a stale NOT_WIRED exception for a wired test.
