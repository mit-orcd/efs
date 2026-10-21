# Summary — orcd-ordinary-rdma-direct-20261012

## Purpose and linked item

Reviewed-source ORCD qualification of ordinary files over native RDMA with
direct I/O for the in-development source reviewed on 2026-10-12 (branch
`fix-orcd-packing-stress`). Linked status page:
`docs/status/orcd-profile-review-20261012.md`; the run was added by commit
`effa10452` ("docs: record passing ORCD ordinary-file RDMA direct
qualification"). Scope: normal operations and orderly stop/restart; `.efs`
commands/queries and unplanned-event repair/recovery are excluded.

## Verdict

PASS (revised candidate `8a91cde3`). The first candidate `f7e8475e` FAILED:
each of the seven clients reported 216/221 pass with 1 fail
(`dir_deep_nesting` at the 30-second deadline) and 4 skips, and its peer
suite never executed (controller SSH second-hop authentication — a harness
problem). That failed run is retained under `first-f7e8475e/`.

## Source commit and build identity

- Tested revision: `8a91cde3037e882bce2ae3b2215ea7f9ac052f3e` (per
  `profile-stage.json` and `profile-runtime.json`).
- Failed first candidate: `f7e8475e21a23ba0037f2f562d96417b6b6ab40e`.
- Build: `make -j8` with a `cc` wrapper adding the supplied sanitizer
  runtime rpath; `EFS_GIT_BRANCH=fix-orcd-packing-stress`.
- Per-host binary SHA256 manifests in `profile-stage.json`; running-process
  exe hashes audited against them in `profile-runtime.json`.
- Reviewed commit list: `reviewed-commits.txt`.

## Environment

- ORCD cluster: 4 storage servers (fcstor003–006), 6 storage roots each
  (`/data1/01..06/efs-orcd-test-erbmi1`, `--storage-engine files
  --direct-io`); 7 FUSE clients (fcstor007–013) on `/tmp/efs-mount`.
- Transport: native RDMA, `EFS_TRANSPORT=rdma`, `EFS_RDMA_DEV=mlx5_2`;
  verbs device fds confirmed per process.
- Features off: `EFS_COMMANDS_SERVER=0`, `EFS_COMMANDS=0`,
  `EFS_PACKED_WRITES=0`, Raft flow/drain/admission/repair experiments all 0.
- Non-root UID 246111 (`erbmi1`). Pre-existing stores preserved; prior
  source trees kept at `/tmp/efs-before-profile-*`.

## Method

Controller scripts live in this directory (run on the operator Mac against
`orcd-test.py`): `stage-profile.py` (git-archive the revision, stage and
build on all 11 hosts), `deploy-profile.py` (native suite on fcstor006,
write fsynced markers, orderly drain/unmount/SIGTERM-stop, swap in staged
tree, start servers/clients, verify cold reads), `test-profile.py`
(`tests/posix/posix_suite.py /tmp/efs-mount --suite core --jobs 16
--timeout-s 30` on all 7 clients concurrently, then
`tests/posix/posix_2client.py` fcstor007/fcstor008 peer run),
`restart-profile.py` (second orderly restart, cold re-read including a new
`ordinary-after-deploy` marker, physical GC scan across all 24 storage
roots), `test-repeat-profile.py` (post-restart core + peer repeat).
Cleanup: none destructive; clients remain mounted, stores retained.

## Results

| Check | Result |
|---|---|
| Native suite (`make test`) on fcstor006, `8a91cde3` | PASS (log tail in evidence) |
| Deploy + cold read after redeploy, 7/7 clients | PASS (identical large/small sha256) |
| Concurrent core POSIX, 7 clients × 216 cases, run 1 | 1,512/1,512 PASS, ~20.4–20.9 s/client |
| Cross-host peer suite (fcstor007↔fcstor008) | 67/67 PASS (86.5 s) |
| Orderly restart: drains, unmounts, SIGTERM stops | clean, no SIGKILL/force |
| Post-restart cold reads (3 markers) | identical hashes on 7/7 clients |
| Ordinary-object physical GC after unlink | all fragments gone on 24 roots in 0.92 s |
| Post-restart core repeat | 1,512/1,512 PASS, ~20.3–21.4 s/client |
| Peer repeat | 67/67 PASS (121.2 s) |
| Failed candidate `f7e8475e` core run | 1,512 PASS / 7 FAIL / 28 SKIP (see Verdict) |

## Limitations and remaining gates

- `.efs` small-file queries/commands are disabled and excluded — not
  qualified.
- Unplanned-event repair/recovery is deferred; only normal operations and
  orderly restart are covered.
- Correctness-suite durations only; no throughput benchmarking.
- The first peer attempt under `test-profile.py` aborted on a harness mount
  precondition (recorded in `profile-test-controller.log`); the passing peer
  evidence is the corrected rerun in `profile-peer-controller.log`.

## Evidence index

| File (relative) | sha256 (16) | Content |
|---|---|---|
| `profile-posix-summary.json` | bd1b3116ac3fa763 | First concurrent core run, 216/216 × 7 |
| `profile-posix-repeat1-summary.json` | 6100996f24a8c9aa | Post-restart core repeat, 216/216 × 7 |
| `profile-peer-controller.log` | c73e4de8cb8f7f34 | Corrected first peer run, 67/67 |
| `profile-runtime.json` | 1c0d5d9ca490f620 | Running-process identity/env/hash audit |
| `profile-stage.json` | 13e19f026dabe90c | Revision + per-host binary manifests |
| `profile-restart-cold-results.json` | d327153de75ea47f | Post-restart cold hash verification |
| `profile-ordinary-gc.json` | f7f9483aa1834e25 | Physical GC before/after fragment scan |
| `first-f7e8475e/profile-posix-summary.json` | fad6169a2242ef53 | Failed first candidate, 1 fail × 7 hosts |
| `remote/fcstor006/profile-native-8a91cde3037e.log` | f773464178c13a69 | Full native suite log |
| `reviewed-commits.txt` | 935aab146db69ed4 | Reviewed commit list |
| `deploy-profile.py` | cc63a43a62554fa4 | Deploy/verify controller script |
