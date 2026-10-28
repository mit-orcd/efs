# Graph-led defect review and repairs — October 28, 2026

**Reviewed source `3debca7ec`: three component defects reproduced (FAIL against
the required behavior), natively and under ASan/UBSan. Fixes pending in this
initial evidence commit.** This is a focused review, not an exhaustive audit or
cluster qualification. Related work: W155 request joining, K35/D42 per-stripe
fsync proof and W154 rebuild protection, and the Raft-store index contract.

## Findings

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
