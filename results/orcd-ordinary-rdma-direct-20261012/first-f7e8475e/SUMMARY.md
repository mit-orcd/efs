# Summary — orcd-ordinary-rdma-direct-20261012/first-f7e8475e

## Purpose and linked item

First-candidate run of the reviewed-source ORCD qualification of ordinary
files over native RDMA with direct I/O (2026-10-12, branch
`fix-orcd-packing-stress`). This subdirectory retains the full evidence for
the first reviewed snapshot `f7e8475e`, which FAILED and was superseded by
the revised candidate `8a91cde3` (evidence in the parent directory). Linked
status page: `docs/status/orcd-profile-review-20261012.md` (sections "First
concurrent run and subsequent review"); the run was added by commit
`effa10452` ("docs: record passing ORCD ordinary-file RDMA direct
qualification"). Parent summary: `../SUMMARY.md`.

## Verdict

FAIL for candidate `f7e8475e`. Staging, native prerequisite tests, orderly
redeploy and post-redeploy cold reads all PASSED, but the concurrent core
POSIX run failed on every client: each of the 7 clients reported
216/221 pass, 1 fail, 4 skip, exit 1 (`profile-posix-summary.json`,
`profile-test-controller.log`). Every failure was `dir_deep_nesting`
hitting the 30-second per-case timeout (per-host TSVs under
`../remote/fcstor*/profile-posix-f7e8475e21a2.tsv`, e.g. fcstor007 summary
`pass=216 fail=1 skip=4 notrun=0`). The cross-host peer suite never
executed: the controller's second-hop SSH to fcstor008 was rejected
(`Permission denied (publickey,...)` in `profile-test-controller.log`) — a
harness problem, so peer coverage for this candidate is absent, not failed.
The candidate was abandoned; the defect was fixed in `8a91cde3`.

## Source commit and build identity

- Tested revision: `f7e8475e21a23ba0037f2f562d96417b6b6ab40e`
  (`profile-stage.json`, `profile-runtime.json`).
- All 11 staged builds passed (`profile-stage-controller.log`); per-host
  SHA256 manifests for `efsd` / `efs-fuse` / `efs-mgmt` in
  `profile-stage.json`.
- Running-process audit (`profile-runtime.json`): all 11 hosts on the exact
  source revision; process exe SHA256s match the staged manifests.

## Environment

- ORCD cluster: 4 storage servers (fcstor003–006), 6 storage roots each
  (`/data1/01..06/efs-orcd-test-erbmi1`, `--storage-engine files
  --direct-io`); 7 FUSE clients (fcstor007–013) on `/tmp/efs-mount`.
- Transport: native RDMA, `EFS_TRANSPORT=rdma`, `EFS_RDMA_DEV=mlx5_2`;
  RDMA verbs device fds recorded per process in `profile-runtime.json`.
- Non-root UID 246111 (`erbmi1`); `.efs` commands and packed writes
  disabled; prior source trees retained at
  `/tmp/efs-before-profile-f7e8475e21a2`, stores preserved.

## Method

Same controller scripts as the parent run (parent directory):
`stage-profile.py`, `deploy-profile.py` (orderly drain/unmount/SIGTERM stop,
redeploy, fsynced-marker cold reads), `test-profile.py`
(`tests/posix/posix_suite.py /tmp/efs-mount --suite core --jobs 16
--timeout-s 30` concurrently on all 7 clients; then
`tests/posix/posix_2client.py` fcstor007/fcstor008 peer run — aborted on the
SSH precondition). Native prerequisite `make test` extract ran on fcstor006
before deploy (`profile-before-audit.log`). No restart or repeat phase was
attempted for this candidate. Cleanup: none destructive.

## Results

| Check | Result |
|---|---|
| Staged builds, 11 hosts | PASS |
| Native prerequisite tests (fcstor006) | PASS |
| Orderly redeploy + cold reads, 7/7 clients | PASS (identical large/small sha256, `profile-cold-results.json`) |
| Concurrent core POSIX, 7 clients × 221 cases | FAIL: 216/221 per client, 1 fail × 7 (all `dir_deep_nesting` 30 s timeout), 4 skips each |
| Cross-host peer suite | NOTRUN (controller SSH second-hop auth failure) |

Aggregate per status page: 1,512 PASS / 7 FAIL / 28 SKIP / 0 NOTRUN.
Skips: `mmap_write_read` (mmap unsupported) and three `.efs` virt cases
(commands disabled by profile).

## Limitations and remaining gates

- Candidate rejected; superseded by `8a91cde3` (parent run, PASS).
- No peer-suite, restart, or physical-GC evidence exists for `f7e8475e`.
- The 30 s `dir_deep_nesting` timeout root cause is not analyzed here; it
  was resolved by the follow-up commits leading to `8a91cde3`.

## Evidence index

| File (relative) | sha256 (16) | Content |
|---|---|---|
| `profile-posix-summary.json` | fad6169a2242ef53 | 7-client core run, 216/221 + exit 1 each |
| `profile-test-controller.log` | 6ccad31f141b88a9 | Core run output + peer SSH failure |
| `../remote/fcstor007/profile-posix-f7e8475e21a2.tsv` | 68a03c5e6c6d757e | Per-case TSV: `dir_deep_nesting` FAIL timeout |
| `profile-stage.json` | 99947cbf5f8188bb | Revision + per-host binary SHA256 manifests |
| `profile-runtime.json` | 6f12838feeb4ee52 | Running-process identity/env/hash audit |
| `profile-deploy-controller.log` | 549287dca3b864a3 | Orderly redeploy, DEPLOY PASS line |
| `profile-cold-results.json` | 524f5f9cff445df2 | Post-redeploy cold hash verification |
| `profile-cold-expected.json` | d68401a0240febd7 | Expected fsynced marker hashes |
| `profile-before-audit.log` | ff1098838629b93f | Native prerequisite test extract |
| `profile-stage-controller.log` | 1dd9c5a69cc19b3c | 11/11 staged builds passed |
