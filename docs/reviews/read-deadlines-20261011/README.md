# Read deadline and remote ReadIndex worker review — 2026-10-11

Pinned new implementation: `b1632feb`; comparison base: `bbe8b475f7351a09348379387e9255b55ea8e82a`. Only the new deadline, peer-pool and paired-worker changes were reviewed. No release worktree edits or deployment occurred.

## Concrete finding

The wrapper correctly preserves the earlier caller deadline, but `host_remote_cmd_impl` routing retries only test a separately created 400ms deadline. A controlled production-function probe with inherited 100ms deadline therefore ran 400ms and issued 61 RPC attempts after the caller's deadline expired. These attempts refused transport authority; the finding is a retry-budget/latency defect, not a demonstrated false ReadIndex proof. The existing scope test substitutes the implementation and therefore misses the routing-loop behavior.

`read-retry-budget.patch` adds one read-only expiry check at the top of the actual routing loop. It returns BUSY before another routing attempt after expiry, including the no-peer and already-expired paths. It does not modify mutation replay/error semantics. `test_read_retry_budget.py` executes the actual wrapper and retry implementation with controlled clock/routing/transport endpoints. Baseline fails the inherited deadline assertion; patched source passes normally and under ASan/UBSan: inherited 100ms, no-peer 100ms, already-expired refusal, unscoped 400ms, scope restoration and unchanged mutation uncertainty. Fixed-tick sleeps can still cross an arbitrary deadline by up to one existing tick; this patch does not introduce a hard realtime guarantee or interrupt a local quorum wait.

This is an integration patch against the pinned source, not a production modification installed in this older review branch. Apply it on the integration branch and rerun native/live qualification.

## Ownership and authority disposition

No additional concrete lifetime or authority bug was found in the reviewed new code.

The fixed worker pool uses at most eight persistent 128KiB stacks, with no per-request thread growth. Task admission is serialized by its mutex; the pending pointer is cleared before executing. The worker publishes its completion under the same mutex. Even a first-proof failure waits for the second task before clearing the worker lease and returning, so the stack task and host reference cannot escape that caller. Initialization failure or saturation takes the existing scalar pair path. Idle workers do not dereference the prior host. Source inspection confirms daemon shutdown drains all live handlers before stopping/freeing the host; the query worker is joined during host stop. The test demonstrates task completion before destroying individual mock hosts, not cancellation of arbitrary callers or live daemon shutdown.

Each group still obtains its own existing fresh ReadIndex proof. The paired result is successful only if both succeed; no coverage is shared across calls and no failed proof is converted into authority. The thread-local caller network deadline is copied into the worker and its previous scope restored afterward. Local ReadIndex waits retain their existing wait budgets; the network deadline change alone does not establish a strict shared wall-clock limit for every local wait.

Peer checkout refuses an expired network scope before taking a slot, caps its busy-pool wait to the remaining scope, and retains the existing 5s unscoped limit. Slot ownership remains protected by the pool mutex. Failed sends, missing/short/wrong replies drop their connection; successful forwarded RPCs restore the connection's prior receive limit and existing default send timeout before returning it to the pool. The busy-pool condition wait still uses CLOCK_REALTIME, while the network scope is monotonic; clock-step behavior was not tested or changed here.

## Focused validation

On the patched pinned snapshot, all four existing gates passed normally and under ASan/UBSan (`CC='cc -fsanitize=address,undefined'`):

- `tests/test_host_read_deadline.py`: scope restoration, inherited/expired scopes, TCP/RDMA timeout setter behavior, large frames and invalid/missing replies.
- `tests/test_peer_pool_deadline.py`: scoped/unscoped waits, expired checkout, retained ownership and release.
- `tests/test_readindex_pair.py`: independent fresh local admissions, pending rounds, overflow, routing and refusal.
- `tests/test_readindex_remote_pair.py`: overlap, 200 worker reuses, inherited scopes, saturation, complete/partial initialization failure, first/second proof errors and joined task/host lifetime.

These extract production functions with controlled boundaries. They do not measure real Raft/TCP/RDMA deadlines, pool contention, shutdown under network failure, or mounted pressure. No native/full suite or remote pressure result was produced in this chat, and no prior release acceptance was altered.

Reproduction after applying the patch to an archive of b1632feb:

```sh
EFS_REVIEW_SNAPSHOT="$snapshot" python3 docs/reviews/read-deadlines-20261011/test_read_retry_budget.py
EFS_REVIEW_SNAPSHOT="$snapshot" CC='cc -fsanitize=address,undefined' python3 docs/reviews/read-deadlines-20261011/test_read_retry_budget.py
```

No optional refactors, durable guard changes, deployments or pushes.
