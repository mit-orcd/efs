# 20261011-intel-buffered-28b2089b — Intel buffered TCP qualification matrix (stopped)

## Purpose and linked item
Intel's share of the human-authorized three-way qualification split (AMD / Intel / NUC)
of frozen release branch `release-v0.3.0-pre-alpha`: run the 48 buffered TCP
first-stage scenarios from the approved live-test runner on the Intel cluster.
Status page: `docs/status/intel-buffered-qualification-20261011.md`; split
authorized in `docs/status/amd-all-suites-20261011.md` ("authorized three-cluster
split"). Added by commit `6acd3bdda` ("docs: checkpoint Intel buffered
qualification at user stop").

## Verdict
**NOTRUN (run aborted at user stop; partial progress 23 PASS / 24 NOTRUN of 47
launched-or-pending scenarios).** No failures were observed in the scenarios that
ran; the matrix was stopped by user request on 2026-10-12 UTC before completion,
so the qualification gate as a whole did not pass.

## Source commit and build identity
- Source: `28b2089be6a5df835cc7bb46b9c2a9e2f10df190`, branch `release-v0.3.0-pre-alpha`
  (private qualification branch `qualification-intel-28b2089b`).
- All 21,738 tracked inputs hash-verified on both hosts against the frozen
  manifest; archive SHA-256 `2ade76f7…4f1454`, zero mismatches
  (`intel-28b2089b-local-source-proof.json`).
- Build IDs: `EFS_GIT_ID=28b2089b-release-normal` (normal variant) and fault
  variant builds; per-binary SHA-256 recorded per case in `all-gates.jsonl`
  (e.g. efsd `a08c199d…b35193d`).

## Environment
- Hosts: intel1_efs (26 normal scenarios assigned) and intel2_efs (22
  fault/worker/integration/Valgrind scenarios assigned).
- Buffered I/O, TCP transport, loopback ports 34210–34213/34410–34413, private
  stores and `/tmp` mounts per fixture; two-mount (mount + peer-mount) topology
  for peer scenarios.
- D25 inactive; experimental raft flags (flow, early-retire, group-drain,
  batch-noop, batch-overlap) OFF per case environment.
- Production cluster on the same hosts left untouched at
  `7bde5f432b6a6c069eeabaa6a753648c481ab4e5` and audited before/after.

## Method
Approved runner preserved as `intel-28b2089b-run.py` (staging/staging scripts
`intel-28b2089b-stage.py`, `intel-28b2089b-generate.py`); exact per-case commands
in `intel-28b2089b-inventory.json` (e.g. `python3 -u tests/live/gc_reclamation.py
--mode buffered --transport tcp --posix --continue-posix-failure`, 1800 s timeout
per case). Adaptations limited to host paths, port allocation, disjoint
scheduling, evidence collection. No automatic failure retries. Cleanup verified:
`finished.json` shows zero remaining processes/mounts and post-run source trees
matching all 21,738 files on both variants.

## Results
| Host | PASS | NOTRUN (user stop) |
|---|---|---|
| intel1_efs | 7 (gc-posix, ordinary-overwrite, query, large-delta, packing-seven, peer-mkdir-race, peer-open-contention) | 19 |
| intel2_efs | 17 (create-retry/adopt, fuse-reply, put-barrier, kv-wal, transaction-apply, compactor, lookup, put-reply, storage-session-fence, publication/drain-pressure, sync-write-fault, stop-dirty, namespace-boundary/max-fault) | 5 (worker-*, fault-integration, valgrind) |

Longest PASS: peer-open-contention-only-buffered 665.4 s; compactor-buffered
713.3 s. Full per-case table with durations in
`docs/status/intel-buffered-qualification-20261011.md`. Production audit after
stop: process/binary/mount identities matched pre-launch baseline; 256 original
tiny-file payload samples verified from each host (512 checks).

## Limitations and remaining gates
- 24 assigned scenarios never ran; buffered matrix is not a passed gate.
- Intel direct-I/O, RDMA and poststage scenarios out of scope here (owned
  elsewhere or unrun); native/sanitizer suites owned by AMD, not claimed.
- No relaunch scheduled; resume requires explicit user authorization.

## Evidence index (paths relative to this directory; sha256 truncated to 16)
- `intel-28b2089b-inventory.json` — fd899b36c402ddfd — approved per-case commands/assignment
- `intel-28b2089b-local-source-proof.json` — 312f1ef55d68103d — 21,738-file source hash verification
- `intel-28b2089b-run.py` — 4d2e61cacb7e722c — approved runner script
- `intel1_efs/final/all-gates.jsonl` — 5a367eaabcf5e35e — per-case status/durations/binary hashes (intel1)
- `intel2_efs/final/all-gates.jsonl` — 4d180206022db2ae — per-case status/durations/binary hashes (intel2)
- `intel1_efs/final/stop-request.json` — 01faca5ed0d41c14 — user stop record (2026-10-12T04:21:55Z)
- `intel1_efs/final/finished.json` — 3269081f498c95cf — idle/cleanup + post-run source proof
- `intel1_efs/production-stop.json` — bdf9d469cd0ab07f — final production audit
- `intel1_efs/retirement-audit.json` — d38490871f467f6f — controller/observer retirement
- `intel1_efs/final/gates/gc-posix-buffered/result.json` — 80338b70b23ea53a — representative case result (other cases under `<host>/final/gates/<case>/` and `<host>/final/live/<case>/`)
