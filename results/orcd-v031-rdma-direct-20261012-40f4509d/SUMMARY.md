# Summary — orcd-v031-rdma-direct-20261012-40f4509d

## Purpose and linked item

ORCD release qualification of `release-v0.3.1-pre-alpha` for ordinary files
over native RDMA with direct I/O. Linked status page:
`docs/status/orcd-v031-rdma-direct-20261012.md`; the run was added by commit
`9fc5e0245` ("docs: qualify v0.3.1 ordinary files RDMA direct on ORCD").
This is a fresh ORCD execution; earlier AMD TCP results do not transfer.
Scope exclusions: `.efs` commands/queries and dedicated unplanned-event
repair/recovery.

## Verdict

PASS for ordinary files / native RDMA / direct I/O, in the selected scope.
Confirmed by `verification.log`: two concurrent core+peer rounds, all
prerequisites exit 0, source/binary identity exact, ordinary physical GC,
96 collected artifact hashes verified. No test failures observed in scope.

## Source commit and build identity

- Frozen release revision: `40f4509dff748aec6d66c73bd2da511b438f42b7`
  (`profile-stage.json`, `code-inputs.json`, `source-proof-installed.json`).
- 777 committed code/test input files matched the frozen release on all 11
  hosts before and after deployment (`source-proof-installed.json`, zero
  mismatches).
- All 11 normal builds passed; running-process exe SHA256s match the staged
  per-host manifests (`profile-runtime.json` vs `profile-stage.json`).
- The release later advanced to `4f8c25e0`, whose product/test tree is
  recorded as identical to the frozen `40f4509d` tested here (status page).

## Environment

- ORCD cluster: 4 storage servers (fcstor003–006), 6 storage roots each,
  `--storage-engine files --direct-io`; 7 FUSE clients (fcstor007–013) on
  `/tmp/efs-mount`.
- Transport: native RDMA, `EFS_TRANSPORT=rdma`, `EFS_RDMA_DEV=mlx5_2`;
  verbs device fds confirmed per process.
- Features off: server and client commands 0, packed writes 0, Raft
  flow/early-retire/drain 0, admission pressure 0, repair experiments 0.
- Non-root UID 246111 (`erbmi1`). Existing stores preserved; previous
  source trees retained at `/tmp/efs-before-profile-40f4509dff74`.

## Method

Controller scripts live in this directory: `prerequisites.py` (on fcstor006:
full native suite, early-ACK Raft gate, separate `EFS_FAULTS=1` build +
suite, 8 ASan/UBSan test extractors, instrumented Raft native + early-ACK
gates), `stage-profile.py` + `deploy-profile.py` (stage exact source,
orderly drain/unmount/SIGTERM-stop of the old deployment, redeploy, verify
fsynced cold reads on all 7 clients), `test-profile.py`
(`tests/posix/posix_suite.py /tmp/efs-mount --suite core --jobs 16
--timeout-s 30` concurrently on 7 clients; then
`tests/posix/posix_2client.py` fcstor007/fcstor008 peer run),
`restart-profile.py` (second orderly restart, cold re-read, physical GC
scan across 24 storage roots), `test-repeat-profile.py` (post-restart core
+ peer repeat), `collect.py` (fetch remote logs/TSVs + sha256 manifest) and
`verify.py` (re-check every hash, PASS counts, identity, GC). Original
30-second minimum per-case budgets retained throughout. Cleanup: none
destructive; all seven clients remain mounted on the tested source.

## Results

| Check | Result |
|---|---|
| Native + early-ACK + fault + 8 sanitizer + raft-san gates | all exit 0 (`v031-40f4509dff74-prerequisites.json`) |
| Concurrent core POSIX, 7 clients × 216 cases, run 1 | 1,512/1,512 PASS, ~30.0–30.2 s/client |
| Cross-host peer suite, run 1 | 67/67 PASS (113.2 s) |
| Orderly restart: drains, unmounts, SIGTERM stops | clean, no SIGKILL/force |
| Post-restart cold reads | identical fsynced bytes on 7/7 clients |
| Ordinary-object physical GC after unlink | all fragments gone across 24 storage roots |
| Post-restart core repeat | 1,512/1,512 PASS, ~20.6–21.1 s/client |
| Peer repeat | 67/67 PASS (100.0 s) |
| Artifact integrity | 96 collected files, all sha256 verified |

Whole-suite times are correctness-suite durations, not throughput numbers.

## Limitations and remaining gates

- `.efs` small-file commands/queries are disabled and excluded — not
  qualified.
- Dedicated unplanned-event repair/recovery remains unqualified; the
  historical TCP-only standalone repair/fault harness is not an RDMA
  mounted test.
- AMD TCP/buffered results for the later `4f8c25e0` release pointer are
  independent and not counted as ORCD RDMA results.
- No throughput benchmarking performed.

## Evidence index

| File (relative) | sha256 (16) | Content |
|---|---|---|
| `verification.log` | 3632067df67fc854 | Final PASS line + per-host PASS counts |
| `verify.py` | e4ff32ebc332c4e6 | Independent re-verification script |
| `profile-posix-summary.json` | 2090040d35ed4334 | First concurrent core run, 216/216 × 7 |
| `profile-posix-repeat1-summary.json` | cbbfc392b9c4be8d | Post-restart core repeat, 216/216 × 7 |
| `remote/fcstor007/profile-peer-40f4509dff74.log` | d3ebbf72a5a9f7da | First peer run, 67/67 (113.2 s) |
| `remote/fcstor007/profile-peer-40f4509dff74-repeat1.log` | ec283126bd2fbd89 | Peer repeat, 67/67 (100.0 s) |
| `remote/fcstor006/v031-40f4509dff74-prerequisites.json` | b7568d279a493bc9 | Prerequisite gate exits/durations |
| `profile-runtime.json` | 10217bd93964d3a1 | Running-process identity/env/hash audit |
| `source-proof-installed.json` | da069135d7bbfa13 | 777-file source match on 11 hosts |
| `profile-ordinary-gc.json` | ae96d446bdfe6cb1 | Physical GC before/after fragment scan |
| `code-inputs.json` | 7ed91ee2a072d486 | Frozen revision + input file hashes |
