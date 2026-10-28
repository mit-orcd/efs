# Task O — directory operations exhausting transient BUSY retries

Branch `agent/devel-task-o`, base devel `063154e0e`, product fix `b78955995`:
qualification IN PROGRESS; no full-gate PASS claimed. Base loaded core:
211/218 PASS, seven case timeouts; deterministic RMDIR negative control FAIL
with EBUSY after 16 refusals. Evidence: this page. Real-host/RDMA acceptance
NOTRUN (assigned xorinox private TCP loopback only). K49: confirmed client
retry-cap defect, fix committed; acceptance pending.

## Purpose and contract

Task O follows W159's directory EBUSY recurrence, separately from W166's
PUT/REPORT pressure repairs. Transient metadata admission refusal must not
exhaust an unrelated attempt-count limit before the operation's bounded
callback deadline. Errors, ambiguous outcomes and exhausted deadlines stay
errors; no application retry or changed POSIX case timeout qualifies a PASS.

## Source and environment

Base product devel `063154e0e`; stress controller `243b92113` changes no
product code. Negative control `f75257527` adds only a fault-build server
hook, its inventory entry and the deterministic test. It retains the base
client unchanged. Product changes at `b78955995`; `50a32db2a` additionally
preserves W159 failure fixtures and emits the original client/server logs.
Binary identities and final tested source manifest are owed after the gate.

Assigned host xorinox, Linux, private trees beneath
`/home/maik/efs-taskO/`, raw outputs beneath
`/home/maik/efs-artifacts/taskO-20261027/`. Four loopback fault daemons,
normal FUSE client, TCP, O_DIRECT, 1 GiB quota per member, online addition
from three to four members; no existing cluster, `/data1/efs` store, user
mount or reserved port is changed. Sixteen CPUs; initial host load was
0.02 with no other test-account processes or screen sessions.

## Reproduction and findings

### Unmodified product under load — failures retained, causality limited

Build and run the committed controller from the source root:

```sh
make -j8 test-bins
# load1 and load2 are independent built copies of this same source tree.
python3 scripts/qualify_dir_ops.py --work ~/efs-taskO/base-stress \
  --output ~/efs-artifacts/taskO-20261027/base-stress --rounds 10 \
  --load-tree ~/efs-taskO/load1 --load-tree ~/efs-taskO/load2
```

Each load tree runs `scripts/make_test_summary.sh` with
`EFS_TEST_TARGET=test-long EFS_TEST_JOBS=8`; the controller runs the unchanged
`posix_suite.py MOUNT --suite core`, with its default deadlines, against a
separate four-member cluster. First core result: **211/218, seven FAIL,
156.5 s**. Failures: `names_crazy_roundtrip`, `names_crazy_dirs`,
`dir_deep_nesting_beyond_64`, `concurrent_creates_same_dir`,
`concurrent_write_and_readdir`, `mtime_monotonic_many_writes` (15 s each)
and `concurrent_appends_two_proc` (30 s). These are measured deadline
failures, not proof of the directory BUSY conversion. The script stops core
rounds at the first failed round; it does not re-run until a PASS erases it.
Both load pools finish and their failures remain evidence too.

### Deterministic application-visible EBUSY — confirmed K49

`tests/test_dir_ops_busy.py` starts four private fault daemons and one normal
FUSE mount. The fault-only `EFS_FAULT_RMDIR_BUSY_FILE` is a shared finite
counter, consumed under a file lock, only for `task-o-busy` RMDIR and before
reads, receipts or proposals. It injects twenty admission BUSYs across all
members combined, then permits the ordinary operation. No log, store, wire
format or durability behavior changes. Each refusal logs the remaining count
and operation sequence.

```sh
# Negative-control commit f75257527; unchanged base client.
make -j8 all tests/faults/efsd
python3 tests/test_dir_ops_busy.py
```

The single `rmdir()` syscall fails **EBUSY** after sixteen refusals, leaving
four unconsumed refusals. Server traces show one sequence (2), count 19 down
to 4, spaced by the existing 50–800 ms backoff: about **9.55 s**. There is
no lost reply or committed RMDIR to resolve. The original application error,
client and all four daemon logs remain in `negative-live.log`; the failed
fixture remains under the private run tree pending final evidence pruning.
The extracted production-loop assertion in `base-retry-probe.log` fails too:
it requires success on reply 21 within an inherited 30 s deadline.

## Fix and boundaries

- `rpc_send_recv_shard_once` formerly ended every operation after sixteen
  attempts. `rpc_send_recv_dual_once` extended only identified MKDIR to 64.
  Both now use the caller's existing monotonic deadline for transient
  BUSY/STALE, routing and transport retries; callers without a deadline keep
  the finite sixteen-attempt cap. The original request identity and receipt
  resolution rules remain in force. FLOCK grant conflicts, APPEND barriers
  and retained-CREATE results still return to their owning caller directly.
- `ll_rmdir_run` now installs the same **30 s callback bound as MKDIR**, with
  any tighter inherited deadline taking priority, then restores the previous
  bound. This covers the parent read, local receipt-slot wait and RMDIR
  together. Previously RMDIR had no callback-wide deadline and hit the
  9.55 s retry cap (or a per-receive timeout); this intentional deadline
  change is explicit. MKDIR, REPORT, resolution and POSIX deadlines do not
  increase. No new behavior switch is introduced.
- Exhaustion logs name the RPC, actual attempts, deadline versus attempt cap,
  last inode status, elapsed time, deadline and operation identity. Unsupported
  encodings, real namespace errors and a genuinely exhausted deadline are
  not converted into success.

The injected count proves the conversion and bounded retry fix. It does not
prove that every W159 occurrence has this origin, or that unbounded healthy
load has bounded latency. The loaded case-timeout findings remain separate.

## Acceptance still running

- Deterministic twenty-refusal RMDIR on the fixed production client, with
  exact count, one operation identity, success on reply 21 and final absence.
- Fast suite and five standalone runs each of `test_k36_recovery.py` and
  `test_w159_apply_wait.py`, no relaxed waits or application retries.
- `make test-all`, eight jobs, with exact summary and base comparison for any
  failure. `test_dir_ops_busy.py` is wired in `make test-long`; its provisional
  35 s/two-slot budget must be checked against the measured pool duration.
- Closed raw manifest, provenance, hash and cleanup record are owed before
  acceptance. Test stores are private; failed logs/results remain preserved.
