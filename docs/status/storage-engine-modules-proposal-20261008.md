# P4.1 · Proposed built-in storage-engine modules

Status: documented proposal; implementation has not started. This is an enhancement
under the existing P4.1 storage-engine work, not a new numbered work item or an
accepted D-series decision. Branch: `devel-storage-engine`, baseline `00db7842`.
The user wants lean daemon code and asked whether storage engines can be plugins.

## Recommendation

Extract files and containers into built-in modules with one small storage-engine
interface. Select a module at startup using the existing
`--storage-engine files|containers`; keep files as default. Compile both into the
binary initially. Source modularity does not require dynamic loading, and built-in
modules do not by themselves reduce binary size. Optional build-time engine
selection can be considered if binary size later matters.

The existing `efs_store_ops` in `include/efs/store.h` is a useful starting point,
but currently the production adapter delegates to `store.c`, which still branches
on the selected engine. GET/PUT/delete alone cannot hide the physical backend:
startup recovery, inventory, usage reconciliation, GC execution and target lifecycle
also need explicit boundaries. Avoid adding a second competing store abstraction;
review whether to extend the existing interface or place a narrowly scoped physical
engine interface beneath it before implementation.

## Ownership boundary

| Shared daemon/storage coordination | Physical engine module |
|---|---|
| Networking, RPCs, Raft and placement policy | Physical layout, reads and writes |
| Writer scheduling and inode/fragment lifetime locks | Engine-private descriptors, allocation and synchronization |
| Logical quota admission, reservations and reconciliation | Reliable old/new payload sizes and publication outcomes |
| GC authority: which identities/generations may be removed | Checksum-conditional deletion and physical reclamation |
| Target configuration and capability checks | Target open/close, root identity validation and recovery |
| Usage policy and diagnostics | Inventory of authoritative records and physical allocation |

Quota coordination may live in a shared storage adapter rather than the daemon
entry point. Preserve the round-8 single mapping lookup: carry a prepared record or
an engine transaction context from admission through publication and retirement.
Do not reintroduce duplicate mapping reads merely to move quota code between files.

Engine modules should receive opaque engine/target contexts and the small set of
services they need, rather than the entire mutable `efsd_server`. Pass fragment
identity and relevant configuration explicitly; do not expand reliance on TLS or
server globals. Keep placement separate from physical paths where practical, while
preserving existing file names and mapping identity during this refactor.

## Interface requirements

Keep a compact operations table and a small configuration/result contract covering:

- Engine/target initialization, exclusive root ownership, validated recovery and
  shutdown after users and writers have drained.
- GET, durable PUT, checksum-conditional delete, and bounded inventory iteration.
- Usage/reclamation reporting and target-management capabilities. Files currently
  supports dynamic target attachment; containers rejects it. Preserve that behavior
  through a capability check rather than engine-name conditionals.
- Publication outcome independently of return status. A directory-barrier failure
  can leave a new mapping visible with an uncertain durable outcome; quota and
  recovery must not interpret every error as an unpublished operation.
- Explicit checksum provenance: NONE, PRESENT, or VERIFIED for the complete
  returned payload. Retain backend corruption detection and avoid duplicate RPC
  hashing when verification already covers the complete result.

Choose one engine per daemon initially, with N >= 1 storage targets. Mixed engines
per target, live switching, migration and hot reload are outside this refactor.
Metadata table fragments retain their existing buffered files path regardless of
data engine or direct-I/O selection. Shared routing should make that exception once.
An engine inventory never grants authority to delete a live generation. Container
startup may reclaim unreachable extents only after validating the authoritative
mapping set, as it does today.

Move engine selection to startup and keep dispatch outside inner I/O loops. The
interface should replace scattered physical-layout branches without growing a
large generic plugin framework. Modules can live in an engine directory with a
small registry; exact file names and operations are implementation choices.

## Benchmark integration

Let `efsd` and applicable `efs-bench` storage paths share the physical modules and
contracts. Keep `efs-bench` an optimized I/O storage-engine simulator: reuse should
not require it to run Raft, RPC handlers, a production namespace or the full daemon.
Its workload arguments and lightweight host adapter should supply the needed
context. Preserve existing benchmark semantics and report any adapter difference.

## Effort estimate and sequence

These are rough engineering estimates, not elapsed cluster-runtime guarantees.

| Work | Estimate |
|---|---:|
| Define interface, ownership and result contracts | 2–4 hours |
| Extract files/containers and remove daemon-specific dispatch branches | 6–10 hours |
| Integrate recovery, inventory, GC and multiple targets | 4–8 hours |
| Regression coverage and xorinox A/B acceptance | 4–8 hours |
| Documentation and integration with current `devel` | 2–4 hours |
| Total | 18–34 hours |

Budget approximately 2–3 focused development days, allowing about four days at the
upper end. Current `devel` has independent Raft/metadata work; inspect and reconcile
that baseline before coding rather than modifying another developer's checkout.
Prefer a series of reviewable extractions that preserve behavior and disk format.

Suggested order: write the ownership contract; introduce shared adapter/context;
move one engine at a time; move recovery/inventory/GC execution behind the interface;
connect benchmark adapters; then complete acceptance and documentation. Keep both
engines usable after each extraction. No automatic change of default is implied.

## Acceptance gates

Preserve on-disk formats, root markers, quota behavior, checksums, publication and
retirement ordering, and existing lifetime-lock semantics. Re-run the full Linux
build/test suite and both engines in buffered/direct mode on private xorinox roots.
Cover N >= 1 targets, reordered roots, invalid/overlapping roots, supported/refused
target attachment, concurrent access to the same fragment, quota grow/shrink/failure,
conditional/inode GC, corruption, restart, and injected publication/reclamation
failures. Verify acknowledged data after restarting all daemons and require physical
reclamation after deletion. Preserve installed services and user data.

Use matched unprofiled before/after comparisons to detect refactor regressions.
Retain per-run throughput and latency, including metadata ReadIndex stalls; the
[rounds 7–8 evidence](container-profiling-rounds7-8-20261008.md) demonstrates why
wide ranges cannot establish engine performance. Functional VM acceptance alone
is not power-cut durability qualification or a bare-metal performance claim.

## Optional later: dynamically loaded modules

Separately installed `.so` plugins are feasible after the built-in contract settles.
Estimate an additional 1–2 development days for a versioned ABI, loader and error
handling, packaging/install paths, compatibility checks and tests. Keep host-private
structures out of that ABI and reject incompatible modules before opening/recovering
a root. Load at startup and unload only after complete shutdown; no hot reload.
Dynamic plugins share the daemon's process and do not isolate engine crashes.

Defer this work until independent engine installation has a concrete benefit.
Built-in modules provide the requested leaner source architecture first.
