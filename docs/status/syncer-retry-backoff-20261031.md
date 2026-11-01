# PUT syncer retried a failed `syncfs(root)` in a tight loop (Task CI, Oct 31)

**Purpose.** Fix the retry storm Task CG saw while repairing the K11 fault case ([page](k11-put-barrier-case-20261031.md)), and decide whether it is a K register row. It is: **K87**, FIXED by this task, blocks alpha: no.

**Verdict.** **PASS** (fix verified on intel1; gate line below).

## Mechanism

`put_sync_thread` (`src/server/store.c`) runs a barrier whenever its epoch is due: `cur_frags` is non-zero and either a COMMIT asked or the epoch is older than `EFS_PUT_SYNC_MS`. When `syncfs(root)` fails, `put_sync_barrier_locked` sets the sticky `error` (logged once: `COMMIT and fsync return EIO until restart`) and puts the failed epoch's fragments back into `cur_*` with their old `cur_first_ns`. The epoch is therefore due again on the very next loop pass, which retries `syncfs` with no wait: a busy loop for as long as the root keeps failing. The daemon's own log does not grow (one line), so the cost is CPU and repeated `syncfs` calls on an already failing device. CG's 15,000 lines/s came from its first shim, which logged every refusal; the shim now logs once.

## Fix

`src/server/store.c` only. After a failed barrier the root waits `retry_delay_ns` before the next attempt: 50 ms, doubling to a 2 s ceiling (`PUT_SYNC_BACKOFF_*`), reset by a successful barrier. The thread's timed wait takes the later of the epoch deadline and `retry_at_ns`; the wait computation now also cannot underflow when the epoch is past its window. A later successful `syncfs` is noticed within one back-off step (at most 2 s) and advances `durable_seq`; `error` stays sticky, so COMMIT and `fsync` keep returning EIO until restart as before. Logging: the first failure as before, then at most one `barrier still failing (N more attempts, retry every M ms)` line per minute per root. No error path was removed.

Check: `test_barrier_backoff` in `tests/test_store_durability.c` (existing unit binary, no gate entry) makes `syncfs` fail with EBADF for 1.2 s and asserts 3..8 attempts (about five; thousands before), then restores the descriptor and asserts the success is noticed with `error` still sticky and the delay reset.

## Measurement

Host intel1 (8 CPU, XFS, direct fragment I/O, TCP, private four-member cluster, ports 18700-18703, 1 GiB quota), tree `54d0db3ff` (CG's shim and case) built with `make -j4`, build id `unknown`; "after" adds this change. `scripts/measure/syncer_retry.sh <tree> <out> 18700 30` runs `gc_reclamation.py --put-barrier-fault`, freezes the driver once node 1's first barrier failed (daemons and marker keep running), then samples node 1 for 30 s: `top -H` for CPU, `strace -f -e trace=faccessat,faccessat2` counting the shim's check of `.efs-durable-epoch` (one per refused attempt; ptrace slows the thread, so the before rate is a lower bound), and the growth of `n1.log`.

| 30 s window, node 1 | before | after |
|---|---|---|
| refused `syncfs` attempts | 431,324 (14,377/s, lower bound) | 16 (0.5/s) |
| syncer thread CPU (`top -H`) | 99.7% of one core | 0.4% (busiest thread) |
| `n1.log` growth | 1 line/s, 290 B/s (periodic iostats, unrelated to retries) | same |

The "before" driver then timed out on `raft-getchunks` after being frozen for about 90 s (expected, a measurement artifact); the "after" case ran to PASS.

## Verification

K11 case (`gc_reclamation.py --put-barrier-fault`, EIO, bytes readable, exact after restart, cold state drains) three runs, `test_store_durability`, then the budgeted gate: K11 case PASS 3/3 (rc 0, `failed server PUT barrier ... PASS` each); `test_store_durability` PASS including the new back-off check; **gate `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`: `TOTAL 583 FAIL 0 NOTRUN 12 STATUS INCOMPLETE (912s) OVERBUDGET 1 BUILD 14s`**. INCOMPLETE is the 12 hardware NOTRUNs on intel1 (RDMA device, valgrind headers, `~/git/cluster` scripts), as on every non-RDMA host. The one OVERBUDGET is `tests/test_pump_logging.py` (111 s against 95 s), a log-pump test that does not touch the syncer; not investigated here.

Raw captures are outside Git: `intel1:~/efs-artifacts/taskCI-20261031/` (`SHA256SUMS` sha256 `f87798f41c6bcac47043b82375fbd25f958195ea42ccb2252f1bb74092469558`).

## Reproduction

```bash
# built tree on a host with FUSE, strace, python3; ports 18700-18799 free
mkdir -p $OUT; bash scripts/measure/syncer_retry.sh $PWD $OUT 18700 30   # summary in $OUT/summary.txt
python3 tests/live/gc_reclamation.py --put-barrier-fault --root $D/p1 --root $D/p2 --port 18710
./tests/test_store_durability
rm -rf $OUT/roots $D        # cleanup (use edelete for trees)
```

## Limits

Only the refused-`syncfs` fault was driven; the real-disk EIO path shares the same code but was not injected separately. The K86 rows in the checklist are copied verbatim from Task CF's branch (`claude/festive-jang-d87266`) so the register stays contiguous on this base.
