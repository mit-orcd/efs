# Graph-led defect review and repairs — October 28, 2026

**Repairs implemented; scoped native and ASan/UBSan checks PASS at
`4ea10be40` (repair commit `dd6d2c5c2`). Original source `3debca7ec` has three reproduced component failures,
retained below and in the independent evidence commit. The subsequent
[full Linux run](devel-linux-orcd-mini-20261028.md) exposed abandoned replay
objects that also need a durable terminal-ticket fence before retirement;
`214c541aa` implements that path with a fresh post-fence metadata check.
The final Linux run has 529 commands, zero failures and two NOTRUN markers:
the fault-only arm separately PASSed; LSan was unavailable on that run. Memcheck
41/41 and Linux docs 5/5 PASS. The [intel2 LSan follow-up](devel-lsan-intel2-20261028.md)
then PASSed all 49 instrumented entries with LSan enabled. No multi-host/release
acceptance is claimed.** Related work:
W155 joining, K35/D42 stripe durability and W154 rebuild protection, and the
Raft-store index contract. No broader W-item acceptance is changed.

**Follow-up F4/F5 repaired at `199ad2afd`: scoped native 15/15 and
ASan/UBSan 11/11 steps PASS; Linux ticket/GETCHUNKS checks and the complete
data-commit component with ASan/UBSan/LSan PASS. The original `b9c2fad85`
observations and failing negative controls remain retained below. This is
component qualification, with no new deployment or full-suite/release verdict.**

## Repairs and regression gates

- **F1:** stripe records remember an acknowledged publication separately from
  PUT acknowledgements. Direct REPORT success and resolved APPLIED receipts
  notify matching FileID/object records. A NOT_FOUND mark retires the obligation
  only when the sweep snapshot already knew publication succeeded before the
  mark RPC. Otherwise it retains the record and fails closed. The regression
  covers full capacity before publication, a wrong FileID, publication racing
  the mark response, successful marking on retry, and existing legitimate
  post-publication retirement. This adds no product flag or stored/wire format.
- **F2:** pending receipt slots remain reserved through execution. An all-pending
  probe window refuses new identities BUSY without executing their callback;
  the distinct REFUSED result makes the server return BUSY rather than leave
  its zero-initialized reply looking successful. Existing owners/duplicates
  still join, complete, replay and evict completed receipts normally.
- **F3:** memory/disk lookups, memory append/truncate and snapshot coverage
  compare full 64-bit distances before narrowing. Existing on-disk tests now
  check alias rejection, no-op beyond-end truncation, a large snapshot jump,
  legitimate indices above 2^32 and reopening. No persistent encoding changes.

All three behavioral requirements fail on the preserved original source using
`tests/review_graph_20261028/regression.py --against 3debca7ec`; all three pass
on the repairs, natively and with ASan/UBSan. This is a regression negative
control, not mixed-build/format compatibility testing. The original diagnostic
`run.py` remains unchanged and is deliberately for the original-source checkout;
it must not be interpreted as a post-fix passing test.

The normal test matrix runs the new `test_receipt_pending_pressure.py`, and the
existing `test_data_commit` and `test_raft_store` binaries carry the other new
cases. The existing REPORT frame/receipt-resolution tests now require publication
notifications too. `test_make_test_recipe.py` passes; no test budget or deadline
was raised. The manual discriminator is additional evidence, not a duplicate
command in `make test`.

Committed reproduction of the scoped verification (fresh output required):

```sh
python3 tests/review_graph_20261028/verify.py --out /tmp/efs-graph-fixes-native
python3 tests/review_graph_20261028/verify.py --sanitize --out /tmp/efs-graph-fixes-asan
```

The verification runner records the tested commit, platform/compiler, relevant
hashes, every command, exit status and log hash. It runs the full standalone Raft
and Raft-store suites, the full data-commit unit suite, receipt ownership/join and
REPORT resolution/identified-operation tests. ASan/UBSan uses the supported
harnesses; native-only recipe/frame/policy checks are not mislabelled sanitized.
Apple Clang arm64 macOS, local files/threads only, no shared cluster. Leak checking
is not claimed. The checked production paths use no test-only source replacement.

Closed runs on merged `4ea10be40` have 14 native check/build steps and
10 sanitizer check/build steps, all exit 0. Logs are under the stable evidence
root below in `merged-4ea10be40-native/` and `merged-4ea10be40-asan-ubsan/`.
The earlier pre-rebase runs remain in `final-5ecc915d7-*`; they are not overwritten.
`regressions-original-3debca7ec.log` independently records all three requirements
failing on the original production source. Every hash in `baseline-sha256.json`
was rechecked unchanged after the repairs; `final-checks-sha256.json` records the
retained closed verification logs and result manifests. No original failure was
relabeled as a pass.

## Original findings (before repair)

### F1 — Sweep can retire a stripe that has not been published (P1)

`efs_client_put_fragments_parallel` records its acknowledged object in
`efs_client_stripe_note` before REPORT publishes it. Capacity pressure in that
function invokes `stripe_prove(0,1)`. If two fragments are durable and the third
is lost/unproven, `stripe_prove` asks metadata to mark it degraded. It interprets
NOT_FOUND as proof that the object was rewritten/truncated/packed/unlinked and
removes the record (`src/client/data_commit.c:384–403`). But NOT_FOUND also means
**not published yet**, as the current `efs_meta_apply_degraded_mark` implementation
checks only the published base/live spans (`src/meta/meta_apply.c:4441`). A later
REPORT can publish that object with no client durability obligation remaining.

The production-bookkeeping probe, reusing the existing test's storage RPC
stubs and capacity 16, fills the table before publication, changes node 3's
verifier and triggers the sweep with another PUT. It observes 16 unsuccessful
mark requests, only the new stripe remaining, then fsync of an old inode returning
OK with zero COMMITs/marks. Production capacity is 262,144, not 16; this is a
capacity-boundary unit reproduction, not a throughput or live data-loss test.
The later REPORT is a modeled lifecycle step, not an executed network request.
Source inspection establishes that recording-before-publication is a real path;
full mount/REPORT/failover qualification remains NOTRUN.

Temporary discrimination control: keep the obligation and fail closed on
NOT_FOUND. It preserves all 16 old records, refuses the next PUT BUSY, and the
later fsync writes its mark. This is not a complete proposed fix: legitimate
already-published objects that were later retired must still be reclaimable.
The repair must distinguish those lifecycle states and test the ordering race.

### F2 — Receipt pressure permits concurrent execution of one identity (P2)

The graph leads from `server_raft_host_report_receipted` to `efs_receipt_run` and
`receipt_insert`. The latter prefers DONE receipts but falls back to the oldest
**PENDING** receipt when all eight probed slots are pending
(`src/common/request_receipt.c:130–155`). A duplicate of the evicted request
cannot find the running owner and starts its callback again. This contradicts
the joining contract in `include/efs/request_receipt.h`.

With the production 4,096-slot table, eight barrier-held attempts with the same
home bucket plus a ninth colliding request evict the first owner. Its duplicate
executes beside it: peak active callbacks for one identity is **2**, `how=RAN`.
No sleep or probabilistic hash collision is needed; the probe computes valid
colliding identities with the production hash. Existing W155 tests preserve one
pending request while churning finished ones; they do not fill a probe window
with pending owners. Durable REPORT idempotency may still prevent duplicate
publication: duplicate durable mutation/data loss is not demonstrated here.

Temporary control: never evict PENDING; return BUSY when this probe window has
no empty/DONE slot. The duplicate then waits/BUSYs, peak execution stays 1.

### F3 — Raft lookup aliases indices separated by 2^32 (P2)

Both `disk_get` (`src/raft/raft_disk.c:433–437`) and `mem_get`
(`src/raft/raft_mem.c:137–142`) narrow `index-first` to uint32 before checking
it against the retained length. With only index 1 present, `get(4294967297)`
returns index 1's term and payload as a success. This reproduces with the actual
memory store and actual disk store, both warm and reopened. A disk append at the
same invalid index correctly returns INVAL: append already checks in 64 bits.

The required fix is a 64-bit range check before narrowing. The memory append
and truncate paths have the same narrowing shape and need consistent bounds.
This confirms an API correctness defect, not an observed Raft consensus failure:
normal callers often check the log end before querying it. No claim that this
invalid index is reachable through an ordinary client write is made.

## Follow-up repairs F4/F5 — shared deadline and retired objects

### F4 — Sweep metadata RPCs miss the shared deadline (P2)

On `b9c2fad85`, `stripe_prove` computed a 60-second absolute deadline for its
COMMIT workers but called the initial degraded-mark RPC with the caller's
unchanged TLS deadline (zero in the original probe). Each serial mark could
therefore use its own retry/socket budget. The repair gives every metadata
operation the same absolute deadline, shortened by any earlier caller deadline,
checks expiry between stripes and after metadata replies, retains unproven
obligations on expiry, and restores the caller's TLS value. It does not raise
the 60-second limit or change the required durability proof.

`test_sweep_deadline` in `tests/test_data_commit.c` checks the initial mark,
a shorter and a longer caller budget, restoration, and a synthetic monotonic
jump during the first of two marks. Expiry leaves both obligations recorded;
a later successful attempt proves them. The deterministic clock jump avoids
a slow or timing-sensitive 60-second regression.

### F5 — Closed absent stripes with fewer than two durable fragments never retire (P2)

The original closed, unpublished ticket with two changed storage incarnations
returned EIO on three successive attempts without asking metadata whether the
object existed. The absence/retirement path was inside the two-durable-fragment
branch. The repair permits cleanup at any fragment count only after publication
or durable ticket closure **and then** a fresh authoritative absence read.
A still-live object, a failed closure, an uncertain/malformed/expired read or an
identity mismatch retains the obligation and returns EIO.

`efs_client_rpc_object_live` uses the existing FileID-bound GETCHUNKS RPC and
its ReadIndex/authority checks, bypassing local mapping caches. It inspects the
exact chunk's base and complete delta list, distinguishing packed references
and permanently masked zero-length parts. It writes no degraded mark when
fewer than two fragments are durable. No stored/wire encoding, replica count,
feature switch, deadline or quota changes.

The complete data-commit test retains the existing pre-publication, late REPORT,
wrong-FileID, concurrent fsync, COMMIT incarnation and capacity cases. New cases
cover uncertain/present/expired/absent reads, terminal failure versus success,
and published retirement with all three old incarnations gone. The existing
`tests/test_getchunks_budget.py` executes the production RPC and helper against
bounded transport stubs: live base, live span, other object, packed base, masked
base/span, missing inode, stale FileID, bad reply and expired reply. This harness
now honors the shared sanitizer/memcheck environment.

**Fail-closed limit:** the new cleanup read refuses the final 64 chunk indices
with BUSY because today's server GETCHUNKS exclusive window end wraps there.
It must not treat that empty window as proof of absence. The general GETCHUNKS
upper-index issue is outside these repairs; the regression preserves the
obligation at this boundary. No live two-node-loss recovery or data-loss result
is claimed by these component tests.

### Commands, results and retained evidence

Tested product/tests are commit `199ad2afd`, rebased onto devel `08a1606df`, retaining its new full-identity durability index.
Mac: Apple Clang, arm64 macOS, local threads/files only. RPCs are stubbed;
there is no deployed transport, direct-I/O profile, packing daemon or shared
cluster. The data-commit table has 16 slots to reach capacity deterministically.
Native 15 check/build steps and ASan/UBSan 11 steps PASS; the manifests' watched
input hashes were compared with the committed tree. Mac leak checking is off
and is not claimed. Fresh external outputs are mandatory:

```sh
python3 tests/review_graph_20261028/sweep_regression.py --against b9c2fad85 --out /tmp/efs-sweep-original
# Expected FAIL: build succeeds; deadline and retirement assertions both fail.
# This is a negative control, never a compatibility or passing-product gate.
python3 tests/review_graph_20261028/sweep_regression.py --out /tmp/efs-sweep-fixed
python3 tests/review_graph_20261028/sweep_regression.py --sanitize --out /tmp/efs-sweep-asan
python3 tests/review_graph_20261028/verify.py --out /tmp/efs-sweep-native-suite
python3 tests/review_graph_20261028/verify.py --sanitize --out /tmp/efs-sweep-asan-suite
```

The discriminator bounds compilation at 90 seconds and each separate regression
at 30 seconds. `verify.py` retains the existing 180-second per-step bound and
also runs the GETCHUNKS helper natively/sanitized. Normal `make test` already
runs both extended data-commit and GETCHUNKS tests; no redundant normal recipe
entry is added.

Linux supplement: idle intel2, Rocky Linux 10.2 x86_64, kernel
`6.12.0-211.67.1.el10_2`, GCC 14.3.1; isolated source at
`/tmp/efs-graph-sweep-199ad2afd`. Ten environment/build/check steps all exit 0:
`make -j8 libefs.a`, ticket owner, K32 entry counts, ticket RPC retry,
GETCHUNKS, production syntax checking, and the complete data-commit and GETCHUNKS
components with ASan/UBSan and `ASAN_OPTIONS=detect_leaks=1`,
`LSAN_OPTIONS=exitcode=66`. No suppressions added; `__asan_init` and linked
libasan/libubsan were checked. No EFS cluster/process was running at the initial
host check; these commands start none. No performance or full Linux gate is
claimed. Reproduce the supplemental commands from a fresh Git export:

```sh
make -j8 libefs.a
python3 tests/test_put_ticket_owner.py
python3 tests/test_k32_entry_count.py
python3 tests/test_put_ticket_rpc.py
python3 tests/test_getchunks_budget.py
cc -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -fsyntax-only -Iinclude src/client/inode_rpc.c src/client/data_commit.c
cc -std=gnu11 -Wall -Wextra -Werror -O1 -g -Iinclude -pthread -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_data_commit.c -o /tmp/efs-sweep-asan-component
ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=exitcode=66 /tmp/efs-sweep-asan-component
ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=exitcode=66 EFS_TEST_SANITIZE=1 python3 tests/test_getchunks_budget.py
```

The Linux runner records exact build identity/environment and bounds library
build at 300 seconds, other checks at 180 seconds. Earlier optional Mac ledger
attempts failed before testing: missing `libefs.a`, then the Linux-only
`fopencookie` implementation prevented building that library on macOS. Both logs
remain separate from the successful Linux supplement; no product change was
made for those prerequisites.

Original and follow-up Mac evidence stays at
`/Users/mike/Documents/Codex/artifacts/efs-graph-followup-20261028/`:
`results.json` is the original bad-behavior discriminator, unchanged;
`negative-final/` records both failing assertions on original production;
`fixed-native-final/`, `fixed-asan-final/`, `closed-native-v2/` and
`closed-asan-ubsan-v2/` hold the initial passing checks on pre-integration
`b12d5e10e`. Final combined-source checks are in `merged-199ad2afd-native/`
and `merged-199ad2afd-asan/`; the large-file index regression also PASSes
natively and sanitized (`merged-index-native.log`, `merged-index-asan.log`). `original-sha256.json`
(SHA256 c484a0ea1aedf33e520c4661d67a55fb56cb1b57d79e144d3f970bf61af968e6) covers 12 original files, all reverified unchanged.
`closed-checks-sha256.json` records the closed text results and commands.

Linux logs, exact runner/results and instrumentation remain at
`intel2:/data1/efs/evidence/graph-sweep-199ad2afd/`.
Source archive SHA256 `00fca24411a1a3e1309e75a663be6c719e79739975c529d44dc961be31554db2`;
`closed-sha256.json` SHA256
`98f6d0bbc0973c41b086d41871ccd06629aa4414818f5cbbdc980365b5e76f3c`.
Sixteen retained files were verified after pruning only the successful source,
build and sanitizer binary. `PRUNED.txt` and the process/mount check record the
cleanup; protected login/user-manager process entries were identified as
infrastructure, and no process was signaled. Original failures were not pruned.

## Reproduction and evidence

Run from the reviewed source plus the committed reproduction assets:

```sh
python3 tests/review_graph_20261028/run.py
ASAN_OPTIONS=detect_leaks=0 python3 tests/review_graph_20261028/run.py --sanitize
```

**Exit 0 in this diagnostic means all three bad observations were reproduced and
all three temporary control variants stopped them. It never means EFS passes
these correctness requirements.** The script modifies production text only in a
private temporary directory; the controls are discrimination experiments, not
shipping repairs. It records source identity, exact commands and source hashes.
Every compiler/process invocation has a timeout; temporary stores are recreated
and removed automatically. No daemon, mount, shared test host or other agent's
cluster is used. Environment: local arm64 macOS, Apple Clang, pthreads, native
filesystem, no transport or deployed I/O mode. Leak checking is not claimed.

Original logs remain outside Git at
`/Users/mike/Documents/Codex/artifacts/efs-graph-bugs-20261028/`, including
`native.log`, `baseline-native-final.log`, `asan-ubsan.log`, existing-test logs
and `baseline-sha256.json`. The probes compile complete production receipt and
Raft-store implementations. Only the client data-commit case stubs network RPCs,
using the repository's existing fixture. No live corruption is claimed.

Original receipt/data-commit tests pass despite these observations. The first
standalone Raft-store test link omitted `src/raft/raft.c`; its linker failure is
retained as `existing-raft-store.log`, separate from the corrected-link result.
No product or test code was changed to explain that build failure.
