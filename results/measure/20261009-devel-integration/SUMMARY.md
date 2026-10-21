# Summary — Consolidated devel acceptance, AMD (2026-10-09)

## Purpose and linked item

Combined devel acceptance after merging all outstanding committed development
branches (devel-known-bugs, devel-gate-closure, devel-compaction-pressure,
devel-w66; others already ancestors). Linked item: devel integration /
release-gate checkpoint — docs/archive/status-20261011/devel-integration-20261009.md
(archived historical checkpoint). Run added by commit f6233987b
("test: record consolidated devel AMD POSIX peer and persistence acceptance").
No push, tag or main merge was performed.

## Verdict

PASS. Both AMD modes pass all six live suites with no failures/skips/not-run;
full Linux `make all` + `make test` pass on the final code; architecture gate
5/5 after the final documentation merge.

## Source commit and build identity

- Tested production/test commit: `7b4f6085` (build log shows
  EFS_BUILD_ID/EFS_VERSION `7b4f6085`, branch devel, built 2026-10-09T02:01:34Z).
- Later `677522ec` merges docs/evidence only; src/include/tests/Makefile
  identical to the passing build.
- First combined build `80ba3b89` also passed all six live suites before the
  two newer commits were incorporated and gates repeated.
- Installed/backup daemon SHA256 match recorded in
  efs-integration-final-health.txt (`bddd6aab...2821a1`).

## Environment

- AMD single host, four daemons on one NVMe filesystem; no distributed
  failure-domain proof.
- TCP only (EFS_TRANSPORT=tcp); RDMA not exercised. Both direct and buffered
  I/O modes cycled. Both command feature masks disabled
  (EFS_COMMANDS=0, EFS_COMMANDS_SERVER=0). D25 remains staged.

## Method

- Runner: `runner.sh` in this directory (flock reservation, clean drains,
  original source/binary backup; original AMD installation restored direct/TCP
  after each cycle; final runner closes fd9 for child services).
- Per mode (direct/TCP, buffered/TCP): start private fixture, run core POSIX
  smoke, two-client smoke, persistence verify (unmount/remount), stop cleanly.
  Logs under `integration-final-20261009/` (final) and `integration-20261009/`
  (first combined build cycle).
- Full Linux `make all` / `make test` from the final tree
  (efs-devel-final-build.log, efs-devel-final-test.log).
- Shared /Users/mike/git/efs dirty documentation/index preserved untouched on
  branch preserved-doc-work-20261009 at 2b9da49f; clean devel checkout was
  /tmp/efs-integration-20261009.

## Results

| AMD mode | Core POSIX | Two-client | Persistence verify |
|---|---:|---:|---:|
| direct/TCP | 212/212 (18.0 s) | 64/64 (14.7 s) | 26/26 (3.1 s) |
| buffered/TCP | 212/212 (18.2 s) | 64/64 (13.6 s) | 26/26 (2.6 s) |

verdicts.tsv (both cycles): all six suites exit 0. Full Linux make all/test
exit 0; build log contains no compiler warning lines. Final health: matching
installed/backup daemon SHA256, both masks disabled, reservation released with
no lock holder; no forced discard or reboot.

## Limitations and remaining gates

- AMD acceptance only: one host, four daemons; not a rerun of earlier failed
  Mac VM gates and not W66 scaling closure.
- No hardware power-loss, RDMA, distributed failure-domain or mixed-version
  proof. Core POSIX excludes optional extras per suite design.
- D25 staged; both command masks disabled.

## Evidence index

| File | sha256[0:16] | Carries |
|---|---|---|
| README.md | e161c90dbc4ef0c7 | Full run report (verdict, branches, cleanup) |
| integration-final-20261009/verdicts.tsv | 6132567092f4baea | Final six-suite exit codes |
| efs-integration-final-health.txt | 25c5d854ea51410c | Suite tallies, SHA256 match, masks |
| integration-final-20261009/direct-smoke.log | a77bbfbad05e25ca | Direct core POSIX log |
| integration-final-20261009/buffered-smoke.log | d80c258b6ab2a981 | Buffered core POSIX log |
| efs-devel-final-build.log | 1403b4591ebcb3cf | Final build (build ID 7b4f6085) |
| efs-devel-final-test.log | 19494c0a0e11ff8f | Final make test |
| runner.sh | 014fdca40286ea67 | Reproduction runner |
| integration-20261009/verdicts.tsv | (first cycle) | First-build six-suite exit codes |
