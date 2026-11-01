# K81: a REPORT reply waited for a blocked stderr sink — Task BL, October 30, 2026

**Verdict: attributed and FIXED (K81, blocks alpha: no).** Linked: K72 (wave 39),
K79 (Task BI, not merged), `tests/test_pump_logging.py` gate failures in waves 43 and 44.
Tested source: branch `claude/bl-report-stall-b59547` on devel `e2c5067d1`;
the fix commit is in `git log` of that branch. Build on xorinox: `make efs-fuse efsd tests/faults/efsd`
(`EFS_BUILD_ID` unknown, local build).

## Attribution

The foreground EIO is a product failure, not a test flake. Chain, all observed:

1. `telemetry.c:emit` holds `ftrylockfile(stderr)` for a whole record and, when the
   sink pipe is full, polls for space in `send_bytes` (25 ms loops). With a blocked
   sink the reporter therefore holds the stdio lock indefinitely.
2. Any other thread's synchronous `fprintf(stderr)` waits for that lock.
3. The REPORT connection thread printed `report-split` (any REPORT with
   pack+push+finish >= 100 ms, `raft_host.c:24855` on devel) and, for the receipt
   query, `report-receipt query` (every query, `:25056`) with `fprintf(stderr)`
   before sending its reply. The reply was withheld until the sink drained. The
   write itself was already committed.
4. The client bound is 8 s per RPC plus 8 s same-identity lookup. The lookup is a
   receipt query, which blocks on the same lock. After 16 s the client returns EIO
   (`outcome unknown after the bound`, fail-closed, correct behaviour).

Evidence (xorinox, `gdb -batch -p` every 3 s on the leader, run `stk-2`, failure
`('foreground failures', 4, 'late writes', 2)`): two `conn_thread`s in
`__lll_lock_wait_private <- fprintf <- server_raft_host_report` at
`raft_host.c:24855`, while the `report` thread sat in `poll <- send_bytes <- emit`
(telemetry.c:135) on a `batch-obs` record. Across five failing sampled runs the
lock waiters were: `report-split` 70 samples, `report-receipt query` 48,
`host_gc_frag_pass` 48 and `host_gc_thread` 9 (the last two are K79-family, not on
the data path). On the retained intel2 fixture the stalled reports resolved as
applied at the moment the sink was released (n1.log resumes at 01:39:47), and only
the two that hit the bound became EIO.

**Payload mismatch** (`0000000000000004` read, `...01` expected): the same failure.
The write whose fsync returned EIO had been applied; the harness only updates
`expected` on success, so readback shows the newer payload. Not a separate finding.

## Why it was intermittent, and the deterministic reproduction

With the 10 ms fsync delay a REPORT crosses 100 ms only sometimes. At 40 ms every
REPORT does. `tests/test_pump_logging.py` now uses 40 ms (still >20 ms for the
read/apply wait lines it already covers); budget unchanged.

| tree, test | result |
|---|---|
| devel binary, old test (10 ms), 10 solo runs | 4 FAIL / 6 PASS |
| devel binary, new test (40 ms), 5 solo runs | 5 FAIL / 0 PASS |
| devel binary, profile 25 s, 40 ms, 10 runs | 10 FAIL |
| fix, new test, 10 solo runs | 10 PASS |
| fix, profile 25 s, 40 ms, 10 runs | 10 PASS |

## Fix

Seven report-path `fprintf(stderr, ...)` sites in `server_raft_host_report` and
`server_raft_host_report_receipted/_receipt` became `efs_telemetry_log(0, ...)`:
the one existing reporter, no second queue, no lock added or moved. A full mailbox
is counted in the existing `telemetry: overflow=` line. Without a bound reporter
the call still writes synchronously, so non-efsd users are unchanged.
Not changed: GC thread lines and other handler lines outside the report path
(K79 family; the same class can still block those threads, not a REPORT reply),
the debug-gated `raft_dbg_on` lines, the once-only table allocation failure line.

## Reproduction

Prereqs: Linux with FUSE, `make efs-fuse efsd tests/faults/efsd`, `TMPDIR` under `$HOME`.
`python3 tests/test_pump_logging.py` (about 2 min); fixture kept on failure.
Raw captures (outside Git): `~/efs-artifacts/taskBL-20261030/` on the Mac with
`SHA256SUMS` (sampled stacks, run logs, loop scripts); intel2 case-514 copy in
`case514/` there. Scripts `loop.sh`, `sample.sh`, `knob.sh` are in that folder.

## Limits and remaining gates

Gate (`TMPDIR=$HOME/tmp EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`, xorinox,
commit `7ddd4bee1`): 581 commands, FAIL 2, NOTRUN 10 (RDMA, cluster scripts), 1019 s.
Failure 1: `test_w155_report_resolve` did not compile, because it extracts
`server_raft_host_report` and now needs an `efs_telemetry_log` stub; fixed in the
next commit and the test passes on xorinox. Failure 2: `check_k_register.py` reports
K79 and K80 missing, because K81 is numbered after rows that live on the unmerged
BI (K79) and BG (K80) branches; it passes once both merge (merge-order artifact,
not a defect). The full gate was not rerun after the stub fix. Sampling with gdb perturbs timing, so only the stack content,
not the failure rate, comes from sampled runs. K79 (SIGTERM with blocked stderr) is
untouched here.
