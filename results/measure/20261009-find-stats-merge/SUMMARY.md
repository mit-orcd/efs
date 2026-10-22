# SUMMARY — 20261009-find-stats-merge

## Purpose and linked item

Committed-branch integration checkpoint for `devel-find-stats`: merge all
then-current local branch tips (devel `fee363f5`, devel-w66 `70daa5a4`,
devel-persistence-pressure `437b7890`) into one tree, regenerate architecture
docs, and run full Linux build/unit acceptance on the merged snapshot. Linked
item: `docs/status/find-stats-all-branches-20261009.md` (deferred from the W92
derived-stat work). Run added by commit `8aeb9c4b9` ("docs: record all-branch
find-stats integration and full Linux gates").

## Verdict

PASS for build/unit acceptance: full native Linux `make all` and `make test`
both exit 0, the build log contains zero compiler warning lines, all five
architecture checks pass, and every recorded branch tip is verified an ancestor
of the merge boundary. This is build/unit acceptance only — no live acceptance
of newly merged protocol behavior is claimed.

## Source commit and build identity

- Merge boundary: `f4f95ed7` (devel-find-stats).
- Build identity embedded in binaries: `EFS_BUILD_ID`/`EFS_VERSION` `"f4f95ed7"`,
  `EFS_GIT_BRANCH` `"devel-find-stats"`, build time `2026-10-10T03:18:38Z`
  (from compile lines in w92-merged-build.log). No deployed-sha256 file.
- Deployed runtime remained `b4212453` from the preceding profiling task; this
  run did not redeploy services.

## Environment

Private committed archive on the AMD Linux host (xefs1) at
`/home/efs/w92-merged-f4f95ed7`; source-only snapshot that does not replace
`/tmp/efs` or stop services. Native gcc build (`-O2/-O3`, `-Wall -Wextra`,
libverbs, FUSE3). No live cluster fixture in this run; runtime deployment
unchanged.

## Method

1. Merge `devel` (`9e87533a`), `devel-w66` (`5e0fe113`) and
   `devel-persistence-pressure` (`f4f95ed7`) into devel-find-stats; regenerate
   architecture HTML from merged sources.
2. Record every local branch tip in branch-boundary.txt; verify each tip is an
   ancestor of the merged branch.
3. Run focused stale-read and inbox-retirement regressions plus all five
   architecture checks locally.
4. From the committed archive: full `make all` and `make test`, capturing logs
   as w92-merged-build.log / w92-merged-test.log.
5. No push, tag, main merge or service change; final indexed documentation
   validated separately after the source-only snapshot.

## Results

| Gate | Result |
|---|---|
| Branch-tip ancestry check (15 tips in branch-boundary.txt) | PASS |
| Focused stale-read / inbox-retirement regressions | PASS |
| Architecture checks | 5/5 PASS |
| `make all` (full Linux build) | exit 0, zero compiler warnings |
| `make test` (full unit suite) | exit 0 |
| Live acceptance of merged behavior | NOTRUN (not in scope) |

## Limitations and remaining gates

- Build/unit acceptance only; merged publication/recovery behavior was not
  deployed or live-tested in this round (deployed Xorinox stayed `b4212453`).
- Does not close W55 NFS acceptance, W66 topology/routing, or W92 full
  incremental derived-stat/scale obligations.

## Evidence index

| File | sha256 (16) |
|---|---|
| README.md | 652124bb70eafbd6 |
| branch-boundary.txt | 63d7c02ea90b5fab |
| w92-merged-build.log | 570aaa75413a206d |
| w92-merged-test.log | 0d37e3b1aec03281 |
