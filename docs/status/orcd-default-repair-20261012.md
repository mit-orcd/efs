# ORCD standalone repair default qualification — October 12

This follow-up completes the standalone suite's ordinary missing-fragment repair
case on `fix-orcd-fault-repair-rdma`, separately from `release-v0.3.0-pre-alpha`.
The implementation is frozen at `52581e6ca37c7806b6ec472b062870a4a7f5f282` (on top of the durable coordinator
in `ed749cab`). It supersedes the earlier report's default integration GAP;
that report remains historical evidence for the explicitly selected fixture.

## Current work scope — user decision

The user has deferred repair and recovery work for unplanned events, including
power loss, abrupt crashes, interrupted repair and failover/leader takeover.
The current focus is normal operations, clean shutdown and orderly restart.
Automatic background repair and packed-segment failure repair are deferred under
this decision. Existing durability safeguards, implemented repair mechanisms,
fault tests and their evidence remain in place; this scope change does not
establish new failure-recovery guarantees or disable existing checks.

The historical item **“Standalone fault suite: two repair gaps; this harness
forces TCP”** is complete within this isolated fix branch: the simulator and
ordinary-base integration repair cases execute and pass, and the harness
supports verified native RDMA as well as TCP. All three qualified integration
configurations pass without `--repair-experiment`. No additional repair
implementation or failure-recovery qualification is required for that item
under the current scope. Broader W74 remains deferred, rather than completed.
These commits have not been promoted to `release-v0.3.0-pre-alpha` or deployed
to the live ORCD services. Any later promotion requires qualification of that
merged source and is separate from closing this isolated test item.

## Behavior

Every standalone integration run now executes real repair, without
`--repair-experiment`. The removed GAP branch cannot substitute a verdict for
reconstruction. Missing binaries, activation failure, coordinator refusal,
wrong physical bytes, changed store identity, or failed second-loss reads fail
the suite. The simulator and integration verdicts remain separate.

Activation is scoped to daemon environments in the harness's newly owned,
disposable roots. The caller and clients do not receive repair flags. Nonempty
fixture/output roots are refused before startup, preserving old stores and
results. Initial startup creates store identities; later startups explicitly
set initialization to zero. The daemon's external activation and deactivation
safety guards are unchanged. This is not a release activation change.

For each role 0/1/2 the suite publishes a complete ordinary base chunk, removes
one physical role, proves a fresh degraded read does not recreate it, invokes
the actual metadata-led coordinator, and compares the reconstructed physical
payload and embedded digest. It cold restarts all four daemons and both clients,
checks unchanged store identities and all three physical roles, removes a
second original role, and checks exact externally generated application bytes
through a fresh peer. `protected=true` describes the three-role check before
the second injected loss, not the final intentionally degraded state.

The coordinator still commits ADMIT, verifies actual durable target DATA and
SEAL receipts, commits their records, and retires the job. Unit regressions cover
poisoned/truncated sources, lost DATA replies, misbound receipts, and failed DATA
commit without unsafe terminal ACK or debt release. Native RDMA acceptance
requires uverbs FDs and successful upgrades from each current process
incarnation, including cold restarts; no silent TCP fallback is accepted.

## Qualification

All gates below were rerun on ORCD `fcstor006` against the frozen implementation,
with repair flags absent from the invoking shell. Each mounted run uses its own
four daemons/two clients and new storage roots. No FAIL, GAP or not_run remains
in these standalone integration results.

| Gate | Result |
|---|---|
| Native RDMA (`mlx5_2`), direct I/O | **23 PASS, exit 0** |
| TCP, direct I/O | **23 PASS, exit 0** |
| Plain defaults: TCP, buffered I/O | **23 PASS, exit 0** |
| Full fault-enabled native `make all test` | **exit 0** |
| Full normal native `make all test` | **exit 0** |
| Real-daemon activation/incarnation/protocol boundary | **PASS** |
| Local harness environment/root-safety self-test | **PASS** |
| Documentation architecture gate | **5/5 PASS** |

Each mounted run contains six simulator and seventeen integration verdicts,
including three independent repaired-role checks. Exact reproduction commands,
source hashes, binary hashes, raw verdicts, current-incarnation transport logs,
cold-protection journals and the before/after runtime audit are retained in
[evidence](../../results/measure/20261012-orcd-default-repair/summary.json).
The collector normalizes the log's trailing colon after the build ID; raw result
rows are retained unchanged.

The post-test audit verifies **1,603 supplied source/evidence inputs** against
SHA256 fingerprints with no mismatches. All eleven original ORCD processes
retain their PIDs, binary hashes, arguments, relevant environment, source commit
`8b7391b3f3edcf9fc99440d9aad3c240325acb83` and mounted state. Four remain direct-I/O
storage daemons and seven remain packed-write clients, all with live native
RDMA and site UID 246111. No private test daemon/client or mount remains.
No command in this task changes, merges, deploys or stops the release worktree
or its live services; the shared release ref may advance independently.

The removed `--repair-experiment` option is absent from every recorded replay
command. For example:

```sh
EFS_FAULT_BIN=/tmp/efs-repair-default-52581e6ca37c \
  bash tests/faults/run_faults.sh /tmp/efs-repair-default-rdma-52581e6ca37c \
  --integrate --mode direct --transport rdma --rdma-device mlx5_2
```

Use a new empty output directory to rerun: existing stores and evidence are
intentionally refused.

## Scope

This closes the standalone simulator and real-process missing ordinary base-role
repair cases on single-root FILES with 2+1 protection. Repair remains a manually
requested operation. Automatic scanning, packed files, delta spans, containers,
spare relocation, empty replacement stores, damaged existing-role replacement,
and interrupted-job leader takeover are not qualified by this work. W74 is not
claimed globally complete. The physical integration tests use their own four
loopback daemons and two mounts on ORCD `fcstor006`, not the live eleven-process
cluster.
