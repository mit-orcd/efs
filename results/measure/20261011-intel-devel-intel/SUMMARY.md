# 20261011-intel-devel-intel — devel-intel branch acceptance ledger (human stop, latest candidate FAIL)

## Purpose and linked item
Long-running acceptance ledger for the `devel-intel` repair branch on the Intel
cluster: qualify each candidate source (775d935c → 0ae7e480 → 31abdd5e →
c2bfb9e9) through native, sanitizer, fault and live mounted gates before any
production deployment. Status page:
`docs/status/intel-deployment-acceptance-20261011.md` (final checkpoint
`docs/archive/status-20261012/intel-stop-checkpoint-20261011.md`). Raw evidence
added across commits `85f98aedd`, `fe39dce41`, `6a42a1ea9` and successors.

## Verdict
**FAIL (ledger terminated by HUMAN STOP 2026-10-12; latest common candidate
c2bfb9e9 has an unresolved live CREATE-retry double-hook FAIL).** No candidate
reached full mounted acceptance or production deployment. Specific recorded
verdicts: c2bfb9e9 native (244 Make-test recipes + 88 standalone scripts) and
sanitizer gates PASS on both hosts (prerequisite only); all four strict
CREATE-retry live gates FAIL on c2bfb9e9; one ordinary namespace-maximum live
FAIL on 0ae7e480 (repaired natively in dcd0d8be, repaired live qualification
NOTRUN); large remainder STOPPED/NOTRUN.

## Source commit and build identity
Multiple candidates, each hash-verified: 0ae7e48021badb2c…, 31abdd5e (1,557
files verified against archive/Git blobs in `intel-31abdd5e-local-bundle-proof.json`),
c2bfb9e9 (sole common candidate, includes reviewed Python AST fixture fix
b3e196c1; inventory `intel-c2bfb9e9-inventory.json`). Production remained at
`7bde5f43` (TCP/direct-I/O); ledger head recorded in `acceptance-in-progress.json`
as `12e65920…`. Per-host artifact SHA-256 manifests under `c2bfb9e9-native/<host>/`.

## Environment
- Two physical Intel hosts (intel1_efs, intel2_efs); production: two daemons per
  host, TCP, 400 GiB quota per daemon; one `/data1/efs/mnt` client per host
  seeded from the opposite host; `.efs` enabled.
- Private fixtures: separate stores/mounts, both direct and buffered storage I/O
  exercised, concurrent POSIX + peer operations, cold payload recovery, Valgrind
  daemon/client tests, ASan/UBSan suites.
- RDMA acceptance explicitly out of scope (belongs to AMD RXE / orcd-test).

## Method
Runner-managed scenario matrices per candidate (scripts staged under each
candidate's private tree; e.g. `intel_fault_contract.py` here); per-case rows
recorded in controller checkpoints (`manager-handoff-20261012.json` lists
controller argv/pids/rows). Gate order: input hash verification → fresh
normal/fault builds → full Make-test + standalone scripts → focused ASan/UBSan +
real-LSM sanitized audits → 131-outer-gate private live matrix → production
acceptance. No PASS carries across candidates; old controllers retired with idle
proofs (`human-stop-20261011/intel-human-stop-idle-proof.json`). Cleanup: all
controllers/fixtures retired, production processes/mounts verified unchanged.

## Results
| Gate | Source | Result |
|---|---|---|
| Normal+fault Make-test (244 recipes) + 88 standalone | c2bfb9e9 | PASS both hosts (180 native outer rows each; 4 initial cluster-stop FAILs from runner path omission, 4 configured rechecks PASS) |
| 20 focused ASan/UBSan + read-errno regression + sanitized LSM audit | c2bfb9e9 | PASS both hosts (24 raw rows each) |
| Strict CREATE-retry (direct+buffered) | c2bfb9e9 | FAIL all four gates — duplicate `0,1,2` fault-stage sequences from an independent retained-only hook in `create_request_rpc`; controlled repro + 27-line fault-only removal drafted, live repair NOTRUN |
| Ordinary namespace-max (buffered) | 0ae7e480 | FAIL (145 participants at 144 ancestry hops; fixture expectation wrong); native repair dcd0d8be PASS, repaired live NOTRUN |
| Portal HTTP, config forwarding, register/namespace native proofs | devel-intel | PASS (native/ephemeral only) |
| E073 fault coverage | e07345c3 | 14 integration PASS per host/mode, 1 repair GAP, 1 simulator NOTRUN (`fault_sim` unbuilt) — correction of an earlier 19-PASS claim |
| Production tiny-file jobs | production | 1,000,000 files per host completed exit 0 (2M total); 256 payload samples verified per host after reboot recovery |
| 131-outer-gate live matrix, Valgrind, mounted acceptance, deployment | c2bfb9e9 | STOPPED/NOTRUN |

## Limitations and remaining gates
- No candidate completed mounted acceptance; CREATE + downstream common-candidate
  acceptance remain FAIL pending AMD's reviewed minimal fix and next freeze.
- Central Linux leak-enabled run for the CREATE repair, fresh mounted/cold gates,
  Valgrind, fault integration, and 27 independent live rounds never ran.
- Two named fault repair GAPs and a writable-mmap SKIP remain open; production
  rollout was never authorized.

## Evidence index (paths relative to this directory; sha256 truncated to 16)
- `acceptance-in-progress.json` — 8cac53669362800f — ledger state, retained failures, pending coverage
- `manager-handoff-20261012.json` — 997f65b56778904e — frozen source, controller/row ownership
- `human-stop-20261011/checkpoint.json` — d6d0283f2e9e687d — human-stop checkpoint
- `human-stop-20261011/intel-human-stop-idle-proof.json` — f006a1d2b5f89375 — both hosts idle, production preserved
- `intel-c2bfb9e9-inventory.json` — 965cbd43092b8564 — c2bfb9e9 input inventory
- `c2bfb9e9-native/intel1_efs/artifact-sha256.json` — 8d272054b201cdaf — 219 verified per-host artifacts (intel2 sibling alongside)
- `create-retry-double-hook-c2bfb9e9/live/intel1_efs/failure-proof.json` — 8306442d3cea8058 — CREATE-retry live FAIL evidence
- `create-retry-double-hook-c2bfb9e9/intel-c2-create-integrated-proof.json` — 1ced4ccaa3962be1 — controlled repro + proposed-fix proof
- `ordinary-namespace-failure-and-repair-dcd0d8be.json` — cccb5be7f8dbc226 — namespace-max FAIL and native repair
- `original-tiny-jobs-complete.json` — 4eee6b6e154d8bfd — 2M tiny-file completion proof
- `host-reboot-production-recovery.json` — 084f08b02b8a40a1 — post-reboot production recovery verification
- `e073-fault-coverage-correction.json` — 9559df2a6410cbc4 — corrected E073 fault coverage accounting
