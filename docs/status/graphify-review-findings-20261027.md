# Graphify review findings — October 27, 2026

**Branch `graphify-review-fixes`, based on devel `063154e0e815`.
Implementation committed through `5d403b68f`; Linux qualification
RUNNING, not an acceptance claim. Nothing merged, pushed or deployed.**

## Purpose and scope

The user requested implementation of the correctness and duplication findings
from the Graphify review. The input graph is the external snapshot at
`/Users/mike/git/efs-graph/core/graphify-out/graph.json`, Graphify 0.9.97,
6,705 nodes / 22,573 edges from source `063154e0e815`. Relationships located
candidates; source inspection and tests determine the findings. No graph
centrality or cohesion score is treated as evidence of a runtime bottleneck.

## Correctness fix

`host_apply` normalized command-family business verdicts, then used a second
command list to restore infrastructure failures. The second list omitted
SETATTR, XATTR, UNLINK, UTIMENS, RMDIR and TRUNCATE. A failed metadata read
before a write does not poison the KV scope, so an I/O or allocation error
could be consumed with the apply cursor advanced. The second list is removed:
I/O, allocation and unsupported-encoding errors always retain their error.
Business-conflict rules remain separate and unchanged.

The expanded `test_txn_apply_retry.py` fails on the base source and passes
with the fix, covering all 65,536 tag/subtag combinations for those three errors.
`test_apply_unsupported.py` runs the production host apply body over an actual
on-disk LSM with a controlled dispatcher: each of the six omitted command
families returns I/O and allocation errors after staging an effect. The scope
and cursor must remain unchanged, including after reopening the store, and
retry at that same index must then commit. This does not claim a live
three-replica disk-read-fault run; dispatch errors are controlled in the test.

## Duplication changes and review correction

- Shared alignment-safe byte primitives replace 17 identical big-endian
  32-bit writers and their duplicated readers. The little-endian KV codecs
  share distinct little-endian primitives; byte order and valid formats do
  not change. Golden-byte and unaligned-input tests cover both orders.
- Client chunk-size, fragment-size and fragment-pointer rules have one home
  in `client_internal.h`. Read, write and operations use it. Writer-state
  snapshot overlap checks use the existing `efs_writer_buffers_overlap`.
- KV byte ordering and snapshot group filtering are shared between the
  memory/LSM and both snapshot export paths, including exclusion of
  node-local prefixes outside the shard range.
- SETATTR's identical production/simulator format now has one encoder and
  decoder. Both paths use it, and the decoder refuses every short size,
  trailing bytes and a wrong tag without changing its output. Its valid
  bytes remain exactly the current 61-byte format.

**Correction to the review:** similar simulator/production function names
are not proof of duplicate wire formats. CREATE, UTIMENS and several other
simulator operations deliberately use smaller formats or different ownership
machinery. They are not mechanically combined; the simulator keeps its
independent state-machine behavior. Sharing SETATTR is justified by its
matching fields and byte layout, not by name matching. The remaining
command-format differences need deliberate per-command decisions, rather
than making the simulator a call-through to production.

## Validation and reproduction

Portable focused checks run on the arm64 macOS workstation with Apple Clang:
shared golden-byte/geometry/KV-order checks PASS natively and with ASan/UBSan;
apply-return classification PASS; real-LSM apply abort/reopen checks PASS.
A portable core archive excludes Linux transport and log interception files;
it is not a full product build. Full client syntax checking on macOS stops
at the existing Linux `pthread_condattr_setclock` dependency.

AMD is a single Ryzen AI MAX+395 Linux host, GCC 15.2, x86_64. Tests use
private temporary fixtures and their built-in timeouts. No deployed cluster,
RDMA device, ORCD host, W123 fixture or stored user payload is changed.
The existing W123 run is already FINISHED; its failures are not reclassified.

First full Linux build `ca111755b` FAIL: an included session fast-path header
still called the removed `rd32be`. `b02cbe878` corrected the header; its build then caught two direct-include
tests still calling the removed little-endian writer. Those tests now use the
shared primitive (`c0586c891`). Thin host/client adapters over the shared
implementations (`0afa84392`, `5d403b68f`) preserve independent focused-test
models without duplicating byte-order or geometry algorithms.
The failed build log is retained, not overwritten:
`/data1/efs/evidence/graphify-review-ca111755b092/build.log` and
`/data1/efs/evidence/graphify-review-b02cbe878/build.log`.

The `c0586c891` Linux product/test-binary build PASS; its complete suite is
running. Final-source `5d403b68f` is queued behind it, with a refusal guard if
prior processes or mounts remain. Fourteen focused host/client Python
regressions PASS on the final adapters. Seven native C regressions (KV,
metadata apply, simulator, transactions, RAM/disk query catalogs and writer
ranges) also PASS against the portable core. The original and corrected
source logs for local checks remain outside Git at
`/Users/mike/Documents/Codex/artifacts/graphify-review-20261027/`.

Corrected exact-source reproduction on a Linux host with GCC, FUSE3 development
headers, libibverbs, Python 3 and the normal project test prerequisites:

```sh
git archive 5d403b68f | tar -xm -C /tmp/efs-graphify-5d403b68f
cd /tmp/efs-graphify-5d403b68f
EFS_GIT_ID=5d403b68f EFS_GIT_BRANCH=graphify-review-fixes \
  EFS_BUILD_JOBS=4 EFS_TEST_JOBS=4 EFS_TEST_TARGET=test-all \
  nice -n 10 bash scripts/make_test_summary.sh
```

Create the empty destination before extracting. The summary runner builds
all test binaries, runs every fast and long command, preserves individual
failures and reports NOTRUN separately. Four test slots respect its exclusive
command scheduling; no per-test timeout is increased. Raw output is outside
Git under `/data1/efs/evidence/graphify-review-5d403b68f/`; the temporary
source and active run are `/tmp/efs-graphify-5d403b68f`. Validation results,
artifact hashes and cleanup status will be finalized after completion.
