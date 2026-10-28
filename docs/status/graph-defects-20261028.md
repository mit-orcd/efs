# Graph-led defect review and repairs — October 28, 2026

**Repairs implemented; scoped native and ASan/UBSan checks PASS at
`4ea10be40` (repair commit `dd6d2c5c2`). Original source `3debca7ec` has three reproduced component failures,
retained below and in the independent evidence commit. The subsequent
[full Linux run](devel-linux-orcd-mini-20261028.md) exposed abandoned replay
objects that also need a durable terminal-ticket fence before retirement;
`214c541aa` implements that path with a fresh post-fence metadata check.
The final Linux run has 529 commands, zero failures and two NOTRUN markers:
the fault-only arm separately PASSed; LSan remains unavailable. Memcheck
41/41 and Linux docs 5/5 PASS. No multi-host/release acceptance is claimed.** Related work:
W155 joining, K35/D42 stripe durability and W154 rebuild protection, and the
Raft-store index contract. No broader W-item acceptance is changed.

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
