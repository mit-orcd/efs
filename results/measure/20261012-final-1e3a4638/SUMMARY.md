# Summary — final integrated source qualification 1e3a4638 (2026-10-12)

## Purpose and linked item

Final integrated qualification of the frozen release-v0.3.0-pre-alpha candidate
`1e3a46384e897a47751871b15e82ead86c9d0abd` (merge of reviewed qualification fixes),
run by root alone on AMD. Linked status pages:
`docs/status/v030-final-qualification-1e3a4638-20261012.md` and
`docs/status/v030-tcp-direct-qualification-20261012.md`; release gate:
`docs/status/v030-pre-alpha-release.md` (v0.3.0 pre-alpha release acceptance).
Added by commit `5de00c40f` ("test: record final integrated ordinary-files
qualification and deep timeout blocker").

## Verdict

**FAIL (candidate not release-accepted).** Native/build/early-ACK/sanitizer gates
PASS (11/11 exit 0 in `native/results.json`), but the seven-client concurrent core
POSIX gate FAILED: 1505/1512 pass, `dir_deep_nesting` timed out after 30 s on all
seven clients (`ordinary/evidence/deep-*.tsv`). Peer POSIX 67/67 PASS; seven
graceful client stops PASS; isolated ordinary-overwrite primary/peer/cold and
physical-drain checks PASS (`overwrite/result.json` exit 0, `"acceptance": false`).
The deep-directory 30 s timeout remains an ordinary-files blocker.

## Source commit and build identity

- Source: `1e3a46384e897a47751871b15e82ead86c9d0abd`, branch
  `release-v0.3.0-pre-alpha`, describe `v0.3.0-pre-alpha-candidate` (recorded in
  `native/results.json` commands and `overwrite/command.json`).
- Binary SHA-256 recorded in `native/binary-hashes.json` (normal + fault
  `efsd`/`efs-fuse`/`efs-mgmt`).
- 24,334 tracked source files hash-matched before/after
  (`native/source-check.json`, `native/source-check-post.json`).

## Environment

- Single AMD host (per `deep-0.tsv` header `host=amd-395`, paths under
  `/data1/efs/evidence/amd-release-final-1e3a4638-20261012`).
- Four private daemons, 4G quota each; seven concurrent FUSE clients, jobs16,
  original 30 s per-case deadlines.
- Files engine, TCP transport, direct I/O; client env verified in
  `ordinary/evidence/client-config.json`: `EFS_TRANSPORT=tcp`,
  `EFS_PACKED_WRITES=0`, `EFS_COMMANDS=0`, `EFS_COMMANDS_SERVER=0`,
  `EFS_MD_RAFT_N=4`. Containers/packing/.efs/buffered/RDMA and dedicated
  repair/recovery excluded from required scope.

## Method

- Runner scripts retained in the run: `native/amd-release-final-1e3a4638-run.py`,
  `native/amd-release-final-ordinary-controller.py`,
  `native/amd-release-final-ordinary-fixture.py`,
  `native/amd-release-final-ordinary-overwrite.py`.
- Native gates: `make -j8 ... all`, `make ... test`, `./tests/test_raft`
  (early-ACK), fault clean/build/native, five ASan/UBSan extraction regressions
  (`san-*` logs); full commands and durations in `native/results.json`.
- Live gate: seven-client POSIX core suite via the ordinary controller/fixture
  scripts; per-client TSVs `ordinary/evidence/deep-[0-6].tsv`.
- Isolated overwrite check: `python3 -u tests/live/gc_reclamation.py --mode
  direct --transport tcp ... --ordinary-overwrite-only --fixture-quota 4G`
  (exact argv in `overwrite/command.json`).
- Cleanup: all private fixtures/mounts stopped; evidence verified against
  SHA-256 manifests (`*/evidence-manifest.json`).

## Results

| Gate | Result |
| --- | --- |
| Normal build / full native | PASS (9.5 s / 90.9 s) |
| Fault build / full native | PASS (9.5 s / 90.4 s) |
| Early-ACK real Raft (`tests/test_raft`) | PASS |
| Five ASan/UBSan extraction regressions | PASS (5/5) |
| Seven-client core POSIX | FAIL: 1505 PASS / 7 FAIL of 1512; `dir_deep_nesting` 30 s timeout on all 7 clients |
| Peer POSIX | PASS 67/67 (8.7 s) |
| Seven graceful client stops | PASS 7/7 |
| Ordinary overwrite primary/peer/cold hashes + physical/queue drain | PASS (17.3 s, isolated run) |
| Ordinary fixture end-to-end | 71.6 s including setup/peer/cleanup |

Per-client core-suite summaries: each client 215/216 pass, ~42.5–46.8 s
(`deep-*.tsv` `# summary` lines).

## Limitations and remaining gates

- `dir_deep_nesting` fails on every client even with packing disabled; the
  candidate is not release-accepted. Next work: attribute/fix ordinary
  deep-directory concurrency, then rerun that gate on a new frozen commit.
- Fresh fixture/launcher/workload differ from the prior packing-specific run;
  no causal claim that excluding packing improved results.
- Cold checks use controlled process crash/restart — not power-loss
  certification or repair acceptance. Buffered/RDMA, containers and dedicated
  repair/recovery remain excluded from this scope.
- Later documentation-only commits do not change the tested source tree.

## Evidence index (sha256 first 16 chars)

| Path | SHA-256 |
| --- | --- |
| `native/results.json` (all native gate commands/exits/durations) | `bfbb384414296699` |
| `native/binary-hashes.json` (normal+fault binary hashes) | `0fcf6f2f2b600d8e` |
| `native/source-check.json` / `native/source-check-post.json` (24,334-file hashes) | `df87e6fba3158297` / `3de5f0f5434b738b` |
| `ordinary/result.json` (live gate exit=1, 71.6 s) | `115e0bf9fdadcb6a` |
| `ordinary/evidence/deep-0.tsv` (client 0 results; FAIL row + summary) | `35c3477641ceb2ef` |
| `ordinary/evidence/deep-results.json` (all seven clients) | `d0bb4afa01c21598` |
| `ordinary/evidence/peer.tsv` (peer POSIX 67/67) | `fb985bba905f6763` |
| `ordinary/evidence/client-config.json` (verified client environments) | `2c5004a1966bf3c5` |
| `overwrite/result.json` (overwrite gate PASS) | `588d872794836dd0` |
| `overwrite/command.json` (exact overwrite command/env) | `be45fa758f41e2e8` |

**Raw evidence note (2026-10-23):** the bulk data in `amd-release-final-1e3a4638-20261012` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
