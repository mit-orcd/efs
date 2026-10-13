# Three required work packages — October 13, 2026

User-assigned cluster ownership and feature scope. This table records the
assignment; each cluster's current evidence governs acceptance. **AMD package 2
functional handoff subsequently PASSes on `3409e0c6`**: see the
[final AMD transition/load/fault ledger](amd-package2-transition-load-20261013.md).
ORCD's integrated `d54949ec` deployment passes native/profile/marker checks
and 85.091-second server readiness, but its full catalogue still exceeds the
fixed generation limit near13.5M inodes. Further transient sort-space accounting
work is required on AMD before ORCD closure; see the
[current package 1 ledger](orcd-package1-20261013.md).
ORCD scale/hardware and Intel acceptance stay with their respective ledgers;
no global W closure is implied. Use integrated `devel` and ordinary-file storage.
The original assignment source was `1a737143`, with earlier source-specific
AMD native evidence at `986b4ec0`; those historical checkpoints do not supersede
later frozen-source qualification. See the
[reviewed integration evidence](devel-reviewed-integration-20261013.md) and
[ORCD gate ledger](orcd-efs-400g-20261012.md).

## Features and required work

| Package | Cluster | `.efs` client commands | `.efs` server queries | Small-file packing | Containers | Expiry | Transport / I/O | Required work and completion gate |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 — `.efs` scale acceptance (W91/W92/W108/W109) | ORCD | **Enabled** | **Enabled** | **Disabled** | **Disabled** | **Disabled / deferred** | Native hardware RDMA / direct I/O | After package 2 fixes: rebuild and refresh the full catalogue within the existing 16 GiB generation / 32 GiB aggregate limits; pass all six focused command suites; verify default/human/JSON stats and owner names on seven clients; pass concurrent full-catalogue queries during ordinary-file I/O; same-source orderly full-size cold restart within 300 seconds; verify every payload and exported path in the 1,638,400-file / 400 GiB fixture; measure loaded latency and resource usage. All must pass on the final source; no inherited or sampled acceptance. |
| 2 — `.efs` fixes and functional qualification (W91/W92/W108/W109) | AMD | **Enabled** | **Enabled** | **Disabled** | **Disabled** | **Disabled / deferred** | Available transport; record exact transport and I/O mode for each run. TCP logic tests do not qualify ORCD RDMA. | Fix query footprint/ENOSPC without raising budgets; verify temporary peak accounting, cache-format transition, initial build and catch-up; fix concurrent query correctness, freshness and timeouts at existing deadlines; qualify catalogue retirement, retained readers, restore, clean unmount and orderly restart; test help/discovery, options/pages, permissions, owner/numeric names and default/human/JSON stats; verify smaller deterministic payload/path fixtures. Pass native, relevant fault/sanitizer and isolated mounted regressions for affected code, then deliver fixes to package 1. |
| 3 — ordinary-file correctness and performance (W97/D30/W106 and shared-core dependencies) | Intel | **Disabled** | **Disabled** | **Disabled** | **Disabled** | **Disabled / deferred** | Available transport; record exact transport and I/O mode for each run. | Investigate and fix original 5.569/22.044-second create/fsync/unlink stalls, snapshot/log-rotation contention, retained CLOSE retry/drain and exact EOF publication under load; investigate fork/direct-read corruption; verify ordinary-file GC, complete deterministic payload integrity, clean unmount and orderly restart; run native, relevant fault/sanitizer and mounted ordinary-file tests with commands and query service off. Measure latency/resources and integrate shared-core fixes for rechecking in enabled packages. |

## Runtime settings and boundaries

| Setting | ORCD package 1 | AMD package 2 | Intel package 3 |
| --- | --- | --- | --- |
| `EFS_COMMANDS` — client | `1` | `1` | `0` |
| `EFS_COMMANDS_SERVER` — server | `1` | `1` | `0` |
| `EFS_PACKED_WRITES` | `0` | `0` | `0` |
| Storage backend | `--storage-engine files` | `--storage-engine files` | `--storage-engine files` |
| `EFS_EXPIRY` — server | `0` | `0` | `0` |

Verify actual process configuration after each deployment/restart, including
persisted command-feature policy where applicable; flags alone do not establish
acceptance. Required mounted workloads exclude packed files, container storage
and expiry operations. Native regressions may use isolated synthetic feature
cases to check shared-code correctness; those do not enable features on any
cluster or qualify an excluded mounted profile. Discovery/feature-transition
unit tests remain relevant, but required AMD/ORCD acceptance runs with `.efs`
enabled and Intel acceptance with it disabled.

Package 2 delivers `.efs` fixes to package 1. Package 3 can proceed independently;
shared Raft/KV/client/GC fixes must be rechecked with `.efs` enabled in package 2
before package 1 scale acceptance. Intel is assigned the disabled profile, not
an additional `.efs` enabled development target. This supersedes the earlier
generic suggestion that either AMD or Intel could host enabled `.efs` work.

Do development, fault/sanitizer and small-fixture diagnosis on AMD/Intel; reserve
ORCD testing time for its final hardware and scale gates. Unplanned-event
repair/recovery remains deferred. Existing fault regressions still protect
normal-operation durability; they do not expand this package into automatic
power-loss repair. Historical branch passes and the earlier 400 GiB result do
not close gates on a newly integrated source; AMD handoff is explicitly tied
to its final source in the linked ledger. Expiry implementation
in Git does not authorize runtime activation here. The original assignment-only update performed no service changes. Later AMD
deployment/tests are recorded in its linked evidence; this feature matrix does
not claim an ORCD or Intel result.
