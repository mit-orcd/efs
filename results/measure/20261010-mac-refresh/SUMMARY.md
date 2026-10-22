# Summary — 20261010-mac-refresh

## Purpose and linked item

User-authorized refresh of the retained Mac gateway clients: stop the old
gateway mounts with `--force-discard`, replace them with the already-tested
build, and revalidate the full Mac correctness suites. Added by commit
`39fb25da4` ("Record authorized Mac client refresh and full revalidation").
No status page links this run by slug; it is a devel integration acceptance
checkpoint in the pattern of
`docs/archive/status-20261011/devel-integration-20261009.md` (linkage inferred
from README/commit message).

## Verdict

PASS — all fresh suites passed with zero failures/skips/unrun and all recorded
exit statuses zero. Correctness-only: original performance deadlines remain
unmet in earlier evidence and are not certified here.

## Source commit and build identity

- Deployed/tested build: `553fbb42d738` (binary identity; reported by all
  three servers and both gateway clients, matching installed hashes on all
  four VMs).
- devel HEAD at capture: `fee363f5e5621c3844575cc68e5cb2e52e9623a5`
  (documentation/evidence only; no new merge needed). Frozen refs in
  `integration-refs.tsv`.

## Environment

Mac host with Tart VMs (Fedora aarch64): three storage nodes (efs1/2/3),
gateway efsgw with primary NFS export, seed peer; all daemons/clients run as
Linux user `efs`. Fixture: 300 MiB content/EOF verification. Transport not
recorded in the run (Mac profile is TCP; no RDMA). Buffered vs direct I/O not
recorded in the run.

## Method

Run-dir scripts `efs-mac-refresh-authorized.sh`, `efs-mac-refresh-tests.sh`,
`efs-mac-refresh-peer.sh`, `efs-mac-refresh-final.sh`: fetch/prune + ancestry
audit, `--force-discard` replacement of gateway clients /mnt/efs and
/mnt/efs-b, normal stop of superseded /mnt/efs-i, then suites under the Mac
correctness profile (core/persistence 600 s per case, peer 2400 s per step,
original operation counts and assertions retained). No data volume wiped.
Cleanup: validation clients drained, primary mount remounted, NFS export
restored.

## Results

| Suite | Result | Duration |
| --- | --- | --- |
| Native core | 212/212 | 18.4 s |
| POSIX core | 212/212 | 1597.3 s |
| Persistence prepare | 26/26 | 282.6 s |
| Persistence verify (normal drain/remount) | 26/26 | 113.4 s |
| Two-VM peer | 64/64 | 1477.9 s |
| Peer 800-record concurrent append | PASS | 1092.24 s |
| Cold 300 MiB content/EOF check | PASS | 4.093 s |

## Limitations and remaining gates

- Longer budgets establish correctness within the declared budget only;
  original latency targets remain open.
- No code changes were tested (build unchanged); this run revalidates the
  refresh procedure, not new source.

## Evidence index

- `README.md` (fdf1e80852993ed2) — run narrative and results
- `final-verdicts.tsv` (0c6b91264c53791e) — suite exit statuses, all 0
- `posix-mac-profile.tsv` (fca84deb9b956af6) — POSIX core 212/212
- `persist-prepare-final.tsv` (150fdc0179e8c673) — persistence prepare 26/26
- `persist-verify-final.tsv` (087c03360a6ce587) — persistence verify 26/26
- `peer-final.tsv` (96a0d91612b32fa9) — peer 64/64
- `integration-refs.tsv` (4755151d8627ce02) — frozen branch refs
- `authorized-refresh.log` (1e135b86512ad494) — completed client replacement
- `health-final.log` (0c03bbddcc45d826) — final 3/3 health, matching versions
