# test_barrier_backoff: wait on observed state (Task CT, Nov 3 2026)

**Verdict: PASS for the test fix; gate FAIL 1 from an unrelated known flake (K54 port race).**
No K row: this is a test defect (the product back-off is unchanged), not a product defect. Blocks alpha: no.

## Problem
`test_barrier_backoff` in `tests/test_store_durability.c` slept a fixed 1.2 s and asserted 3 to 8 failed `syncfs` retries.
The syncer thread idles on a 1 s timed wait that `put_sync_note` does not wake, so the first failure lands anywhere in the first second;
under 8 gate jobs on a 192-cpu host the thread is also descheduled, so the count left the window (wave 53 on fstor007; Task CS gates on node9900, rc -6 each time, solo 3 of 3 passes).

## Change
Source commit `db8a837b6` (on `ab264d99e`); test file only, no product code.
- Poll `failures` under `r->mu` (10 s cap) until it reaches 3; assert `retry_delay_ns` equals the exact doubling schedule (50 ms doubled, capped at 2 s) for the observed count, and at least 150 ms elapsed. The sticky-error and recovery assertions are kept (recovery wait 4 s to 10 s).
- Harness fix found while proving it: `reattach()` and the corrupt-record step memset a root whose syncer thread still ran; that stray thread barriered the reused root and set `durable_seq` (about 50% of solo runs of the first draft). `quiesce()` stops and joins it first.

## Environment
node9900 (ORCD, 192 cpus, Linux 5.14 el9), TCP build, `EFS_FUSE_PREFIX=/data1/efs/deps/libfuse`, `TMPDIR=/tmp/efs-taskCT-tmp`, tree from `git archive`. Old binary = same tree with the pre-change test file.

## Reproduce
Ship `git archive HEAD` to `/tmp/efs-taskCT`, `make tests/test_store_durability`, build the old binary the same way from `git show HEAD~1:tests/test_store_durability.c`, then run each binary 10 times while `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh` runs the gate; run the new binary 3 times on an idle host. Clean up `/tmp/efs-taskCT*`.

## Results
| Run | Result |
|---|---|
| old test, 10 runs under the gate pool | 8 pass, 2 fail |
| new test, 10 runs under the gate pool | 10 pass, 0 fail |
| new test, 60 solo runs | 60 pass |
| new test, 3 solo runs, idle host | 3 pass |
| `make test-all`, 8 jobs | `TOTAL 584 FAIL 1 NOTRUN 4`, `test_store_durability` PASS |

The single gate FAIL is `tests/test_k54_member_down_fence.py` (port race on member restart; solo reruns 2 of 3 pass), fixed by Task CS's port-hold change, which is not on devel yet; not caused by this change.
NOTRUN: cluster_stop, rdma_memcheck, qualify_rdma_live, leak_gate (as expected on node9900).
A first draft run under 400 busy loops did not reproduce the old failure (0 of 10); only real gate load did.

## Limits and artifacts
The old-test failure rate is from one 10-run sample taken while the gate was building and early in its pool; the gate FAIL 0 expectation is not met until Task CS lands.
Raw logs: `~/efs-artifacts/taskCT-20261103/` on node9900, `SHA256SUMS` sha256 `5e85e230bbe0bd5580c4cda23f9b746024c8a5b57a305d77f02d10bf81007d4a`.
