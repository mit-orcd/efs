# Graphify review findings — October 27, 2026

**Branch `graphify-review-fixes`, based on devel `063154e0e815`.
Original production fixes built at `e95cf62b4`; FUSE geometry follow-up
`240255818` has focused local checks only, as recorded below.
Full Linux suite FAIL; corrected focused follow-ups PASS.
Nine unchanged-base RDMA failures and five NOTRUN checks remain. Nothing merged, pushed or deployed.**

## Purpose and scope

Related apply-refusal gates: [W127–W131](../backlog/work-items.md).

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

The `c0586c891` Linux product/test-binary build PASS; complete run **FAIL**:
515 commands, 40 failures, 5 NOTRUN, 1046 seconds (build 33 seconds).
31 failures are extraction harness dependencies on removed helper names and
one changed comment boundary. The final adapters and harness corrections
address those dependencies without disabling warnings or removing assertions.
Nine failures are RDMA setup (`CQ moderation off failed`): declaration gate,
RDMA transport, and seven segmented modes. They remain failures; the unchanged
base comparison reproduces all nine failures unchanged. Complete failed-suite
commands, build log, summary and all 40 failure logs are retained at
`/data1/efs/evidence/graphify-review-c0586c891/`, with a full hash manifest.

`5d403b68f` was superseded while still waiting, before build or tests (rc143,
not a product failure or gate PASS). Staged `1b83afbf6` was not run.
**`e95cf62b4` complete Linux suite FAIL:** 515 commands, 12 failures,
5 NOTRUN, 1089 seconds (build 32 seconds), 2 over-budget commands.
NOTRUN: external cluster scripts absent, follower fault-build prerequisite,
Valgrind absent, sanitizer runtime absent, and async durability needs XFS.
None of these is counted as a passing check. Nine
failures are the RDMA setup cases above. All nine also fail on unchanged
`063154e0e815`, with CQ-moderation setup errors; this establishes that they
are not introduced here, not that RDMA works. The host exposes an RDMA device;
its setup failure has not been diagnosed by this work. No RDMA tests are
skipped or forced to TCP to obtain a passing label.

The other three failures were two extraction/assertion harness dependencies
and B6's full-quota packing assertion. `64d991c1e` narrows the WB registry
extraction and corrects the old GC verdict expectation that APPEND_RSV I/O
could be consumed at one length. Both pass locally; Linux follow-ups PASS as recorded below. The historical full-suite FAIL is retained.

### B6 follow-up

`6b3991d0a` fixes an independently demonstrated counter defect: the packer
logs a process-wide cumulative counter, but B6 summed it once per group and
term. Its deterministic regression fails on the original function and passes
with the correction. B6 now preserves failed fixtures and records partial
results and cleanup errors; cleanup errors cannot produce a PASS.

The corrected counter alone produced one full-quota PASS followed by a FAIL
in two live runs. The retained second fixture shows two segments, each with
three 16,384-byte fragments on distinct nodes. A quota refusing an ordinary
64 KiB fragment can still fit a smaller packed fragment; quota exhaustion is
not the `refuse` fault hook. `6fc24284d` restricts the unavailable-destination
assertion to down/refuse, keeps full-mode ENOSPC and no-degraded-state checks,
and adds fresh-mount exact reads of all acknowledged small files while still
at quota. The subsequent existing post-restore and cold-restart exact reads
remain. No product timeout, quota or durability rule changes.

The original failed B6 fixture from `e95cf62b4` was deleted by the old
harness; only its failure log survives. It cannot retrospectively be blamed
solely on either oracle defect. New B6 fixtures were retained throughout diagnosis. After follow-up,
logs plus every stored file's size and SHA256 were saved before pruning
these synthetic stores, per repository policy. Exact-store replay is
forfeited; reproduction uses the committed recipe. Results and inventories:
`/data1/efs/evidence/graphify-review-b6-6b3991d0a/`, manifest SHA256
`c9f06788c9708aaa947d4790981c2bc581df523b7235432fd9871f8114b991a4`.
The final repeats' stores were pruned the same way, with `PRUNED.txt` and
inventories in their evidence directory. Original W123 evidence remains untouched.
Final focused driver `6fc24284d` runs WB/GC/counter regressions and three
consecutive full-quota cases, each with the original 900-second outer bound,
under `/data1/efs/evidence/graphify-review-final-6fc24284d/`.
The reused e95 production binaries are unchanged by intervening commits:
daemon SHA256 `9c0058741df63f51f495b2550995fa948372fdc063f6405f4adffc3a22fbba1e`,
mgmt `f8823521cb3d1aafb685dbda111489a7ca9652eda913090fcec8091e610aa752`,
FUSE `9074b992256a633dc22708c0c5e634c07c37a6443961374161d4656cd6de6de4`.
All three daemons and mounts use those same-build binaries. This is focused
follow-up evidence, not a freshly green full-suite result.

**Final focused follow-up PASS:** WB runtime, GC verdict and counter
regressions pass on Linux. Three consecutive full-quota runs pass in 40.8,
47.0 and 51.3 seconds. Every acknowledged file is read exactly after restore
and again after whole-cluster orderly restart: 47, 48 and 48 files respectively.
This finite synthetic gate does not replace large-scale or power-loss testing.
Final evidence manifest SHA256
`58c9c23b55b1f99d247880b383917b75ec5b41674ca54243362a161a80612134`.
All private daemons and mounts from these runs stopped cleanly; no test
runner remains active. The branch is ready for review, not a fully qualified
RDMA release or a green rerun of all 515 commands.

Fourteen focused host/client Python regressions and the participant/prepare
bounds checks PASS locally. Seven native C regressions (KV, metadata apply,
simulator, transactions, RAM/disk query catalogs and writer ranges) PASS
against the portable core. Original/corrected local logs remain outside Git
at `/Users/mike/Documents/Codex/artifacts/graphify-review-20261027/`.

Corrected exact-source reproduction on a Linux host with GCC, FUSE3 development
headers, libibverbs, Python 3 and the normal project test prerequisites:

```sh
git archive e95cf62b4 | tar -xm -C /tmp/efs-graphify-e95cf62b4
cd /tmp/efs-graphify-e95cf62b4
EFS_GIT_ID=e95cf62b4 EFS_GIT_BRANCH=graphify-review-fixes \
  EFS_BUILD_JOBS=4 EFS_TEST_JOBS=4 EFS_TEST_TARGET=test-all \
  nice -n 10 bash scripts/make_test_summary.sh
```

Create the empty destination before extracting. The summary runner builds
all test binaries, runs every fast and long command, preserves individual
failures and reports NOTRUN separately. Four test slots respect its exclusive
command scheduling; no per-test timeout is increased. Raw output is outside
Git under `/data1/efs/evidence/graphify-review-e95cf62b4/`; the temporary
source run is `/tmp/efs-graphify-e95cf62b4`. Full-suite text evidence SHA256 manifest:
`sha256.json`, SHA256 `6fe872945d9b3653a899a87a0d32ab788cd9cfb5e131a817338589cfaffd12ac`.
Unchanged-base evidence is `/data1/efs/evidence/graphify-review-base-063154e0e815/`,
manifest SHA256 `33ce1319ec3f013340a776b2e5c27ce39ed4dd3a9b144c8e17d10a13c83e4150`.
Temporary build trees remain on AMD for the unresolved RDMA setup
investigation. B6 synthetic stores were retained during diagnosis and then pruned as above;
no deployed cluster or original W123 store
was modified. Raw artifacts are not copied into Git.

Focused full-quota reproduction after building this branch's Linux binaries:

```sh
python3 tests/test_wb_runtime.py
python3 tests/test_gc_verdict.py
python3 tests/test_b6_log_accounting.py
for i in 0 1 2; do
  EFS_B6_MODES=full EFS_B6_KEEP=1 EFS_B6_ARTIFACTS=/tmp/b6-result-$i \
    timeout 900 python3 -u tests/test_b6_unavailable.py || break
done
```

Use fresh evidence destinations. `EFS_B6_KEEP=1` retains synthetic stores
for inspection; default cleanup removes passing fixtures, and failures are
retained automatically. Check logs for cleanup errors before deleting a
stopped fixture. Device-specific RDMA qualification and a live replicated
metadata disk-read fault gate remain outside the demonstrated scope.

## Refreshed graph and final geometry duplication — October 27

The original graph had not refreshed after the first implementation: its
208 manifest fingerprints matched the base snapshot, while 31 indexed files
differed from the branch. Rebuilding a separate graph from `71d0553ba` exposed
one remaining identical geometry rule in `fuse_chunk_size()`. Code commit
`240255818` makes that function delegate to `efs_client_chunk_size()`; its
name remains a thin adapter for callers and focused test seams.

Focused validation PASS on the macOS arm64 workstation: native shared
primitives (valid/default/invalid geometry), client-memory/read-pressure,
pressure-admission and WB worker-budget tests. Exact commands:

```sh
cc -std=gnu11 -O1 -Wall -Wextra -Werror -Iinclude -I. \
  tests/test_shared_primitives.c src/common/common.c -o /tmp/efs-graphify-shared-primitives
/tmp/efs-graphify-shared-primitives
python3 tests/test_client_memory.py
python3 tests/test_pressure_admission.py
python3 tests/test_wb_worker_budget.py
```

These are native helper and extracted-harness checks, not a new full Linux
FUSE build or live mount qualification. The historical full-suite failures
and NOTRUN outcomes above remain unchanged.

**Graphify 0.9.97 refresh PASS:** all 211 tracked files under `src/` and
`include/` are copied from code commit `240255818` into the external snapshot
`/Users/mike/git/efs-graph/core/efs/`; its `.graphify-source-commit` records the
full commit id. All 211 manifest content hashes were verified against that
snapshot and all source files against the branch. Graphs were generated by
Graphify itself, not constructed or edited manually:

```sh
GRAPHIFY_OUT=/Users/mike/git/efs-graph/core/graphify-out \
  graphify update /Users/mike/git/efs-graph/core/efs --force
```

`--force` permits the intentional node-count reduction from removing cloned
helpers. No semantic extraction or external LLM is used. Result: 6,678 nodes,
22,564 edges and 212 communities. The HTML aggregates these into 212 community
nodes and 1,235 cross-community edges because the graph exceeds 5,000 nodes.
The Graphify query for `fuse_chunk_size` now connects to the shared chunk-size
helper; source inspection independently confirms that delegation.

Updated outputs are at the original external location:
`/Users/mike/git/efs-graph/core/graphify-out/graph.html` and `graph.json`.
SHA256 HTML: `c2b73046736822d935e15041b9eba4d9face181a14c800b672ccd60bfaefcc15`;
JSON: `04e1ef76b831c7b3d827fb8b12001656e35b06cfdbf32d95d0feac891762c9f6`.
The previous complete graph/source snapshot, provenance, refresh output and
focused check logs are retained outside Git at
`/Users/mike/Documents/Codex/artifacts/graphify-review-refresh-240255818/`
(previous snapshot in `previous-core/`). Generated graph captures stay outside
Git; this summary and the source change are committed.

Graph coverage limits: Graphify reports partial parsing at protocol.h line
322 and no symbols in 43 files. Those are extractor limitations, not compiler
findings or proof that those files are correct. Its inferred call edges and
community layout guide source review; the refreshed graph does not establish
that every defect or duplication in EFS has been removed. HTML generation and
source/manifest consistency were checked; no visual-browser QA is claimed.
