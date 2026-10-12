# Intel frozen handoff — 2026-10-12

Snapshot: 2026-10-12 00:34:58 UTC. This is a source and running-job handoff, not complete release acceptance. Mike's documentation-first instruction in `efs-coding 6 - mgr` was verified directly. New deployment and feature work are paused pending merged-candidate reconciliation; existing private qualification jobs remain running unchanged. No promotion or push occurred.

## Branches and merge inventory

- EFS worktree: `/Users/mike/Documents/Codex/2026-10-10/n-x20/work/efs-intel`, branch `devel-intel`, pre-handoff HEAD `60c86d881480a3e8de55f09a2a9bbf63987869ba`. It was clean before these handoff documents. This handoff commit adds only this status file and its evidence JSON.
- Frozen implementation/test candidate: `0ae7e48021badb2c49956a43e6ddb46796bd487f`, clean independent clone `/Users/mike/Documents/Codex/2026-10-10/n-x20/work/efs-deploy-0ae7e480`. All later branch commits through `60c86d88` contain documentation/evidence only; `src`, `include`, `tests` and `Makefile` have no differences from the frozen candidate.
- DevOps worktree: `/Users/mike/Documents/Codex/2026-10-10/n-x20/work/devops-intel`, clean `devel-intel`, HEAD `12508df70da5f661647d036828f03b9ae9e58e93`. Intel concurrency and explicit Raft settings are validated there. This is a separate repository checkpoint.
- Portal repository: `/Users/mike/git/cluster`, checkpoint `752e115`, physical-host display repair already committed. User `error.log` is unrelated and untouched. Earlier actual HTTP evidence reports two physical hosts, four daemons and two clients; no persistent portal process was replaced.
- Shared EFS `devel` observed at `48a2719d19fb5a6b0464935c70a967a1f1a86236`; original `/Users/mike/git/efs` detached scratch documentation is untouched. Intel does not edit the AMD release worktree.

The [machine-readable handoff](../../results/measure/20261011-intel-devel-intel/manager-handoff-20261012.json) contains **every Intel commit absent from observed `devel`, in chronological merge order**, with full SHA and subject. Merge the complete branch history or reconcile equivalent changes with AMD; do not cherry-pick an isolated tip while omitting dependencies. The latest implementation/test changes, in order, are `3d69edb4` (certified APPEND cleanup retry), `18572efe` (actual exact namespace participant placement), `331566c0` (physical compactor drain), `5abf8e7b` (placement/status helper collision), `236121dc` (explicit storage modes in standalone cold tests), and `0ae7e480` (typed read errno). Intervening evidence commits retain failures and incomplete runs. Source is merge-ready for integration review, **not production-accepted**. There are no unfinished uncommitted source changes or owned subagents.

## Exact current evidence

| Gate on frozen `0ae7e480` | Intel1 | Intel2 | Remaining |
|---|---|---|---|
| Normal/native/standalone main gates | 89 PASS | 89 PASS | 21 normal gates, including mounted concurrency, `.efs`, partitions and Valgrind |
| CREATE/APPEND preflight | 7 PASS | 7 PASS | None in preflight |
| Supplemental full fault/live rows | 41/84 PASS | 40/84 PASS | Remaining direct fixtures and all buffered fixtures |
| Canonical independent rounds | 0/30 | 0/30 | NOTRUN, waiting on main progression |
| Publication-apply modes | 0/2 | 0/2 | NOTRUN, waiting on supplemental terminal |
| Standalone cold safety modes | 0/6 | 0/6 | NOTRUN, waiting on main terminal |
| Fresh complete fault identity contract | NOTRUN | NOTRUN | Exactly 21 identities: 19 PASS and two named repair GAPs |
| New candidate production tests | NOTRUN | NOTRUN | Deployment paused; source qualification incomplete |

No nonzero row exists in this fresh snapshot. This does not waive historical failures or imply complete acceptance. Both hosts already passed the **complete direct peer-open-contention fixture**: twenty concurrent APPEND/OPEN iterations with 800 unique intact records each, full 66-case peer POSIX, exact cold-byte verification and physical cleanup. Existing timeouts were unchanged.

Targeted complete repairs before the fresh matrix passed: exact namespace and compactor fixtures in both modes on both hosts; all six worker allocation/TLS/growth fixtures in both modes on both hosts; native production read-handler regression and expected preceding-code negative controls; extracted production read-handler ASan/UBSan on both hosts. [Worker evidence](../../results/measure/20261011-intel-devel-intel/worker-errno-live-0ae7e480.json) and [sanitizer evidence](../../results/measure/20261011-intel-devel-intel/read-errno-sanitizers-0ae7e480.json) retain their exact scope. Earlier real namespace/compactor/worker failures and one premature-upload build setup failure remain recorded; they are not relabeled PASS.

W104 writable mmap remains an explicit ENODEV SKIP. The two named fault-harness repair GAPs are also explicitly open, not PASS. W100 performance and full source acceptance, W101 preservation, W102/W103 production/concurrency acceptance, and W105 NFS handles remain open at their documented scopes. W106 RDMA placement is decided: AMD software RDMA or `orcd-test` hardware IB/RDMA only.

## Running ownership and continuation

All eight controllers are owned by SSH user `efs`; their exact argv, PID, state, complete raw row lists and evidence paths are in the snapshot. At this instant:

| Controller | Intel1 PID | Intel2 PID | Evidence root on each host |
|---|---:|---:|---|
| Main | 1851116 | 1973709 | `/data1/efs/logs/intel-session-accept-0ae7e480` |
| Supplemental | 1851244 | 1973852 | `/data1/efs/logs/intel-supplemental-live-0ae7e480` |
| Publication apply | 1851365 | 1973993 | `/data1/efs/logs/intel-publication-apply-0ae7e480` |
| Independent cold safety | 1851486 | 1974129 | `/data1/efs/logs/intel-independent-safety-0ae7e480` |

Main source is `/data1/efs/intel-session-accept-0ae7e480`; fault builds use the separate `/data1/efs/intel-supplemental-fault-0ae7e480` tree. Private fixture storage/mounts and ports belong to the runners; do not kill generic `efsd`/FUSE processes, read mounted test data during cold/timed checks, or edit executing inputs. Controllers perform owned cleanup. All 172 build inputs and 400 test inputs were verified before build. The selected candidate configuration is TCP, `bundle1000`, applied interval 1000 ms, overlap 1, no-op 1, `.efs` enabled; both storage modes are required. The five normal binary hashes on each host were captured **once** and are embedded in the handoff; never regenerate them to conceal a changed binary.

Local automatic qualification-to-rollout queue PID 84893, unified session 52406, was terminated with SIGTERM (actual exit 143) after exact UID/argv verification. **No remote controller was stopped.** This removes automatic deployment permission while allowing the existing gates to finish. A read-only observer may remain; it has no mutation authority. Local helper scripts under `/Users/mike/Documents/Codex/2026-10-10/n-x20/work` are task tooling, not uncommitted EFS changes.

Read-only progress: `python3 work/intel-live-progress-short.py` from the task root, using SSH to the two aliases. Preserve exact final raw outputs. A later complete-source verifier is `work/intel-complete-0ae7e480-coverage.py`; it checks the 121-row main aggregate, 84 supplemental rows, 30 canonical rounds, publication modes, six cold cases, exact fault identities and frozen binary/input identities. Do not restart the old automatic queue: its one-time binary capture assertion intentionally refuses an existing capture. Reconcile the manager's candidate and assignment before any new rollout.

## Deployed identities and preservation blockers

Production is still **`7bde5f432b6a6c069eeabaa6a753648c481ab4e5`**, TCP/direct I/O. The handoff records installed version and all five installed hashes, plus the actual four daemon and two client process hashes/argv. No candidate rollout occurred. Intel1 daemons 5945/5953 use `/data1/efs/n1` and `/data2/efs/n2`; Intel2 daemons 10086/10094 use `/data1/efs/n3` and `/data2/efs/n4`; all have 400G quotas. Clients 6602/10331 mount only `/data1/efs/mnt`, seeded from the opposite host, with `EFS_COMMANDS=1`. Production has no explicit bundle Raft environment overrides. Current version/argv are observed, not inferred from repository HEAD.

The original tiny-file jobs completed exactly **one million files per host**, exit 0. No replacement writers were started. Latest retained 512 immutable sample payloads passed exact size/SHA verification; this is not a hash of all two million files. Complete strict before/after inventories and both-mode production cold/persistence verification remain NOTRUN. Immutable sample manifest is `/Users/mike/Documents/Codex/2026-10-10/n-x20/outputs/intel-tiny-payload-manifest.json`; do not regenerate it. Original trees are `tiny-intel1-1m-20261010` and `tiny-intel2-1m-20261010` under the mounts.

Prepared but unexecuted strict rollout tooling (`work/intel-strict-production-0ae7e480.py`) requires complete exact qualification, full strict inventories before/after, exact normal binary installation, concurrent production POSIX and `.efs` in direct and buffered modes, cold persistence, sample checks and a final direct-mode runtime audit. It must be reconciled to the merged release candidate before use. Root packages are already installed; no additional root dependency is currently identified.

## Compatibility and release blockers

Intel introduces no new wire or durable operation IDs in this repair checkpoint. Certified session cleanup refusal uses salt 2; APPEND pre-proposal refusal uses salt 3, only with zero index/term, matching identity and the original deadline. Ambiguous/post-submit/hard failures remain refusals. Do not weaken those certificates or cold-byte/reclamation oracles during integration. Coordinated reservations elsewhere remain 141/142 packing, 143/144 ORCD authority envelopes, durable tags 45–51 packing, 52 NUC, 53 Mac and 54 newer Mac work; those are not Intel acceptance claims.

Release blockers: complete frozen qualification; integration of the merged candidate and its separate requalification; outstanding named GAP/SKIP contracts; full original-tree preservation; new production deployment and both-mode concurrent/`.efs`/cold acceptance; current merged-release W112/packing blockers owned by AMD. Intel has not promoted `main`, pushed, merged shared `devel`, or modified other clusters. Await a specific stability assignment after the manager reconciles this handoff.
