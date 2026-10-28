# Task Z — candidate settings audit and Stage 1 decision sheet

`agent/devel-task-z`, audit code `1164b55a4`, base `9e4d66a8c`: audit 7/7 PASS; full `make test-all` NOTRUN (startup/docs-only scope), base product unchanged. Evidence: `docs/status/stage1-decision-sheet-20261028.md`. Real-host/RDMA behavior qualification NOTRUN here. No product defect found or fixed. Task Z choices accepted in D46; implementation and qualification remain open.

## Verdict and exact scope

**Inventory and startup-refusal audit PASS on devel `9e4d66a8c` (wave 28).
Task Z scope choices are DECIDED in D46; implementation and S1–S5 qualification remain OPEN.**
The source is pinned for this audit, not declared the frozen alpha candidate.
A1 must choose that candidate; rerun the audit if its product readers, help,
refusals or policy surface change. No tag, merge or push is authorized; A4 test resources are assigned in D46.

The graph located the inventory, CLI and decision pages; the committed source
and actual binary results establish the findings. No product source, format,
policy, timeout, support dimension or default changed on this branch. The only
new executable is the audit driver. Shared checkout edits were not taken.

### Environment and reproduction

Intel1, Linux `6.12.0-211.67.1.el10_2.x86_64`, native GCC build, private source
and TMPDIR under `/home/maik/efs-taskZ-9e4d66a8c/`. TCP/RDMA service, storage,
packing, expiry execution and payloads are not exercised: **no cluster, store
or mount is started by this audit**. Existing production services and Task W's
Intel2 gate were left untouched. No host packages or external repositories changed.

Actual `efsd --version` and `efs-fuse --version` identify `9e4d66a8c`, branch
`agent/devel-task-z`, build `2026-10-28T19:20:51Z on intel1`. Binary SHA-256:

| Binary | SHA-256 |
| --- | --- |
| efsd | fc08331919cf759b4d7a1f7acba97389b1486013e08eb4a3295bb6a8d5db948d |
| efs-fuse | ea651b3aa817c92a4748e775f9091a4846f08b62c6062911ab777c0952727948 |
| efs-mgmt | 5fc559e2afda48eb6dd8daa0b870a72b12de0807dadda2e0428a9cedac2bd6b5 |

Recreate in a **new private home directory**, exporting product source with
`git archive 9e4d66a8c`; add only `scripts/audit_settings_refusals.py` from
`1164b55a4`. Do not substitute a different product build:

```sh
export TMPDIR="$HOME/efs-taskZ-NEW/tmp"
export EFS_GIT_ID=9e4d66a8c EFS_GIT_DESCRIBE=9e4d66a8c EFS_GIT_BRANCH=agent/devel-task-z
make -j8 efsd efs-fuse efs-mgmt
python3 scripts/env_inventory.py --check docs/status/a6-settings-inventory-20261024.md
python3 scripts/env_inventory.py
python3 scripts/env_inventory.py --json
python3 scripts/env_inventory.py --kinds docs/status/a6-settings-inventory-20261024.md
python3 tests/test_env_inventory.py
python3 tests/test_env_removed.py
python3 tests/test_faults_env_ignored.py
python3 tests/test_cli_surface.py
python3 tests/test_cli_docs.py
python3 scripts/audit_settings_refusals.py --expected-build 9e4d66a8c
python3 scripts/qualify.py plan --check
```

The seven audit groups are inventory check, inventory negative controls, real
startup removed-name test, fault-ignore test, CLI surface, CLI/docs agreement,
and the per-name refusal driver: **7 PASS, 0 FAIL, 0 NOTRUN**. This is an audit
tally, **not** a fabricated summary-runner `TOTAL` line. Full `make test-all`
was not run for this audit-only change. Local Mac inventory/negative controls,
CLI/docs and matrix-plan checks also PASS; no Mac runtime support is claimed.

Retained raw evidence: **intel1:/home/maik/efs-artifacts/taskZ-20261028/**,
`SHA256SUMS` SHA-256
`d7264b50c53f542dfc23a4526e7abf68eea185216134c43c33eebed421874724`.
It includes full inventory listing/JSON, kinds, baseline inventory, per-name
probe records, help, exact commands, build log, binary identities and cleanup.
The private source/build tree was removed with `edelete`, errors 0, after
checking no owned process/mount remained; `PRUNED.txt` states what was removed.
No raw artifacts or binaries are committed here.

Two collection mistakes remain visible: the first screen invocation refused
`/run/screen` permissions (fixed by private `SCREENDIR`), and the first new
per-name driver used `--help`, which deliberately bypasses startup validation.
That capture reports 132 false failures and is retained as a **harness failure**.
Commit `1164b55a4` corrects the invocation, not the product or assertion:
missing required arguments exercise the startup gate without any possibility
of serving, writing a store or mounting. All 132 corrected probes pass.
The existing real-startup-argument test independently passes and asserts no
store is created. Help/version are identity/surface checks, not refusal probes.

## A6–A12 findings and changes since October 24–25

| Check | Candidate result | Meaning / limit |
| --- | --- | --- |
| Every reader classified | 139 readers, all with retained kinds | 9 deployment, 33 numeric, 34 observability, 62 fault/test, 1 tool input |
| Fault separation | 60 `EFS_FAULT_*` names; every read fault-gated; 0 outside | Other 2 test readers are `EFS_SIM_KV_DIR` and `EFS_SIM_RAFT_DIR` in the simulator; not normal daemon switches |
| Normal product readers | 77, plus 2 simulator readers in the broad scanner | Total non-fault-gated count is 79; do not call all 139 production settings |
| Refused legacy names | 33 names, 2 values (set/empty), 2 binaries: 132/132 PASS | No removed-name reader remains outside the refusal table |
| A/B selectors | 0 inventoried selector kinds, 0 newly found implementation selectors | One policy per retained path; TCP/RDMA remain intentional support dimensions |
| Compile modes / scanner | 15 modes; 1 `EFS_FAULT_` reporting scanner | Scanner warns normal builds; it does not activate hooks |
| CLI surface | 39 management commands; 11 daemon and 9 FUSE options documented | Removed management/client forms refused by existing live-binary test |
| A11 plan | `qualify.py plan --check` PASS | Plan validation only, not matrix execution |

For a measured static comparison, the old inventory script was run on an
actual Git export of **`5a7463440`**, the [A12 closure](a12-closure-20261025.md)
source: 124 readers, 45 fault-gated, 16 modes, 33 refused names. Candidate:
139 readers, 60 fault-gated, 15 modes, same 33 refused names. **All 15 added
readers are fault-only; no old reader was removed in this interval.** Added:

- `EFS_FAULT_APPEND_HOLD_FILE`, `EFS_FAULT_AWAY_MARK_MS`, `EFS_FAULT_DIR_CORRUPT_FILE`,
  `EFS_FAULT_DIR_SERVE_FILE`, `EFS_FAULT_FWD_TXN_STALE`;
- `EFS_FAULT_PUB_UNIT_NP_FILE`, `EFS_FAULT_QUERY_FIRST_UNFORMED`, `EFS_FAULT_RAFT_ELECT_LOG`,
  `EFS_FAULT_READDIR_LAYOUT_FILE`, `EFS_FAULT_REBUILD_SAMPLE_HOLD`;
- `EFS_FAULT_RMDIR_BUSY_FILE`, `EFS_FAULT_SEGMENT_PUT_STALE`,
  `EFS_FAULT_SEGMENT_PUT_TRANSIENT`, `EFS_FAULT_SNAP_BYTES`, `EFS_FAULT_TICKET_READ_LAG_FILE`.

`EFS_BUILD_EXPIRY` is the removed compile mode: D43's `9d654e55c` compiles
expiry into every build. `05efb3bcf` keeps creates on the ordinary path until
a folder policy is enabled. `EFS_EXPIRY` remains refused; cluster/folder policies
are application semantics, not a second implementation selected by environment.
K41's counters did not add a normal-product settings reader in this comparison.

### Corrections and unfinished audit dimensions

- The old inventory/closure pages' expiry-build and pending-READ_VERIFY wording
  is historical. Current expiry is compiled in; the GET audit retained client
  re-hashing as the **one built-in behavior**, not "verification off".
- D42's late sentence still says READ_VERIFY waits on an audit. The completed
  [GET audit](read-verify-audit-20261024.md) and merged source resolve it. No new
  user choice is needed to keep hashing; no integrity gate was rerun here.
- The checklist's broad K23/S1 crash-recovery deferral cannot override later
  D42/F23: **verified crash recovery is required**; whole-system power-loss
  qualification and the broader W71/W74/W123 scope are separate.
- A8 still has **33 numeric tunables** and several dispositions say "Keep until
  A8". Typed inventory is not proof every override is justified or qualified.
  The reduced profile qualifies built-in values; no arbitrary-value support
  or elimination of tuning complexity is claimed. No new selector was found.
- A9 co-location control is gone; elections and placement reporting remain.
  Actual leadership under current candidate load was **NOTRUN** here; the old
  loopback measurement is historical, not independent-host scaling evidence.
- A10 external devops/cluster/portal changes are **NOTRUN** here. Do not edit
  those repositories under this task; the [A7 external-change list](a7-removals-20261024.md#for-the-parent-changes-outside-this-repository-a10)
  remains a checklist for their owners. Binary refusal detects stale inputs,
  it does not prove every deployment script has been corrected.
- A11's historical prose still mentions add-storage and compiled-out expiry;
  current CLI refuses add-storage and D43 ships expiry. Current matrix-plan
  validation passes, but complete TCP/RDMA × 3/4/5 distinct-host qualification
  is **NOTRUN** here. Task V's private four-host rehearsal failed D3/K52; it
  is not a passing A4 release run or a reason to skip the three-host case.
- The source scanner handles literal/macro/helper reads and preprocessor
  guards with regression controls; it is not a whole-program proof for
  arbitrary dynamically constructed environment names. Inventory completeness
  must continue to be checked when readers/helpers change.

Thus the **candidate static/startup audit passes**, while A8 justification,
A9 load measurement, A10 external deployment consistency, frozen-candidate
rerun and A11 execution remain separate obligations. No topology release
cell or unchecked S item is promoted to PASS by this page.

## Decisions already taken — do not ask again

These are scope contracts, not assertions that their implementation/gates pass.
The [decision register](decisions.md), linked contracts and later user amendments
are authoritative; old chronology is not a competing configuration.

| Subject | Recorded answer for alpha 1.0 | Authority / implication |
| --- | --- | --- |
| Nodes and protection | 2+1 data stripes on three distinct members, on 3, 4 and >4 nodes; all nodes take data placement | D38/D40; no alpha 2+2 or two-loss promise. Metadata groups are 3 full Raft voters, not erasure-coded metadata |
| Metadata placement | N=3: both groups on 1–3; N≥4: groups on 1–3 and 2–4 | D38 amendment/K28; nodes5+ add data capacity, no many-group scaling claim |
| Startup and growth | `efsd ADDR:PORT STORAGE`; `--join [SEED] STORAGE`; automatic ids, formation at ≥3 agreeing members, safe online addition | D40/K33; no mandatory cluster file/manual node id/member count; removal deferred |
| Single loss and rebuilding | Keep write availability on 3 nodes with 1 down; re-protect on member return; on ≥4 prefer three available eligible members | D40/W154, K34/K37; liveness alone does not prove restored protection |
| `fsync` with loss | Degraded success only with matching reconstructible durable fragments, committed metadata and no unresolved publication/incarnation ambiguity | D42/F22/K35; never simply ignore failed COMMIT replies |
| Unclean stop | Verified store recovery before clearing uncertainty; an old ambiguous application write does not become successful | D42/F23/K36; crash tests required, power-loss scope open separately |
| Ticketing and atomic writes | Finish ticketed PUT/flush; atomic multi-chunk publication/observation before alpha | D40/K13/K14; implementations and live gates remain independently reviewable |
| K10 readback limitation | Publish the FUSE kernel page-pinning trigger; no alpha workaround | D40/K10; does not excuse unclassified K52 or other mismatches |
| K9/W87 | Closed without reproduction, root cause or fix; reopen on observed record loss | D41; not a passing defect-fix gate |
| Client loss / retention | No survival promise for unpublished buffered writes after client death; ordinary unresolved teardown refused; explicit forced-discard reporting | D27/D28; not permission to discard live unresolved writes |
| Receipts and admission | Durable operation outcomes and bounded admission with recovery/cleanup capacity | D29/D30; a timeout or missing receipt does not prove abort |
| Abandoned uploads | Collect only after durable session revocation and ticket authority | D31/K13; age/disconnect alone never sufficient |
| Truncate and namespace | Logical truncate contract; bounded namespace operations atomically refuse beyond supported limit | D25 and D32; no larger-operation protocol or compatibility added |
| Directory layout | Sustained-contention policy with bounded migration/hysteresis, subject to its gates | D35; no new always/adaptive selector is approved by this audit |
| `.efs` and expiry | Both ship; commands-policy governs `.efs`; versioned expiry policies govern expiry, cluster default off | D43 supersedes only D42's expiry exclusion; D33/D34 semantics remain |
| Direct fragment I/O | O_DIRECT only, unaligned/small I/O handled internally, durability barriers retained | D42; refused storage/removed flags never silently fall back to buffered |
| One read/query/Raft behavior | Client GET re-hash built in; prefetch16; no splice; bloom on; disk catalog; both Raft batching paths always enabled | D42 + GET audit; retained-path correctness still needs its tests |
| Leadership and diagnostics | Elections only; no co-location pump; RDMA trace only and renamed | D42/A9; no throughput or balanced-leader guarantee follows |
| Lifecycle | Clean unmount drains/settles then always revokes; membership persists | D42; failed drain/revoke is not a clean unmount |
| Transport | auto resolves actual TCP/RDMA; explicit unavailable RDMA fails; both require equivalent guarantees | D42/A8; report actual transport and qualify both |
| Deferred platforms/features | Containers removed; NFS re-export and macOS client out of alpha | D37 and W152; later-alpha roadmap unchanged |
| Gate budget | K22 import/admission gates stay in test-long; W160 target1100s at8jobs | D44; no target increase or gate omission is approved here |
| Broader recovery/repair | W71/W74/W123 deferred as broader release scope, qualified by D42's required crash-recovery subset | D40/K23 then D42; power-loss/recovery claims must name the tested fault model |

## Decisions accepted by the user — October 28 (D46)

The user accepted the recommendations and assigned A4 to fcstor007–011 and
`/data1/01`. [D46 in the decision register](decisions.md) is authoritative;
it supersedes D45's conflicting host allocation and W122 deferral already on
devel, without waiving unrelated campaign prerequisites. No open choice from
the Task Z recommendation package remains; execution is not thereby complete.

| Topic | Accepted answer | Remaining work |
| --- | --- | --- |
| A4 | 2+1, 3/4/5 distinct physical hosts, TCP and RDMA; fcstor007–011, owned test directories under `/data1/01` | Six topology/transport cells, actual A5 parameters and retained full verification; NOTRUN by this task |
| S4 / W122 | Minimal read-only effective-settings listing required in alpha | Implement/bound/test actual values, defaults/provenance, policies, transport and build; no new knob |
| F12 / L22 | Trusted-network-only alpha 1.0; authentication deferred | Publish the unrestricted management-port consequence and externally restrict access; no security-boundary claim |
| K36 | Verified crash recovery required; broad sudden-power-loss qualification deferred | Preserve fsync guarantees/barriers; qualify the crash model and publish the separate power-loss evidence limit |
| S1 supported CLI | `sync-status` and tested `raft-resync` supported; routine packing automatic | Qualify those workflows; manual pack-compact/quota shrink/leader transfer/data-commit remain outside the supported operator workflow |
| `--writers` | Remove the public override; keep tested built-in worker policy | Parser/docs/test cleanup is required; this decision does not implement it |
| S3 / round-2 U4 | **Member-down clean unmount must be fixed before alpha 1.0; blocks alpha: yes** | Complete flush/settle and safe durable revocation with one member down, retain unresolved writes and authority, qualify the fix; no false clean-stop or limitation waiver |
| Numeric tunables | Justify/bound remaining33 overrides, remove unjustified ones | Engineering audit and removal/qualification, not more user configuration decisions |

A4's nested storage-host sets are fcstor007–009 (3), fcstor007–010 (4) and
fcstor007–011 (5), over each transport. `/data1/01` is assigned test storage,
not a claim that it is empty: use owned run subdirectories and do not touch
another agent's cluster/data. Record actual storage headroom/quota, interfaces,
RDMA devices, client placement, fixtures and evidence before running. This
commit records the assignment only; no remote operation or test was performed.

S1's seven former proposed dispositions are now decided as above. The
all-member storage-fence clean-unmount gap is **required fix**, not an accepted
limitation. K42's healthy-cluster retry fix does not prove that workflow.
S2/S3/S5 reviews, W122, writer-override removal, the numeric-tunable audit and
live qualification still need to finish before alpha. The Stage 1 scope row
records DECIDED choices with those implementation/qualification obligations.
